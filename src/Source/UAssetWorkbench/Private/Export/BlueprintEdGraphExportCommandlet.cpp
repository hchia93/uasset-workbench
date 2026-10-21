#include "Export/BlueprintEdGraphExportCommandlet.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/ActorComponent.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveLinearColor.h"
#include "Curves/CurveVector.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Engine/TimelineTemplate.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UObjectIterator.h"

#include "Export/EdGraphJsonSerializer.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

namespace
{
    const TCHAR* ContainerTypeName(EPinContainerType ContainerType)
    {
        switch (ContainerType)
        {
        case EPinContainerType::Array:
            return TEXT("Array");
        case EPinContainerType::Set:
            return TEXT("Set");
        case EPinContainerType::Map:
            return TEXT("Map");
        default:
            return TEXT("None");
        }
    }

    const TCHAR* BlueprintTypeName(EBlueprintType Type)
    {
        switch (Type)
        {
        case BPTYPE_Const:
            return TEXT("Const");
        case BPTYPE_MacroLibrary:
            return TEXT("MacroLibrary");
        case BPTYPE_Interface:
            return TEXT("Interface");
        case BPTYPE_LevelScript:
            return TEXT("LevelScript");
        case BPTYPE_FunctionLibrary:
            return TEXT("FunctionLibrary");
        default:
            return TEXT("Normal");
        }
    }

    const TCHAR* TimelineLengthModeName(ETimelineLengthMode LengthMode)
    {
        return LengthMode == TL_LastKeyFrame ? TEXT("LastKeyFrame") : TEXT("TimelineLength");
    }

    // A Blueprint package yields both the Blueprint and its generated class, registry order between them is not stable.
    FString PrimaryAssetClassName(const TArray<FAssetData>& AssetDataList)
    {
        for (const FAssetData& AssetData : AssetDataList)
        {
            const FString ClassName = AssetData.AssetClassPath.GetAssetName().ToString();
            if (!ClassName.EndsWith(TEXT("BlueprintGeneratedClass")))
            {
                return ClassName;
            }
        }

        return AssetDataList[0].AssetClassPath.GetAssetName().ToString();
    }

    TSharedPtr<FJsonValue> MakeTimelineTrackEntry(FName TrackName, const UObject* Curve)
    {
        TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
        TrackObj->SetStringField(TEXT("Name"), TrackName.ToString());

        if (Curve)
        {
            TrackObj->SetStringField(TEXT("Curve"), Curve->GetPathName());
        }

        return MakeShared<FJsonValueObject>(TrackObj);
    }

    // Keys print in the shape EditBlueprint's Timelines section takes back.
    TSharedPtr<FJsonValue> MakeTimelineFloatTrackEntry(const FTTFloatTrack& Track)
    {
        TSharedPtr<FJsonValue> Entry = MakeTimelineTrackEntry(Track.GetTrackName(), Track.CurveFloat);
        const TSharedPtr<FJsonObject>& TrackObj = Entry->AsObject();
        TrackObj->SetBoolField(TEXT("External"), Track.bIsExternalCurve);
        if (!Track.CurveFloat)
        {
            return Entry;
        }

        TArray<TSharedPtr<FJsonValue>> KeysArray;
        for (const FRichCurveKey& Key : Track.CurveFloat->FloatCurve.GetConstRefOfKeys())
        {
            TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
            KeyObj->SetNumberField(TEXT("Time"), Key.Time);
            KeyObj->SetNumberField(TEXT("Value"), Key.Value);
            KeyObj->SetStringField(TEXT("InterpMode"), StaticEnum<ERichCurveInterpMode>()->GetNameStringByValue(static_cast<int64>(Key.InterpMode.GetValue())));
            KeysArray.Add(MakeShared<FJsonValueObject>(KeyObj));
        }
        TrackObj->SetArrayField(TEXT("Keys"), KeysArray);

        return Entry;
    }

