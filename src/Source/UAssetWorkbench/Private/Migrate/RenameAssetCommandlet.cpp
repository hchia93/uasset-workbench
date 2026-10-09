#include "Migrate/RenameAssetCommandlet.h"
#include "Migrate/RedirectorFixup.h"
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
#include "Serialization/FindReferencersArchive.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/UObjectHash.h"

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
        TSet<FName> StaleReferencers;
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

    // A source naming no package is read as a folder, every asset under it moves to the same relative path below the
    // destination. One pair per asset, so a folder moves the way single assets do and never as one whole-folder rename.
    bool ExpandFolderPair(const IAssetRegistry& Registry, const FString& SourcePath, const FString& DestPath, TArray<TPair<FString, FString>>& OutPairs)
    {
        FString SourceFolder = SourcePath;
        FString DestFolder = DestPath;
        SourceFolder.RemoveFromEnd(TEXT("/"));
        DestFolder.RemoveFromEnd(TEXT("/"));

        if (FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(SourceFolder)))
        {
            return false;
        }

        TArray<FAssetData> Assets;
        Registry.GetAssetsByPath(FName(*SourceFolder), Assets, /* bRecursive */ true, /* bIncludeOnlyOnDiskAssets */ true);
        if (Assets.IsEmpty())
        {
            return false;
        }

        Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.LexicalLess(B.PackageName); });

        const int32 PairsBefore = OutPairs.Num();
        for (const FAssetData& Asset : Assets)
        {
            const FString PackageName = Asset.PackageName.ToString();
            if (Asset.IsRedirector())
            {
                UE_LOG(LogUAssetWorkbenchMigrator, Warning, TEXT("  %s is a redirector and stays behind, run FixupRedirector on the folder first"), *PackageName);
                continue;
            }

            const FString RelativePath = PackageName.RightChop(SourceFolder.Len());
            OutPairs.Emplace(PackageName, DestFolder + RelativePath);
        }

        UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("  folder %s expands to %d asset(s)"), *SourceFolder, OutPairs.Num() - PairsBefore);
        return true;
    }

    // An import can outlive the last object that used it, left behind by a copy-paste or an edited graph. A save drops
    // it instead of repointing it, so the check afterwards would read that as a lost reference. Serializing with weak
    // references included sees what a save sees, graph pin types among them.
    bool HoldsReferenceInto(UPackage* Referencer, UPackage* Target)
    {
        TArray<UObject*> TargetObjects;
        GetObjectsWithPackage(Target, TargetObjects, /* bIncludeNestedObjects */ true);

        TArray<UObject*> ReferencerObjects;
        GetObjectsWithPackage(Referencer, ReferencerObjects, /* bIncludeNestedObjects */ true);

        FFindReferencersArchive Archive(nullptr, TargetObjects, /* bFindAlsoWeakReferences */ true);
        for (UObject* Object : ReferencerObjects)
        {
            Archive.ResetPotentialReferencer(Object);

            TMap<UObject*, int32> Counts;
            if (Archive.GetReferenceCounts(Counts) > 0)
            {
                return true;
            }
        }

        return false;
    }

    FString DescribeBatch(const TArray<FRenameJob>& Jobs)
    {
        if (Jobs.Num() == 1)
        {
            return Jobs[0].SourcePath;
        }

        return FString::Printf(TEXT("%s and %d more"), *Jobs[0].SourcePath, Jobs.Num() - 1);
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
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Usage: -run=RenameAsset -pairs=\"/Game/A>/Game/Sub/B,/Game/Folder>/Game/NewFolder\" [-apply] [-keepredirectors]"));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
    IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

    // A standalone process starts with an empty registry, the in-editor queue only takes tasks once the scan is done.
    if (IsRunningCommandlet())
    {
        Registry.SearchAllAssets(/* bSynchronousSearch */ true);
    }

    TArray<TPair<FString, FString>> AssetPairs;
    for (const FString& Pair : Pairs)
    {
        FString SourcePath;
        FString DestPath;
        if (!Pair.Split(TEXT(">"), &SourcePath, &DestPath))
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Malformed pair '%s', expected Source>Destination"), *Pair);
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }

        SourcePath.TrimStartAndEndInline();
        DestPath.TrimStartAndEndInline();

        if (!ExpandFolderPair(Registry, SourcePath, DestPath, AssetPairs))
        {
            AssetPairs.Emplace(SourcePath, DestPath);
        }
    }

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("RenameAsset: %d pair(s) %s"), AssetPairs.Num(), bApply ? TEXT("[APPLY]") : TEXT("[DRY RUN]"));

    TArray<FRenameJob> Jobs;
    Jobs.Reserve(AssetPairs.Num());

    for (const TPair<FString, FString>& AssetPair : AssetPairs)
    {
        FRenameJob Job;
        Job.SourcePath = AssetPair.Key;
        Job.DestPath = AssetPair.Value;

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

    // Runs once every source is loaded, a referencer moving in this batch is one of them. Unloaded referencers are
    // left to the check after the rename, nothing loads them just to look.
    for (FRenameJob& Job : Jobs)
    {
        TArray<FName> HardReferencers;
        Registry.GetReferencers(ToPackageName(Job.SourcePath), HardReferencers, UE::AssetRegistry::EDependencyCategory::Package, UE::AssetRegistry::EDependencyQuery::Hard);

        for (const FName Referencer : HardReferencers)
        {
            UPackage* ReferencerPackage = FindPackage(nullptr, *Referencer.ToString());
            if (ReferencerPackage && !HoldsReferenceInto(ReferencerPackage, Job.Source->GetOutermost()))
            {
                Job.StaleReferencers.Add(Referencer);
                UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("  %s imports %s but no object in it uses it, saving drops that import"), *Referencer.ToString(), *Job.SourcePath);
            }
        }
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

    {
        UAssetWorkbench::FFailureReasonCapture FailureReason;
        if (!AssetTools.RenameAssets(RenameData))
        {
            const FString Reason = FailureReason.GetReason();
            UAssetWorkbench::ReportTargetFailure(TEXT("RenameAsset"), DescribeBatch(Jobs), Reason.IsEmpty() ? FString(TEXT("AssetTools refused the rename and logged why above")) : Reason);

            // AssetRenameManager raises an OkCancel prompt for a CDO or config soft reference into the batch, an unattended run answers Cancel.
            UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("Nothing was written. If the reason names a config or CDO reference, repoint the ini at the destination path, restart the editor, then re-run"));
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }
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

    int32 RedirectorsKept = 0;
    if (!bKeepRedirectors)
    {
        TArray<UObjectRedirector*> Redirectors;
        for (const FRenameJob& Job : Jobs)
        {
            if (UObjectRedirector* Redirector = UAssetWorkbench::FindRedirectorInPackage(ToPackageName(Job.SourcePath).ToString()))
            {
                Redirectors.Add(Redirector);
            }
        }

        if (Redirectors.Num() > 0)
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("Fixing up %d redirector(s)"), Redirectors.Num());
            const UAssetWorkbench::FRedirectorFixupResult FixupResult = UAssetWorkbench::FixupRedirectors(Redirectors);
            for (const FString& DeletedFile : FixupResult.DeletedFiles)
            {
                UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("  deleted redirector %s"), *DeletedFile);
            }
            RedirectorsKept = FixupResult.KeptCount;
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
    int32 ThroughRedirector = 0;
    for (const FRenameJob& Job : Jobs)
    {
        if (!LoadObject<UObject>(nullptr, *Job.DestPath))
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Renamed asset does not load at its new path: %s"), *Job.DestPath);
            ++Lost;
            continue;
        }

        const FName SourcePackage = ToPackageName(Job.SourcePath);
        const FName DestPackage = ToPackageName(Job.DestPath);
        const bool bRedirectorAtSource = UAssetWorkbench::FindRedirectorInPackage(SourcePackage.ToString()) != nullptr;

        int32 Reconnected = 0;
        int32 ViaRedirector = 0;
        for (const FName Referencer : Job.ReferencersBefore)
        {
            if (Job.StaleReferencers.Contains(Referencer))
            {
                continue;
            }

            const FName* Moved = MovedPackages.Find(Referencer);
            const FName ReferencerNow = Moved ? *Moved : Referencer;

            TArray<FName> Dependencies;
            Registry.GetDependencies(ReferencerNow, Dependencies);
            if (Dependencies.Contains(DestPackage))
            {
                ++Reconnected;
                continue;
            }

            // Rename only rewrites referencers it loads. One it left alone still names the old path and gets there
            // through the redirector, which holds until that redirector is fixed up.
            if (bRedirectorAtSource && Dependencies.Contains(SourcePackage))
            {
                UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("  %s still reaches %s through the redirector at %s"), *ReferencerNow.ToString(), *DestPackage.ToString(), *SourcePackage.ToString());
                ++ViaRedirector;
                continue;
            }

            UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("%s no longer depends on %s, the reference was dropped instead of repointed"), *ReferencerNow.ToString(), *DestPackage.ToString());
            ++Lost;
        }

        const int32 Expected = Job.ReferencersBefore.Num() - Job.StaleReferencers.Num();
        UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("  %s  %d/%d referencer(s) repointed, %d through the redirector, %d stale import(s) dropped"), *Job.DestPath, Reconnected, Expected, ViaRedirector, Job.StaleReferencers.Num());
        ThroughRedirector += ViaRedirector;
    }

    if (Lost > 0)
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Done with %d broken rename(s), inspect before committing"), Lost);
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    if (RedirectorsKept > 0)
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Done. %d asset(s) renamed, %d redirector(s) could not be fixed up, the errors above say why"), Jobs.Num(), RedirectorsKept);
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    if (ThroughRedirector > 0)
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Warning, TEXT("Done. %d asset(s) renamed, %d referencer(s) still go through a redirector, run FixupRedirector before deleting it"), Jobs.Num(), ThroughRedirector);
    }
    else
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("Done. %d asset(s) renamed, every referencer accounted for"), Jobs.Num());
    }

    UAssetWorkbench::WarnIfWrittenOutsideEditor();
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}
