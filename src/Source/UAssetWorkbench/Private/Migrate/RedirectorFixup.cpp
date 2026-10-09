#include "Migrate/RedirectorFixup.h"
#include "UAssetWorkbenchModule.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Engine/Blueprint.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "LevelInstance/LevelInstanceSubsystem.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectHash.h"

namespace
{
    struct FRedirectorEntry
    {
        TWeakObjectPtr<UObjectRedirector> Redirector;
        FName PackageName;
        TArray<FName> Referencers;
        bool bBlocked = false;
    };

    void AddPackageFile(const FName PackageName, TArray<FString>& OutFiles)
    {
        FString Filename;
        if (FPackageName::DoesPackageExist(PackageName.ToString(), &Filename))
        {
            OutFiles.AddUnique(FPaths::ConvertRelativePathToFull(Filename));
        }
    }

    void BlockEntriesReferencedBy(const FName ReferencerName, TArray<FRedirectorEntry>& Entries)
    {
        for (FRedirectorEntry& Entry : Entries)
        {
            if (Entry.Referencers.Contains(ReferencerName))
            {
                Entry.bBlocked = true;
            }
        }
    }

    // Same map the engine builds. A Blueprint is also reached through its generated class and that class's CDO.
    TMap<FSoftObjectPath, FSoftObjectPath> BuildSoftPathMap(const TArray<FRedirectorEntry>& Entries)
    {
        TMap<FSoftObjectPath, FSoftObjectPath> Map;
        for (const FRedirectorEntry& Entry : Entries)
        {
            const UObjectRedirector* Redirector = Entry.Redirector.Get();
            if (!Redirector || !Redirector->DestinationObject)
            {
                continue;
            }

            const FSoftObjectPath OldPath(Redirector);
            const FSoftObjectPath NewPath(Redirector->DestinationObject);
            Map.Add(OldPath, NewPath);

            if (Cast<UBlueprint>(Redirector->DestinationObject))
            {
                Map.Add(FSoftObjectPath(OldPath.ToString() + TEXT("_C")), FSoftObjectPath(NewPath.ToString() + TEXT("_C")));

                const FString OldDefault = FString::Printf(TEXT("%s.Default__%s_C"), *OldPath.GetLongPackageName(), *OldPath.GetAssetName());
                const FString NewDefault = FString::Printf(TEXT("%s.Default__%s_C"), *NewPath.GetLongPackageName(), *NewPath.GetAssetName());
                Map.Add(FSoftObjectPath(OldDefault), FSoftObjectPath(NewDefault));
            }
        }
        return Map;
    }
}

UObjectRedirector* UAssetWorkbench::FindRedirectorInPackage(const FString& PackageName)
{
    // A redirector just deleted can still sit in memory until the next garbage collection, the file is the authority.
    if (!FPackageName::DoesPackageExist(PackageName))
    {
        return nullptr;
    }

    UPackage* Package = FindPackage(nullptr, *PackageName);
    if (!Package)
    {
        Package = LoadPackage(nullptr, *PackageName, LOAD_NoWarn | LOAD_Quiet);
    }

    if (!Package)
    {
        return nullptr;
    }

    return FindObject<UObjectRedirector>(Package, *FPackageName::GetShortName(PackageName));
}

