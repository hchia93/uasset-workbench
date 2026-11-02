#include "Migrate/SanitizeLevelReferenceCommandlet.h"

// Editor-only by design: drives package load + save. Trap any Runtime-type drift early.
static_assert(WITH_EDITOR, "UAssetWorkbench commandlets are editor-only, keep the uplugin Module Type=Editor.");

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/ArchiveReplaceObjectRef.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"

#include "UAssetWorkbenchModule.h"

USanitizeLevelReferenceCommandlet::USanitizeLevelReferenceCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 USanitizeLevelReferenceCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("SanitizeLevelReference commandlet starting..."));

    FOptions Options;
    if (!ParseOptions(Params, Options))
    {
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }
    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("SanitizeLevelReference: levels=%d pairs=%d dryRun=%s"), Options.LevelPaths.Num(), Options.Pairs.Num(), Options.bDryRun ? TEXT("true") : TEXT("false"));

    TMap<UObject*, UObject*> ReplaceMap;
    if (!ResolvePairs(Options.Pairs, ReplaceMap))
    {
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    TArray<FLevelResult> Results;
    Results.Reserve(Options.LevelPaths.Num());
    int32 LevelsFailed = 0;

    for (const FString& LevelPath : Options.LevelPaths)
    {
        FLevelResult Result;
        Result.LevelPath = LevelPath;

        const bool bOK = ProcessLevel(LevelPath, ReplaceMap, Options.bDryRun, Result);
        if (!bOK)
        {
            ++LevelsFailed;
            if (Result.FailReason.IsEmpty())
            {
                Result.FailReason = TEXT("unknown failure");
            }
        }
        Results.Add(Result);

        UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("SanitizeLevelReference: %s -> replaced=%d saved=%s reason=%s"), *LevelPath, Result.ReferencesReplaced, Result.bSaved ? TEXT("true") : TEXT("false"), *Result.FailReason);

        // GC between levels so package memory is reclaimed. Replacement assets are rooted in ResolvePairs
        // so they survive.
        CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    }

    // Unroot the replacement assets loaded for the swap.
    for (const TPair<UObject*, UObject*>& Entry : ReplaceMap)
    {
        if (Entry.Key)
        {
            Entry.Key->RemoveFromRoot();
        }
        if (Entry.Value)
        {
            Entry.Value->RemoveFromRoot();
        }
    }

    if (!WriteReport(Options.ReportPath, Results, Options.bDryRun))
    {
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    int32 TotalReplaced = 0;
    for (const FLevelResult& R : Results)
    {
        TotalReplaced += R.ReferencesReplaced;
    }

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("SanitizeLevelReference: complete. levels=%d failed=%d total_replaced=%d (dryRun=%s)"), Results.Num(), LevelsFailed, TotalReplaced, Options.bDryRun ? TEXT("true") : TEXT("false"));

    return ToExitCode(LevelsFailed > 0 ? EUAssetWorkbenchExitType::Failed : EUAssetWorkbenchExitType::Success);
}

bool USanitizeLevelReferenceCommandlet::ParseOptions(const FString& Params, FOptions& OutOptions) const
{
    FString LevelsValue;
    if (!FParse::Value(*Params, TEXT("-levels="), LevelsValue, false) || LevelsValue.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("SanitizeLevelReference: -levels=<paths> is required"));
        return false;
    }
    LevelsValue.TrimQuotesInline();
    LevelsValue.ParseIntoArray(OutOptions.LevelPaths, TEXT(","), true);
    for (FString& S : OutOptions.LevelPaths)
    {
        S.TrimStartAndEndInline();
    }

    FString ReplaceValue;
    if (!FParse::Value(*Params, TEXT("-replace="), ReplaceValue, false) || ReplaceValue.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("SanitizeLevelReference: -replace=<old=new,...> is required"));
        return false;
    }
    ReplaceValue.TrimQuotesInline();
    TArray<FString> PairTokens;
    ReplaceValue.ParseIntoArray(PairTokens, TEXT(","), true);
    for (const FString& Token : PairTokens)
    {
        FString OldPath, NewPath;
        if (!Token.Split(TEXT("="), &OldPath, &NewPath))
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("SanitizeLevelReference: bad replace pair (need old=new): %s"), *Token);
            return false;
        }
        FReplacePair Pair;
        Pair.OldPath = OldPath.TrimStartAndEnd();
        Pair.NewPath = NewPath.TrimStartAndEnd();
        OutOptions.Pairs.Add(Pair);
    }

    if (!FParse::Value(*Params, TEXT("-report="), OutOptions.ReportPath, false) || OutOptions.ReportPath.IsEmpty())
    {
        const FString Stamp = FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"));
        OutOptions.ReportPath = FPaths::Combine(FPaths::ProjectDir(), TEXT("Intermediate"), TEXT("SanitizeLevelReference"), FString::Printf(TEXT("%s.json"), *Stamp));
    }
    OutOptions.ReportPath.TrimQuotesInline();

    OutOptions.bDryRun = FParse::Param(*Params, TEXT("dryrun"));

    return true;
}

