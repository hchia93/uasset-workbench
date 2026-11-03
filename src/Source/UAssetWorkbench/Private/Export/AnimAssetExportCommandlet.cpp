#include "Export/AnimAssetExportCommandlet.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

#include "AlphaBlend.h"
#include "Animation/AnimBoneCompressionSettings.h"
#include "Animation/AnimCurveCompressionSettings.h"
#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/BlendProfile.h"
#include "Curves/CurveFloat.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
    // Baked curves can carry thousands of keys, enough to swamp the file the reader came for.
    constexpr int32 kMaxCurveKeysPerCurve = 64;

    TArray<TSharedPtr<FJsonValue>> MakeCurveFlagsJson(int32 CurveTypeFlags)
    {
        TArray<TSharedPtr<FJsonValue>> Flags;

        auto AddFlag = [&Flags, CurveTypeFlags](int32 Bit, const TCHAR* Label)
        {
            if ((CurveTypeFlags & Bit) != 0)
            {
                Flags.Add(MakeShared<FJsonValueString>(Label));
            }
        };

        AddFlag(AACF_Editable, TEXT("Editable"));
        AddFlag(AACF_Metadata, TEXT("Metadata"));
        // Material and Morph moved onto the skeleton, the bits only survive on assets saved before that.
        AddFlag(AACF_DriveMaterial_DEPRECATED, TEXT("Material"));
        AddFlag(AACF_DriveMorphTarget_DEPRECATED, TEXT("Morph"));
        AddFlag(AACF_DriveTrack, TEXT("DriveTrack"));
        AddFlag(AACF_Disabled, TEXT("Disabled"));

        return Flags;
    }

    TSharedPtr<FJsonObject> MakeBlendJson(const FAlphaBlend& Blend)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("Time"), Blend.GetBlendTime());
        Obj->SetStringField(TEXT("Option"), StaticEnum<EAlphaBlendOption>()->GetNameStringByValue(static_cast<int64>(Blend.GetBlendOption())));

        if (const UCurveFloat* CustomCurve = Blend.GetCustomCurve())
        {
            Obj->SetStringField(TEXT("CustomCurve"), CustomCurve->GetPathName());
        }

        return Obj;
    }
}

UAnimAssetExportCommandlet::UAnimAssetExportCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UAnimAssetExportCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("UAssetWorkbench v%s - AnimAssetExport"), UASSET_WORKBENCH_VERSION_STRING);

    TArray<FString> AssetPaths = UAssetWorkbench::ParseAssetPaths(Params);

    if (AssetPaths.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchExporter, Error, TEXT("No assets specified. Usage: -assets=\"/Game/Path/AM_A,/Game/Path/AM_B\""));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    int32 ExportedCount = 0;

    for (const FString& AssetPath : AssetPaths)
    {
        UAnimSequenceBase* AnimAsset = LoadObject<UAnimSequenceBase>(nullptr, *AssetPath);
        if (!AnimAsset)
        {
            UE_LOG(LogUAssetWorkbenchExporter, Warning, TEXT("Failed to load anim asset (sequence or montage): %s"), *AssetPath);
            continue;
        }

        TSharedPtr<FJsonObject> JsonObject = ExportAnimAsset(AnimAsset);
        if (!JsonObject.IsValid())
        {
            UE_LOG(LogUAssetWorkbenchExporter, Warning, TEXT("Failed to export anim asset: %s"), *AssetPath);
            continue;
        }

        UAssetWorkbench::FExportTarget ExportTarget(AssetPath);
        if (ExportTarget.Save(JsonObject.ToSharedRef()))
        {
            UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Exported: %s -> %s"), *AssetPath, *ExportTarget.GetPath());
            ExportedCount++;
        }
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Export complete. %d/%d montages exported."), ExportedCount, AssetPaths.Num());
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}

