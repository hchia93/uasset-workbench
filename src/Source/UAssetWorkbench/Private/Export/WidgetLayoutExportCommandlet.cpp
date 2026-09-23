#include "Export/WidgetLayoutExportCommandlet.h"
#include "Export/EdGraphJsonSerializer.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

#include "Animation/WidgetAnimation.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Channels/MovieSceneChannelEditorData.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Curves/RealCurve.h"
#include "MovieScene.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "WidgetBlueprint.h"

namespace
{
    FString InterpModeToString(ERichCurveInterpMode Mode)
    {
        switch (Mode)
        {
            case RCIM_Linear:   return TEXT("Linear");
            case RCIM_Constant: return TEXT("Constant");
            case RCIM_Cubic:    return TEXT("Cubic");
            default:            return TEXT("None");
        }
    }

    // Emit each channel tagged with its proxy meta name (e.g. "Angle") plus per-key time/value/interp.
    // Widget animations store float channels, other assets may use double, so this is templated over both.
    template <typename ChannelType, typename ValueType>
    void ExportNamedChannels(const FMovieSceneChannelProxy& Proxy, FFrameRate TickResolution, TArray<TSharedPtr<FJsonValue>>& OutChannels)
    {
        TArrayView<ChannelType*> Channels = Proxy.GetChannels<ChannelType>();
        TArrayView<const FMovieSceneChannelMetaData> MetaData = Proxy.GetMetaData<ChannelType>();

        for (int32 ChannelIndex = 0; ChannelIndex < Channels.Num(); ChannelIndex++)
        {
            ChannelType* Channel = Channels[ChannelIndex];
            if (!Channel)
            {
                continue;
            }

            TSharedPtr<FJsonObject> ChannelObj = MakeShared<FJsonObject>();
            if (MetaData.IsValidIndex(ChannelIndex))
            {
                ChannelObj->SetStringField(TEXT("Name"), MetaData[ChannelIndex].Name.ToString());
            }

            TMovieSceneChannelData<ValueType> ChannelData = Channel->GetData();
            TArrayView<const FFrameNumber> Times = ChannelData.GetTimes();
            TArrayView<const ValueType> Values = ChannelData.GetValues();

            TArray<TSharedPtr<FJsonValue>> KeysArray;
            for (int32 KeyIndex = 0; KeyIndex < Times.Num(); KeyIndex++)
            {
                TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
                KeyObj->SetNumberField(TEXT("Time"), TickResolution.AsSeconds(Times[KeyIndex]));
                KeyObj->SetNumberField(TEXT("Value"), Values[KeyIndex].Value);
                KeyObj->SetStringField(TEXT("Interp"), InterpModeToString(Values[KeyIndex].InterpMode));
                KeysArray.Add(MakeShared<FJsonValueObject>(KeyObj));
            }
            ChannelObj->SetArrayField(TEXT("Keys"), KeysArray);

            OutChannels.Add(MakeShared<FJsonValueObject>(ChannelObj));
        }
    }
}

UWidgetLayoutExportCommandlet::UWidgetLayoutExportCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UWidgetLayoutExportCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("UAssetWorkbench v%s - WidgetLayoutExport"), UASSET_WORKBENCH_VERSION_STRING);

    TArray<FString> AssetPaths = UAssetWorkbench::ParseAssetPaths(Params);

    if (AssetPaths.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchExporter, Error, TEXT("No assets specified. Usage: -assets=\"/Game/Path/WBP_A,/Game/Path/WBP_B\""));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    int32 ExportedCount = 0;

    for (const FString& AssetPath : AssetPaths)
    {
        UWidgetBlueprint* WidgetBP = LoadObject<UWidgetBlueprint>(nullptr, *AssetPath);
        if (!WidgetBP)
        {
            UE_LOG(LogUAssetWorkbenchExporter, Warning, TEXT("Failed to load WidgetBlueprint: %s"), *AssetPath);
            continue;
        }

        TSharedPtr<FJsonObject> JsonObject = ExportWidgetBlueprint(WidgetBP);
        if (!JsonObject.IsValid())
        {
            UE_LOG(LogUAssetWorkbenchExporter, Warning, TEXT("Failed to export WidgetBlueprint: %s"), *AssetPath);
            continue;
        }

        UAssetWorkbench::FExportTarget ExportTarget(AssetPath);
        if (ExportTarget.Save(JsonObject.ToSharedRef()))
        {
            UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Exported: %s -> %s"), *AssetPath, *ExportTarget.GetPath());
            ExportedCount++;
        }
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Export complete. %d/%d widgets exported."), ExportedCount, AssetPaths.Num());
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}

