#include "Export/PCGGraphJsonSerializer.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchVersion.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor/PCGGraphComment.h"
#include "Elements/PCGExecuteBlueprint.h"
#include "PCGCommon.h"
#include "PCGEdge.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "PCGSubgraph.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/UnrealType.h"

namespace
{
    FString EnumName(const UEnum* Enum, int64 Value)
    {
        return Enum ? Enum->GetNameStringByValue(Value) : FString::FromInt(Value);
    }

    // Base class fields print on the node itself, so the walk stops at StopAtClass. An instanced subobject
    // recurses against its own archetype, which is how a spawner mesh selector is reached.
    TSharedPtr<FJsonObject> ExportObjectDelta(const UObject* Object, const UObject* Archetype, const UClass* StopAtClass, bool bFull, int32 Depth)
    {
        TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
        if (!Object || Depth > 4)
        {
            return Props;
        }

        for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
        {
            FProperty* Prop = *It;
            const UClass* OwnerClass = Prop->GetOwnerClass();
            if (StopAtClass && OwnerClass && StopAtClass->IsChildOf(OwnerClass))
            {
                continue;
            }
            if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
            {
                continue;
            }

            const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Object);
            const bool bArchetypeHasProp = Archetype && OwnerClass && Archetype->IsA(OwnerClass);
            const void* ArchetypePtr = bArchetypeHasProp ? Prop->ContainerPtrToValuePtr<void>(Archetype) : nullptr;

            const FObjectProperty* ObjectProp = CastField<FObjectProperty>(Prop);
            if (ObjectProp && Prop->HasAnyPropertyFlags(CPF_InstancedReference))
            {
                const UObject* SubObject = ObjectProp->GetObjectPropertyValue(ValuePtr);
                if (!SubObject)
                {
                    continue;
                }

                TSharedPtr<FJsonObject> SubJson = MakeShared<FJsonObject>();
                SubJson->SetStringField(TEXT("Class"), SubObject->GetClass()->GetPathName());
                TSharedPtr<FJsonObject> SubProps = ExportObjectDelta(SubObject, SubObject->GetArchetype(), nullptr, bFull, Depth + 1);
                if (SubProps->Values.Num() > 0)
                {
                    SubJson->SetObjectField(TEXT("Properties"), SubProps);
                }
                Props->SetObjectField(Prop->GetName(), SubJson);
                continue;
            }

            if (!bFull && ArchetypePtr && Prop->Identical(ValuePtr, ArchetypePtr, PPF_DeepComparison))
            {
                continue;
            }

            FString Value;
            Prop->ExportTextItem_Direct(Value, ValuePtr, nullptr, const_cast<UObject*>(Object), PPF_None);
            Props->SetStringField(Prop->GetName(), Value);
        }

        return Props;
    }

    // Reflection by name keeps the graph private editor-only fields readable without friending the class.
    TSharedPtr<FJsonObject> ExportNamedProperties(const UObject* Object, TArrayView<const TCHAR* const> Names)
    {
        TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
        for (const TCHAR* Name : Names)
        {
            const FProperty* Prop = FindFProperty<FProperty>(Object->GetClass(), Name);
            if (!Prop)
            {
                continue;
            }

            FString Value;
            Prop->ExportTextItem_InContainer(Value, Object, nullptr, const_cast<UObject*>(Object), PPF_None);
            if (!Value.IsEmpty())
            {
                Props->SetStringField(Name, Value);
            }
        }
        return Props;
    }

    TSharedPtr<FJsonObject> ExportEdgeEnd(const UPCGPin* Pin)
    {
        TSharedPtr<FJsonObject> End = MakeShared<FJsonObject>();
        End->SetStringField(TEXT("Node"), PCGGraphJson::NodeId(Pin ? Pin->Node.Get() : nullptr));
        End->SetStringField(TEXT("Pin"), Pin ? Pin->Properties.Label.ToString() : FString());
        return End;
    }

    TArray<TSharedPtr<FJsonValue>> ExportPins(const TArray<TObjectPtr<UPCGPin>>& Pins)
    {
        TArray<TSharedPtr<FJsonValue>> Array;
        for (const UPCGPin* Pin : Pins)
        {
            if (Pin)
            {
                Array.Add(MakeShared<FJsonValueObject>(PCGGraphJson::ExportPin(Pin)));
            }
        }
        return Array;
    }

    void AddCommonHeader(const TSharedPtr<FJsonObject>& Root, const TCHAR* ExportType, const UObject* Asset)
    {
        Root->SetStringField(TEXT("ExporterVersion"), UASSET_WORKBENCH_VERSION_STRING);
        Root->SetStringField(TEXT("ExportType"), ExportType);
        Root->SetStringField(TEXT("AssetPath"), Asset->GetPathName());
        Root->SetStringField(TEXT("ExportTimestamp"), FDateTime::Now().ToString());
    }
}

