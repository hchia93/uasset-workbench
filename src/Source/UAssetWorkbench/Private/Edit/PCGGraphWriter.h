#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;
class UPCGGraph;
class UPCGNode;
class UPCGPin;

// One PCG graph, carried across every writer of a single target. NodesById holds existing nodes by object
// name and nodes the Nodes writer added under their spec Id, so Properties, Edges and Layout can name either.
struct FPCGGraphEditContext
{
    UPCGGraph* Graph = nullptr;
    FString AssetPath;

    TMap<FString, UPCGNode*> NodesById;
    int32 Ops = 0;
};

// Same split as IBlueprintWriter: the driver owns the asset, each writer owns one facet.
class IPCGGraphWriter
{
public:
    virtual ~IPCGGraphWriter() = default;

    // Spec key this writer answers to. A target without the key skips the writer entirely.
    virtual const TCHAR* GetSpecKey() const = 0;

    // Returns false once anything fails, which aborts the whole target before it is saved.
    virtual bool Apply(FPCGGraphEditContext& Context, const TSharedPtr<FJsonValue>& Section) = 0;
};

TUniquePtr<IPCGGraphWriter> MakePCGGraphParameterWriter();
TUniquePtr<IPCGGraphWriter> MakePCGGraphSettingsWriter();
TUniquePtr<IPCGGraphWriter> MakePCGGraphNodeWriter();
TUniquePtr<IPCGGraphWriter> MakePCGGraphPropertyWriter();
TUniquePtr<IPCGGraphWriter> MakePCGGraphEdgeWriter();
TUniquePtr<IPCGGraphWriter> MakePCGGraphLayoutWriter();

namespace PCGGraphEdit
{
    // Existing nodes answer to their object name, the same NodeId PCGGraphExport prints.
    void RegisterExistingNodes(UPCGGraph* Graph, TMap<FString, UPCGNode*>& OutNodesById);

    // A miss lists what the graph actually has, since nothing else names the nodes.
    UPCGNode* ResolveNode(const FPCGGraphEditContext& Context, const FString& NodeKey);

    FString DescribeNodes(const FPCGGraphEditContext& Context);

    // Direction is part of the lookup, a label can exist on both sides of the same node.
    UPCGPin* FindInputPin(UPCGNode* Node, const FString& Label);
    UPCGPin* FindOutputPin(UPCGNode* Node, const FString& Label);

    FString DescribePins(const UPCGNode* Node);

    // Settings edits do not go through the details panel, so the change broadcast that refreshes pins
    // has to be raised by hand. A null property means the deepest change type.
    void NotifySettingsChanged(UPCGNode* Node);
}