    // Editor-side variable metadata lives on NewVariables, the generated FProperty only carries the compiled result.
    void AppendVariableMetadata(const FBPVariableDescription& Description, const TSharedPtr<FJsonObject>& VarObj)
    {
        if (!Description.Category.IsEmpty())
        {
            VarObj->SetStringField(TEXT("Category"), Description.Category.ToString());
        }

        if (Description.HasMetaData(TEXT("tooltip")))
        {
            const FString& Tooltip = Description.GetMetaData(TEXT("tooltip"));
            if (!Tooltip.IsEmpty())
            {
                VarObj->SetStringField(TEXT("Tooltip"), Tooltip);
            }
        }

        VarObj->SetStringField(TEXT("PinType"), Description.VarType.PinCategory.ToString());

        if (Description.VarType.PinSubCategoryObject.IsValid())
        {
            VarObj->SetStringField(TEXT("SubType"), Description.VarType.PinSubCategoryObject->GetName());
        }

        VarObj->SetStringField(TEXT("Container"), ContainerTypeName(Description.VarType.ContainerType));

        if (Description.VarType.ContainerType == EPinContainerType::Map)
        {
            VarObj->SetStringField(TEXT("ValueType"), Description.VarType.PinValueType.TerminalCategory.ToString());
        }

        const uint64 Flags = Description.PropertyFlags;

        if (!(Flags & CPF_DisableEditOnInstance))
        {
            VarObj->SetBoolField(TEXT("InstanceEditable"), true);
        }
        if (Flags & CPF_BlueprintReadOnly)
        {
            VarObj->SetBoolField(TEXT("ReadOnly"), true);
        }
        if (Description.HasMetaData(FBlueprintMetadata::MD_ExposeOnSpawn) && Description.GetMetaData(FBlueprintMetadata::MD_ExposeOnSpawn) == TEXT("true"))
        {
            VarObj->SetBoolField(TEXT("ExposeOnSpawn"), true);
        }
        if (Description.HasMetaData(FBlueprintMetadata::MD_Private))
        {
            VarObj->SetBoolField(TEXT("Private"), true);
        }
        if (Flags & CPF_Transient)
        {
            VarObj->SetBoolField(TEXT("Transient"), true);
        }
        if (Flags & CPF_SaveGame)
        {
            VarObj->SetBoolField(TEXT("SaveGame"), true);
        }
        if (Flags & CPF_Config)
        {
            VarObj->SetBoolField(TEXT("Config"), true);
        }
        if (Flags & CPF_Net)
        {
            VarObj->SetBoolField(TEXT("Replicated"), true);
        }
        if (Flags & CPF_RepNotify)
        {
            VarObj->SetBoolField(TEXT("RepNotify"), true);
            if (!Description.RepNotifyFunc.IsNone())
            {
                VarObj->SetStringField(TEXT("RepNotifyFunc"), Description.RepNotifyFunc.ToString());
            }
        }
        if (Flags & CPF_AdvancedDisplay)
        {
            VarObj->SetBoolField(TEXT("AdvancedDisplay"), true);
        }
        if (Flags & CPF_Interp)
        {
            VarObj->SetBoolField(TEXT("ExposeToCinematics"), true);
        }

        // Stays put across renames, external tooling addresses variables by it.
        VarObj->SetStringField(TEXT("Guid"), Description.VarGuid.ToString());
    }
}

UBlueprintEdGraphExportCommandlet::UBlueprintEdGraphExportCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UBlueprintEdGraphExportCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("UAssetWorkbench v%s - BlueprintEdGraphExport"), UASSET_WORKBENCH_VERSION_STRING);

    TArray<FString> AssetPaths = UAssetWorkbench::ParseAssetPaths(Params);
    FExportOptions Options = ParseExportOptions(Params);

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Options: IncludeGraphs=%s"),
        Options.bIncludeGraphs ? TEXT("true") : TEXT("false"));

    if (AssetPaths.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchExporter, Error, TEXT("No assets specified. Usage: -assets=\"/Game/Path/BP_A,/Game/Path/BP_B\""));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    int32 ExportedCount = 0;

    for (const FString& AssetPath : AssetPaths)
    {
        UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
        if (!Blueprint)
        {
            UE_LOG(LogUAssetWorkbenchExporter, Warning, TEXT("Failed to load Blueprint: %s"), *AssetPath);
            continue;
        }

        TSharedPtr<FJsonObject> JsonObject = ExportBlueprint(Blueprint, Options);
        if (!JsonObject.IsValid())
        {
            UE_LOG(LogUAssetWorkbenchExporter, Warning, TEXT("Failed to export Blueprint: %s"), *AssetPath);
            continue;
        }

        UAssetWorkbench::FExportTarget ExportTarget(AssetPath);
        if (ExportTarget.Save(JsonObject.ToSharedRef()))
        {
            UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Exported: %s -> %s"), *AssetPath, *ExportTarget.GetPath());
            ExportedCount++;
        }
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Export complete. %d/%d blueprints exported."), ExportedCount, AssetPaths.Num());
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}