TSharedPtr<FJsonObject> UAnimAssetExportCommandlet::ExportAnimAsset(UAnimSequenceBase* AnimAsset) const
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();

    Root->SetStringField(TEXT("ExporterVersion"), UASSET_WORKBENCH_VERSION_STRING);
    const UAnimMontage* Montage = Cast<UAnimMontage>(AnimAsset);

    Root->SetStringField(TEXT("ExportType"), Montage ? TEXT("AnimMontage") : TEXT("AnimSequence"));
    Root->SetStringField(TEXT("AssetName"), AnimAsset->GetName());
    Root->SetStringField(TEXT("AssetPath"), AnimAsset->GetPathName());
    Root->SetStringField(TEXT("ExportTimestamp"), FDateTime::Now().ToString());

    // Skeleton
    if (USkeleton* Skeleton = AnimAsset->GetSkeleton())
    {
        Root->SetStringField(TEXT("Skeleton"), Skeleton->GetPathName());
    }

    // Duration
    Root->SetNumberField(TEXT("SequenceLength"), AnimAsset->GetPlayLength());
    Root->SetNumberField(TEXT("RateScale"), AnimAsset->RateScale);
    Root->SetBoolField(TEXT("bLoop"), AnimAsset->bLoop);

    // Sampling, root motion, additive and compression settings only exist on a plain sequence.
    if (const UAnimSequence* Sequence = Cast<UAnimSequence>(AnimAsset))
    {
        ExportSequenceSettings(Sequence, Root);
    }

    // Sections, slot tracks and blends are montage composition, a plain sequence carries none of it.
    if (Montage)
    {
        Root->SetNumberField(TEXT("BlendInTime"), Montage->BlendIn.GetBlendTime());
        Root->SetNumberField(TEXT("BlendOutTime"), Montage->BlendOut.GetBlendTime());
        ExportMontageBlend(Montage, Root);

        TArray<TSharedPtr<FJsonValue>> SectionsArray;
        for (int32 i = 0; i < Montage->CompositeSections.Num(); i++)
        {
            const FCompositeSection& Section = Montage->CompositeSections[i];
            TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
            SectionObj->SetStringField(TEXT("Name"), Section.SectionName.ToString());
            SectionObj->SetNumberField(TEXT("StartTime"), Section.GetTime());
            SectionObj->SetNumberField(TEXT("SectionIndex"), i);

            if (Section.NextSectionName != NAME_None)
            {
                SectionObj->SetStringField(TEXT("NextSection"), Section.NextSectionName.ToString());
            }

            SectionsArray.Add(MakeShared<FJsonValueObject>(SectionObj));
        }
        Root->SetArrayField(TEXT("Sections"), SectionsArray);

        TArray<TSharedPtr<FJsonValue>> SlotsArray;
        for (const FSlotAnimationTrack& SlotTrack : Montage->SlotAnimTracks)
        {
            TSharedPtr<FJsonObject> SlotObj = MakeShared<FJsonObject>();
            SlotObj->SetStringField(TEXT("SlotName"), SlotTrack.SlotName.ToString());

            TArray<TSharedPtr<FJsonValue>> SegmentsArray;
            for (const FAnimSegment& Segment : SlotTrack.AnimTrack.AnimSegments)
            {
                TSharedPtr<FJsonObject> SegObj = MakeShared<FJsonObject>();

                if (Segment.GetAnimReference())
                {
                    SegObj->SetStringField(TEXT("AnimSequence"), Segment.GetAnimReference()->GetName());
                    SegObj->SetStringField(TEXT("AnimSequencePath"), Segment.GetAnimReference()->GetPathName());
                }

                SegObj->SetNumberField(TEXT("StartPos"), Segment.StartPos);
                SegObj->SetNumberField(TEXT("AnimStartTime"), Segment.AnimStartTime);
                SegObj->SetNumberField(TEXT("AnimEndTime"), Segment.AnimEndTime);
                SegObj->SetNumberField(TEXT("AnimPlayRate"), Segment.AnimPlayRate);

                SegmentsArray.Add(MakeShared<FJsonValueObject>(SegObj));
            }
            SlotObj->SetArrayField(TEXT("Segments"), SegmentsArray);

            SlotsArray.Add(MakeShared<FJsonValueObject>(SlotObj));
        }
        Root->SetArrayField(TEXT("SlotTracks"), SlotsArray);
    }

    ExportCurves(AnimAsset, Root);
    ExportNotifyTracks(AnimAsset, Root);

    // Notifies (ANS + AN)
    TArray<TSharedPtr<FJsonValue>> NotifiesArray;
    for (const FAnimNotifyEvent& NotifyEvent : AnimAsset->Notifies)
    {
        TSharedPtr<FJsonObject> NotifyObj = ExportNotify(NotifyEvent);
        if (NotifyObj.IsValid())
        {
            NotifiesArray.Add(MakeShared<FJsonValueObject>(NotifyObj));
        }
    }
    Root->SetArrayField(TEXT("Notifies"), NotifiesArray);

    return Root;
}

