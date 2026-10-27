#include "Migrate/RenameDefaultSubobjectCommandlet.h"
#include "Migrate/PackageTableReader.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Blueprint.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"

namespace
{
    struct FRenameRequest
    {
        TArray<UClass*> Classes;
        FName OldName;
        FName NewName;
        bool bApply = false;
    };

    bool IsTargetClass(const UClass* Class, const TArray<UClass*>& Targets)
    {
        return Targets.ContainsByPredicate([Class](const UClass* Target) { return Class && Class->IsChildOf(Target); });
    }

    // The registry tag reads "/Script/CoreUObject.Class'/Script/Module.Name'", only the quoted path matters.
    UClass* ReadNativeParent(const FAssetData& Asset)
    {
        FString Tag;
        if (!Asset.GetTagValue(FBlueprintTags::NativeParentClassPath, Tag))
        {
            return nullptr;
        }

        FString Path = FPackageName::ExportTextPathToObjectPath(Tag);
        return FindObject<UClass>(nullptr, *Path);
    }

    TArray<FString> FindBlueprintPackages(const FString& ScanDir, const TArray<UClass*>& Targets)
    {
        IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        Registry.SearchAllAssets(true);

        FARFilter Filter;
        Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
        Filter.bRecursiveClasses = true;
        Filter.PackagePaths.Add(FName(*ScanDir));
        Filter.bRecursivePaths = true;

        TArray<FAssetData> Assets;
        Registry.GetAssets(Filter, Assets);

        TArray<FString> Result;
        for (const FAssetData& Asset : Assets)
        {
            if (IsTargetClass(ReadNativeParent(Asset), Targets))
            {
                Result.Add(Asset.PackageName.ToString());
            }
        }

        Result.Sort();
        return Result;
    }

    // Header scan first, a level whose name table lacks the old name holds no such subobject and never loads.
    TArray<FString> FindLevelPackages(const FString& ScanDir, FName OldName)
    {
        TArray<FString> Result;

        for (const FString& Folder : UAssetWorkbench::CollectScanFolders(ScanDir))
        {
            FString Directory;
            if (!FPackageName::TryConvertLongPackageNameToFilename(Folder + TEXT("/"), Directory) || !IFileManager::Get().DirectoryExists(*Directory))
            {
                continue;
            }

            TArray<FString> Files;
            IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("*.umap"), true, false);
            if (Folder != ScanDir)
            {
                IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("*.uasset"), true, false, /* bClearFileNames */ false);
            }

            for (const FString& File : Files)
            {
                UAssetWorkbench::FPackageTables Tables;
                FString PackageName;
                if (!UAssetWorkbench::ReadPackageTables(File, Tables) || !Tables.NameMap.Contains(OldName))
                {
                    continue;
                }

                if (FPackageName::TryConvertFilenameToLongPackageName(File, PackageName))
                {
                    Result.Add(PackageName);
                }
            }
        }

        Result.Sort();
        return Result;
    }

    // Saving would also write whatever edit is still pending in this editor, which is not this run's to commit.
    UPackage* LoadCleanPackage(const FString& PackageName)
    {
        UPackage* Loaded = FindPackage(nullptr, *PackageName);
        if (Loaded && Loaded->IsDirty())
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("%s has unsaved edits in this editor, save or discard them first"), *PackageName);
            return nullptr;
        }

        // A level loads as a plain package, no world initialization. A streaming sublevel brought up alone drops its navmesh chunk on save.
        UPackage* Package = Loaded ? Loaded : LoadPackage(nullptr, *PackageName, LOAD_None);
        if (!Package)
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Failed to load %s"), *PackageName);
            return nullptr;
        }

        Package->FullyLoad();
        return Package;
    }

    // Returns false on a failure. Outer is the CDO or the placed actor that owns its own copy of the subobject.
    bool RenameOwnedSubobject(UObject* Outer, const FRenameRequest& Request, int32& OutRenamed)
    {
        UObject* Subobject = StaticFindObjectFast(UObject::StaticClass(), Outer, Request.OldName);
        if (!Subobject)
        {
            return true;
        }

        if (StaticFindObjectFast(UObject::StaticClass(), Outer, Request.NewName))
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("  %s already owns %s, %s stays"), *Outer->GetPathName(), *Request.NewName.ToString(), *Subobject->GetPathName());
            return false;
        }

        if (!Request.bApply)
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("  would rename  %s  ->  %s"), *Subobject->GetPathName(), *Request.NewName.ToString());
            ++OutRenamed;
            return true;
        }

        if (!Subobject->Rename(*Request.NewName.ToString(), nullptr, REN_DontCreateRedirectors | REN_NonTransactional))
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("  rename refused for %s"), *Subobject->GetPathName());
            return false;
        }

        UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("  renamed  %s"), *Subobject->GetPathName());
        Outer->MarkPackageDirty();
        ++OutRenamed;
        return true;
    }
}