TSharedPtr<FJsonObject> UBlueprintEdGraphExportCommandlet::ExportBlueprint(UBlueprint* Blueprint, const FExportOptions& Options) const
{
    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

    Root->SetStringField(TEXT("ExporterVersion"), UASSET_WORKBENCH_VERSION_STRING);
    Root->SetStringField(TEXT("ExportType"), TEXT("BlueprintEdGraph"));
    Root->SetStringField(TEXT("Blueprint"), Blueprint->GetName());
    Root->SetStringField(TEXT("AssetPath"), Blueprint->GetPathName());
    Root->SetStringField(TEXT("ExportTimestamp"), FDateTime::Now().ToString());

    Root->SetStringField(TEXT("BlueprintType"), BlueprintTypeName(Blueprint->BlueprintType));

    // Parent class
    if (Blueprint->ParentClass)
    {
        Root->SetStringField(TEXT("ParentClass"), Blueprint->ParentClass->GetName());
        Root->SetStringField(TEXT("ParentClassPath"), Blueprint->ParentClass->GetPathName());
    }

    if (Blueprint->GeneratedClass)
    {
        Root->SetStringField(TEXT("GeneratedClassPath"), Blueprint->GeneratedClass->GetPathName());
    }

    // Implemented interfaces (BP "Implements Interface" list). Answers "who implements interface X"
    // directly, without -graphs node scraping for the overriding event titles.
    TArray<TSharedPtr<FJsonValue>> InterfacesArray;
    for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
    {
        UClass* InterfaceClass = InterfaceDesc.Interface.Get();
        if (!InterfaceClass)
        {
            continue;
        }

        TSharedPtr<FJsonObject> IfaceObj = MakeShared<FJsonObject>();
        IfaceObj->SetStringField(TEXT("Name"), InterfaceClass->GetName());
        IfaceObj->SetStringField(TEXT("Path"), InterfaceClass->GetPathName());
        InterfacesArray.Add(MakeShared<FJsonValueObject>(IfaceObj));
    }
    Root->SetArrayField(TEXT("ImplementedInterfaces"), InterfacesArray);
    Root->SetNumberField(TEXT("ImplementedInterfaceCount"), InterfacesArray.Num());

    // Build a set of SCS component variable names so we can mark auto-generated variables.
    // Each SCS component registers a property of the same name as the variable on the generated class.
    TSet<FString> SCSComponentNames;
    if (Blueprint->SimpleConstructionScript)
    {
        for (USCS_Node* SCSNode : Blueprint->SimpleConstructionScript->GetAllNodes())
        {
            if (SCSNode)
            {
                SCSComponentNames.Add(SCSNode->GetVariableName().ToString());
            }
        }
    }

    // Variables (member properties on generated class)
    TMap<FName, const FBPVariableDescription*> VariableDescriptions;
    for (const FBPVariableDescription& Description : Blueprint->NewVariables)
    {
        VariableDescriptions.Add(Description.VarName, &Description);
    }

    TArray<TSharedPtr<FJsonValue>> VariablesArray;
    int32 UserVariableCount = 0;
    if (UClass* GeneratedClass = Blueprint->GeneratedClass)
    {
        UObject* CDO = GeneratedClass->GetDefaultObject();
        for (TFieldIterator<FProperty> PropIt(GeneratedClass, EFieldIteratorFlags::ExcludeSuper); PropIt; ++PropIt)
        {
            FProperty* Property = *PropIt;
            TSharedPtr<FJsonObject> VarObj = MakeShared<FJsonObject>();
            const FString PropName = Property->GetName();
            VarObj->SetStringField(TEXT("Name"), PropName);
            VarObj->SetStringField(TEXT("Type"), Property->GetCPPType());

            const bool bIsAutoGen = SCSComponentNames.Contains(PropName);
            VarObj->SetBoolField(TEXT("IsAutoGenerated"), bIsAutoGen);
            if (!bIsAutoGen)
            {
                ++UserVariableCount;
            }

            if (CDO)
            {
                FString DefaultValue;
                Property->ExportTextItem_Direct(DefaultValue, Property->ContainerPtrToValuePtr<void>(CDO), nullptr, CDO, PPF_None);
                if (!DefaultValue.IsEmpty())
                {
                    VarObj->SetStringField(TEXT("Default"), DefaultValue);
                }
            }

            if (const FBPVariableDescription** Description = VariableDescriptions.Find(Property->GetFName()))
            {
                AppendVariableMetadata(**Description, VarObj);
            }

            VariablesArray.Add(MakeShared<FJsonValueObject>(VarObj));
        }
    }
    Root->SetArrayField(TEXT("Variables"), VariablesArray);
    Root->SetNumberField(TEXT("VariableCount"), VariablesArray.Num());
    Root->SetNumberField(TEXT("UserVariableCount"), UserVariableCount);

    // Event dispatchers, addressable by name with their signature. They also stay in Variables[] as delegate properties.
    TArray<TSharedPtr<FJsonValue>> DispatchersArray;
    for (const FBPVariableDescription& Description : Blueprint->NewVariables)
    {
        const bool bIsMulticast = Description.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate;
        const bool bIsSingleCast = Description.VarType.PinCategory == UEdGraphSchema_K2::PC_Delegate;
        if (!bIsMulticast && !bIsSingleCast)
        {
            continue;
        }

        TSharedPtr<FJsonObject> DispatcherObj = MakeShared<FJsonObject>();
        DispatcherObj->SetStringField(TEXT("Name"), Description.VarName.ToString());

        if (!Description.Category.IsEmpty())
        {
            DispatcherObj->SetStringField(TEXT("Category"), Description.Category.ToString());
        }

        for (UEdGraph* SignatureGraph : Blueprint->DelegateSignatureGraphs)
        {
            if (!SignatureGraph || SignatureGraph->GetFName() != Description.VarName)
            {
                continue;
            }

            TSharedPtr<FJsonObject> Signature = EdGraphJson::ExportFunctionSignature(SignatureGraph, Blueprint, false);
            if (Signature.IsValid())
            {
                DispatcherObj->SetObjectField(TEXT("Signature"), Signature);
            }
            break;
        }

        DispatchersArray.Add(MakeShared<FJsonValueObject>(DispatcherObj));
    }
    Root->SetArrayField(TEXT("EventDispatchers"), DispatchersArray);
    Root->SetNumberField(TEXT("EventDispatcherCount"), DispatchersArray.Num());

    // Timelines. The generated class only exposes an opaque component property plus a __Direction_ enum, tracks live here.
    TArray<TSharedPtr<FJsonValue>> TimelinesArray;
    for (const UTimelineTemplate* Timeline : Blueprint->Timelines)
    {
        if (!Timeline)
        {
            continue;
        }

        TSharedPtr<FJsonObject> TimelineObj = MakeShared<FJsonObject>();
        TimelineObj->SetStringField(TEXT("Name"), Timeline->GetVariableName().ToString());
        TimelineObj->SetNumberField(TEXT("Length"), Timeline->TimelineLength);
        TimelineObj->SetStringField(TEXT("LengthMode"), TimelineLengthModeName(Timeline->LengthMode));
        TimelineObj->SetBoolField(TEXT("Loop"), Timeline->bLoop != 0);
        TimelineObj->SetBoolField(TEXT("AutoPlay"), Timeline->bAutoPlay != 0);
        TimelineObj->SetBoolField(TEXT("Replicated"), Timeline->bReplicated != 0);
        TimelineObj->SetBoolField(TEXT("IgnoreTimeDilation"), Timeline->bIgnoreTimeDilation != 0);

        TArray<TSharedPtr<FJsonValue>> FloatTracksArray;
        for (const FTTFloatTrack& Track : Timeline->FloatTracks)
        {
            FloatTracksArray.Add(MakeTimelineFloatTrackEntry(Track));
        }
        if (FloatTracksArray.Num() > 0)
        {
            TimelineObj->SetArrayField(TEXT("FloatTracks"), FloatTracksArray);
        }

        TArray<TSharedPtr<FJsonValue>> VectorTracksArray;
        for (const FTTVectorTrack& Track : Timeline->VectorTracks)
        {
            VectorTracksArray.Add(MakeTimelineTrackEntry(Track.GetTrackName(), Track.CurveVector));
        }
        if (VectorTracksArray.Num() > 0)
        {
            TimelineObj->SetArrayField(TEXT("VectorTracks"), VectorTracksArray);
        }

        TArray<TSharedPtr<FJsonValue>> LinearColorTracksArray;
        for (const FTTLinearColorTrack& Track : Timeline->LinearColorTracks)
        {
            LinearColorTracksArray.Add(MakeTimelineTrackEntry(Track.GetTrackName(), Track.CurveLinearColor));
        }
        if (LinearColorTracksArray.Num() > 0)
        {
            TimelineObj->SetArrayField(TEXT("LinearColorTracks"), LinearColorTracksArray);
        }

        TArray<TSharedPtr<FJsonValue>> EventTracksArray;
        for (const FTTEventTrack& Track : Timeline->EventTracks)
        {
            EventTracksArray.Add(MakeTimelineTrackEntry(Track.GetTrackName(), Track.CurveKeys));
        }
        if (EventTracksArray.Num() > 0)
        {
            TimelineObj->SetArrayField(TEXT("EventTracks"), EventTracksArray);
        }

        TimelinesArray.Add(MakeShared<FJsonValueObject>(TimelineObj));
    }
    Root->SetArrayField(TEXT("Timelines"), TimelinesArray);
    Root->SetNumberField(TEXT("TimelineCount"), TimelinesArray.Num());

    // ParentName / IsRoot ride along so a consumer identifies the root without re-traversing the tree.
    TArray<TSharedPtr<FJsonValue>> ComponentsArray;
    FString RootComponentName;
    FString RootComponentClass;
    int32 NonEditorComponentCount = 0;

    if (Blueprint->SimpleConstructionScript)
    {
        const TArray<USCS_Node*>& RootNodes = Blueprint->SimpleConstructionScript->GetRootNodes();

        // Build parent lookup map by walking the tree from roots
        TMap<USCS_Node*, USCS_Node*> ParentMap;
        TArray<USCS_Node*> Stack = RootNodes;
        TSet<USCS_Node*> Visited;
        while (Stack.Num() > 0)
        {
            USCS_Node* Cur = Stack.Pop();
            if (!Cur || Visited.Contains(Cur))
            {
                continue;
            }
            Visited.Add(Cur);
            for (USCS_Node* Child : Cur->GetChildNodes())
            {
                if (Child)
                {
                    ParentMap.Add(Child, Cur);
                    Stack.Push(Child);
                }
            }
        }

        for (USCS_Node* SCSNode : Blueprint->SimpleConstructionScript->GetAllNodes())
        {
            if (!SCSNode || !SCSNode->ComponentTemplate)
            {
                continue;
            }

            TSharedPtr<FJsonObject> CompObj = MakeShared<FJsonObject>();
            const FString CompName = SCSNode->GetVariableName().ToString();
            const FString CompClass = SCSNode->ComponentTemplate->GetClass()->GetName();
            CompObj->SetStringField(TEXT("Name"), CompName);
            CompObj->SetStringField(TEXT("Class"), CompClass);

            const bool bIsRoot = RootNodes.Contains(SCSNode);
            CompObj->SetBoolField(TEXT("IsRoot"), bIsRoot);

            if (USCS_Node** ParentPtr = ParentMap.Find(SCSNode))
            {
                if (*ParentPtr)
                {
                    CompObj->SetStringField(TEXT("ParentName"), (*ParentPtr)->GetVariableName().ToString());
                }
            }

            // SCS root nodes attached to an inherited native component record the target here, not in the tree.
            if (!SCSNode->ParentComponentOrVariableName.IsNone())
            {
                CompObj->SetStringField(TEXT("ParentName"), SCSNode->ParentComponentOrVariableName.ToString());
                CompObj->SetBoolField(TEXT("ParentIsNative"), SCSNode->bIsParentComponentNative);
            }
            if (!SCSNode->AttachToName.IsNone())
            {
                CompObj->SetStringField(TEXT("AttachSocket"), SCSNode->AttachToName.ToString());
            }

            const bool bIsEditorOnly = SCSNode->ComponentTemplate->IsEditorOnly();
            CompObj->SetBoolField(TEXT("IsEditorOnly"), bIsEditorOnly);
            if (!bIsEditorOnly)
            {
                ++NonEditorComponentCount;
            }

            if (bIsRoot && RootComponentName.IsEmpty())
            {
                RootComponentName = CompName;
                RootComponentClass = CompClass;
            }

            TArray<TSharedPtr<FJsonValue>> OverridesArray;
            ExportPropertyOverrides(SCSNode->ComponentTemplate, OverridesArray);
            if (OverridesArray.Num() > 0)
            {
                CompObj->SetArrayField(TEXT("PropertyOverrides"), OverridesArray);
            }

            TArray<TSharedPtr<FJsonValue>> ResolvedArray;
            ExportResolvedProperties(SCSNode->ComponentTemplate, ResolvedArray);
            if (ResolvedArray.Num() > 0)
            {
                CompObj->SetArrayField(TEXT("ResolvedProperties"), ResolvedArray);
            }

            ComponentsArray.Add(MakeShared<FJsonValueObject>(CompObj));
        }
    }
    Root->SetArrayField(TEXT("Components"), ComponentsArray);
    Root->SetNumberField(TEXT("ComponentCount"), ComponentsArray.Num());
    Root->SetNumberField(TEXT("NonEditorComponentCount"), NonEditorComponentCount);
    if (!RootComponentName.IsEmpty())
    {
        Root->SetStringField(TEXT("RootComponentName"), RootComponentName);
        Root->SetStringField(TEXT("RootComponentClass"), RootComponentClass);
    }

    // Components[] is this Blueprint's own SCS only, but a Defaults spec addresses parent-Blueprint
    // components by these names too, so the addressable set is not readable without them.
    TArray<TSharedPtr<FJsonValue>> InheritedComponentsArray;
    for (UClass* Ancestor = Blueprint->ParentClass; Ancestor; Ancestor = Ancestor->GetSuperClass())
    {
        UBlueprintGeneratedClass* GeneratedAncestor = Cast<UBlueprintGeneratedClass>(Ancestor);
        if (!GeneratedAncestor || !GeneratedAncestor->SimpleConstructionScript)
        {
            continue;
        }

        for (USCS_Node* AncestorNode : GeneratedAncestor->SimpleConstructionScript->GetAllNodes())
        {
            if (!AncestorNode || !AncestorNode->ComponentTemplate)
            {
                continue;
            }

            TSharedPtr<FJsonObject> InheritedComp = MakeShared<FJsonObject>();
            InheritedComp->SetStringField(TEXT("Name"), AncestorNode->GetVariableName().ToString());
            InheritedComp->SetStringField(TEXT("Class"), AncestorNode->ComponentTemplate->GetClass()->GetName());
            InheritedComp->SetStringField(TEXT("ParentBlueprint"), GeneratedAncestor->GetPathName());
            InheritedComponentsArray.Add(MakeShared<FJsonValueObject>(InheritedComp));
        }
    }
    if (InheritedComponentsArray.Num() > 0)
    {
        Root->SetArrayField(TEXT("InheritedComponents"), InheritedComponentsArray);
    }

    // Inherited component property overrides, two sources: C++ default subobjects modified in the
    // Blueprint CDO, and parent-Blueprint SCS components the handler keeps an override template for.
    TArray<TSharedPtr<FJsonValue>> InheritedOverridesArray;
    if (UClass* GeneratedClass = Blueprint->GeneratedClass)
    {
        UObject* CDO = GeneratedClass->GetDefaultObject();
        UClass* ParentClass = GeneratedClass->GetSuperClass();
        UObject* ParentCDO = ParentClass ? ParentClass->GetDefaultObject() : nullptr;

        if (CDO && ParentCDO)
        {
            for (TFieldIterator<FObjectProperty> PropIt(ParentClass); PropIt; ++PropIt)
            {
                FObjectProperty* ObjProp = *PropIt;
                if (!ObjProp || ObjProp->HasAnyPropertyFlags(CPF_Transient))
                {
                    continue;
                }

                UObject* ChildSubObj = ObjProp->GetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(CDO));
                UObject* ParentSubObj = ObjProp->GetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(ParentCDO));

                if (!ChildSubObj || !ParentSubObj || ChildSubObj->GetClass() != ParentSubObj->GetClass())
                {
                    continue;
                }
                if (!ChildSubObj->IsDefaultSubobject())
                {
                    continue;
                }

                TArray<TSharedPtr<FJsonValue>> SubObjOverrides;
                ExportPropertyOverridesCompare(ChildSubObj, ParentSubObj, SubObjOverrides);
                if (SubObjOverrides.Num() > 0)
                {
                    TSharedPtr<FJsonObject> InheritedObj = MakeShared<FJsonObject>();
                    InheritedObj->SetStringField(TEXT("Name"), ObjProp->GetName());
                    InheritedObj->SetStringField(TEXT("Class"), ChildSubObj->GetClass()->GetName());
                    InheritedObj->SetArrayField(TEXT("PropertyOverrides"), SubObjOverrides);
                    InheritedOverridesArray.Add(MakeShared<FJsonValueObject>(InheritedObj));
                }
            }
        }
    }

    // A parent-Blueprint component is not editable in place, its override template hangs off the
    // handler rather than the CDO, so the subobject diff above never sees it.
    if (UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass))
    {
        if (UInheritableComponentHandler* Handler = GeneratedClass->GetInheritableComponentHandler())
        {
            for (TArray<FComponentOverrideRecord>::TIterator RecordIt = Handler->CreateRecordIterator(); RecordIt; ++RecordIt)
            {
                const FComponentOverrideRecord& Record = *RecordIt;
                USCS_Node* ParentNode = Record.ComponentKey.FindSCSNode();
                if (!Record.ComponentTemplate || !ParentNode || !ParentNode->ComponentTemplate)
                {
                    continue;
                }

                TArray<TSharedPtr<FJsonValue>> RecordOverrides;
                ExportPropertyOverridesCompare(Record.ComponentTemplate, ParentNode->ComponentTemplate, RecordOverrides);
                if (RecordOverrides.Num() == 0)
                {
                    continue;
                }

                TSharedPtr<FJsonObject> InheritedObj = MakeShared<FJsonObject>();
                InheritedObj->SetStringField(TEXT("Name"), Record.ComponentKey.GetSCSVariableName().ToString());
                InheritedObj->SetStringField(TEXT("Class"), Record.ComponentTemplate->GetClass()->GetName());
                InheritedObj->SetStringField(TEXT("Source"), TEXT("ParentBlueprint"));
                if (UClass* OwnerClass = Record.ComponentKey.GetComponentOwner())
                {
                    InheritedObj->SetStringField(TEXT("ParentBlueprint"), OwnerClass->GetPathName());
                }
                InheritedObj->SetArrayField(TEXT("PropertyOverrides"), RecordOverrides);
                InheritedOverridesArray.Add(MakeShared<FJsonValueObject>(InheritedObj));
            }
        }
    }

    if (InheritedOverridesArray.Num() > 0)
    {
        Root->SetArrayField(TEXT("InheritedComponentOverrides"), InheritedOverridesArray);
    }

    // Full resolved actor CDO plus the delta against the parent, so a reader sees what the author tweaked.
    if (UClass* GeneratedClass = Blueprint->GeneratedClass)
    {
        if (UObject* CDO = GeneratedClass->GetDefaultObject())
        {
            TArray<TSharedPtr<FJsonValue>> ActorResolvedArray;
            ExportResolvedProperties(CDO, ActorResolvedArray);
            if (ActorResolvedArray.Num() > 0)
            {
                Root->SetArrayField(TEXT("ActorCDOProperties"), ActorResolvedArray);
            }

            UClass* ParentClass = GeneratedClass->GetSuperClass();
            UObject* ParentCDO = ParentClass ? ParentClass->GetDefaultObject() : nullptr;
            if (ParentCDO)
            {
                TArray<TSharedPtr<FJsonValue>> ActorOverridesArray;
                ExportPropertyOverridesCompare(CDO, ParentCDO, ActorOverridesArray);
                if (ActorOverridesArray.Num() > 0)
                {
                    Root->SetArrayField(TEXT("ActorCDOOverrides"), ActorOverridesArray);
                }
            }
        }
    }

    // IntermediateGeneratedGraphs stays out, it is transient compiler output.
    FEdGraphJsonOptions GraphOptions;
    GraphOptions.bWithNodes = Options.bIncludeGraphs;
    GraphOptions.bRecurseSubGraphs = Options.bIncludeGraphs;
    FEdGraphJsonSerializer Serializer(GraphOptions);

    TArray<TSharedPtr<FJsonValue>> GraphsArray;
    int32 EventGraphNodeTotal = 0;
    int32 FunctionGraphNodeTotal = 0;
    bool bHasAnyLogic = false;

    // Signature rides outside bWithNodes, the lean shape still has to answer what a function takes and returns.
    auto AppendGraph = [&](UEdGraph* Graph, const TCHAR* GraphType, bool bWithSignature) -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> GraphObj = Serializer.ExportGraph(Graph, GraphType);
        if (!GraphObj.IsValid())
        {
            return nullptr;
        }

        if (bWithSignature)
        {
            const bool bIsInterfaceGraph = FCString::Strcmp(GraphType, TEXT("InterfaceFunction")) == 0;
            TSharedPtr<FJsonObject> Signature = EdGraphJson::ExportFunctionSignature(Graph, Blueprint, bIsInterfaceGraph);
            if (Signature.IsValid())
            {
                GraphObj->SetObjectField(TEXT("Signature"), Signature);
            }
        }

        bool bGraphLogic = false;
        GraphObj->TryGetBoolField(TEXT("HasLogic"), bGraphLogic);
        bHasAnyLogic = bHasAnyLogic || bGraphLogic;

        GraphsArray.Add(MakeShared<FJsonValueObject>(GraphObj));
        return GraphObj;
    };

    for (UEdGraph* Graph : Blueprint->UbergraphPages)
    {
        TSharedPtr<FJsonObject> GraphObj = AppendGraph(Graph, TEXT("EventGraph"), false);
        if (GraphObj.IsValid())
        {
            int32 NodeCount = 0;
            GraphObj->TryGetNumberField(TEXT("NodeCount"), NodeCount);
            EventGraphNodeTotal += NodeCount;
        }
    }

    for (UEdGraph* Graph : Blueprint->FunctionGraphs)
    {
        TSharedPtr<FJsonObject> GraphObj = AppendGraph(Graph, TEXT("Function"), true);
        if (GraphObj.IsValid())
        {
            int32 NodeCount = 0;
            GraphObj->TryGetNumberField(TEXT("NodeCount"), NodeCount);
            FunctionGraphNodeTotal += NodeCount;
        }
    }

    for (UEdGraph* Graph : Blueprint->MacroGraphs)
    {
        AppendGraph(Graph, TEXT("Macro"), true);
    }

    for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
    {
        AppendGraph(Graph, TEXT("DelegateSignature"), true);
    }

    int32 InterfaceGraphCount = 0;
    for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
    {
        UClass* InterfaceClass = InterfaceDesc.Interface.Get();
        for (UEdGraph* Graph : InterfaceDesc.Graphs)
        {
            TSharedPtr<FJsonObject> GraphObj = AppendGraph(Graph, TEXT("InterfaceFunction"), true);
            if (!GraphObj.IsValid())
            {
                continue;
            }

            if (InterfaceClass)
            {
                GraphObj->SetStringField(TEXT("Interface"), InterfaceClass->GetName());
            }
            ++InterfaceGraphCount;
        }
    }

    Root->SetArrayField(TEXT("Graphs"), GraphsArray);
    Root->SetNumberField(TEXT("EventGraphNodeTotal"), EventGraphNodeTotal);
    Root->SetNumberField(TEXT("FunctionGraphNodeTotal"), FunctionGraphNodeTotal);
    Root->SetNumberField(TEXT("FunctionGraphCount"), Blueprint->FunctionGraphs.Num());
    Root->SetNumberField(TEXT("MacroGraphCount"), Blueprint->MacroGraphs.Num());
    Root->SetNumberField(TEXT("DelegateSignatureGraphCount"), Blueprint->DelegateSignatureGraphs.Num());
    Root->SetNumberField(TEXT("InterfaceGraphCount"), InterfaceGraphCount);
    Root->SetBoolField(TEXT("HasAnyGraphLogic"), bHasAnyLogic);

    // Referenced assets via AssetRegistry
    // Split into Levels (umap) and Other to support quick "level-only references" filtering.
    TArray<TSharedPtr<FJsonValue>> LevelRefsArray;
    TArray<TSharedPtr<FJsonValue>> OtherRefsArray;
    {
        IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

        // Reverse dependencies: assets that reference this Blueprint's package
        // Default FDependencyQuery() = include hard + soft references, no flag filtering.
        TArray<FName> Referencers;
        AssetRegistry.GetReferencers(
            Blueprint->GetOutermost()->GetFName(),
            Referencers,
            UE::AssetRegistry::EDependencyCategory::Package);

        for (const FName& RefName : Referencers)
        {
            FString RefPath = RefName.ToString();

            if (RefPath.StartsWith(TEXT("/Script/")) || RefPath.StartsWith(TEXT("/Engine/")))
            {
                continue;
            }

            TSharedPtr<FJsonObject> RefObj = MakeShared<FJsonObject>();
            RefObj->SetStringField(TEXT("PackageName"), RefPath);

            FString AssetClassName;
            TArray<FAssetData> AssetDataList;
            AssetRegistry.GetAssetsByPackageName(RefName, AssetDataList, true);
            if (AssetDataList.Num() > 0)
            {
                AssetClassName = PrimaryAssetClassName(AssetDataList);
                RefObj->SetStringField(TEXT("AssetClass"), AssetClassName);
            }

            const bool bIsLevel = (AssetClassName == TEXT("World"))
                || RefPath.EndsWith(TEXT(".umap"))
                || RefPath.Contains(TEXT("/Maps/"));
            if (bIsLevel)
            {
                LevelRefsArray.Add(MakeShared<FJsonValueObject>(RefObj));
            }
            else
            {
                OtherRefsArray.Add(MakeShared<FJsonValueObject>(RefObj));
            }
        }
    }
    Root->SetArrayField(TEXT("Referencers_Levels"), LevelRefsArray);
    Root->SetArrayField(TEXT("Referencers_Other"), OtherRefsArray);
    Root->SetNumberField(TEXT("Referencers_LevelCount"), LevelRefsArray.Num());
    Root->SetNumberField(TEXT("Referencers_OtherCount"), OtherRefsArray.Num());

    // Forward dependencies: assets this Blueprint references (mesh, material, etc.)
    TArray<TSharedPtr<FJsonValue>> RefsArray;
    {
        IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        TArray<FName> Dependencies;
        AssetRegistry.GetDependencies(Blueprint->GetOutermost()->GetFName(), Dependencies);

        for (const FName& DepName : Dependencies)
        {
            FString DepPath = DepName.ToString();

            if (DepPath.StartsWith(TEXT("/Script/")) || DepPath.StartsWith(TEXT("/Engine/")))
            {
                continue;
            }

            TSharedPtr<FJsonObject> RefObj = MakeShared<FJsonObject>();
            RefObj->SetStringField(TEXT("PackageName"), DepPath);

            TArray<FAssetData> AssetDataList;
            AssetRegistry.GetAssetsByPackageName(DepName, AssetDataList, true);
            if (AssetDataList.Num() > 0)
            {
                RefObj->SetStringField(TEXT("AssetClass"), PrimaryAssetClassName(AssetDataList));
            }

            RefsArray.Add(MakeShared<FJsonValueObject>(RefObj));
        }
    }
    Root->SetArrayField(TEXT("ReferencedAssets"), RefsArray);

    return Root;
}