void UAnimAssetExportCommandlet::ExportSequenceSettings(const UAnimSequence* Sequence, const TSharedRef<FJsonObject>& Root) const
{
    const IAnimationDataModel* DataModel = Sequence->IsDataModelValid() ? Sequence->GetDataModel() : nullptr;
    if (DataModel)
    {
        Root->SetNumberField(TEXT("NumberOfFrames"), DataModel->GetNumberOfFrames());
        Root->SetNumberField(TEXT("FrameRate"), DataModel->GetFrameRate().AsDecimal());
    }

    Root->SetStringField(TEXT("Interpolation"), StaticEnum<EAnimInterpolationType>()->GetNameStringByValue(static_cast<int64>(Sequence->Interpolation)));

    Root->SetBoolField(TEXT("bEnableRootMotion"), Sequence->bEnableRootMotion);
    Root->SetStringField(TEXT("RootMotionRootLock"), StaticEnum<ERootMotionRootLock::Type>()->GetNameStringByValue(static_cast<int64>(Sequence->RootMotionRootLock.GetValue())));
    Root->SetBoolField(TEXT("bForceRootLock"), Sequence->bForceRootLock);
    Root->SetBoolField(TEXT("bUseNormalizedRootMotionScale"), Sequence->bUseNormalizedRootMotionScale);

    Root->SetStringField(TEXT("AdditiveAnimType"), StaticEnum<EAdditiveAnimationType>()->GetNameStringByValue(static_cast<int64>(Sequence->AdditiveAnimType.GetValue())));

    if (Sequence->AdditiveAnimType != AAT_None)
    {
        Root->SetStringField(TEXT("RefPoseType"), StaticEnum<EAdditiveBasePoseType>()->GetNameStringByValue(static_cast<int64>(Sequence->RefPoseType.GetValue())));
        Root->SetNumberField(TEXT("RefFrameIndex"), Sequence->RefFrameIndex);

        if (Sequence->RefPoseSeq)
        {
            Root->SetStringField(TEXT("RefPoseSeq"), Sequence->RefPoseSeq->GetPathName());
        }
    }

    if (Sequence->RetargetSource != NAME_None)
    {
        Root->SetStringField(TEXT("RetargetSource"), Sequence->RetargetSource.ToString());
    }

    const TSoftObjectPtr<USkeletalMesh>& RetargetSourceAsset = Sequence->GetRetargetSourceAsset();
    if (!RetargetSourceAsset.IsNull())
    {
        Root->SetStringField(TEXT("RetargetSourceAsset"), RetargetSourceAsset.ToString());
    }

    if (Sequence->BoneCompressionSettings)
    {
        Root->SetStringField(TEXT("BoneCompressionSettings"), Sequence->BoneCompressionSettings->GetPathName());
    }

    if (Sequence->CurveCompressionSettings)
    {
        Root->SetStringField(TEXT("CurveCompressionSettings"), Sequence->CurveCompressionSettings->GetPathName());
    }

    TArray<TSharedPtr<FJsonValue>> MarkersArray;
    for (const FAnimSyncMarker& Marker : Sequence->AuthoredSyncMarkers)
    {
        TSharedPtr<FJsonObject> MarkerObj = MakeShared<FJsonObject>();
        MarkerObj->SetStringField(TEXT("Name"), Marker.MarkerName.ToString());
        MarkerObj->SetNumberField(TEXT("Time"), Marker.Time);
        MarkerObj->SetNumberField(TEXT("TrackIndex"), Marker.TrackIndex);

        MarkersArray.Add(MakeShared<FJsonValueObject>(MarkerObj));
    }

    if (MarkersArray.Num() > 0)
    {
        Root->SetArrayField(TEXT("SyncMarkers"), MarkersArray);
    }
}

