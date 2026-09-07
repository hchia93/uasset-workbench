#include "Edit/BlueprintWriter.h"
#include "UAssetWorkbenchModule.h"

#include "Animation/WidgetAnimation.h"
#include "Channels/MovieSceneChannelEditorData.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Curves/RealCurve.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MovieScene.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"
#include "WidgetBlueprint.h"

namespace
{
    // Inverse of the exporter's InterpModeToString, so a WidgetLayoutExport key feeds straight back.
    bool InterpModeFromString(const FString& Name, ERichCurveInterpMode& OutMode)
    {
        if (Name == TEXT("Linear"))
        {
            OutMode = RCIM_Linear;
            return true;
        }
        if (Name == TEXT("Constant"))
        {
            OutMode = RCIM_Constant;
            return true;
        }
        if (Name == TEXT("Cubic"))
        {
            OutMode = RCIM_Cubic;
            return true;
        }
        return false;
    }

    FString DescribeAnimations(const UWidgetBlueprint* WidgetBP)
    {
        TArray<FString> Names;
        for (const UWidgetAnimation* Animation : WidgetBP->Animations)
        {
            if (Animation)
            {
                Names.Add(Animation->GetDisplayName().ToString());
            }
        }
        return FString::Join(Names, TEXT(", "));
    }

    UWidgetAnimation* FindAnimation(const UWidgetBlueprint* WidgetBP, const FString& Name)
    {
        for (UWidgetAnimation* Animation : WidgetBP->Animations)
        {
            if (Animation && Animation->GetDisplayName().ToString() == Name)
            {
                return Animation;
            }
        }
        return nullptr;
    }

    // Same walk the exporter prints: possessable guid back to the widget name it was bound under.
    void CollectTracksForWidget(UWidgetAnimation* Animation, const FString& BoundWidget, TArray<UMovieSceneTrack*>& OutTracks)
    {
        TMap<FGuid, FString> BindingNames;
        for (const FWidgetAnimationBinding& Binding : Animation->AnimationBindings)
        {
            BindingNames.Add(Binding.AnimationGuid, Binding.WidgetName.ToString());
        }

        const UMovieScene* MovieScene = Animation->GetMovieScene();
        for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
        {
            const FString* WidgetName = BindingNames.Find(Binding.GetObjectGuid());
            if (!WidgetName || *WidgetName != BoundWidget)
            {
                continue;
            }

            for (UMovieSceneTrack* Track : Binding.GetTracks())
            {
                if (Track)
                {
                    OutTracks.Add(Track);
                }
            }
        }
    }

    FString DescribeTracks(const TArray<UMovieSceneTrack*>& Tracks)
    {
        TArray<FString> Names;
        for (const UMovieSceneTrack* Track : Tracks)
        {
            Names.Add(Track->GetDisplayName().ToString());
        }
        return FString::Join(Names, TEXT(", "));
    }

    FString DescribeChannels(const FMovieSceneChannelProxy& Proxy)
    {
        TArray<FString> Names;
        for (const FMovieSceneChannelMetaData& Meta : Proxy.GetMetaData<FMovieSceneFloatChannel>())
        {
            Names.Add(Meta.Name.ToString());
        }
        for (const FMovieSceneChannelMetaData& Meta : Proxy.GetMetaData<FMovieSceneDoubleChannel>())
        {
            Names.Add(Meta.Name.ToString());
        }
        return FString::Join(Names, TEXT(", "));
    }