void UBlueprintEdGraphExportCommandlet::ExportPropertyOverrides(UObject* Instance, TArray<TSharedPtr<FJsonValue>>& OutArray) const
{
    if (!Instance)
    {
        return;
    }

    UClass* ObjClass = Instance->GetClass();
    UObject* ClassCDO = ObjClass->GetDefaultObject();
    if (!ClassCDO || ClassCDO == Instance)
    {
        return;
    }

    ExportPropertyOverridesCompare(Instance, ClassCDO, OutArray);
}

void UBlueprintEdGraphExportCommandlet::ExportPropertyOverridesCompare(UObject* Instance, UObject* Reference, TArray<TSharedPtr<FJsonValue>>& OutArray) const
{
    if (!Instance || !Reference)
    {
        return;
    }

    for (TFieldIterator<FProperty> PropIt(Instance->GetClass()); PropIt; ++PropIt)
    {
        FProperty* Prop = *PropIt;
        if (!Prop || Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
        {
            continue;
        }

        // With Reference a base-class CDO the iterator surfaces properties owned by Instance subclasses,
        // and reading those off Reference asserts inside ContainerPtrToValuePtr.
        UClass* PropOwner = Prop->GetOwner<UClass>();
        if (PropOwner && !Reference->GetClass()->IsChildOf(PropOwner))
        {
            continue;
        }

        // Skip default-subobject pointers (nested components handled separately via InheritedComponentOverrides).
        // External asset references (StaticMesh, Material, etc.) are kept and serialized as PathName.
        if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
        {
            UObject* CurObj = ObjProp->GetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(Instance));
            UObject* RefObj = ObjProp->GetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(Reference));
            const bool bIsSubobjectRef = (CurObj && CurObj->IsDefaultSubobject()) || (RefObj && RefObj->IsDefaultSubobject());
            if (bIsSubobjectRef)
            {
                continue;
            }
        }

        FString CurrentValue;
        Prop->ExportTextItem_Direct(CurrentValue, Prop->ContainerPtrToValuePtr<void>(Instance), nullptr, Instance, PPF_None);

        FString DefaultValue;
        Prop->ExportTextItem_Direct(DefaultValue, Prop->ContainerPtrToValuePtr<void>(Reference), nullptr, Reference, PPF_None);

        if (CurrentValue != DefaultValue)
        {
            TSharedPtr<FJsonObject> OverrideObj = MakeShared<FJsonObject>();
            OverrideObj->SetStringField(TEXT("Name"), Prop->GetName());
            OverrideObj->SetStringField(TEXT("Type"), Prop->GetCPPType());
            OverrideObj->SetStringField(TEXT("Default"), DefaultValue);
            OverrideObj->SetStringField(TEXT("Value"), CurrentValue);
            OutArray.Add(MakeShared<FJsonValueObject>(OverrideObj));
        }
    }
}