void UAnimAssetExportCommandlet::ExportMontageBlend(const UAnimMontage* Montage, const TSharedRef<FJsonObject>& Root) const
{
    Root->SetObjectField(TEXT("BlendIn"), MakeBlendJson(Montage->BlendIn));
    Root->SetObjectField(TEXT("BlendOut"), MakeBlendJson(Montage->BlendOut));
    Root->SetNumberField(TEXT("BlendOutTriggerTime"), Montage->BlendOutTriggerTime);
    Root->SetBoolField(TEXT("bEnableAutoBlendOut"), Montage->bEnableAutoBlendOut);

    if (Montage->BlendProfileIn)
    {
        Root->SetStringField(TEXT("BlendProfileIn"), Montage->BlendProfileIn->GetPathName());
    }

    if (Montage->BlendProfileOut)
    {
        Root->SetStringField(TEXT("BlendProfileOut"), Montage->BlendProfileOut->GetPathName());
    }

    // The class default is a real name, "MontageTimeStretchCurve", so NAME_None never filters it and
    // every montage printed one whether or not it hosts that curve.
    const FName DefaultTimeStretchCurveName = UAnimMontage::StaticClass()->GetDefaultObject<UAnimMontage>()->TimeStretchCurveName;
    if (Montage->TimeStretchCurveName != NAME_None && Montage->TimeStretchCurveName != DefaultTimeStretchCurveName)
    {
        Root->SetStringField(TEXT("TimeStretchCurveName"), Montage->TimeStretchCurveName.ToString());
    }

    if (Montage->SyncGroup != NAME_None)
    {
        Root->SetStringField(TEXT("SyncGroup"), Montage->SyncGroup.ToString());
        Root->SetNumberField(TEXT("SyncSlotIndex"), Montage->SyncSlotIndex);
    }
}

void UAnimAssetExportCommandlet::ExportCurves(const UAnimSequenceBase* AnimAsset, const TSharedRef<FJsonObject>& Root) const
{
    const IAnimationDataModel* DataModel = AnimAsset->IsDataModelValid() ? AnimAsset->GetDataModel() : nullptr;
    if (!DataModel)
    {
        return;
    }

    TArray<TSharedPtr<FJsonValue>> CurvesArray;
    for (const FFloatCurve& Curve : DataModel->GetFloatCurves())
    {
        TSharedPtr<FJsonObject> CurveObj = MakeShared<FJsonObject>();
        CurveObj->SetStringField(TEXT("Name"), Curve.GetName().ToString());

        TArray<TSharedPtr<FJsonValue>> FlagsArray = MakeCurveFlagsJson(Curve.GetCurveTypeFlags());
        if (FlagsArray.Num() > 0)
        {
            CurveObj->SetArrayField(TEXT("Flags"), FlagsArray);
        }

        const TArray<FRichCurveKey>& Keys = Curve.FloatCurve.GetConstRefOfKeys();
        CurveObj->SetNumberField(TEXT("KeyCount"), Keys.Num());

        const int32 EmittedKeyCount = FMath::Min(Keys.Num(), kMaxCurveKeysPerCurve);
        TArray<TSharedPtr<FJsonValue>> KeysArray;
        for (int32 i = 0; i < EmittedKeyCount; i++)
        {
            TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
            KeyObj->SetNumberField(TEXT("Time"), Keys[i].Time);
            KeyObj->SetNumberField(TEXT("Value"), Keys[i].Value);
            KeyObj->SetStringField(TEXT("InterpMode"), StaticEnum<ERichCurveInterpMode>()->GetNameStringByValue(static_cast<int64>(Keys[i].InterpMode.GetValue())));

            KeysArray.Add(MakeShared<FJsonValueObject>(KeyObj));
        }
        CurveObj->SetArrayField(TEXT("Keys"), KeysArray);

        if (Keys.Num() > EmittedKeyCount)
        {
            CurveObj->SetBoolField(TEXT("KeysTruncated"), true);
        }

        CurvesArray.Add(MakeShared<FJsonValueObject>(CurveObj));
    }

    if (CurvesArray.Num() > 0)
    {
        Root->SetArrayField(TEXT("Curves"), CurvesArray);
    }

    TArray<TSharedPtr<FJsonValue>> TransformCurvesArray;
    for (const FTransformCurve& Curve : DataModel->GetTransformCurves())
    {
        const int32 TranslationKeyCount = Curve.TranslationCurve.GetNumKeys();
        const int32 RotationKeyCount = Curve.RotationCurve.GetNumKeys();
        const int32 ScaleKeyCount = Curve.ScaleCurve.GetNumKeys();

        TSharedPtr<FJsonObject> CurveObj = MakeShared<FJsonObject>();
        CurveObj->SetStringField(TEXT("Name"), Curve.GetName().ToString());
        CurveObj->SetNumberField(TEXT("KeyCount"), FMath::Max3(TranslationKeyCount, RotationKeyCount, ScaleKeyCount));

        TransformCurvesArray.Add(MakeShared<FJsonValueObject>(CurveObj));
    }

    if (TransformCurvesArray.Num() > 0)
    {
        Root->SetArrayField(TEXT("TransformCurves"), TransformCurvesArray);
    }
}

