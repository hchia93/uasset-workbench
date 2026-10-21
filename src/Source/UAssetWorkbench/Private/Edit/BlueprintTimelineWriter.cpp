#include "Edit/BlueprintWriter.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"

#include "Curves/CurveFloat.h"
#include "Curves/RichCurve.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/TimelineTemplate.h"

namespace
{
    FString DescribeTimelines(const UBlueprint* Blueprint)
    {
        TArray<FString> Names;
        for (const UTimelineTemplate* Timeline : Blueprint->Timelines)
        {
            if (Timeline)
            {
                Names.Add(Timeline->GetVariableName().ToString());
            }
        }

        return Names.IsEmpty() ? FString(TEXT("<none>")) : FString::Join(Names, TEXT(", "));
    }

    FString DescribeFloatTracks(const UTimelineTemplate* Timeline)
    {
        TArray<FString> Names;
        for (const FTTFloatTrack& Track : Timeline->FloatTracks)
        {
            Names.Add(Track.GetTrackName().ToString());
        }

        return Names.IsEmpty() ? FString(TEXT("<none>")) : FString::Join(Names, TEXT(", "));
    }

    class FBlueprintTimelineWriter : public IBlueprintWriter
    {
    public:
        virtual const TCHAR* GetSpecKey() const override
        {
            return TEXT("Timelines");
        }

        virtual bool Apply(FBlueprintEditContext& Context, const TSharedPtr<FJsonValue>& Section) override
        {
            const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
            if (!Section->TryGetArray(Entries))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Timelines must be an array of timelines"), *Context.AssetPath);
                return false;
            }

            for (const TSharedPtr<FJsonValue>& Value : *Entries)
            {
                const TSharedPtr<FJsonObject>& Desc = Value->AsObject();
                FString Name;
                if (!Desc.IsValid() || !Desc->TryGetStringField(TEXT("Name"), Name))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Timelines entry needs Name"), *Context.AssetPath);
                    return false;
                }

                UTimelineTemplate* Timeline = Context.Blueprint->FindTimelineTemplateByVariableName(FName(*Name));
                if (!Timeline)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no timeline '%s'. Timelines: %s"), *Context.AssetPath, *Name, *DescribeTimelines(Context.Blueprint));
                    return false;
                }

