#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;
class UNiagaraNode;
class UNiagaraNodeFunctionCall;
class UNiagaraSystem;
struct FNiagaraEmitterHandle;
struct FNiagaraStackInput;
struct FNiagaraStackStage;

// One Niagara system, carried across every writer of a single target. FunctionNamesById maps the spec Id of
// a module or dynamic input added in this run to the function name the graph gave it.
struct FNiagaraSystemEditContext
{
    UNiagaraSystem* System = nullptr;
    FString AssetPath;

    TMap<FString, FString> FunctionNamesById;
    int32 Ops = 0;

    // Entries that already held the given value. A whole exported spec fed back counts here instead of in Ops.
    int32 Unchanged = 0;
};

// Same split as IPCGGraphWriter: the driver owns the asset, each writer owns one facet.
class INiagaraSystemWriter
{
public:
    virtual ~INiagaraSystemWriter() = default;

    // Spec key this writer answers to. A target without the key skips the writer entirely.
    virtual const TCHAR* GetSpecKey() const = 0;

    // Returns false once anything fails, which aborts the whole target before it is saved.
    virtual bool Apply(FNiagaraSystemEditContext& Context, const TSharedPtr<FJsonValue>& Section) = 0;
};

TUniquePtr<INiagaraSystemWriter> MakeNiagaraSystemSettingsWriter();
TUniquePtr<INiagaraSystemWriter> MakeNiagaraEmitterWriter();
TUniquePtr<INiagaraSystemWriter> MakeNiagaraRendererWriter();
TUniquePtr<INiagaraSystemWriter> MakeNiagaraModuleWriter();
TUniquePtr<INiagaraSystemWriter> MakeNiagaraStaticSwitchWriter();
TUniquePtr<INiagaraSystemWriter> MakeNiagaraInputWriter();

namespace NiagaraSystemEdit
{
    // Writers whose section is an array of op objects share this loop, the first failing op stops it.
    bool ForEachOp(const TSharedPtr<FJsonValue>& Section, const TCHAR* SpecKey, TFunctionRef<bool(const TSharedPtr<FJsonObject>&)> ApplyOp);

    // Emitters answer to their handle name, the Name that NiagaraSystemExport prints.
    const FNiagaraEmitterHandle* ResolveEmitter(const FNiagaraSystemEditContext& Context, const FString& Name);

    // No "Emitter" field addresses the system stages.
    bool ResolveStage(const FNiagaraSystemEditContext& Context, const TSharedPtr<FJsonObject>& Op, FNiagaraStackStage& OutStage);

    // Modules and dynamic inputs answer to their function name or to a spec Id given earlier in the run.
    UNiagaraNodeFunctionCall* ResolveFunctionCall(const FNiagaraSystemEditContext& Context, const FNiagaraStackStage& Stage, const FString& Key);

    // Inputs answer to the name after "Module.", the key NiagaraSystemExport prints under Inputs.
    bool ResolveInput(const FNiagaraStackStage& Stage, UNiagaraNodeFunctionCall& FunctionCall, const FString& Name, FNiagaraStackInput& OutInput);

    // Removes every node feeding the pin, without crossing the parameter map chain into neighbouring modules.
    void RemoveNodesFeedingPin(UEdGraphPin& Pin);

    void MarkChanged(UNiagaraNode* Node, const TCHAR* Reason);

    // Keeps only the entries whose text differs from the current export text, so ApplyProperties writes real changes.
    TSharedPtr<FJsonObject> DropUnchangedProperties(UObject* Target, const TSharedPtr<FJsonObject>& Properties, int32& InOutUnchanged);

    // Struct variant, compares top-level fields only, nested paths always pass through.
    TSharedPtr<FJsonObject> DropUnchangedStructProperties(const UScriptStruct* Struct, const void* Data, UObject* Owner, const TSharedPtr<FJsonObject>& Properties, int32& InOutUnchanged);
}
