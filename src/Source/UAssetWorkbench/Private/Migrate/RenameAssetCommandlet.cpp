#include "Migrate/RenameAssetCommandlet.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

#include "AssetToolsModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "FileHelpers.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"
#include "UObject/ObjectRedirector.h"

namespace
{
    struct FRenameJob
    {
        FString SourcePath;
        FString DestPath;
        FString DestPackagePath;
        FString DestAssetName;
        UObject* Source = nullptr;
        TArray<FName> ReferencersBefore;
    };

    FName ToPackageName(const FString& ObjectPath)
    {
        return FName(*FPackageName::ObjectPathToPackageName(ObjectPath));
    }

    TArray<FName> GatherReferencers(const IAssetRegistry& Registry, const FString& ObjectPath)
    {
        TArray<FName> Referencers;
        Registry.GetReferencers(ToPackageName(ObjectPath), Referencers);
        return Referencers;
    }

    void AddPackageFile(const FName PackageName, TArray<FString>& OutFiles)
    {
        FString Filename;
        if (FPackageName::DoesPackageExist(PackageName.ToString(), &Filename))
        {
            OutFiles.AddUnique(Filename);
        }
    }

    // A redirector left behind by the rename still sits at the old package path.
    UObjectRedirector* FindRedirectorAt(const FString& ObjectPath)
    {
        const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
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
}

URenameAssetCommandlet::URenameAssetCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 URenameAssetCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("UAssetWorkbench v%s - RenameAsset"), UASSET_WORKBENCH_VERSION_STRING);

    const TArray<FString> Pairs = UAssetWorkbench::ParsePathList(Params, TEXT("-pairs="));
    const bool bApply = FParse::Param(*Params, TEXT("apply"));
    const bool bKeepRedirectors = FParse::Param(*Params, TEXT("keepredirectors"));

    if (Pairs.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Usage: -run=RenameAsset -pairs=\"/Game/A>/Game/Sub/B\" [-apply] [-keepredirectors]"));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("RenameAsset: %d pair(s) %s"), Pairs.Num(), bApply ? TEXT("[APPLY]") : TEXT("[DRY RUN]"));

    IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
    IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

    TArray<FRenameJob> Jobs;
    Jobs.Reserve(Pairs.Num());

    for (const FString& Pair : Pairs)
    {
        FRenameJob Job;
        if (!Pair.Split(TEXT(">"), &Job.SourcePath, &Job.DestPath))
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Malformed pair '%s', expected Source>Destination"), *Pair);
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }

        Job.SourcePath.TrimStartAndEndInline();
        Job.DestPath.TrimStartAndEndInline();

        Job.Source = LoadObject<UObject>(nullptr, *Job.SourcePath);
        if (!Job.Source)
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Failed to load source: %s"), *Job.SourcePath);
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }

        Job.DestPackagePath = FPackageName::GetLongPackagePath(Job.DestPath);
        Job.DestAssetName = FPackageName::GetShortName(Job.DestPath);

        // Overwriting would discard whatever already lives at the destination.
        if (FPackageName::DoesPackageExist(Job.DestPath))
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Destination already exists: %s"), *Job.DestPath);
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }

        Job.ReferencersBefore = GatherReferencers(Registry, Job.SourcePath);

        UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("  %s -> %s  (%d referencer package(s))"), *Job.SourcePath, *Job.DestPath, Job.ReferencersBefore.Num());
        Jobs.Add(MoveTemp(Job));
    }

    if (!bApply)
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("Done. %d asset(s) would be renamed (dry run, nothing written)"), Jobs.Num());
        return ToExitCode(EUAssetWorkbenchExitType::Success);
    }

    TArray<FAssetRenameData> RenameData;
    RenameData.Reserve(Jobs.Num());
    for (const FRenameJob& Job : Jobs)
    {
        RenameData.Emplace(Job.Source, Job.DestPackagePath, Job.DestAssetName);
    }

    if (!AssetTools.RenameAssets(RenameData))
    {
        // A CDO or config soft reference into this batch makes AssetRenameManager raise an OkCancel prompt,
        // which an unattended run answers with Cancel. A loaded CDO keeps the old path until the editor restarts.
        TArray<FString> SourcePaths;
        for (const FRenameJob& Job : Jobs)
        {
            SourcePaths.Add(Job.SourcePath);
        }

        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("RenameAssets failed for %s, nothing was written. Search the log for \"Message dialog closed\": a CDO / config soft reference to one of these assets turns the rename into a prompt this run cannot answer. Repoint the ini at the destination path, restart the editor, then re-run."), *FString::Join(SourcePaths, TEXT(", ")));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    // Rename repoints referencers in memory only, they stay dirty until saved here.
    TArray<UPackage*> DirtyPackages;
    FEditorFileUtils::GetDirtyContentPackages(DirtyPackages);
    FEditorFileUtils::GetDirtyWorldPackages(DirtyPackages);
    if (DirtyPackages.Num() > 0)
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("Saving %d touched package(s)"), DirtyPackages.Num());

        TArray<UPackage*> FailedPackages;
        const FEditorFileUtils::EPromptReturnCode SaveResult = FEditorFileUtils::PromptForCheckoutAndSave(DirtyPackages, /* bCheckDirty */ false, /* bPromptToSave */ false, &FailedPackages);
        if (SaveResult != FEditorFileUtils::PR_Success)
        {
            for (const UPackage* Failed : FailedPackages)
            {
                UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Failed to save %s"), *GetNameSafe(Failed));
            }

            UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Save pass returned %d, working copy is mid-rename"), static_cast<int32>(SaveResult));
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }
    }

    if (!bKeepRedirectors)
    {
        TArray<UObjectRedirector*> Redirectors;
        for (const FRenameJob& Job : Jobs)
        {
            if (UObjectRedirector* Redirector = FindRedirectorAt(Job.SourcePath))
            {
                Redirectors.Add(Redirector);
            }
        }

        if (Redirectors.Num() > 0)
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("Fixing up %d redirector(s)"), Redirectors.Num());
            AssetTools.FixupReferencers(Redirectors, /* bCheckoutDialogPrompt */ false, ERedirectFixupMode::DeleteFixedUpRedirectors);
        }
    }

    // A referencer that is itself in this batch moved too, its old package name no longer resolves.
    TMap<FName, FName> MovedPackages;
    for (const FRenameJob& Job : Jobs)
    {
        MovedPackages.Add(ToPackageName(Job.SourcePath), ToPackageName(Job.DestPath));
    }

    // The registry keeps the dependency graph it read at startup, so the rewritten packages have to be
    // read off disk again before any referencer question gives an answer about the current state.
    TArray<FString> FilesToRescan;
    for (const FRenameJob& Job : Jobs)
    {
        AddPackageFile(ToPackageName(Job.DestPath), FilesToRescan);
        for (const FName Referencer : Job.ReferencersBefore)
        {
            const FName* Moved = MovedPackages.Find(Referencer);
            AddPackageFile(Moved ? *Moved : Referencer, FilesToRescan);
        }
    }

    if (FilesToRescan.Num() > 0)
    {
        Registry.ScanFilesSynchronous(FilesToRescan, /* bForceRescan */ true);
    }

    int32 Lost = 0;
    for (const FRenameJob& Job : Jobs)
    {
        if (!LoadObject<UObject>(nullptr, *Job.DestPath))
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Renamed asset does not load at its new path: %s"), *Job.DestPath);
            ++Lost;
            continue;
        }

        const FName DestPackage = ToPackageName(Job.DestPath);
        int32 Reconnected = 0;
        for (const FName Referencer : Job.ReferencersBefore)
        {
            const FName* Moved = MovedPackages.Find(Referencer);
            const FName ReferencerNow = Moved ? *Moved : Referencer;

            TArray<FName> Dependencies;
            Registry.GetDependencies(ReferencerNow, Dependencies);
            if (Dependencies.Contains(DestPackage))
            {
                ++Reconnected;
                continue;
            }

            UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("%s no longer depends on %s, the reference was dropped instead of repointed"), *ReferencerNow.ToString(), *DestPackage.ToString());
            ++Lost;
        }

        UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("  %s  %d/%d referencer(s) repointed"), *Job.DestPath, Reconnected, Job.ReferencersBefore.Num());
    }

    if (Lost > 0)
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Done with %d broken rename(s), inspect before committing"), Lost);
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("Done. %d asset(s) renamed, every referencer accounted for"), Jobs.Num());
    UAssetWorkbench::WarnIfWrittenOutsideEditor();
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}