                if (!ApplyTimeline(Context, Timeline, Desc))
                {
                    return false;
                }
            }

            return true;
        }

    private:
        bool ApplyTimeline(FBlueprintEditContext& Context, UTimelineTemplate* Timeline, const TSharedPtr<FJsonObject>& Desc) const
        {
            const FString TimelineName = Timeline->GetVariableName().ToString();

            double Length = 0.0;
            const bool bHasLength = Desc->TryGetNumberField(TEXT("Length"), Length);
            const TArray<TSharedPtr<FJsonValue>>* Tracks = nullptr;
            const bool bHasTracks = Desc->TryGetArrayField(TEXT("FloatTracks"), Tracks);
            if (!bHasLength && !bHasTracks)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: timeline '%s' writes nothing. Expected Length or FloatTracks"), *Context.AssetPath, *TimelineName);
                return false;
            }

            if (bHasLength)
            {
                if (Length <= 0.0)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: timeline '%s' Length must be positive, got %.3f"), *Context.AssetPath, *TimelineName, Length);
                    return false;
                }

                UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: timeline '%s' length %.3f -> %.3f"), *Context.Blueprint->GetName(), *TimelineName, Timeline->TimelineLength, Length);
                Timeline->Modify();
                Timeline->TimelineLength = static_cast<float>(Length);
                ++Context.Ops;
            }

            if (bHasTracks)
            {
                for (const TSharedPtr<FJsonValue>& Item : *Tracks)
                {
                    if (!ApplyFloatTrack(Context, Timeline, Item->AsObject()))
                    {
                        return false;
                    }
                }
            }

            Context.bNeedsStructuralRecompile = true;
            return true;
        }

        bool ApplyFloatTrack(FBlueprintEditContext& Context, UTimelineTemplate* Timeline, const TSharedPtr<FJsonObject>& TrackDesc) const
        {
            const FString TimelineName = Timeline->GetVariableName().ToString();

            FString TrackName;
            if (!TrackDesc.IsValid() || !TrackDesc->TryGetStringField(TEXT("Name"), TrackName))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: a FloatTracks entry of timeline '%s' needs Name"), *Context.AssetPath, *TimelineName);
                return false;
            }

            FTTFloatTrack* Track = Timeline->FloatTracks.FindByPredicate([&TrackName](const FTTFloatTrack& Candidate) { return Candidate.GetTrackName() == FName(*TrackName); });
            if (!Track)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: timeline '%s' has no float track '%s'. Float tracks: %s"), *Context.AssetPath, *TimelineName, *TrackName, *DescribeFloatTracks(Timeline));
                return false;
            }

            // An external curve is a shared asset, writing it here would retune every timeline that points at it.
            if (Track->bIsExternalCurve || !Track->CurveFloat)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: float track '%s' of timeline '%s' reads external curve '%s'. Edit that asset instead"), *Context.AssetPath, *TrackName, *TimelineName, *GetPathNameSafe(Track->CurveFloat));
                return false;
            }

            TArray<FRichCurveKey> Keys;
            if (!ReadKeys(Context, TrackDesc, TrackName, Keys))
            {
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: timeline '%s' track '%s' <- %d key(s)"), *Context.Blueprint->GetName(), *TimelineName, *TrackName, Keys.Num());

            UCurveFloat* Curve = Track->CurveFloat;
            Curve->Modify();
            FRichCurve& RichCurve = Curve->FloatCurve;
            RichCurve.Reset();
            for (const FRichCurveKey& Key : Keys)
            {
                const FKeyHandle Handle = RichCurve.AddKey(Key.Time, Key.Value);
                RichCurve.SetKeyInterpMode(Handle, Key.InterpMode, /* bAutoSetTangents */ false);
            }
            RichCurve.AutoSetTangents();

            ++Context.Ops;
            return true;
        }

        // Same key shape EditAnimAsset's Curves takes and BlueprintEdGraphExport prints for a float track.
        bool ReadKeys(const FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& TrackDesc, const FString& TrackName, TArray<FRichCurveKey>& OutKeys) const
        {
            const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
            if (!TrackDesc->TryGetArrayField(TEXT("Keys"), Items) || Items->IsEmpty())
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: float track '%s' needs a non-empty Keys array"), *Context.AssetPath, *TrackName);
                return false;
            }

            for (const TSharedPtr<FJsonValue>& Item : *Items)
            {
                const TSharedPtr<FJsonObject>& KeyDesc = Item->AsObject();
                double Time = 0.0;
                double CurveValue = 0.0;
                if (!KeyDesc.IsValid() || !KeyDesc->TryGetNumberField(TEXT("Time"), Time) || !KeyDesc->TryGetNumberField(TEXT("Value"), CurveValue))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: a key of float track '%s' needs Time and Value"), *Context.AssetPath, *TrackName);
                    return false;
                }

                // AddKey keeps a second key at the same time instead of replacing the first.
                if (!OutKeys.IsEmpty() && Time <= OutKeys.Last().Time)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: keys of float track '%s' must ascend in Time, %.3f follows %.3f"), *Context.AssetPath, *TrackName, Time, OutKeys.Last().Time);
                    return false;
                }

                FRichCurveKey Key(static_cast<float>(Time), static_cast<float>(CurveValue));

                FString InterpText;
                if (KeyDesc->TryGetStringField(TEXT("InterpMode"), InterpText))
                {
                    ERichCurveInterpMode InterpMode = RCIM_Linear;
                    if (!UAssetWorkbench::ReadRichCurveInterpMode(InterpText, InterpMode))
                    {
                        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: unknown InterpMode '%s'. Expected Linear, Constant, Cubic or None"), *Context.AssetPath, *InterpText);
                        return false;
                    }

                    Key.InterpMode = InterpMode;
                }

                OutKeys.Add(Key);
            }

            return true;
        }
    };
}

TUniquePtr<IBlueprintWriter> MakeBlueprintTimelineWriter()
{
    return MakeUnique<FBlueprintTimelineWriter>();
}
