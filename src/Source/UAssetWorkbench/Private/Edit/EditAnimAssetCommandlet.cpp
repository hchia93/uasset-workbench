#include "Edit/EditAnimAssetCommandlet.h"
#include "Edit/AnimAssetWriter.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

#include "Animation/AnimSequenceBase.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
    // Composition first: slots decide the montage length, and every writer after this one validates
    // its times against that length.
    TArray<TUniquePtr<IAnimAssetWriter>> MakeWriters()
    {
        TArray<TUniquePtr<IAnimAssetWriter>> Writers;
        Writers.Add(MakeAnimMontageSlotWriter());
        Writers.Add(MakeAnimMontageSectionWriter());
        Writers.Add(MakeAnimCurveWriter());
        Writers.Add(MakeAnimSyncMarkerWriter());
        Writers.Add(MakeAnimNotifyWriter());
        return Writers;
    }
}

UEditAnimAssetCommandlet::UEditAnimAssetCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UEditAnimAssetCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("UAssetWorkbench v%s - EditAnimAsset"), UASSET_WORKBENCH_VERSION_STRING);

    FString SpecPath;
    if (!FParse::Value(*Params, TEXT("spec="), SpecPath))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("No spec specified. Usage: -spec=\"C:/path/spec.json\" [-apply]"));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    SpecPath = SpecPath.TrimQuotes();
    const bool bApply = FParse::Param(*Params, TEXT("apply"));

    // Writers mutate either way, only the save reads bApply, so a dry run relies on the process exiting
    // to throw the mutation away. In-editor there is no such exit.
    if (!bApply && !IsRunningCommandlet())
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Dry run needs its own process to discard the in-memory edit. Close the editor and run the commandlet, or pass -apply."));
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    FString SpecText;
    if (!FFileHelper::LoadFileToString(SpecText, *SpecPath))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to read spec: %s"), *SpecPath);
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    TSharedPtr<FJsonObject> Spec;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(SpecText);
    if (!FJsonSerializer::Deserialize(Reader, Spec) || !Spec.IsValid())
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Spec is not valid JSON: %s"), *SpecPath);
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    const TArray<TSharedPtr<FJsonValue>>* Targets = nullptr;
    if (!Spec->TryGetArrayField(TEXT("Targets"), Targets))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Spec has no Targets array"));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("EditAnimAsset: %d target(s) %s"), Targets->Num(), bApply ? TEXT("[APPLY]") : TEXT("[DRY RUN]"));

    TSet<UAnimSequenceBase*> Touched;
    int32 Ops = 0;
    for (const TSharedPtr<FJsonValue>& Value : *Targets)
    {
        const TSharedPtr<FJsonObject>& Entry = Value->AsObject();
        UAssetWorkbench::FFailureReasonCapture FailureReason;
        if (!Entry.IsValid() || !ApplyTarget(Entry, Touched, Ops))
        {
            UAssetWorkbench::ReportTargetFailure(TEXT("EditAnimAsset"), UAssetWorkbench::DescribeSpecTarget(Entry), FailureReason.GetReason());
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }
    }

    if (!bApply)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Done. %d operation(s) staged across %d asset(s) (dry run, not saved)."), Ops, Touched.Num());
        return ToExitCode(EUAssetWorkbenchExitType::Success);
    }

    for (UAnimSequenceBase* AnimAsset : Touched)
    {
        UAssetWorkbench::FFailureReasonCapture FailureReason;
        if (!UAssetWorkbench::CompileAndSavePackage(AnimAsset, false))
        {
            UAssetWorkbench::ReportTargetFailure(TEXT("EditAnimAsset"), AnimAsset->GetPathName(), FailureReason.GetReason());
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }
    }

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Done. %d operation(s) across %d asset(s) (saved)."), Ops, Touched.Num());
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}

bool UEditAnimAssetCommandlet::ApplyTarget(const TSharedPtr<FJsonObject>& Entry, TSet<UAnimSequenceBase*>& OutTouched, int32& OutOps) const
{
    FString AssetPath;
    if (!Entry->TryGetStringField(TEXT("AssetPath"), AssetPath))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Target has no AssetPath field"));
        return false;
    }

    UAnimSequenceBase* AnimAsset = LoadObject<UAnimSequenceBase>(nullptr, *AssetPath);
    if (!AnimAsset)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to load anim asset (sequence or montage): %s"), *AssetPath);
        return false;
    }

    FAnimAssetEditContext Context;
    Context.AnimAsset = AnimAsset;
    Context.AssetPath = AssetPath;

    TArray<TUniquePtr<IAnimAssetWriter>> Writers = MakeWriters();

    int32 Matched = 0;
    for (const TUniquePtr<IAnimAssetWriter>& Writer : Writers)
    {
        const TSharedPtr<FJsonValue> Section = Entry->TryGetField(Writer->GetSpecKey());
        if (!Section.IsValid())
        {
            continue;
        }

        ++Matched;
        if (!Writer->Apply(Context, Section))
        {
            return false;
        }
    }

    if (Matched == 0)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s writes nothing. Expected Notifies, Curves, SyncMarkers, Sections or Slots"), *AssetPath);
        return false;
    }

    OutOps += Context.Ops;

    // Sorts the notify and sync marker arrays and rebuilds the panel's per-track lists. Ops address
    // both by array position, so this runs once the target is done, never between its ops.
    AnimAsset->RefreshCacheData();
    AnimAsset->PostEditChange();
    OutTouched.Add(AnimAsset);
    return true;
}
