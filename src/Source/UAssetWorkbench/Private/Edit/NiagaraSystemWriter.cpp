#include "Edit/NiagaraSystemWriter.h"
#include "Export/NiagaraStackJsonSerializer.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphPin.h"

#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNode.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraSystem.h"

namespace
{
    FString DescribeStages(const TArray<FNiagaraStackStage>& Stages)
    {
        TArray<FString> Names;
        for (const FNiagaraStackStage& Stage : Stages)
        {
            Names.Add(Stage.Name);
        }
        return FString::Join(Names, TEXT(", "));
    }
}

bool NiagaraSystemEdit::ForEachOp(const TSharedPtr<FJsonValue>& Section, const TCHAR* SpecKey, TFunctionRef<bool(const TSharedPtr<FJsonObject>&)> ApplyOp)
{
    const TArray<TSharedPtr<FJsonValue>>* Ops = nullptr;
    if (!Section->TryGetArray(Ops))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s must be an array of op objects"), SpecKey);
        return false;
    }

    for (const TSharedPtr<FJsonValue>& Value : *Ops)
    {
        const TSharedPtr<FJsonObject> Op = Value.IsValid() ? Value->AsObject() : nullptr;
        if (!Op.IsValid())
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s entries must be objects"), SpecKey);
            return false;
        }
        if (!ApplyOp(Op))
        {
            return false;
        }
    }
    return true;
}

const FNiagaraEmitterHandle* NiagaraSystemEdit::ResolveEmitter(const FNiagaraSystemEditContext& Context, const FString& Name)
{
    TArray<FString> Names;
    for (const FNiagaraEmitterHandle& Handle : Context.System->GetEmitterHandles())
    {
        if (Handle.GetName().ToString() == Name)
        {
            return &Handle;
        }
        Names.Add(Handle.GetName().ToString());
    }

    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Emitter '%s' not found. Emitters: %s"), *Name, *FString::Join(Names, TEXT(", ")));
    return nullptr;
}

bool NiagaraSystemEdit::ResolveStage(const FNiagaraSystemEditContext& Context, const TSharedPtr<FJsonObject>& Op, FNiagaraStackStage& OutStage)
{
    FString StageName;
    if (!Op->TryGetStringField(TEXT("Stage"), StageName))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Op has no Stage field"));
        return false;
    }

    const FNiagaraEmitterHandle* Handle = nullptr;
    FString EmitterName;
    if (Op->TryGetStringField(TEXT("Emitter"), EmitterName))
    {
        Handle = ResolveEmitter(Context, EmitterName);
        if (!Handle)
        {
            return false;
        }
    }

    TArray<FNiagaraStackStage> Stages;
    NiagaraStackJson::CollectStages(Context.System, Handle, Stages);
    for (const FNiagaraStackStage& Stage : Stages)
    {
        if (Stage.Name == StageName)
        {
            OutStage = Stage;
            return true;
        }
    }

    const FString Owner = Handle ? EmitterName : TEXT("the system");
    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Stage '%s' not found on %s. Stages: %s"), *StageName, *Owner, *DescribeStages(Stages));
    return false;
}

UNiagaraNodeFunctionCall* NiagaraSystemEdit::ResolveFunctionCall(const FNiagaraSystemEditContext& Context, const FNiagaraStackStage& Stage, const FString& Key)
{
    const FString* IdName = Context.FunctionNamesById.Find(Key);
    const FString& Name = IdName ? *IdName : Key;

    TArray<UNiagaraNode*> Nodes;
    NiagaraStackJson::CollectStageNodes(*Stage.OutputNode, Nodes);

    TArray<FString> Names;
    for (UNiagaraNode* Node : Nodes)
    {
        UNiagaraNodeFunctionCall* FunctionCall = Cast<UNiagaraNodeFunctionCall>(Node);
        if (!FunctionCall)
        {
            continue;
        }
        if (FunctionCall->GetFunctionName() == Name)
        {
            return FunctionCall;
        }
        Names.Add(FunctionCall->GetFunctionName());
    }

    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Module or dynamic input '%s' not found in stage %s. Functions: %s"), *Key, *Stage.Name, *FString::Join(Names, TEXT(", ")));
    return nullptr;
}