TSharedPtr<FJsonObject> UWidgetLayoutExportCommandlet::ExportWidgetBlueprint(UWidgetBlueprint* WidgetBP) const
{
    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

    Root->SetStringField(TEXT("ExporterVersion"), UASSET_WORKBENCH_VERSION_STRING);
    Root->SetStringField(TEXT("ExportType"), TEXT("WidgetLayout"));
    Root->SetStringField(TEXT("WidgetBlueprint"), WidgetBP->GetName());
    Root->SetStringField(TEXT("AssetPath"), WidgetBP->GetPathName());
    Root->SetStringField(TEXT("ExportTimestamp"), FDateTime::Now().ToString());

    if (WidgetBP->ParentClass)
    {
        Root->SetStringField(TEXT("ParentClass"), WidgetBP->ParentClass->GetName());
    }

    // WidgetTree
    if (WidgetBP->WidgetTree && WidgetBP->WidgetTree->RootWidget)
    {
        Root->SetObjectField(TEXT("WidgetTree"), ExportWidget(WidgetBP->WidgetTree->RootWidget));
    }

    // Widget Animations
    TArray<TSharedPtr<FJsonValue>> AnimationsArray;
    for (UWidgetAnimation* Anim : WidgetBP->Animations)
    {
        if (Anim)
        {
            TSharedPtr<FJsonObject> AnimObj = ExportAnimation(Anim);
            if (AnimObj.IsValid())
            {
                AnimationsArray.Add(MakeShared<FJsonValueObject>(AnimObj));
            }
        }
    }
    Root->SetArrayField(TEXT("Animations"), AnimationsArray);

    // EdGraph (EventGraphs + FunctionGraphs)
    FEdGraphJsonOptions GraphOptions;
    GraphOptions.bRecurseSubGraphs = true;
    FEdGraphJsonSerializer Serializer(GraphOptions);

    TArray<TSharedPtr<FJsonValue>> GraphsArray;

    for (UEdGraph* Graph : WidgetBP->UbergraphPages)
    {
        TSharedPtr<FJsonObject> GraphObj = Serializer.ExportGraph(Graph, TEXT("EventGraph"));
        if (GraphObj.IsValid())
        {
            GraphsArray.Add(MakeShared<FJsonValueObject>(GraphObj));
        }
    }

    for (UEdGraph* Graph : WidgetBP->FunctionGraphs)
    {
        TSharedPtr<FJsonObject> GraphObj = Serializer.ExportGraph(Graph, TEXT("Function"));
        if (GraphObj.IsValid())
        {
            GraphsArray.Add(MakeShared<FJsonValueObject>(GraphObj));
        }
    }

    Root->SetArrayField(TEXT("Graphs"), GraphsArray);

    return Root;
}