URenameDefaultSubobjectCommandlet::URenameDefaultSubobjectCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 URenameDefaultSubobjectCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("UAssetWorkbench v%s - RenameDefaultSubobject"), UASSET_WORKBENCH_VERSION_STRING);

    const TArray<FString> ClassNames = UAssetWorkbench::ParsePathList(Params, TEXT("-classes="));
    FString OldName;
    FString NewName;
    FParse::Value(*Params, TEXT("-oldname="), OldName);
    FParse::Value(*Params, TEXT("-newname="), NewName);

    if (ClassNames.IsEmpty() || OldName.IsEmpty() || NewName.IsEmpty() || OldName == NewName)
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Usage: -run=RenameDefaultSubobject -classes=\"AMyActor\" -oldname=Old -newname=New [-scandir=/Game] [-apply]"));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    FString ScanDir;
    if (!FParse::Value(*Params, TEXT("-scandir="), ScanDir, false) || ScanDir.IsEmpty())
    {
        ScanDir = TEXT("/Game");
    }
    ScanDir.TrimQuotesInline();
    ScanDir.RemoveFromEnd(TEXT("/"));

    FRenameRequest Request;
    Request.OldName = FName(*OldName);
    Request.NewName = FName(*NewName);
    Request.bApply = FParse::Param(*Params, TEXT("apply"));

    for (const FString& ClassName : ClassNames)
    {
        UClass* Class = UAssetWorkbench::ResolveClass(ClassName);
        if (!Class)
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Cannot resolve class %s"), *ClassName);
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }

        // The C++ side still has to create the old name, otherwise the rename ran after the code change and the orphans already exist.
        if (!StaticFindObjectFast(UObject::StaticClass(), Class->GetDefaultObject(), Request.OldName))
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("%s creates no default subobject named %s. Run this before the C++ rename."), *Class->GetPathName(), *OldName);
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }

        Request.Classes.Add(Class);
    }

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("RenameDefaultSubobject: %s -> %s on %d class(es) under %s %s"), *OldName, *NewName, Request.Classes.Num(), *ScanDir, Request.bApply ? TEXT("[APPLY]") : TEXT("[DRY RUN]"));

    int32 Renamed = 0;
    int32 Failed = 0;
    TArray<UPackage*> PackagesToSave;

    const TArray<FString> BlueprintPackages = FindBlueprintPackages(ScanDir, Request.Classes);
    for (const FString& PackageName : BlueprintPackages)
    {
        UPackage* Package = LoadCleanPackage(PackageName);
        UBlueprint* Blueprint = Package ? Cast<UBlueprint>(Package->FindAssetInPackage()) : nullptr;
        if (!Blueprint || !Blueprint->GeneratedClass)
        {
            ++Failed;
            continue;
        }

        // No compile. It rebuilds the CDO subobjects from the C++ constructor, which still creates the old name.
        const int32 Before = Renamed;
        if (!RenameOwnedSubobject(Blueprint->GeneratedClass->GetDefaultObject(), Request, Renamed))
        {
            ++Failed;
        }
        else if (Renamed > Before && Request.bApply)
        {
            PackagesToSave.AddUnique(Package);
        }
    }

    for (const FString& PackageName : FindLevelPackages(ScanDir, Request.OldName))
    {
        UPackage* Package = LoadCleanPackage(PackageName);
        if (!Package)
        {
            ++Failed;
            continue;
        }

        // Collected first, a rename inside the walk changes the object hash it iterates.
        TArray<AActor*> Actors;
        ForEachObjectWithPackage(Package, [&Request, &Actors](UObject* Object)
        {
            AActor* Actor = Cast<AActor>(Object);
            if (Actor && !Actor->IsTemplate() && IsTargetClass(Actor->GetClass(), Request.Classes))
            {
                Actors.Add(Actor);
            }
            return true;
        });

        const int32 Before = Renamed;
        for (AActor* Actor : Actors)
        {
            // A placed subobject is saved under its archetype's name, which lives on the CDO of the actor's own class.
            // Read off the class, not GetArchetype, which loses the archetype once the Blueprint pass renamed it.
            const UObject* Subobject = StaticFindObjectFast(UObject::StaticClass(), Actor, Request.OldName);
            const UClass* ActorClass = Actor->GetClass();
            const bool bNativeActor = ActorClass->HasAnyClassFlags(CLASS_Native);
            const FString ClassPackage = ActorClass->GetOutermost()->GetName();
            if (Subobject && (bNativeActor || !BlueprintPackages.Contains(ClassPackage)))
            {
                const TCHAR* Reason = bNativeActor ? TEXT("a placed native actor keeps the C++ name until the code changes") : TEXT("widen -scandir to take in that Blueprint");
                UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("  %s belongs to %s, not renamed in this run, %s"), *Subobject->GetPathName(), *ActorClass->GetPathName(), Reason);
                ++Failed;
                continue;
            }

            if (!RenameOwnedSubobject(Actor, Request, Renamed))
            {
                ++Failed;
            }
        }

        if (Renamed > Before && Request.bApply)
        {
            PackagesToSave.AddUnique(Package);
        }
    }

    if (!Request.bApply)
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("Done. %d subobject(s) would be renamed (dry run, nothing written)"), Renamed);
        return ToExitCode(Failed > 0 ? EUAssetWorkbenchExitType::Failed : EUAssetWorkbenchExitType::Success);
    }

    // All or nothing. A level saved without its Blueprint, or the other way round, leaves orphans behind.
    if (Failed > 0)
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Done with %d failure(s), nothing saved. The errors above say why"), Failed);
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    for (UPackage* Package : PackagesToSave)
    {
        // Saves without compiling, which keeps the rename above. The unattended-script guard inside it matters: a bare
        // PromptForCheckoutAndSave in a commandlet answers a save prompt with its default and skips the package silently.
        if (!UEditorLoadingAndSavingUtils::SavePackages({ Package }, /* bOnlyDirty */ false))
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Failed to save %s"), *Package->GetName());
            ++Failed;
            continue;
        }

        // A save can report success yet leave the file alone. The new name in the written name table is the proof.
        const FString Extension = Package->ContainsMap() ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension();
        UAssetWorkbench::FPackageTables Tables;
        if (!UAssetWorkbench::ReadPackageTables(FPackageName::LongPackageNameToFilename(Package->GetName(), Extension), Tables) || !Tables.NameMap.Contains(Request.NewName))
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("%s does not carry %s on disk after the save"), *Package->GetName(), *NewName);
            ++Failed;
        }
    }

    if (Failed > 0)
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Done with %d failure(s), the errors above say why"), Failed);
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    // Property tags and Blueprint data still carry the old name until a compiling resave against the renamed C++.
    const FString AssetList = FString::JoinBy(PackagesToSave, TEXT(","), [](const UPackage* Package) { return Package->GetName(); });
    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("Done. %d subobject(s) renamed in %d package(s). Next: rename it in C++, rebuild, then ResaveAsset these packages:"), Renamed, PackagesToSave.Num());
    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("  %s"), *AssetList);
    UAssetWorkbench::WarnIfWrittenOutsideEditor();
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}