bool NiagaraSystemEdit::ResolveInput(const FNiagaraStackStage& Stage, UNiagaraNodeFunctionCall& FunctionCall, const FString& Name, FNiagaraStackInput& OutInput)
{
    TArray<FNiagaraStackInput> Inputs;
    NiagaraStackJson::CollectInputs(Stage, FunctionCall, Inputs);

    TArray<FString> Names;
    for (const FNiagaraStackInput& Input : Inputs)
    {
        if (Input.DisplayName == Name)
        {
            OutInput = Input;
            return true;
        }
        Names.Add(Input.DisplayName);
    }

    // Static switches hide inputs, so a miss may only mean the switch has to be set first.
    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Input '%s' not visible on %s under its current static switches. Inputs: %s"), *Name, *FunctionCall.GetFunctionName(), *FString::Join(Names, TEXT(", ")));
    return false;
}

void NiagaraSystemEdit::RemoveNodesFeedingPin(UEdGraphPin& Pin)
{
    TArray<UNiagaraNode*> Pending;
    for (UEdGraphPin* Linked : Pin.LinkedTo)
    {
        Pending.Add(Linked ? Cast<UNiagaraNode>(Linked->GetOwningNode()) : nullptr);
    }

    TArray<UNiagaraNode*> ToRemove;
    while (Pending.Num() > 0)
    {
        UNiagaraNode* Node = Pending.Pop(EAllowShrinking::No);
        if (!Node || ToRemove.Contains(Node))
        {
            continue;
        }
        ToRemove.Add(Node);

        // A dynamic input links its parameter map pin back to the previous module, that edge leaves the subtree.
        for (UEdGraphPin* InputPin : Node->Pins)
        {
            if (InputPin && InputPin->Direction == EGPD_Input && !Node->IsParameterMapPin(InputPin))
            {
                for (UEdGraphPin* Linked : InputPin->LinkedTo)
                {
                    Pending.Add(Linked ? Cast<UNiagaraNode>(Linked->GetOwningNode()) : nullptr);
                }
            }
        }
    }

    for (UNiagaraNode* Node : ToRemove)
    {
        UEdGraph* Graph = Node->GetGraph();
        Node->Modify();
        Graph->Modify();
        Graph->RemoveNode(Node);
    }
}

void NiagaraSystemEdit::MarkChanged(UNiagaraNode* Node, const TCHAR* Reason)
{
    if (Node)
    {
        Node->MarkNodeRequiresSynchronization(Reason, true);
    }
}

TSharedPtr<FJsonObject> NiagaraSystemEdit::DropUnchangedProperties(UObject* Target, const TSharedPtr<FJsonObject>& Properties, int32& InOutUnchanged)
{
    TSharedPtr<FJsonObject> Changed = MakeShared<FJsonObject>();
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
    {
        FString Text;
        const bool bSameText = Pair.Value->TryGetString(Text) && UAssetWorkbench::ReadPropertyPathText(Target, Pair.Key) == Text;
        if (bSameText)
        {
            ++InOutUnchanged;
            continue;
        }
        Changed->SetField(Pair.Key, Pair.Value);
    }
    return Changed;
}

TSharedPtr<FJsonObject> NiagaraSystemEdit::DropUnchangedStructProperties(const UScriptStruct* Struct, const void* Data, UObject* Owner, const TSharedPtr<FJsonObject>& Properties, int32& InOutUnchanged)
{
    TSharedPtr<FJsonObject> Changed = MakeShared<FJsonObject>();
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
    {
        const FProperty* Property = Struct->FindPropertyByName(FName(*Pair.Key));
        FString Text;
        if (Property && Pair.Value->TryGetString(Text))
        {
            FString Current;
            Property->ExportTextItem_Direct(Current, Property->ContainerPtrToValuePtr<void>(Data), nullptr, Owner, PPF_None);
            if (Current == Text)
            {
                ++InOutUnchanged;
                continue;
            }
        }
        Changed->SetField(Pair.Key, Pair.Value);
    }
    return Changed;
}