TSharedPtr<FJsonObject> UWidgetLayoutExportCommandlet::ExportWidget(UWidget* Widget) const
{
    if (!Widget)
    {
        return nullptr;
    }

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

    Obj->SetStringField(TEXT("Name"), Widget->GetName());
    Obj->SetStringField(TEXT("Class"), Widget->GetClass()->GetName());
    Obj->SetStringField(TEXT("Visibility"), StaticEnum<ESlateVisibility>()->GetNameStringByValue(static_cast<int64>(Widget->GetVisibility())));
    Obj->SetNumberField(TEXT("RenderOpacity"), Widget->GetRenderOpacity());

    if (!Widget->GetIsEnabled())
    {
        Obj->SetBoolField(TEXT("IsEnabled"), false);
    }

    // Subclass-specific properties (properties declared below UWidget)
    TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
    UClass* CurrentClass = Widget->GetClass();
    while (CurrentClass && CurrentClass != UWidget::StaticClass())
    {
        for (TFieldIterator<FProperty> PropIt(CurrentClass, EFieldIteratorFlags::ExcludeSuper); PropIt; ++PropIt)
        {
            FProperty* Prop = *PropIt;
            if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
            {
                continue;
            }

            FString Value;
            Prop->ExportTextItem_Direct(Value, Prop->ContainerPtrToValuePtr<void>(Widget), nullptr, Widget, PPF_None);
            if (!Value.IsEmpty())
            {
                Props->SetStringField(Prop->GetName(), Value);
            }
        }
        CurrentClass = CurrentClass->GetSuperClass();
    }
    if (Props->Values.Num() > 0)
    {
        Obj->SetObjectField(TEXT("Properties"), Props);
    }

    // Slot (layout properties from parent panel)
    if (Widget->Slot)
    {
        Obj->SetObjectField(TEXT("Slot"), ExportSlotProperties(Widget->Slot));
    }

    // Children (recursive)
    if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
    {
        TArray<TSharedPtr<FJsonValue>> ChildrenArray;
        for (int32 i = 0; i < Panel->GetChildrenCount(); i++)
        {
            UWidget* Child = Panel->GetChildAt(i);
            TSharedPtr<FJsonObject> ChildObj = ExportWidget(Child);
            if (ChildObj.IsValid())
            {
                ChildrenArray.Add(MakeShared<FJsonValueObject>(ChildObj));
            }
        }
        Obj->SetArrayField(TEXT("Children"), ChildrenArray);
    }

    return Obj;
}

TSharedPtr<FJsonObject> UWidgetLayoutExportCommandlet::ExportSlotProperties(UPanelSlot* Slot) const
{
    TSharedPtr<FJsonObject> SlotObj = MakeShared<FJsonObject>();

    SlotObj->SetStringField(TEXT("SlotClass"), Slot->GetClass()->GetName());

    for (TFieldIterator<FProperty> PropIt(Slot->GetClass()); PropIt; ++PropIt)
    {
        FProperty* Prop = *PropIt;

        // Skip UObject/UVisual base properties
        if (Prop->GetOwnerClass() == UObject::StaticClass() || Prop->GetOwnerClass() == UPanelSlot::StaticClass())
        {
            continue;
        }

        if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
        {
            continue;
        }

        FString Value;
        Prop->ExportTextItem_Direct(Value, Prop->ContainerPtrToValuePtr<void>(Slot), nullptr, Slot, PPF_None);
        if (!Value.IsEmpty())
        {
            SlotObj->SetStringField(Prop->GetName(), Value);
        }
    }

    return SlotObj;
}

