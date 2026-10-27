#pragma once

#include "CoreMinimal.h"
#include "NiagaraCommon.h"
#include "NiagaraEmitter.h"
#include "NiagaraTypes.h"

class FJsonObject;
class UEdGraphPin;
class UNiagaraDataInterface;
class UNiagaraGraph;
class UNiagaraNode;
class UNiagaraNodeFunctionCall;
class UNiagaraNodeOutput;
class UNiagaraScript;
class UNiagaraSystem;
struct FNiagaraEmitterHandle;

// One stack section, the same split the system editor shows: an output node of the system or emitter graph.
struct FNiagaraStackStage
{
    FString Name;
    ENiagaraScriptUsage Usage = ENiagaraScriptUsage::Function;
    FGuid UsageId;
    UNiagaraNodeOutput* OutputNode = nullptr;
    UNiagaraSystem* System = nullptr;

    // Null for the system stages.
    const FNiagaraEmitterHandle* EmitterHandle = nullptr;
    FVersionedNiagaraEmitter Emitter;

    bool IsSystemStage() const { return EmitterHandle == nullptr; }
};

// How a module input gets its value, matching the value modes of the stack input widget.
enum class ENiagaraStackInputMode : uint8
{
    Default,
    Local,
    Linked,
    DynamicInput,
    DataInterface,
    ObjectAsset,
    Expression
};

struct FNiagaraStackInput
{
    // Unaliased module-namespace name, e.g. "Module.Lifetime". The spec addresses the part after "Module.".
    FNiagaraVariable Variable;
    FName AliasedName;
    FString DisplayName;
    bool bRapidIteration = false;
};

namespace NiagaraStackJson
{
    // System stages first, then every emitter's stages in stack order. A null handle collects system stages only.
    void CollectStages(UNiagaraSystem* System, const FNiagaraEmitterHandle* EmitterHandle, TArray<FNiagaraStackStage>& OutStages);

    // Walks the parameter map chain up from the output node, so the order is the stack order.
    void GetOrderedModules(UNiagaraNodeOutput& OutputNode, TArray<UNiagaraNodeFunctionCall*>& OutModules);

    // The node that starts the parameter map chain of a stage, the input group the first module links to.
    UNiagaraNode* FindStageStartNode(UNiagaraNodeOutput& OutputNode);

    // Every node upstream of the stage output, dynamic inputs included.
    void CollectStageNodes(UNiagaraNodeOutput& OutputNode, TArray<UNiagaraNode*>& OutNodes);

    UEdGraphPin* GetParameterMapInputPin(UNiagaraNode& Node);
    UEdGraphPin* GetParameterMapOutputPin(UNiagaraNode& Node);

    // Module inputs visible under the current static switch values, in declaration order.
    void CollectInputs(const FNiagaraStackStage& Stage, UNiagaraNodeFunctionCall& FunctionCall, TArray<FNiagaraStackInput>& OutInputs);
    void CollectStaticSwitchPins(const FNiagaraStackStage& Stage, UNiagaraNodeFunctionCall& FunctionCall, TArray<UEdGraphPin*>& OutPins);

    // The ParameterMapSet node in front of a function call holds one override pin per overridden input.
    UNiagaraNode* FindOverrideNode(UNiagaraNodeFunctionCall& FunctionCall);
    UEdGraphPin* FindOverridePin(UNiagaraNodeFunctionCall& FunctionCall, FName AliasedName);

    FNiagaraVariable MakeRapidIterationVariable(const FNiagaraStackStage& Stage, const FNiagaraStackInput& Input);

    // Every compiled script that bakes the stage's rapid iteration values, the editor writes them all.
    void GetAffectedScripts(UNiagaraSystem* System, const FNiagaraStackStage& Stage, TArray<UNiagaraScript*>& OutScripts);

    // Local value text of an input, from its unlinked override pin or its rapid iteration parameter.
    bool ReadLocalValue(const FNiagaraStackStage& Stage, UNiagaraNodeFunctionCall& FunctionCall, const FNiagaraStackInput& Input, FString& OutText);

    ENiagaraStackInputMode ResolveInputMode(const FNiagaraStackStage& Stage, UNiagaraNodeFunctionCall& FunctionCall, const FNiagaraStackInput& Input);
    const TCHAR* InputModeName(ENiagaraStackInputMode Mode);

    // Values travel as pin default strings, the same text the export prints, so an export round-trips into a spec.
    FString ValueToString(const FNiagaraVariable& AllocatedVariable);
    bool StringToValue(const FString& Text, FNiagaraVariable& InOutVariable);

    // Data interface and object asset inputs hang an input node off the override pin.
    UNiagaraDataInterface* GetDataInterface(const UNiagaraNode* InputNode);
    UObject* GetObjectAsset(const UNiagaraNode* InputNode);

    bool IsNodeOfClass(const UObject* Node, const TCHAR* ClassName);

    // Enum switches print the artist-facing display name, the edit side accepts either name.
    FString DescribeStaticSwitchValue(const UEdGraphPin* Pin);
    bool ResolveStaticSwitchValue(const UEdGraphPin* Pin, const FString& Text, FString& OutPinValue);

    // Only values that differ from the class default, or from a default-constructed struct.
    TSharedPtr<FJsonObject> ExportPropertyDelta(UObject* Object, UClass* StopAtClass, const TSet<FName>& SkippedProperties = TSet<FName>());
    TSharedPtr<FJsonObject> ExportStructDelta(const UScriptStruct* Struct, const void* Data, UObject* Owner, const TSet<FName>& SkippedProperties);

    TSharedPtr<FJsonObject> SerializeStage(const FNiagaraStackStage& Stage);
    TSharedPtr<FJsonObject> SerializeFunctionCall(const FNiagaraStackStage& Stage, UNiagaraNodeFunctionCall& FunctionCall);
    TSharedPtr<FJsonObject> SerializeDataInterface(UNiagaraDataInterface* DataInterface);
}
