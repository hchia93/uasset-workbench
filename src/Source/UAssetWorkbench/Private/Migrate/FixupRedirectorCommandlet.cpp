#include "Migrate/FixupRedirectorCommandlet.h"
#include "Migrate/RedirectorFixup.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "UObject/ObjectRedirector.h"

UFixupRedirectorCommandlet::UFixupRedirectorCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UFixupRedirectorCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("UAssetWorkbench v%s - FixupRedirector"), UASSET_WORKBENCH_VERSION_STRING);

    const TArray<FString> Paths = UAssetWorkbench::ParseAssetPaths(Params);
    const bool bApply = FParse::Param(*Params, TEXT("apply"));

    if (Paths.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Usage: -run=FixupRedirector -assets=\"/Game/Folder,/Game/Old/WBP_Old\" [-apply]"));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    IAssetRegistry& Registry = IAssetRegistry::GetChecked();

    // A standalone process starts with an empty registry, the in-editor queue only takes tasks once the scan is done.
    if (IsRunningCommandlet())
    {
        Registry.SearchAllAssets(/* bSynchronousSearch */ true);
    }

    TArray<FName> RedirectorPackages;
    for (FString Path : Paths)
    {
        Path.RemoveFromEnd(TEXT("/"));

        FARFilter Filter;
        Filter.ClassPaths.Add(UObjectRedirector::StaticClass()->GetClassPathName());
        if (FPackageName::DoesPackageExist(Path))
        {
            Filter.PackageNames.Add(FName(*Path));
        }
        else
        {
            Filter.PackagePaths.Add(FName(*Path));
            Filter.bRecursivePaths = true;
        }

        TArray<FAssetData> Found;
        Registry.GetAssets(Filter, Found);
        if (Found.IsEmpty())
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Warning, TEXT("%s holds no redirector"), *Path);
        }

        for (const FAssetData& Asset : Found)
        {
            RedirectorPackages.AddUnique(Asset.PackageName);
        }
    }

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("FixupRedirector: %d redirector(s) %s"), RedirectorPackages.Num(), bApply ? TEXT("[APPLY]") : TEXT("[DRY RUN]"));

    TArray<UObjectRedirector*> Redirectors;
    for (const FName PackageName : RedirectorPackages)
    {
        UObjectRedirector* Redirector = UAssetWorkbench::FindRedirectorInPackage(PackageName.ToString());
        if (!Redirector)
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("%s is listed as a redirector but none loads from it"), *PackageName.ToString());
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }

        TArray<FName> Referencers;
        Registry.GetReferencers(PackageName, Referencers);
        UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("  %s -> %s  (%d referencer package(s))"), *PackageName.ToString(), *GetPathNameSafe(Redirector->DestinationObject), Referencers.Num());
        Redirectors.Add(Redirector);
    }

    if (!bApply)
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("Done. %d redirector(s) would be fixed up (dry run, nothing written)"), Redirectors.Num());
        return ToExitCode(EUAssetWorkbenchExitType::Success);
    }

    const UAssetWorkbench::FRedirectorFixupResult Result = UAssetWorkbench::FixupRedirectors(Redirectors);
    for (const FString& DeletedFile : Result.DeletedFiles)
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("  deleted %s"), *DeletedFile);
    }

    if (Result.KeptCount > 0)
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Done with %d redirector(s) kept, the errors above say why. %d deleted"), Result.KeptCount, Result.DeletedFiles.Num());
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("Done. %d redirector(s) fixed up and deleted, record the deleted files in version control"), Result.DeletedFiles.Num());
    UAssetWorkbench::WarnIfWrittenOutsideEditor();
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}
