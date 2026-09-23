#include "Edit/BlueprintWriter.h"
#include "UAssetWorkbenchModule.h"

#include "Animation/MovieScene2DTransformTrack.h"
#include "Animation/MovieSceneMarginTrack.h"
#include "Animation/WidgetAnimation.h"
#include "Blueprint/WidgetTree.h"
#include "Channels/MovieSceneChannelEditorData.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Curves/RealCurve.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Components/Widget.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "MovieScene.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"
#include "Tracks/MovieSceneBoolTrack.h"
#include "Tracks/MovieSceneByteTrack.h"
#include "Tracks/MovieSceneColorTrack.h"
#include "Tracks/MovieSceneDoubleTrack.h"
#include "Tracks/MovieSceneEnumTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieSceneIntegerTrack.h"
#include "Tracks/MovieSceneObjectPropertyTrack.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "Tracks/MovieSceneStringTrack.h"
#include "Tracks/MovieSceneVectorTrack.h"
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

    UWidget* FindWidget(UWidgetBlueprint* WidgetBP, const FString& WidgetName)
    {
        UWidget* Found = nullptr;
        WidgetBP->WidgetTree->ForEachWidget([&Found, &WidgetName](UWidget* Widget)
        {
            if (Widget && Widget->GetName() == WidgetName)
            {
                Found = Widget;
            }
        });
        return Found;
    }

    FString DescribeWidgets(UWidgetBlueprint* WidgetBP)
    {
        TArray<FString> Names;
        WidgetBP->WidgetTree->ForEachWidget([&Names](UWidget* Widget)
        {
            if (Widget)
            {
                Names.Add(Widget->GetName());
            }
        });
        return FString::Join(Names, TEXT(", "));
    }

    // Same pairing the engine registers its property track editors with: the runtime property registry in
    // MovieSceneTracksComponentTypes for the generic types, the UMG track editors for Margin and WidgetTransform.
    UClass* ResolveTrackClass(const FProperty* Property)
    {
        if (CastField<FBoolProperty>(Property))
        {
            return UMovieSceneBoolTrack::StaticClass();
        }
        if (CastField<FByteProperty>(Property))
        {
            return UMovieSceneByteTrack::StaticClass();
        }
        if (CastField<FEnumProperty>(Property))
        {
            return UMovieSceneEnumTrack::StaticClass();
        }
        if (CastField<FIntProperty>(Property))
        {
            return UMovieSceneIntegerTrack::StaticClass();
        }
        if (CastField<FFloatProperty>(Property))
        {
            return UMovieSceneFloatTrack::StaticClass();
        }
        if (CastField<FDoubleProperty>(Property))
        {
            return UMovieSceneDoubleTrack::StaticClass();
        }
        if (CastField<FStrProperty>(Property))
        {
            return UMovieSceneStringTrack::StaticClass();
        }
        if (CastField<FObjectProperty>(Property))
        {
            return UMovieSceneObjectPropertyTrack::StaticClass();
        }

        const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
        if (!StructProperty || !StructProperty->Struct)
        {
            return nullptr;
        }

        const FName StructName = StructProperty->Struct->GetFName();
        if (StructName == TEXT("WidgetTransform"))
        {
            return UMovieScene2DTransformTrack::StaticClass();
        }
        if (StructName == TEXT("Margin"))
        {
            return UMovieSceneMarginTrack::StaticClass();
        }
        if (StructName == TEXT("LinearColor") || StructName == TEXT("Color") || StructName == TEXT("SlateColor"))
        {
            return UMovieSceneColorTrack::StaticClass();
        }
        if (StructName == TEXT("Vector2D") || StructName == TEXT("Vector2f") || StructName == TEXT("Vector") || StructName == TEXT("Vector3f") || StructName == TEXT("Vector4"))
        {
            return UMovieSceneFloatVectorTrack::StaticClass();
        }
        return nullptr;
    }

    int32 VectorChannelCount(const FProperty* Property)
    {
        const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
        const FName StructName = StructProperty && StructProperty->Struct ? StructProperty->Struct->GetFName() : NAME_None;
        if (StructName == TEXT("Vector2D") || StructName == TEXT("Vector2f"))
        {
            return 2;
        }
        if (StructName == TEXT("Vector4"))
        {
            return 4;
        }
        return 3;
    }

    // A path walks struct members, "RenderTransform.Translation" style, and resolves against the last hop.
    const FProperty* ResolvePropertyPath(const UClass* WidgetClass, const FString& PropertyPath)
    {
        TArray<FString> Segments;
        PropertyPath.ParseIntoArray(Segments, TEXT("."));

        const UStruct* Owner = WidgetClass;
        const FProperty* Property = nullptr;
        for (const FString& Segment : Segments)
        {
            if (!Owner)
            {
                return nullptr;
            }
            Property = FindFProperty<FProperty>(Owner, *Segment);
            if (!Property)
            {
                return nullptr;
            }
            const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
            Owner = StructProperty ? StructProperty->Struct : nullptr;
        }
        return Property;
    }

    // Possessable plus the UMG side binding the runtime resolves widget names through. Mirrors
    // UWidgetAnimation::BindPossessableObject, which is what Sequencer ends up calling.
    FGuid FindOrAddBinding(UWidgetAnimation* Animation, UWidgetBlueprint* WidgetBP, UWidget* Widget, bool bIsRootWidget)
    {
        const FName WidgetName = Widget ? Widget->GetFName() : WidgetBP->GetFName();
        for (const FWidgetAnimationBinding& Binding : Animation->AnimationBindings)
        {
            if (Binding.WidgetName == WidgetName && Binding.SlotWidgetName.IsNone())
            {
                return Binding.AnimationGuid;
            }
        }

        UClass* BoundClass = Widget ? Widget->GetClass() : WidgetBP->GeneratedClass.Get();
        const FGuid NewGuid = Animation->GetMovieScene()->AddPossessable(WidgetName.ToString(), BoundClass);

        FWidgetAnimationBinding NewBinding;
        NewBinding.AnimationGuid = NewGuid;
        NewBinding.WidgetName = WidgetName;
        NewBinding.bIsRootWidget = bIsRootWidget;
        Animation->AnimationBindings.Add(NewBinding);

        return NewGuid;
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

                UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: animation %s %s"), *WidgetBP->GetName(), *Op, *AnimationName);
                ++Context.Ops;

                bool bOk = false;
                if (Op == TEXT("SetKeys"))
                {
                    bOk = ApplySetKeys(Context, WidgetBP, AnimationName, Desc);
                }
                else if (Op == TEXT("Add"))
                {
                    bOk = ApplyAdd(Context, WidgetBP, AnimationName, Desc);
                }
                else if (Op == TEXT("Delete"))
                {
                    bOk = ApplyDelete(Context, WidgetBP, AnimationName);
                }
                else if (Op == TEXT("Rename"))
                {
                    bOk = ApplyRename(Context, WidgetBP, AnimationName, Desc);
                }
                else if (Op == TEXT("SetPlaybackRange"))
                {
                    bOk = ApplySetPlaybackRange(Context, WidgetBP, AnimationName, Desc);
                }
                else if (Op == TEXT("AddTrack"))
                {
                    bOk = ApplyAddTrack(Context, WidgetBP, AnimationName, Desc);
                }
                else if (Op == TEXT("DeleteTrack"))
                {
                    bOk = ApplyDeleteTrack(Context, WidgetBP, AnimationName, Desc);
                }
                else
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: unknown animation Op '%s'. Accepted: Add, Delete, Rename, SetPlaybackRange, AddTrack, DeleteTrack, SetKeys"), *Context.AssetPath, *Op);
                    return false;
                }

                if (!bOk)
                {
                    return false;
                }

                // Animations are Blueprint variables, so every structural change goes back through the compiler.
                Context.bNeedsStructuralRecompile = true;
            }

            return true;
        }

    private:
        // Same construction order the editor's New Animation button uses: object, movie scene, display rate,
        // playback range, then registration as a Blueprint variable.
        bool ApplyAdd(const FBlueprintEditContext& Context, UWidgetBlueprint* WidgetBP, const FString& AnimationName, const TSharedPtr<FJsonObject>& Desc) const
        {
            if (FindAnimation(WidgetBP, AnimationName))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: animation '%s' already exists"), *Context.AssetPath, *AnimationName);
                return false;
            }

            double StartTime = 0.0;
            double EndTime = 5.0;
            Desc->TryGetNumberField(TEXT("StartTime"), StartTime);
            Desc->TryGetNumberField(TEXT("EndTime"), EndTime);

            int32 DisplayRate = 20;
            Desc->TryGetNumberField(TEXT("DisplayRate"), DisplayRate);
            if (DisplayRate <= 0)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: DisplayRate must be positive, got %d"), *Context.AssetPath, DisplayRate);
                return false;
            }

            WidgetBP->Modify();

            UWidgetAnimation* Animation = NewObject<UWidgetAnimation>(WidgetBP, FName(), RF_Transactional);
            Animation->SetDisplayLabel(AnimationName);
            Animation->Rename(*AnimationName);

            Animation->MovieScene = NewObject<UMovieScene>(Animation, FName(*AnimationName), RF_Transactional);
            Animation->MovieScene->SetDisplayRate(FFrameRate(DisplayRate, 1));

            const FFrameRate TickResolution = Animation->MovieScene->GetTickResolution();
            const FFrameNumber StartFrame = TickResolution.AsFrameNumber(StartTime);
            const FFrameNumber EndFrame = TickResolution.AsFrameNumber(EndTime);
            Animation->MovieScene->SetPlaybackRange(TRange<FFrameNumber>(StartFrame, EndFrame + 1));
            Animation->MovieScene->GetEditorData().WorkStart = StartTime;
            Animation->MovieScene->GetEditorData().WorkEnd = EndTime;

            WidgetBP->Animations.Add(Animation);
            WidgetBP->OnVariableAdded(Animation->GetFName());

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("    added animation '%s' (%g to %gs at %dfps)"), *AnimationName, StartTime, EndTime, DisplayRate);
            return true;
        }

        bool ApplyDelete(const FBlueprintEditContext& Context, UWidgetBlueprint* WidgetBP, const FString& AnimationName) const
        {
            UWidgetAnimation* Animation = FindAnimation(WidgetBP, AnimationName);
            if (!Animation)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: no animation named '%s'. Asset has: %s"), *Context.AssetPath, *AnimationName, *DescribeAnimations(WidgetBP));
                return false;
            }

            const FName RemovedName = Animation->GetFName();
            WidgetBP->Modify();
            WidgetBP->Animations.Remove(Animation);
            WidgetBP->OnVariableRemoved(RemovedName);

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("    deleted animation '%s'"), *AnimationName);
            return true;
        }

        // The display label is what the editor lists, the object name is what a Blueprint graph references,
        // so both move and variable references follow.
        bool ApplyRename(const FBlueprintEditContext& Context, UWidgetBlueprint* WidgetBP, const FString& AnimationName, const TSharedPtr<FJsonObject>& Desc) const
        {
            UWidgetAnimation* Animation = FindAnimation(WidgetBP, AnimationName);
            if (!Animation)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: no animation named '%s'. Asset has: %s"), *Context.AssetPath, *AnimationName, *DescribeAnimations(WidgetBP));
                return false;
            }

            FString NewName;
            if (!Desc->TryGetStringField(TEXT("NewName"), NewName) || NewName.IsEmpty())
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: animation Rename needs a NewName"), *Context.AssetPath);
                return false;
            }

            if (FindAnimation(WidgetBP, NewName))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: animation '%s' already exists"), *Context.AssetPath, *NewName);
                return false;
            }

            const FName OldFName = Animation->GetFName();
            const FName NewFName(*NewName);

            WidgetBP->Modify();
            Animation->Modify();
            Animation->GetMovieScene()->Modify();

            Animation->SetDisplayLabel(NewName);
            Animation->Rename(*NewName);
            Animation->GetMovieScene()->Rename(*NewName);
            WidgetBP->OnVariableRenamed(OldFName, NewFName);
            FBlueprintEditorUtils::ReplaceVariableReferences(WidgetBP, OldFName, NewFName);

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("    renamed animation '%s' to '%s'"), *AnimationName, *NewName);
            return true;
        }

        bool ApplySetPlaybackRange(const FBlueprintEditContext& Context, UWidgetBlueprint* WidgetBP, const FString& AnimationName, const TSharedPtr<FJsonObject>& Desc) const
        {
            UWidgetAnimation* Animation = FindAnimation(WidgetBP, AnimationName);
            if (!Animation || !Animation->GetMovieScene())
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: no animation named '%s'. Asset has: %s"), *Context.AssetPath, *AnimationName, *DescribeAnimations(WidgetBP));
                return false;
            }

            double StartTime = 0.0;
            double EndTime = 0.0;
            if (!Desc->TryGetNumberField(TEXT("StartTime"), StartTime) || !Desc->TryGetNumberField(TEXT("EndTime"), EndTime))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: animation SetPlaybackRange needs StartTime and EndTime"), *Context.AssetPath);
                return false;
            }
            if (EndTime <= StartTime)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: EndTime %g must be greater than StartTime %g"), *Context.AssetPath, EndTime, StartTime);
                return false;
            }

            UMovieScene* MovieScene = Animation->GetMovieScene();
            MovieScene->Modify();

            const FFrameRate TickResolution = MovieScene->GetTickResolution();
            MovieScene->SetPlaybackRange(TRange<FFrameNumber>(TickResolution.AsFrameNumber(StartTime), TickResolution.AsFrameNumber(EndTime) + 1));
            MovieScene->GetEditorData().WorkStart = StartTime;
            MovieScene->GetEditorData().WorkEnd = EndTime;

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("    set '%s' playback range to %g - %gs"), *AnimationName, StartTime, EndTime);
            return true;
        }

        bool ApplyAddTrack(const FBlueprintEditContext& Context, UWidgetBlueprint* WidgetBP, const FString& AnimationName, const TSharedPtr<FJsonObject>& Desc) const
        {
            UWidgetAnimation* Animation = FindAnimation(WidgetBP, AnimationName);
            if (!Animation || !Animation->GetMovieScene())
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: no animation named '%s'. Asset has: %s"), *Context.AssetPath, *AnimationName, *DescribeAnimations(WidgetBP));
                return false;
            }

            FString BoundWidget;
            FString PropertyPath;
            if (!Desc->TryGetStringField(TEXT("BoundWidget"), BoundWidget) || !Desc->TryGetStringField(TEXT("PropertyPath"), PropertyPath))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: animation AddTrack needs BoundWidget and PropertyPath"), *Context.AssetPath);
                return false;
            }

            // The root widget binds as the user widget itself, every other name resolves in the widget tree.
            const bool bIsRootWidget = BoundWidget == WidgetBP->GetName();
            UWidget* Widget = bIsRootWidget ? nullptr : FindWidget(WidgetBP, BoundWidget);
            if (!bIsRootWidget && !Widget)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: no widget named '%s'. Asset has: %s"), *Context.AssetPath, *BoundWidget, *DescribeWidgets(WidgetBP));
                return false;
            }

            const UClass* WidgetClass = Widget ? Widget->GetClass() : WidgetBP->GeneratedClass.Get();
            const FProperty* Property = ResolvePropertyPath(WidgetClass, PropertyPath);
            if (!Property)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' has no property at path '%s'"), *Context.AssetPath, *BoundWidget, *PropertyPath);
                return false;
            }

            UClass* TrackClass = ResolveTrackClass(Property);
            if (!TrackClass)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: property '%s' is a %s, which no animation track covers"), *Context.AssetPath, *PropertyPath, *Property->GetClass()->GetName());
                return false;
            }

            UMovieScene* MovieScene = Animation->GetMovieScene();
            Animation->Modify();
            MovieScene->Modify();

            const FGuid ObjectGuid = FindOrAddBinding(Animation, WidgetBP, Widget, bIsRootWidget);

            // A second track for the same property would keep the first one's keys and silently lose to it.
            for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
            {
                if (Binding.GetObjectGuid() != ObjectGuid)
                {
                    continue;
                }
                for (const UMovieSceneTrack* Existing : Binding.GetTracks())
                {
                    const UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(Existing);
                    if (PropertyTrack && PropertyTrack->GetPropertyPath().ToString() == PropertyPath)
                    {
                        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' already animates '%s' on '%s'"), *Context.AssetPath, *AnimationName, *PropertyPath, *BoundWidget);
                        return false;
                    }
                }
            }

            UMovieSceneTrack* Track = MovieScene->AddTrack(TrackClass, ObjectGuid);
            if (!Track)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: movie scene refused a %s for '%s'"), *Context.AssetPath, *TrackClass->GetName(), *BoundWidget);
                return false;
            }

            UMovieScenePropertyTrack* PropertyTrack = CastChecked<UMovieScenePropertyTrack>(Track);
            PropertyTrack->SetPropertyNameAndPath(Property->GetFName(), PropertyPath);

            // Default display name is the raw property name. The timeline shows the spaced form, and that is
            // what an exported TrackName carries, so a spec written off an export resolves either way.
            PropertyTrack->SetDisplayName(FText::FromString(FName::NameToDisplayString(Property->GetName(), false)));

            if (UMovieSceneFloatVectorTrack* VectorTrack = Cast<UMovieSceneFloatVectorTrack>(Track))
            {
                VectorTrack->SetNumChannelsUsed(VectorChannelCount(Property));
            }

            // Sections carry the keys, and a track without one cannot be keyed at all.
            UMovieSceneSection* NewSection = PropertyTrack->CreateNewSection();
            NewSection->SetRange(MovieScene->GetPlaybackRange());
            PropertyTrack->AddSection(*NewSection);

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("    added %s on '%s' for '%s'"), *TrackClass->GetName(), *BoundWidget, *PropertyPath);
            return true;
        }

        bool ApplyDeleteTrack(const FBlueprintEditContext& Context, UWidgetBlueprint* WidgetBP, const FString& AnimationName, const TSharedPtr<FJsonObject>& Desc) const
        {
            UWidgetAnimation* Animation = FindAnimation(WidgetBP, AnimationName);
            if (!Animation || !Animation->GetMovieScene())
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: no animation named '%s'. Asset has: %s"), *Context.AssetPath, *AnimationName, *DescribeAnimations(WidgetBP));
                return false;
            }

            FString BoundWidget;
            if (!Desc->TryGetStringField(TEXT("BoundWidget"), BoundWidget))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: animation DeleteTrack needs BoundWidget"), *Context.AssetPath);
                return false;
            }

            TArray<UMovieSceneTrack*> Tracks;
            CollectTracksForWidget(Animation, BoundWidget, Tracks);
            if (Tracks.Num() == 0)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' has no track bound to '%s'"), *Context.AssetPath, *AnimationName, *BoundWidget);
                return false;
            }

            FString PropertyPath;
            if (Desc->TryGetStringField(TEXT("PropertyPath"), PropertyPath))
            {
                Tracks.RemoveAll([&PropertyPath](const UMovieSceneTrack* Track)
                {
                    const UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(Track);
                    return !PropertyTrack || PropertyTrack->GetPropertyPath().ToString() != PropertyPath;
                });
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
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' on '%s' matches %d tracks, name one with PropertyPath or TrackName. Candidates: %s"), *Context.AssetPath, *AnimationName, *BoundWidget, Tracks.Num(), *DescribeTracks(Tracks));
                return false;
            }

            UMovieScene* MovieScene = Animation->GetMovieScene();
            MovieScene->Modify();

            const FString RemovedName = Tracks[0]->GetDisplayName().ToString();
            if (!MovieScene->RemoveTrack(*Tracks[0]))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: movie scene refused to remove track '%s'"), *Context.AssetPath, *RemovedName);
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("    deleted track '%s' on '%s'"), *RemovedName, *BoundWidget);
            return true;
        }

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

            FString PropertyPath;
            if (Desc->TryGetStringField(TEXT("PropertyPath"), PropertyPath))
            {
                Tracks.RemoveAll([&PropertyPath](const UMovieSceneTrack* Track)
                {
                    const UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(Track);
                    return !PropertyTrack || PropertyTrack->GetPropertyPath().ToString() != PropertyPath;
                });
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
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' on '%s' matches %d tracks, name one with PropertyPath or TrackName. Candidates: %s"), *Context.AssetPath, *AnimationName, *BoundWidget, Tracks.Num(), *DescribeTracks(Tracks));
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