void UBlueprintEdGraphExportCommandlet::ExportResolvedProperties(UObject* Instance, TArray<TSharedPtr<FJsonValue>>& OutArray) const
{
    if (!Instance)
    {
        return;
    }

    for (TFieldIterator<FProperty> PropIt(Instance->GetClass()); PropIt; ++PropIt)
    {
        FProperty* Prop = *PropIt;
        if (!Prop || Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
        {
            continue;
        }

        // Skip default-subobject pointers (component nesting noise, external asset refs are kept).
        if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
        {
            UObject* CurObj = ObjProp->GetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(Instance));
            if (CurObj && CurObj->IsDefaultSubobject())
            {
                continue;
            }
        }

        FString CurrentValue;
        Prop->ExportTextItem_Direct(CurrentValue, Prop->ContainerPtrToValuePtr<void>(Instance), nullptr, Instance, PPF_None);

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("Name"), Prop->GetName());
        Entry->SetStringField(TEXT("Type"), Prop->GetCPPType());
        Entry->SetStringField(TEXT("Value"), CurrentValue);
        OutArray.Add(MakeShared<FJsonValueObject>(Entry));
    }
}

UBlueprintEdGraphExportCommandlet::FExportOptions UBlueprintEdGraphExportCommandlet::ParseExportOptions(const FString& Params) const
{
    FExportOptions Options;
    Options.bIncludeGraphs = FParse::Param(*Params, TEXT("graphs"));
    return Options;
}
