#include "Edit/PCGGraphWriter.h"
#include "Export/PCGGraphJsonSerializer.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Elements/PCGExecuteBlueprint.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGSettings.h"
#include "PCGSubgraph.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"
#include "Utils/PCGPreconfiguration.h"

namespace
{
    // The catalog prints full paths. A short name still resolves, with the PCG / Settings wrapping tried
    // second so "StaticMeshSpawner" finds UPCGStaticMeshSpawnerSettings.
    UClass* ResolveSettingsClass(const FString& ClassName)
    {
        UClass* Found = nullptr;
        if (ClassName.Contains(TEXT("/")))
        {
            Found = FSoftClassPath(ClassName).TryLoadClass<UPCGSettings>();
        }
        else
        {
            Found = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::ExactClass);
            if (!Found)
            {
                const FString Wrapped = TEXT("PCG") + ClassName + TEXT("Settings");
                Found = FindFirstObject<UClass>(*Wrapped, EFindFirstObjectOptions::ExactClass);
            }
        }
        return Found && Found->IsChildOf(UPCGSettings::StaticClass()) ? Found : nullptr;
    }

    // Accepts the generated class path or the Blueprint asset path.
    UClass* ResolveBlueprintElementClass(const FString& Path)
    {
        if (UClass* Direct = FSoftClassPath(Path).TryLoadClass<UPCGBlueprintBaseElement>())
        {
            return Direct;
        }
        const FString AssetName = FPackageName::GetShortName(Path);
        const FString GeneratedPath = FString::Printf(TEXT("%s.%s_C"), *Path, *AssetName);
        return FSoftClassPath(GeneratedPath).TryLoadClass<UPCGBlueprintBaseElement>();
    }

    bool ApplyPreconfigured(UPCGSettings* Settings, const TSharedPtr<FJsonObject>& Desc)
    {
        const TSharedPtr<FJsonValue> Wanted = Desc->TryGetField(TEXT("Preconfigured"));
        if (!Wanted.IsValid())
        {
            return true;
        }

        TArray<FString> Labels;
        for (const FPCGPreConfiguredSettingsInfo& Info : Settings->GetPreconfiguredInfo())
        {
            const FString Label = Info.Label.ToString();
            const bool bByIndex = Wanted->Type == EJson::Number && Info.PreconfiguredIndex == static_cast<int32>(Wanted->AsNumber());
            const bool bByLabel = Wanted->Type == EJson::String && Label == Wanted->AsString();
            if (bByIndex || bByLabel)
            {
                Settings->ApplyPreconfiguredSettings(Info);
                return true;
            }
            Labels.Add(FString::Printf(TEXT("%d:%s"), Info.PreconfiguredIndex, *Label));
        }

        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Preconfigured option not found on %s. Available: %s"), *Settings->GetClass()->GetName(), *FString::Join(Labels, TEXT(", ")));
        return false;
    }

    bool ConfigureNewSettings(FPCGGraphEditContext& Context, UPCGSettings* Settings, const TSharedPtr<FJsonObject>& Desc)
    {
        if (!ApplyPreconfigured(Settings, Desc))
        {
            return false;
        }

        FString SubgraphPath;
        if (Desc->TryGetStringField(TEXT("Subgraph"), SubgraphPath))
        {
            UPCGBaseSubgraphSettings* SubgraphSettings = Cast<UPCGBaseSubgraphSettings>(Settings);
            UPCGGraphInterface* Subgraph = LoadObject<UPCGGraphInterface>(nullptr, *SubgraphPath);
            if (!SubgraphSettings)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Subgraph given but %s is not a subgraph settings class"), *Settings->GetClass()->GetName());
                return false;
            }
            if (!Subgraph)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to load subgraph: %s"), *SubgraphPath);
                return false;
            }
            SubgraphSettings->SetSubgraph(Subgraph);
        }

        FString ElementPath;
        if (Desc->TryGetStringField(TEXT("BlueprintElement"), ElementPath))
        {
            UPCGBlueprintSettings* BlueprintSettings = Cast<UPCGBlueprintSettings>(Settings);
            UClass* ElementClass = ResolveBlueprintElementClass(ElementPath);
            if (!BlueprintSettings)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("BlueprintElement given but %s is not PCGBlueprintSettings"), *Settings->GetClass()->GetName());
                return false;
            }
            if (!ElementClass)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to load Blueprint element class: %s"), *ElementPath);
                return false;
            }
            UPCGBlueprintBaseElement* ElementInstance = nullptr;
            BlueprintSettings->SetBlueprintElementType(ElementClass, ElementInstance);
        }

        const TSharedPtr<FJsonObject>* Properties = nullptr;
        if (Desc->TryGetObjectField(TEXT("Properties"), Properties))
        {
            int32 Failures = 0;
            Context.Ops += UAssetWorkbench::ApplyProperties(Settings, *Properties, Failures);
            if (Failures > 0)
            {
                return false;
            }
        }
        return true;
    }

    bool AddNode(FPCGGraphEditContext& Context, const TSharedPtr<FJsonObject>& Desc)
    {
        UPCGNode* Node = nullptr;
        UPCGSettings* Settings = nullptr;

        FString SettingsAsset;
        FString ClassName;
        if (Desc->TryGetStringField(TEXT("SettingsAsset"), SettingsAsset))
        {
            Settings = LoadObject<UPCGSettings>(nullptr, *SettingsAsset);
            if (!Settings)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to load settings asset: %s"), *SettingsAsset);
                return false;
            }
            Node = Context.Graph->AddNodeInstance(Settings);
        }
        else if (Desc->TryGetStringField(TEXT("Class"), ClassName))
        {
            UClass* Class = ResolveSettingsClass(ClassName);
            if (!Class)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Settings class not found: %s. Run PCGCatalogExport for the list"), *ClassName);
                return false;
            }
            Node = Context.Graph->AddNodeOfType(Class, Settings);
        }
        else
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Add needs Class or SettingsAsset"));
            return false;
        }

        if (!Node)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Graph refused the node: %s%s"), *ClassName, *SettingsAsset);
            return false;
        }

        if (Settings && !Node->IsInstance())
        {
            if (!ConfigureNewSettings(Context, Settings, Desc))
            {
                return false;
            }
            Node->UpdateAfterSettingsChangeDuringCreation();
        }

        FString Title;
        if (Desc->TryGetStringField(TEXT("Title"), Title))
        {
            Node->SetNodeTitle(FName(*Title));
        }

        double PositionX = 0.0;
        double PositionY = 0.0;
        if (Desc->TryGetNumberField(TEXT("PositionX"), PositionX) | Desc->TryGetNumberField(TEXT("PositionY"), PositionY))
        {
            Node->SetNodePosition(static_cast<int32>(PositionX), static_cast<int32>(PositionY));
        }

        const FString NodeId = PCGGraphJson::NodeId(Node);
        Context.NodesById.Add(NodeId, Node);
        FString Id;
        if (Desc->TryGetStringField(TEXT("Id"), Id) && !Id.IsEmpty())
        {
            Context.NodesById.Add(Id, Node);
        }

        ++Context.Ops;
        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Added node %s (%s)%s"), *NodeId, *Node->GetNodeTitle(EPCGNodeTitleType::ListView).ToString(), Id.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" as %s"), *Id));
        return true;
    }

    bool DeleteNode(FPCGGraphEditContext& Context, UPCGNode* Node)
    {
        if (Node == Context.Graph->GetInputNode() || Node == Context.Graph->GetOutputNode())
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("The graph input and output nodes cannot be deleted"));
            return false;
        }

        const FString NodeId = PCGGraphJson::NodeId(Node);
        Context.Graph->RemoveNode(Node);

        for (TMap<FString, UPCGNode*>::TIterator It(Context.NodesById); It; ++It)
        {
            if (It.Value() == Node)
            {
                It.RemoveCurrent();
            }
        }

        ++Context.Ops;
        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Deleted node %s"), *NodeId);
        return true;
    }

    class FPCGGraphNodeWriter : public IPCGGraphWriter
    {
    public:
        virtual const TCHAR* GetSpecKey() const override
        {
            return TEXT("Nodes");
        }

        virtual bool Apply(FPCGGraphEditContext& Context, const TSharedPtr<FJsonValue>& Section) override
        {
            const TArray<TSharedPtr<FJsonValue>>* Ops = nullptr;
            if (!Section->TryGetArray(Ops))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Nodes must be an array of operations"));
                return false;
            }

            for (const TSharedPtr<FJsonValue>& Value : *Ops)
            {
                const TSharedPtr<FJsonObject> Desc = Value->AsObject();
                if (!Desc.IsValid())
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Nodes entry is not an object"));
                    return false;
                }

                FString Op;
                Desc->TryGetStringField(TEXT("Op"), Op);
                if (Op == TEXT("Add"))
                {
                    if (!AddNode(Context, Desc))
                    {
                        return false;
                    }
                    continue;
                }

                FString NodeKey;
                if (!Desc->TryGetStringField(TEXT("Node"), NodeKey))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Nodes op '%s' needs a Node"), *Op);
                    return false;
                }
                UPCGNode* Node = PCGGraphEdit::ResolveNode(Context, NodeKey);
                if (!Node)
                {
                    return false;
                }

                if (Op == TEXT("Delete"))
                {
                    if (!DeleteNode(Context, Node))
                    {
                        return false;
                    }
                }
                else if (Op == TEXT("Rename"))
                {
                    FString Title;
                    Desc->TryGetStringField(TEXT("Title"), Title);
                    Node->SetNodeTitle(FName(*Title));
                    ++Context.Ops;
                }
                else if (Op == TEXT("Move"))
                {
                    double PositionX = 0.0;
                    double PositionY = 0.0;
                    Desc->TryGetNumberField(TEXT("PositionX"), PositionX);
                    Desc->TryGetNumberField(TEXT("PositionY"), PositionY);
                    Node->SetNodePosition(static_cast<int32>(PositionX), static_cast<int32>(PositionY));
                    ++Context.Ops;
                }
                else if (Op == TEXT("Enable"))
                {
                    bool bEnabled = true;
                    Desc->TryGetBoolField(TEXT("Enabled"), bEnabled);
                    if (UPCGSettingsInterface* SettingsInterface = Node->GetSettingsInterface())
                    {
                        SettingsInterface->SetEnabled(bEnabled);
                        ++Context.Ops;
                    }
                }
                else
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Unknown Nodes op '%s'. Expected Add, Delete, Rename, Move, Enable"), *Op);
                    return false;
                }
            }
            return true;
        }
    };
}

TUniquePtr<IPCGGraphWriter> MakePCGGraphNodeWriter()
{
    return MakeUnique<FPCGGraphNodeWriter>();
}