FString PCGGraphJson::NodeId(const UPCGNode* Node)
{
    return Node ? Node->GetName() : FString();
}

FString PCGGraphJson::ElementType(const UPCGGraph* Graph, const UPCGNode* Node)
{
    if (Graph && Node == Graph->GetInputNode())
    {
        return TEXT("Input");
    }
    if (Graph && Node == Graph->GetOutputNode())
    {
        return TEXT("Output");
    }
    if (Node->IsInstance())
    {
        return TEXT("SettingsInstance");
    }

    const UPCGSettings* Settings = Node->GetSettings();
    if (Settings && Settings->IsA<UPCGBlueprintSettings>())
    {
        return TEXT("Blueprint");
    }
    if (Settings && Settings->IsA<UPCGBaseSubgraphSettings>())
    {
        return TEXT("Subgraph");
    }
    return TEXT("Native");
}

TSharedPtr<FJsonObject> PCGGraphJson::ExportPinProperties(const FPCGPinProperties& Properties)
{
    TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetStringField(TEXT("Label"), Properties.Label.ToString());
    Json->SetStringField(TEXT("AllowedTypes"), Properties.AllowedTypes.ToString());
    Json->SetStringField(TEXT("Usage"), EnumName(StaticEnum<EPCGPinUsage>(), static_cast<int64>(Properties.Usage)));
    Json->SetStringField(TEXT("PinStatus"), EnumName(StaticEnum<EPCGPinStatus>(), static_cast<int64>(Properties.PinStatus)));
    Json->SetBoolField(TEXT("AllowMultipleData"), Properties.bAllowMultipleData);
    Json->SetBoolField(TEXT("AllowMultipleConnections"), Properties.AllowsMultipleConnections());
    if (Properties.bInvisiblePin)
    {
        Json->SetBoolField(TEXT("Invisible"), true);
    }
    return Json;
}

TSharedPtr<FJsonObject> PCGGraphJson::ExportPin(const UPCGPin* Pin)
{
    TSharedPtr<FJsonObject> Json = ExportPinProperties(Pin->Properties);
    Json->SetBoolField(TEXT("Connected"), Pin->IsConnected());
    Json->SetNumberField(TEXT("EdgeCount"), Pin->EdgeCount());
    if (Pin->IsConnected())
    {
        Json->SetStringField(TEXT("CurrentTypes"), Pin->GetCurrentTypesID().ToString());
    }
    return Json;
}

TSharedPtr<FJsonObject> PCGGraphJson::ExportSettingsProperties(const UPCGSettings* Settings, bool bFull)
{
    if (!Settings)
    {
        return MakeShared<FJsonObject>();
    }
    return ExportObjectDelta(Settings, Settings->GetClass()->GetDefaultObject(), UPCGSettings::StaticClass(), bFull, 0);
}

TSharedPtr<FJsonObject> PCGGraphJson::ExportNode(const UPCGGraph* Graph, const UPCGNode* Node, bool bFull)
{
    TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetStringField(TEXT("NodeId"), NodeId(Node));
    Json->SetStringField(TEXT("Title"), Node->GetNodeTitle(EPCGNodeTitleType::ListView).ToString());
    if (Node->HasAuthoredTitle())
    {
        Json->SetStringField(TEXT("AuthoredTitle"), Node->GetAuthoredTitleName().ToString());
    }
    Json->SetStringField(TEXT("ElementType"), ElementType(Graph, Node));

    int32 PositionX = 0;
    int32 PositionY = 0;
    Node->GetNodePosition(PositionX, PositionY);
    Json->SetNumberField(TEXT("PositionX"), PositionX);
    Json->SetNumberField(TEXT("PositionY"), PositionY);
    if (!Node->NodeComment.IsEmpty())
    {
        Json->SetStringField(TEXT("Comment"), Node->NodeComment);
    }

    const UPCGSettingsInterface* SettingsInterface = Node->GetSettingsInterface();
    if (SettingsInterface)
    {
        Json->SetBoolField(TEXT("Enabled"), SettingsInterface->bEnabled);
    }

    const UPCGSettings* Settings = Node->GetSettings();
    if (Settings)
    {
        Json->SetStringField(TEXT("SettingsClass"), Settings->GetClass()->GetPathName());
        Json->SetStringField(TEXT("SettingsType"), EnumName(StaticEnum<EPCGSettingsType>(), static_cast<int64>(Settings->GetType())));
        if (Node->IsInstance())
        {
            Json->SetStringField(TEXT("SettingsAsset"), Settings->GetPathName());
        }
        if (Settings->UseSeed())
        {
            Json->SetNumberField(TEXT("Seed"), Settings->Seed);
        }

        if (const UPCGBaseSubgraphSettings* SubgraphSettings = Cast<UPCGBaseSubgraphSettings>(Settings))
        {
            const UPCGGraph* Subgraph = SubgraphSettings->GetSubgraph();
            Json->SetStringField(TEXT("Subgraph"), Subgraph ? Subgraph->GetPathName() : FString());
        }
        if (const UPCGBlueprintSettings* BlueprintSettings = Cast<UPCGBlueprintSettings>(Settings))
        {
            const UClass* ElementClass = BlueprintSettings->GetElementType();
            Json->SetStringField(TEXT("BlueprintElement"), ElementClass ? ElementClass->GetPathName() : FString());
        }

        TSharedPtr<FJsonObject> Props = ExportSettingsProperties(Settings, bFull);
        if (Props->Values.Num() > 0)
        {
            Json->SetObjectField(TEXT("Settings"), Props);
        }
    }

    Json->SetArrayField(TEXT("InputPins"), ExportPins(Node->GetInputPins()));
    Json->SetArrayField(TEXT("OutputPins"), ExportPins(Node->GetOutputPins()));
    return Json;
}