// Notifies carry a bare integer TrackIndex, the track list is what turns it back into a name.
void UAnimAssetExportCommandlet::ExportNotifyTracks(const UAnimSequenceBase* AnimAsset, const TSharedRef<FJsonObject>& Root) const
{
    TArray<TSharedPtr<FJsonValue>> TracksArray;
    for (int32 i = 0; i < AnimAsset->AnimNotifyTracks.Num(); i++)
    {
        const FAnimNotifyTrack& Track = AnimAsset->AnimNotifyTracks[i];
        TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
        TrackObj->SetNumberField(TEXT("Index"), i);
        TrackObj->SetStringField(TEXT("Name"), Track.TrackName.ToString());
        TrackObj->SetStringField(TEXT("Color"), Track.TrackColor.ToString());

        TracksArray.Add(MakeShared<FJsonValueObject>(TrackObj));
    }

    if (TracksArray.Num() > 0)
    {
        Root->SetArrayField(TEXT("NotifyTracks"), TracksArray);
    }
}

TSharedPtr<FJsonObject> UAnimAssetExportCommandlet::ExportNotify(const FAnimNotifyEvent& NotifyEvent) const
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

    Obj->SetStringField(TEXT("NotifyName"), NotifyEvent.NotifyName.ToString());
    Obj->SetNumberField(TEXT("TriggerTime"), NotifyEvent.GetTriggerTime());
    Obj->SetNumberField(TEXT("Duration"), NotifyEvent.GetDuration());
    Obj->SetNumberField(TEXT("TrackIndex"), NotifyEvent.TrackIndex);

    bool bIsState = NotifyEvent.NotifyStateClass != nullptr;
    Obj->SetBoolField(TEXT("IsState"), bIsState);

    if (bIsState)
    {
        UAnimNotifyState* State = NotifyEvent.NotifyStateClass;
        Obj->SetStringField(TEXT("NotifyClass"), State->GetClass()->GetName());

        TSharedPtr<FJsonObject> Params = ExportSubclassProperties(State, UAnimNotifyState::StaticClass());
        if (Params.IsValid() && Params->Values.Num() > 0)
        {
            Obj->SetObjectField(TEXT("Parameters"), Params);
        }
    }
    else if (NotifyEvent.Notify)
    {
        UAnimNotify* Notify = NotifyEvent.Notify;
        Obj->SetStringField(TEXT("NotifyClass"), Notify->GetClass()->GetName());

        TSharedPtr<FJsonObject> Params = ExportSubclassProperties(Notify, UAnimNotify::StaticClass());
        if (Params.IsValid() && Params->Values.Num() > 0)
        {
            Obj->SetObjectField(TEXT("Parameters"), Params);
        }
    }

    return Obj;
}

TSharedPtr<FJsonObject> UAnimAssetExportCommandlet::ExportSubclassProperties(UObject* Object, UClass* StopAtClass) const
{
    TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();

    UClass* CurrentClass = Object->GetClass();
    while (CurrentClass && CurrentClass != StopAtClass)
    {
        for (TFieldIterator<FProperty> PropIt(CurrentClass, EFieldIteratorFlags::ExcludeSuper); PropIt; ++PropIt)
        {
            FProperty* Prop = *PropIt;
            if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
            {
                continue;
            }

            FString Value;
            Value += UAssetWorkbench::ExportPropertyText(Prop, Prop->ContainerPtrToValuePtr<void>(Object), Object);
            if (!Value.IsEmpty())
            {
                Props->SetStringField(Prop->GetName(), Value);
            }
        }
        CurrentClass = CurrentClass->GetSuperClass();
    }

    return Props;
}