    // Whole-channel replacement rather than per-key patching. Key times shift as often as values do, so an
    // index-addressed patch would silently apply to the wrong key once a spec is re-run against a changed curve.
    template <typename ChannelType, typename ValueType>
    bool WriteChannelKeys(ChannelType* Channel, FFrameRate TickResolution, const TArray<TSharedPtr<FJsonValue>>& Keys, const FString& AssetPath, const FString& ChannelName)
    {
        TArray<FFrameNumber> Times;
        TArray<ValueType> Values;
        Times.Reserve(Keys.Num());
        Values.Reserve(Keys.Num());

        for (const TSharedPtr<FJsonValue>& KeyValue : Keys)
        {
            const TSharedPtr<FJsonObject>& KeyObj = KeyValue->AsObject();
            double Time = 0.0;
            double Value = 0.0;
            if (!KeyObj.IsValid() || !KeyObj->TryGetNumberField(TEXT("Time"), Time) || !KeyObj->TryGetNumberField(TEXT("Value"), Value))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: key on channel '%s' needs Time and Value"), *AssetPath, *ChannelName);
                return false;
            }

            // Float and double channels carry the same shape, only the storage width differs.
            ValueType Entry;
            Entry.Value = static_cast<decltype(Entry.Value)>(Value);

            FString InterpName;
            if (KeyObj->TryGetStringField(TEXT("Interp"), InterpName))
            {
                ERichCurveInterpMode InterpMode = RCIM_Cubic;
                if (!InterpModeFromString(InterpName, InterpMode))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: unknown Interp '%s' on channel '%s'. Accepted: Linear, Constant, Cubic"), *AssetPath, *InterpName, *ChannelName);
                    return false;
                }
                Entry.InterpMode = InterpMode;
            }