void PCGGraphJson::CollectNodes(const UPCGGraph* Graph, TArray<const UPCGNode*>& OutNodes)
{
    TSet<const UPCGNode*> Seen;
    auto Add = [&OutNodes, &Seen](const UPCGNode* Node)
    {
        if (Node && !Seen.Contains(Node))
        {
            Seen.Add(Node);
            OutNodes.Add(Node);
        }
    };

    Add(Graph->GetInputNode());
    Add(Graph->GetOutputNode());
    for (const UPCGNode* Node : Graph->GetNodes())
    {
        Add(Node);
    }
}

TArray<TSharedPtr<FJsonValue>> PCGGraphJson::ExportEdges(const UPCGGraph* Graph)
{
    TArray<TSharedPtr<FJsonValue>> Edges;

    TArray<const UPCGNode*> Nodes;
    CollectNodes(Graph, Nodes);

    for (const UPCGNode* Node : Nodes)
    {
        for (const UPCGPin* Pin : Node->GetOutputPins())
        {
            if (!Pin)
            {
                continue;
            }
            for (const UPCGEdge* Edge : Pin->Edges)
            {
                if (!Edge || !Edge->IsValid())
                {
                    continue;
                }
                TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
                Json->SetObjectField(TEXT("From"), ExportEdgeEnd(Edge->InputPin));
                Json->SetObjectField(TEXT("To"), ExportEdgeEnd(Edge->OutputPin));
                Edges.Add(MakeShared<FJsonValueObject>(Json));
            }
        }
    }
    return Edges;
}

TArray<TSharedPtr<FJsonValue>> PCGGraphJson::ExportParameters(const UPCGGraphInterface* Interface)
{
    TArray<TSharedPtr<FJsonValue>> Parameters;

    const FInstancedPropertyBag* Bag = Interface ? Interface->GetUserParametersStruct() : nullptr;
    const UPropertyBag* BagStruct = Bag ? Bag->GetPropertyBagStruct() : nullptr;
    if (!BagStruct)
    {
        return Parameters;
    }

    const bool bIsInstance = Interface->IsInstance();
    for (const FPropertyBagPropertyDesc& Desc : BagStruct->GetPropertyDescs())
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("Name"), Desc.Name.ToString());
        Json->SetStringField(TEXT("Type"), EnumName(StaticEnum<EPropertyBagPropertyType>(), static_cast<int64>(Desc.ValueType)));
        if (Desc.ValueTypeObject)
        {
            Json->SetStringField(TEXT("TypeObject"), Desc.ValueTypeObject->GetPathName());
        }
        if (Desc.CachedProperty)
        {
            Json->SetStringField(TEXT("CppType"), Desc.CachedProperty->GetCPPType());
        }

        TValueOrError<FString, EPropertyBagResult> Value = Bag->GetValueSerializedString(Desc.Name);
        if (Value.HasValue())
        {
            Json->SetStringField(TEXT("Value"), Value.GetValue());
        }
        if (bIsInstance)
        {
            Json->SetBoolField(TEXT("Overridden"), Interface->IsGraphParameterOverridden(Desc.Name));
        }
        Parameters.Add(MakeShared<FJsonValueObject>(Json));
    }
    return Parameters;
}

