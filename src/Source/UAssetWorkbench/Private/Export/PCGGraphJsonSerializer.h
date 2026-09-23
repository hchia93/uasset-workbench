#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;
class UPCGGraph;
class UPCGGraphInstance;
class UPCGGraphInterface;
class UPCGNode;
class UPCGPin;
class UPCGSettings;
struct FPCGPinProperties;

// PCG graphs are not EdGraphs. UPCGGraph owns UPCGNode objects with their own pins and edges, the editor
// graph is a mirror built when a window opens, so a headless read goes straight to the runtime model.
namespace PCGGraphJson
{
    // Object name, the only stable handle a UPCGNode carries. Same string EditPCGGraph resolves.
    FString NodeId(const UPCGNode* Node);

    // Input / Output / SettingsInstance / Blueprint / Subgraph / Native.
    FString ElementType(const UPCGGraph* Graph, const UPCGNode* Node);

    TSharedPtr<FJsonObject> ExportPinProperties(const FPCGPinProperties& Properties);

    TSharedPtr<FJsonObject> ExportPin(const UPCGPin* Pin);

    // bFull writes every subclass property. Otherwise only values that differ from the class default,
    // which is what a reader needs to see how a node was configured.
    TSharedPtr<FJsonObject> ExportSettingsProperties(const UPCGSettings* Settings, bool bFull);

    TSharedPtr<FJsonObject> ExportNode(const UPCGGraph* Graph, const UPCGNode* Node, bool bFull);

    // One entry per edge, upstream end first. Walks output pins so a shared edge prints once.
    TArray<TSharedPtr<FJsonValue>> ExportEdges(const UPCGGraph* Graph);

    // User parameters of a graph, or the override bag of a graph instance. Overridden only appears on an instance.
    TArray<TSharedPtr<FJsonValue>> ExportParameters(const UPCGGraphInterface* Interface);

    // Nodes plus the input and output node, which UPCGGraph keeps outside its Nodes array.
    void CollectNodes(const UPCGGraph* Graph, TArray<const UPCGNode*>& OutNodes);

    TSharedPtr<FJsonObject> ExportGraph(const UPCGGraph* Graph, bool bFull);

    TSharedPtr<FJsonObject> ExportGraphInstance(const UPCGGraphInstance* Instance);
}