            Times.Add(TickResolution.AsFrameNumber(Time));
            Values.Add(Entry);
        }

        Channel->Reset();

        TMovieSceneChannelData<ValueType> Data = Channel->GetData();
        for (int32 KeyIndex = 0; KeyIndex < Times.Num(); KeyIndex++)
        {
            Data.AddKey(Times[KeyIndex], Values[KeyIndex]);
        }

        Channel->AutoSetTangents();
        return true;
    }

    class FBlueprintWidgetAnimationWriter : public IBlueprintWriter
    {
    public:
        virtual const TCHAR* GetSpecKey() const override
        {
            return TEXT("WidgetAnimations");
        }

        virtual bool Apply(FBlueprintEditContext& Context, const TSharedPtr<FJsonValue>& Section) override
        {
            const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
            if (!Section->TryGetArray(Operations))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: WidgetAnimations must be an array of operations"), *Context.AssetPath);
                return false;
            }

            UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(Context.Blueprint);
            if (!WidgetBP)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s is not a WidgetBlueprint, WidgetAnimations does not apply"), *Context.AssetPath);
                return false;
            }

            for (const TSharedPtr<FJsonValue>& Value : *Operations)
            {
                const TSharedPtr<FJsonObject>& Desc = Value->AsObject();
                FString Op;
                FString AnimationName;
                if (!Desc.IsValid() || !Desc->TryGetStringField(TEXT("Op"), Op) || !Desc->TryGetStringField(TEXT("Animation"), AnimationName))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: animation operation needs Op and Animation"), *Context.AssetPath);
                    return false;
                }

                if (Op != TEXT("SetKeys"))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: unknown animation Op '%s'. Accepted: SetKeys"), *Context.AssetPath, *Op);
                    return false;
                }

                UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: animation %s %s"), *WidgetBP->GetName(), *Op, *AnimationName);
                ++Context.Ops;

                if (!ApplySetKeys(Context, WidgetBP, AnimationName, Desc))
                {
                    return false;
                }
            }

            return true;
        }

    private:
        bool ApplySetKeys(const FBlueprintEditContext& Context, UWidgetBlueprint* WidgetBP, const FString& AnimationName, const TSharedPtr<FJsonObject>& Desc) const
        {
            UWidgetAnimation* Animation = FindAnimation(WidgetBP, AnimationName);
            if (!Animation || !Animation->GetMovieScene())
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: no animation named '%s'. Asset has: %s"), *Context.AssetPath, *AnimationName, *DescribeAnimations(WidgetBP));
                return false;
            }

            FString BoundWidget;
            FString ChannelName;
            const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
            if (!Desc->TryGetStringField(TEXT("BoundWidget"), BoundWidget) || !Desc->TryGetStringField(TEXT("Channel"), ChannelName) || !Desc->TryGetArrayField(TEXT("Keys"), Keys))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: animation SetKeys needs BoundWidget, Channel and Keys"), *Context.AssetPath);
                return false;
            }

            TArray<UMovieSceneTrack*> Tracks;
            CollectTracksForWidget(Animation, BoundWidget, Tracks);
            if (Tracks.Num() == 0)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' has no track bound to '%s'"), *Context.AssetPath, *AnimationName, *BoundWidget);
                return false;
            }

            FString TrackName;
            if (Desc->TryGetStringField(TEXT("TrackName"), TrackName))
            {
                Tracks.RemoveAll([&TrackName](const UMovieSceneTrack* Track)
                {
                    return Track->GetDisplayName().ToString() != TrackName;
                });
            }

            if (Tracks.Num() != 1)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' on '%s' matches %d tracks, name one with TrackName. Candidates: %s"), *Context.AssetPath, *AnimationName, *BoundWidget, Tracks.Num(), *DescribeTracks(Tracks));
                return false;
            }

            int32 SectionIndex = 0;
            Desc->TryGetNumberField(TEXT("SectionIndex"), SectionIndex);

            const TArray<UMovieSceneSection*>& Sections = Tracks[0]->GetAllSections();
            if (!Sections.IsValidIndex(SectionIndex))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: track '%s' has %d section(s), SectionIndex %d is out of range"), *Context.AssetPath, *Tracks[0]->GetDisplayName().ToString(), Sections.Num(), SectionIndex);
                return false;
            }

            UMovieSceneSection* SectionObject = Sections[SectionIndex];
            UMovieScene* MovieScene = Animation->GetMovieScene();
            const FFrameRate TickResolution = MovieScene->GetTickResolution();

            MovieScene->Modify();
            SectionObject->Modify();

            FMovieSceneChannelProxy& Proxy = SectionObject->GetChannelProxy();
            const FName ChannelFName(*ChannelName);

            if (FMovieSceneFloatChannel* FloatChannel = FindNamedChannel<FMovieSceneFloatChannel>(Proxy, ChannelFName))
            {
                return WriteChannelKeys<FMovieSceneFloatChannel, FMovieSceneFloatValue>(FloatChannel, TickResolution, *Keys, Context.AssetPath, ChannelName);
            }

            if (FMovieSceneDoubleChannel* DoubleChannel = FindNamedChannel<FMovieSceneDoubleChannel>(Proxy, ChannelFName))
            {
                return WriteChannelKeys<FMovieSceneDoubleChannel, FMovieSceneDoubleValue>(DoubleChannel, TickResolution, *Keys, Context.AssetPath, ChannelName);
            }

            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: no channel named '%s' on that section. Section has: %s"), *Context.AssetPath, *ChannelName, *DescribeChannels(Proxy));
            return false;
        }

        // Meta name is the label the exporter prints and the sequencer shows, e.g. "Translation.X".
        template <typename ChannelType>
        static ChannelType* FindNamedChannel(const FMovieSceneChannelProxy& Proxy, const FName& ChannelName)
        {
            TArrayView<ChannelType*> Channels = Proxy.GetChannels<ChannelType>();
            TArrayView<const FMovieSceneChannelMetaData> MetaData = Proxy.GetMetaData<ChannelType>();

            for (int32 ChannelIndex = 0; ChannelIndex < Channels.Num(); ChannelIndex++)
            {
                if (Channels[ChannelIndex] && MetaData.IsValidIndex(ChannelIndex) && MetaData[ChannelIndex].Name == ChannelName)
                {
                    return Channels[ChannelIndex];
                }
            }
            return nullptr;
        }
    };
}

TUniquePtr<IBlueprintWriter> MakeBlueprintWidgetAnimationWriter()
{
    return MakeUnique<FBlueprintWidgetAnimationWriter>();
}
