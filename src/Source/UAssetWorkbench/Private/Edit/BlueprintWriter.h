#pragma once

#include "CoreMinimal.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"

class FJsonObject;
class FJsonValue;
class UAnimationStateMachineGraph;
class UAnimGraphNode_StateMachineBase;
class UAnimStateNodeBase;
class UAnimStateTransitionNode;
class UBlueprint;
class UEdGraph;
class UK2Node_EditablePinBase;
class UK2Node_FunctionEntry;

// One Blueprint, carried across every writer of a single target. NodesById is the reason it exists, the
// Graph writer registers spec Ids and Layout addresses those instead of NodeGuids nobody knows yet.
struct FBlueprintEditContext
{
    UBlueprint* Blueprint = nullptr;
    FString AssetPath;

    UEdGraph* Graph = nullptr;
    TMap<FString, UEdGraphNode*> NodesById;

    // Layout is cosmetic. Everything else changes the class and has to go back through the compiler.
    bool bNeedsStructuralRecompile = false;
    int32 Ops = 0;
};

// Mirrors the engine's IDiffControl split: the driver owns the asset, each writer owns one facet.
class IBlueprintWriter
{
public:
    virtual ~IBlueprintWriter() = default;

    // Spec key this writer answers to. A target without the key skips the writer entirely.
    virtual const TCHAR* GetSpecKey() const = 0;

    // Returns false once anything fails, which aborts the whole target before it is saved.
    virtual bool Apply(FBlueprintEditContext& Context, const TSharedPtr<FJsonValue>& Section) = 0;
};

TUniquePtr<IBlueprintWriter> MakeBlueprintComponentWriter();
TUniquePtr<IBlueprintWriter> MakeBlueprintWidgetWriter();
TUniquePtr<IBlueprintWriter> MakeBlueprintWidgetAnimationWriter();
TUniquePtr<IBlueprintWriter> MakeBlueprintVariableWriter();
TUniquePtr<IBlueprintWriter> MakeBlueprintDefaultsWriter();
TUniquePtr<IBlueprintWriter> MakeBlueprintGraphWriter();
TUniquePtr<IBlueprintWriter> MakeBlueprintLayoutWriter();
TUniquePtr<IBlueprintWriter> MakeBlueprintFunctionWriter();
TUniquePtr<IBlueprintWriter> MakeBlueprintDispatcherWriter();
TUniquePtr<IBlueprintWriter> MakeBlueprintInterfaceWriter();
TUniquePtr<IBlueprintWriter> MakeBlueprintStateMachineWriter();

namespace BlueprintEdit
{
    UEdGraph* FindGraph(UBlueprint* Blueprint, const FString& GraphName);

    UK2Node_FunctionEntry* FindEntryNode(const UEdGraph* Graph);

    // Pre-existing nodes answer to their 32-hex NodeGuid, the same one BlueprintEdGraphExport prints.
    void RegisterExistingNodes(UEdGraph* Graph, TMap<FString, UEdGraphNode*>& OutNodesById);

    // Exact PinName first, then PinFriendlyName, because the editor shows the friendly one and that
    // is what a spec author copies. "Return Value" and "ReturnValue" are the same pin.
    UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& PinName);

    // A wrong pin name is the most common spec error, so failures print what was actually there.
    FString DescribePins(UEdGraphNode* Node);

    // Type words are what the editor's dropdown shows, not the PC_ constants, so a spec reads like the UI
    // it replaces. Functions add IsReference on top, which the caller sets after this returns.
    bool ResolvePinType(const FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Desc, FEdGraphPinType& OutType);

    // Same keys, but only the ones the spec actually carries are written over InOutType. Type is what
    // qualifies SubType and ValueType, so those two are refused without it.
    bool ResolvePinTypeOverrides(const FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Desc, FEdGraphPinType& InOutType, bool& bOutTouched);

    // One entry of Inputs / Outputs / LocalVariables. All three carry the same Type / SubType / Container
    // triplet, only the direction and the node they land on differ.
    struct FSignatureEntry
    {
        FName Name;
        FEdGraphPinType Type;
        bool bHasDefault = false;
        FString Default;
    };

    bool ReadSignatureEntries(const FBlueprintEditContext& Context, const TArray<TSharedPtr<FJsonValue>>& Items, const TCHAR* Label, TArray<FSignatureEntry>& OutEntries);

    // Rewrites a terminator to carry exactly Entries, in spec order, which is the parameter order the
    // compiler emits. Pins the spec no longer lists are dropped.
    bool ApplyPinShape(const FBlueprintEditContext& Context, UK2Node_EditablePinBase* Node, const TArray<FSignatureEntry>& Entries, EEdGraphPinDirection Direction);

    // A Default parses against the pin, which only carries the new type once the node is rebuilt, so this
    // runs after ReconstructTerminator, never before it.
    bool ApplyPinDefaults(const FBlueprintEditContext& Context, UK2Node_EditablePinBase* Node, const TArray<FSignatureEntry>& Entries);

    // Orphan saving would keep the pins the spec just dropped, the editor turns it off around the same call.
    void ReconstructTerminator(UEdGraphNode* Node);

    // Every state machine the asset holds, nested ones included. Same walk the exporter does.
    void CollectMachines(const UBlueprint* Blueprint, TArray<UAnimationStateMachineGraph*>& OutMachines);

    // A miss lists the machines the asset actually has, since nothing else names them.
    UAnimationStateMachineGraph* FindMachine(const FBlueprintEditContext& Context, const FString& MachineName);

    // The anim graph node a machine hangs off, which is what an AnimGetter records as its source.
    UAnimGraphNode_StateMachineBase* FindMachineNode(const UBlueprint* Blueprint, const UEdGraph* MachineGraph);

    // A transition is also a UAnimStateNodeBase and answers GetStateName with its rule graph name, so it is
    // never a From / To candidate.
    UAnimStateNodeBase* FindState(const UAnimationStateMachineGraph* MachineGraph, const FString& StateName);

    FString DescribeStates(const UAnimationStateMachineGraph* MachineGraph);

    void CollectTransitions(const UAnimationStateMachineGraph* MachineGraph, const FString& FromName, const FString& ToName, TArray<UAnimStateTransitionNode*>& OutTransitions);

    FString DescribeTransitions(const UAnimationStateMachineGraph* MachineGraph);

    // One pair of states can carry several transitions, so an Index is the only way to name a single one.
    UAnimStateTransitionNode* ResolveTransition(const FBlueprintEditContext& Context, const UAnimationStateMachineGraph* MachineGraph, const FString& FromName, const FString& ToName, const TSharedPtr<FJsonObject>& Desc);
}