TSharedPtr<FJsonObject> UWidgetLayoutExportCommandlet::ExportAnimation(UWidgetAnimation* Animation) const
{
    TSharedPtr<FJsonObject> AnimObj = MakeShared<FJsonObject>();

    AnimObj->SetStringField(TEXT("Name"), Animation->GetDisplayName().ToString());

    UMovieScene* MovieScene = Animation->GetMovieScene();
    if (!MovieScene)
    {
        return AnimObj;
    }

    // Animation time range
    FFrameRate TickResolution = MovieScene->GetTickResolution();
    FFrameRate DisplayRate = MovieScene->GetDisplayRate();
    TRange<FFrameNumber> PlaybackRange = MovieScene->GetPlaybackRange();

    if (PlaybackRange.HasLowerBound() && PlaybackRange.HasUpperBound())
    {
        double StartSeconds = TickResolution.AsSeconds(PlaybackRange.GetLowerBoundValue());
        double EndSeconds = TickResolution.AsSeconds(PlaybackRange.GetUpperBoundValue());
        AnimObj->SetNumberField(TEXT("StartTime"), StartSeconds);
        AnimObj->SetNumberField(TEXT("EndTime"), EndSeconds);
        AnimObj->SetNumberField(TEXT("Duration"), EndSeconds - StartSeconds);
    }

    AnimObj->SetStringField(TEXT("DisplayRate"), DisplayRate.ToPrettyText().ToString());
    AnimObj->SetStringField(TEXT("TickResolution"), TickResolution.ToPrettyText().ToString());

    // Build binding GUID -> widget name map
    TMap<FGuid, FString> BindingNameMap;
    TArray<TSharedPtr<FJsonValue>> BindingsArray;
    for (const FWidgetAnimationBinding& Binding : Animation->AnimationBindings)
    {
        BindingNameMap.Add(Binding.AnimationGuid, Binding.WidgetName.ToString());

        TSharedPtr<FJsonObject> BindingObj = MakeShared<FJsonObject>();
        BindingObj->SetStringField(TEXT("Widget"), Binding.WidgetName.ToString());
        BindingObj->SetStringField(TEXT("Guid"), Binding.AnimationGuid.ToString());
        BindingObj->SetBoolField(TEXT("IsRootWidget"), Binding.bIsRootWidget);
        // Set on a slot binding only. The animated object is then the slot, reached through the widget it holds.
        if (!Binding.SlotWidgetName.IsNone())
        {
            BindingObj->SetStringField(TEXT("SlotWidget"), Binding.SlotWidgetName.ToString());
        }
        BindingsArray.Add(MakeShared<FJsonValueObject>(BindingObj));
    }
    AnimObj->SetArrayField(TEXT("Bindings"), BindingsArray);

    // Object binding tracks (tracks bound to specific widgets)
    TArray<TSharedPtr<FJsonValue>> TracksArray;

    const UMovieScene* ConstMovieScene = MovieScene;
    const TArray<FMovieSceneBinding>& Bindings = ConstMovieScene->GetBindings();
    for (const FMovieSceneBinding& Binding : Bindings)
    {
        FString WidgetName = TEXT("Unknown");
        if (const FString* Found = BindingNameMap.Find(Binding.GetObjectGuid()))
        {
            WidgetName = *Found;
        }

        for (UMovieSceneTrack* Track : Binding.GetTracks())
        {
            if (!Track)
            {
                continue;
            }

            TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
            TrackObj->SetStringField(TEXT("BoundWidget"), WidgetName);
            TrackObj->SetStringField(TEXT("TrackType"), Track->GetClass()->GetPathName());
            TrackObj->SetStringField(TEXT("TrackName"), Track->GetDisplayName().ToString());

            // PropertyPath is what AddTrack needs back, the track class follows from the property type.
            if (const UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(Track))
            {
                TrackObj->SetStringField(TEXT("PropertyName"), PropertyTrack->GetPropertyName().ToString());
                TrackObj->SetStringField(TEXT("PropertyPath"), PropertyTrack->GetPropertyPath().ToString());
            }

            TArray<TSharedPtr<FJsonValue>> SectionsArray;

            for (UMovieSceneSection* Section : Track->GetAllSections())
            {
                if (!Section)
                {
                    continue;
                }

                TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
                SectionObj->SetNumberField(TEXT("RowIndex"), Section->GetRowIndex());

                // Section time range
                TRange<FFrameNumber> SectionRange = Section->GetRange();
                if (SectionRange.HasLowerBound())
                {
                    SectionObj->SetNumberField(TEXT("StartTime"), TickResolution.AsSeconds(SectionRange.GetLowerBoundValue()));
                }
                if (SectionRange.HasUpperBound())
                {
                    SectionObj->SetNumberField(TEXT("EndTime"), TickResolution.AsSeconds(SectionRange.GetUpperBoundValue()));
                }

                // Keyframes from channels
                TArray<TSharedPtr<FJsonValue>> ChannelsArray;

                FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();

                // Widget animations store float channels (transform, render opacity), other assets may use double.
                ExportNamedChannels<FMovieSceneFloatChannel, FMovieSceneFloatValue>(Proxy, TickResolution, ChannelsArray);
                ExportNamedChannels<FMovieSceneDoubleChannel, FMovieSceneDoubleValue>(Proxy, TickResolution, ChannelsArray);

                if (ChannelsArray.Num() > 0)
                {
                    SectionObj->SetArrayField(TEXT("Channels"), ChannelsArray);
                }

                SectionsArray.Add(MakeShared<FJsonValueObject>(SectionObj));
            }

            TrackObj->SetArrayField(TEXT("Sections"), SectionsArray);
            TracksArray.Add(MakeShared<FJsonValueObject>(TrackObj));
        }
    }

    AnimObj->SetArrayField(TEXT("Tracks"), TracksArray);

    return AnimObj;
}