bool USanitizeLevelReferenceCommandlet::ResolvePairs(const TArray<FReplacePair>& Pairs, TMap<UObject*, UObject*>& OutReplaceMap) const
{
    for (const FReplacePair& Pair : Pairs)
    {
        UObject* OldObj = FSoftObjectPath(Pair.OldPath).TryLoad();
        if (!OldObj)
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("SanitizeLevelReference: cannot load old asset (must still exist): %s"), *Pair.OldPath);
            return false;
        }
        UObject* NewObj = FSoftObjectPath(Pair.NewPath).TryLoad();
        if (!NewObj)
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("SanitizeLevelReference: cannot load new asset: %s"), *Pair.NewPath);
            return false;
        }

        // Root so the inter-level GC does not collect them mid-run.
        OldObj->AddToRoot();
        NewObj->AddToRoot();
        OutReplaceMap.Add(OldObj, NewObj);
    }
    return OutReplaceMap.Num() > 0;
}

bool USanitizeLevelReferenceCommandlet::ProcessLevel(const FString& LevelPath, const TMap<UObject*, UObject*>& ReplaceMap, bool bDryRun, FLevelResult& OutResult) const
{
    UPackage* Package = LoadPackage(nullptr, *LevelPath, LOAD_None);
    if (!Package)
    {
        OutResult.FailReason = TEXT("LoadPackage failed");
        return false;
    }

    UWorld* World = UWorld::FindWorldInPackage(Package);
    if (!World)
    {
        OutResult.FailReason = TEXT("FindWorldInPackage returned null");
        return false;
    }

    World->PersistentLevel->OnLevelLoaded();
    World->PersistentLevel->UpdateLevelComponents(/*bRerunConstructionScripts=*/ false);

    // Replace references across every object that lives in this level package.
    TArray<UObject*> ObjectsInPackage;
    GetObjectsWithPackage(Package, ObjectsInPackage, /*bIncludeNestedObjects=*/ true);

    int32 TotalReplaced = 0;
    for (UObject* Obj : ObjectsInPackage)
    {
        if (!Obj)
        {
            continue;
        }
        FArchiveReplaceObjectRef<UObject> ReplaceAr(Obj, ReplaceMap, EArchiveReplaceObjectFlags::None);
        TotalReplaced += (int32)ReplaceAr.GetCount();
    }

    OutResult.ReferencesReplaced = TotalReplaced;

    if (bDryRun || TotalReplaced == 0)
    {
        OutResult.bSaved = false;
        return true;
    }

    const FString PackageFilename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetMapPackageExtension());

    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Standalone;
    SaveArgs.SaveFlags = SAVE_None;
    SaveArgs.bForceByteSwapping = false;
    SaveArgs.bWarnOfLongFilename = true;
    SaveArgs.Error = GWarn;

    const bool bSaved = UPackage::SavePackage(Package, World, *PackageFilename, SaveArgs);
    OutResult.bSaved = bSaved;
    if (!bSaved)
    {
        OutResult.FailReason = TEXT("SavePackage failed");
        return false;
    }

    return true;
}

bool USanitizeLevelReferenceCommandlet::WriteReport(const FString& ReportPath, const TArray<FLevelResult>& Results, bool bDryRun) const
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("phase"), bDryRun ? TEXT("sanitize-level-reference-dryrun") : TEXT("sanitize-level-reference"));
    Root->SetStringField(TEXT("timestamp_utc"), FDateTime::UtcNow().ToIso8601());

    int32 TotalReplaced = 0;
    int32 LevelsFailed = 0;
    TArray<TSharedPtr<FJsonValue>> LevelsJson;

    for (const FLevelResult& R : Results)
    {
        TSharedRef<FJsonObject> L = MakeShared<FJsonObject>();
        L->SetStringField(TEXT("level"), R.LevelPath);
        L->SetNumberField(TEXT("references_replaced"), R.ReferencesReplaced);
        L->SetBoolField(TEXT("saved"), R.bSaved);
        L->SetStringField(TEXT("fail_reason"), R.FailReason);
        LevelsJson.Add(MakeShared<FJsonValueObject>(L));

        TotalReplaced += R.ReferencesReplaced;
        if (!R.FailReason.IsEmpty())
        {
            ++LevelsFailed;
        }
    }

    Root->SetArrayField(TEXT("levels"), LevelsJson);
    Root->SetNumberField(TEXT("total_replaced"), TotalReplaced);
    Root->SetNumberField(TEXT("levels_failed"), LevelsFailed);

    FString OutString;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutString);
    if (!FJsonSerializer::Serialize(Root, Writer))
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("SanitizeLevelReference: failed to serialize report JSON"));
        return false;
    }

    const FString ReportDir = FPaths::GetPath(ReportPath);
    if (!IFileManager::Get().DirectoryExists(*ReportDir))
    {
        IFileManager::Get().MakeDirectory(*ReportDir, /*Tree=*/ true);
    }

    if (!FFileHelper::SaveStringToFile(OutString, *ReportPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("SanitizeLevelReference: failed to write report: %s"), *ReportPath);
        return false;
    }

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("SanitizeLevelReference: report written: %s"), *ReportPath);
    return true;
}