TSharedPtr<FJsonObject> PCGGraphJson::ExportGraph(const UPCGGraph* Graph, bool bFull)
{
    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    AddCommonHeader(Root, TEXT("PCGGraph"), Graph);
    Root->SetStringField(TEXT("GraphName"), Graph->GetName());

    static const TCHAR* const GraphSettingNames[] =
    {
        TEXT("bUseHierarchicalGeneration"),
        TEXT("HiGenGridSize"),
        TEXT("HiGenExponential"),
        TEXT("bUse2DGrid"),
        TEXT("bHasDefaultConstructedInputs"),
        TEXT("bLandscapeUsesMetadata"),
        TEXT("bIgnoreLandscapeTracking"),
        TEXT("bIsEditorOnly"),
        TEXT("bIsStandaloneGraph"),
        TEXT("bDebugFlagAppliesToIndividualComponents"),
        TEXT("Category"),
        TEXT("Description"),
        TEXT("GenerationRadii")
    };
    Root->SetObjectField(TEXT("GraphSettings"), ExportNamedProperties(Graph, GraphSettingNames));
    Root->SetArrayField(TEXT("Parameters"), ExportParameters(Graph));

    TArray<const UPCGNode*> Nodes;
    CollectNodes(Graph, Nodes);

    TArray<TSharedPtr<FJsonValue>> NodeArray;
    TMap<FString, TArray<TSharedPtr<FJsonValue>>> SubgraphUsers;
    for (const UPCGNode* Node : Nodes)
    {
        NodeArray.Add(MakeShared<FJsonValueObject>(ExportNode(Graph, Node, bFull)));

        const UPCGBaseSubgraphSettings* SubgraphSettings = Cast<UPCGBaseSubgraphSettings>(Node->GetSettings());
        if (SubgraphSettings)
        {
            const UPCGGraph* Subgraph = SubgraphSettings->GetSubgraph();
            SubgraphUsers.FindOrAdd(Subgraph ? Subgraph->GetPathName() : FString()).Add(MakeShared<FJsonValueString>(NodeId(Node)));
        }
    }
    Root->SetArrayField(TEXT("Nodes"), NodeArray);
    Root->SetNumberField(TEXT("NodeCount"), NodeArray.Num());

    TArray<TSharedPtr<FJsonValue>> Edges = ExportEdges(Graph);
    Root->SetArrayField(TEXT("Edges"), Edges);
    Root->SetNumberField(TEXT("EdgeCount"), Edges.Num());

    TArray<TSharedPtr<FJsonValue>> Subgraphs;
    for (const TPair<FString, TArray<TSharedPtr<FJsonValue>>>& Pair : SubgraphUsers)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("Graph"), Pair.Key);
        Json->SetArrayField(TEXT("Nodes"), Pair.Value);
        if (Pair.Key == Graph->GetPathName())
        {
            Json->SetBoolField(TEXT("Recursive"), true);
        }
        Subgraphs.Add(MakeShared<FJsonValueObject>(Json));
    }
    Root->SetArrayField(TEXT("Subgraphs"), Subgraphs);

    TArray<TSharedPtr<FJsonValue>> Comments;
    for (const FPCGGraphCommentNodeData& Comment : Graph->GetCommentNodes())
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("Text"), Comment.NodeComment);
        Json->SetNumberField(TEXT("PositionX"), Comment.NodePosX);
        Json->SetNumberField(TEXT("PositionY"), Comment.NodePosY);
        Json->SetNumberField(TEXT("Width"), Comment.NodeWidth);
        Json->SetNumberField(TEXT("Height"), Comment.NodeHeight);
        Comments.Add(MakeShared<FJsonValueObject>(Json));
    }
    Root->SetArrayField(TEXT("Comments"), Comments);
    Root->SetNumberField(TEXT("ExtraEditorNodeCount"), Graph->GetExtraEditorNodes().Num());

    return Root;
}

TSharedPtr<FJsonObject> PCGGraphJson::ExportGraphInstance(const UPCGGraphInstance* Instance)
{
    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    AddCommonHeader(Root, TEXT("PCGGraphInstance"), Instance);
    Root->SetStringField(TEXT("GraphInstanceName"), Instance->GetName());

    static const TCHAR* const ParentNames[] = { TEXT("Graph") };
    TSharedPtr<FJsonObject> Parent = ExportNamedProperties(Instance, ParentNames);
    Root->SetStringField(TEXT("Parent"), Parent->HasField(TEXT("Graph")) ? Parent->GetStringField(TEXT("Graph")) : FString());

    const UPCGGraph* Resolved = Instance->GetGraph();
    Root->SetStringField(TEXT("ResolvedGraph"), Resolved ? Resolved->GetPathName() : FString());
    Root->SetArrayField(TEXT("Parameters"), ExportParameters(Instance));
    return Root;
}
