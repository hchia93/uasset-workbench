#include "Edit/PCGGraphWriter.h"
#include "Export/PCGGraphJsonSerializer.h"
#include "UAssetWorkbenchModule.h"

#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"

void PCGGraphEdit::RegisterExistingNodes(UPCGGraph* Graph, TMap<FString, UPCGNode*>& OutNodesById)
{
    TArray<const UPCGNode*> Nodes;
    PCGGraphJson::CollectNodes(Graph, Nodes);
    for (const UPCGNode* Node : Nodes)
    {
        OutNodesById.Add(PCGGraphJson::NodeId(Node), const_cast<UPCGNode*>(Node));
    }
}

UPCGNode* PCGGraphEdit::ResolveNode(const FPCGGraphEditContext& Context, const FString& NodeKey)
{
    if (UPCGNode* const* Found = Context.NodesById.Find(NodeKey))
    {
        return *Found;
    }

    // Titles are not unique, so they only resolve when exactly one node carries the title.
    UPCGNode* ByTitle = nullptr;
    int32 TitleHits = 0;
    for (const TPair<FString, UPCGNode*>& Pair : Context.NodesById)
    {
        if (Pair.Value && Pair.Value->GetNodeTitle(EPCGNodeTitleType::ListView).ToString() == NodeKey)
        {
            ByTitle = Pair.Value;
            ++TitleHits;
        }
    }
    if (TitleHits == 1)
    {
        return ByTitle;
    }

    if (TitleHits > 1)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Node title '%s' matches %d nodes, use the NodeId. Nodes: %s"), *NodeKey, TitleHits, *DescribeNodes(Context));
    }
    else
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Node '%s' not found. Nodes: %s"), *NodeKey, *DescribeNodes(Context));
    }
    return nullptr;
}

FString PCGGraphEdit::DescribeNodes(const FPCGGraphEditContext& Context)
{
    TArray<FString> Entries;
    for (const TPair<FString, UPCGNode*>& Pair : Context.NodesById)
    {
        if (Pair.Value)
        {
            Entries.Add(FString::Printf(TEXT("%s (%s)"), *Pair.Key, *Pair.Value->GetNodeTitle(EPCGNodeTitleType::ListView).ToString()));
        }
    }
    return FString::Join(Entries, TEXT(", "));
}

UPCGPin* PCGGraphEdit::FindInputPin(UPCGNode* Node, const FString& Label)
{
    return Node ? Node->GetInputPin(FName(*Label)) : nullptr;
}

UPCGPin* PCGGraphEdit::FindOutputPin(UPCGNode* Node, const FString& Label)
{
    return Node ? Node->GetOutputPin(FName(*Label)) : nullptr;
}

FString PCGGraphEdit::DescribePins(const UPCGNode* Node)
{
    TArray<FString> Inputs;
    for (const UPCGPin* Pin : Node->GetInputPins())
    {
        if (Pin)
        {
            Inputs.Add(FString::Printf(TEXT("%s:%s"), *Pin->Properties.Label.ToString(), *Pin->Properties.AllowedTypes.ToString()));
        }
    }
    TArray<FString> Outputs;
    for (const UPCGPin* Pin : Node->GetOutputPins())
    {
        if (Pin)
        {
            Outputs.Add(FString::Printf(TEXT("%s:%s"), *Pin->Properties.Label.ToString(), *Pin->Properties.AllowedTypes.ToString()));
        }
    }
    return FString::Printf(TEXT("in [%s] out [%s]"), *FString::Join(Inputs, TEXT(", ")), *FString::Join(Outputs, TEXT(", ")));
}

void PCGGraphEdit::NotifySettingsChanged(UPCGNode* Node)
{
    UPCGSettings* Settings = Node ? Node->GetSettings() : nullptr;
    if (!Settings)
    {
        return;
    }

    // Protected on UPCGSettings, public on UObject, same virtual.
    FPropertyChangedEvent Event(nullptr);
    static_cast<UObject*>(Settings)->PostEditChangeProperty(Event);
}