UAssetWorkbench::FRedirectorFixupResult UAssetWorkbench::FixupRedirectors(const TArray<UObjectRedirector*>& Redirectors)
{
    FRedirectorFixupResult Result;

    IAssetRegistry& Registry = IAssetRegistry::GetChecked();
    IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();

    TArray<FRedirectorEntry> Entries;
    for (UObjectRedirector* Redirector : Redirectors)
    {
        if (!Redirector)
        {
            continue;
        }

        FRedirectorEntry& Entry = Entries.AddDefaulted_GetRef();
        Entry.Redirector = Redirector;
        Entry.PackageName = Redirector->GetOutermost()->GetFName();
        Registry.GetReferencers(Entry.PackageName, Entry.Referencers);
    }

    // A referencer resolves the redirector while it loads, so saving it writes the destination into its imports.
    TSet<UPackage*> PackagesToSave;
    TArray<UPackage*> LoadedPackages;
    for (FRedirectorEntry& Entry : Entries)
    {
        for (const FName ReferencerName : Entry.Referencers)
        {
            const FString ReferencerPath = ReferencerName.ToString();
            UPackage* Package = FindPackage(nullptr, *ReferencerPath);
            if (!Package)
            {
                Package = LoadPackage(nullptr, *ReferencerPath, LOAD_None);
                if (Package)
                {
                    LoadedPackages.Add(Package);
                }
            }

            if (!Package)
            {
                UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("%s references %s but does not load, the redirector stays"), *ReferencerPath, *Entry.PackageName.ToString());
                Entry.bBlocked = true;
                continue;
            }

            if (Package->HasAnyPackageFlags(PKG_CompiledIn))
            {
                UE_LOG(LogUAssetWorkbenchMigrator, Warning, TEXT("Code references %s through %s, the redirector stays"), *Entry.PackageName.ToString(), *ReferencerPath);
                Entry.bBlocked = true;
                continue;
            }

            PackagesToSave.Add(Package);
        }
    }

    // Loaded referencers hold no standalone flag, garbage collection during the saves below would take them.
    TArray<TStrongObjectPtr<UObject>> RootedObjects;
    for (UPackage* Package : PackagesToSave)
    {
        ForEachObjectWithPackage(Package, [&RootedObjects](UObject* Object)
        {
            RootedObjects.Emplace(Object);
            return true;
        }, false, RF_Standalone, EInternalObjectFlags::RootSet);
    }

    // A level instance keeps a loader open on its world asset, and that open loader blocks saving the package.
    TSet<FName> WorldAssets;
    for (UPackage* Package : PackagesToSave)
    {
        TArray<FAssetData> PackageAssets;
        Registry.GetAssetsByPackageName(Package->GetFName(), PackageAssets, /* bIncludeOnlyOnDiskAssets */ true);
        for (const FAssetData& Asset : PackageAssets)
        {
            if (!Asset.GetOptionalOuterPathName().IsNone())
            {
                WorldAssets.Add(FSoftObjectPath(Asset.GetOptionalOuterPathName().ToString()).GetLongPackageFName());
            }
        }
    }
    for (const FName WorldAsset : WorldAssets)
    {
        ULevelInstanceSubsystem::ResetLoadersForWorldAsset(WorldAsset.ToString());
    }

    for (auto It = PackagesToSave.CreateIterator(); It; ++It)
    {
        FString Filename;
        const bool bReadOnly = FPackageName::DoesPackageExist((*It)->GetName(), &Filename) && IFileManager::Get().IsReadOnly(*Filename);
        if (bReadOnly)
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("%s is read-only on disk, the redirectors it references stay"), *Filename);
            BlockEntriesReferencedBy((*It)->GetFName(), Entries);
            It.RemoveCurrent();
        }
    }

    TArray<UPackage*> SaveList = PackagesToSave.Array();
    AssetTools.RenameReferencingSoftObjectPaths(SaveList, BuildSoftPathMap(Entries));

    if (SaveList.Num() > 0)
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("Resaving %d referencer(s) of %d redirector(s)"), SaveList.Num(), Entries.Num());

        TArray<UPackage*> FailedPackages;
        FEditorFileUtils::PromptForCheckoutAndSave(SaveList, /* bCheckDirty */ false, /* bPromptToSave */ false, &FailedPackages);
        for (const UPackage* Failed : FailedPackages)
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Failed to save %s, the redirectors it references stay"), *GetNameSafe(Failed));
            BlockEntriesReferencedBy(Failed->GetFName(), Entries);
        }
    }

    // The registry answers from the graph it read at startup until the rewritten files are read again.
    TArray<FString> FilesToRescan;
    for (const UPackage* Package : SaveList)
    {
        AddPackageFile(Package->GetFName(), FilesToRescan);
    }
    if (FilesToRescan.Num() > 0)
    {
        Registry.ScanFilesSynchronous(FilesToRescan, /* bForceRescan */ true);
    }

    TArray<UObject*> ObjectsToDelete;
    for (FRedirectorEntry& Entry : Entries)
    {
        UObjectRedirector* Redirector = Entry.Redirector.Get();
        if (!Redirector || Entry.bBlocked)
        {
            ++Result.KeptCount;
            continue;
        }

        TArray<FName> StillReferencing;
        Registry.GetReferencers(Entry.PackageName, StillReferencing);
        if (StillReferencing.Num() > 0)
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("%s is still referenced by %s after the resave, the redirector stays"), *Entry.PackageName.ToString(), *FString::JoinBy(StillReferencing, TEXT(", "), [](const FName Name) { return Name.ToString(); }));
            ++Result.KeptCount;
            continue;
        }

        FString Filename;
        if (FPackageName::DoesPackageExist(Entry.PackageName.ToString(), &Filename))
        {
            Result.DeletedFiles.Add(FPaths::ConvertRelativePathToFull(Filename));
        }

        // Same rule as the engine: every redirector in the package goes, the package itself only when nothing else lives in it.
        UPackage* RedirectorPackage = Redirector->GetOutermost();
        bool bHoldsOtherAssets = false;
        ForEachObjectWithOuter(RedirectorPackage, [&ObjectsToDelete, &bHoldsOtherAssets](UObject* Object)
        {
            if (UObjectRedirector* PackageRedirector = Cast<UObjectRedirector>(Object))
            {
                PackageRedirector->RemoveFromRoot();
                ObjectsToDelete.Add(PackageRedirector);
            }
            else
            {
                bHoldsOtherAssets = true;
            }
        });

        if (!bHoldsOtherAssets)
        {
            RedirectorPackage->RemoveFromRoot();
            ObjectsToDelete.Add(RedirectorPackage);
        }
    }

    RootedObjects.Empty();

    if (ObjectsToDelete.Num() > 0)
    {
        ObjectTools::DeleteObjectsUnchecked(ObjectsToDelete);
    }

    if (LoadedPackages.Num() > 0)
    {
        FText UnloadError;
        if (!UPackageTools::UnloadPackages(LoadedPackages, UnloadError, /* bUnloadDirtyPackages */ true))
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Warning, TEXT("Packages loaded for the fixup did not all unload: %s"), *UnloadError.ToString());
        }
    }

    return Result;
}
