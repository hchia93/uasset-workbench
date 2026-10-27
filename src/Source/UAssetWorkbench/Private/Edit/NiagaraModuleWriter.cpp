#include "Edit/NiagaraSystemWriter.h"
#include "Export/NiagaraStackJsonSerializer.h"
#include "UAssetWorkbenchModule.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphPin.h"

#include "NiagaraGraph.h"
#include "NiagaraNode.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraScript.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"

namespace
{
    // The nodes of one module between its predecessor and its successor on the parameter map chain. StartNodes
    // are what the predecessor's map output feeds: the module itself, its override node and its dynamic inputs.
    struct FModuleGroup
    {
        UNiagaraNode* Previous = nullptr;
        TArray<UNiagaraNode*> StartNodes;
        TArray<UNiagaraNode*> NextNodes;
    };

    bool FindModuleGroup(UNiagaraNodeFunctionCall& Module, FModuleGroup& OutGroup)
    {
        // Override and dynamic input nodes sit between the predecessor and the module, walk past them.
        UNiagaraNode* Current = &Module;
        while (true)
        {
            UEdGraphPin* InputPin = NiagaraStackJson::GetParameterMapInputPin(*Current);
            UNiagaraNode* Upstream = InputPin && InputPin->LinkedTo.Num() == 1 ? Cast<UNiagaraNode>(InputPin->LinkedTo[0]->GetOwningNode()) : nullptr;
            if (!Upstream)
            {
                return false;
            }
            const bool bIsModule = Cast<UNiagaraNodeFunctionCall>(Upstream) && !NiagaraStackJson::IsNodeOfClass(Upstream, TEXT("NiagaraNodeParameterMapSet"));
            if (bIsModule || !NiagaraStackJson::GetParameterMapInputPin(*Upstream) || NiagaraStackJson::GetParameterMapInputPin(*Upstream)->LinkedTo.Num() == 0)
            {
                OutGroup.Previous = Upstream;
                break;
            }
            Current = Upstream;
        }

        for (UEdGraphPin* Linked : NiagaraStackJson::GetParameterMapOutputPin(*OutGroup.Previous)->LinkedTo)
        {
            OutGroup.StartNodes.Add(CastChecked<UNiagaraNode>(Linked->GetOwningNode()));
        }
        for (UEdGraphPin* Linked : NiagaraStackJson::GetParameterMapOutputPin(Module)->LinkedTo)
        {
            OutGroup.NextNodes.Add(CastChecked<UNiagaraNode>(Linked->GetOwningNode()));
        }
        return true;
    }

    void LinkMapChain(UNiagaraNode& From, const TArray<UNiagaraNode*>& ToNodes)
    {
        UEdGraphPin* OutputPin = NiagaraStackJson::GetParameterMapOutputPin(From);
        for (UNiagaraNode* To : ToNodes)
        {
            UEdGraphPin* InputPin = NiagaraStackJson::GetParameterMapInputPin(*To);
            OutputPin->Modify();
            InputPin->Modify();
            OutputPin->MakeLinkTo(InputPin);
        }
    }

    void BreakMapOutput(UNiagaraNode& Node)
    {
        UEdGraphPin* OutputPin = NiagaraStackJson::GetParameterMapOutputPin(Node);
        OutputPin->Modify();
        OutputPin->BreakAllPinLinks();
    }

    // Detaches the group and bridges its neighbours, as FNiagaraStackGraphUtilities::DisconnectStackNodeGroup does.
    void DisconnectGroup(UNiagaraNodeFunctionCall& Module, const FModuleGroup& Group)
    {
        BreakMapOutput(*Group.Previous);
        BreakMapOutput(Module);
        LinkMapChain(*Group.Previous, Group.NextNodes);
    }

    void CollectGroupNodes(UNiagaraNodeFunctionCall& Module, TArray<UNiagaraNode*>& OutNodes)
    {
        // Run after DisconnectGroup, every upstream edge then stays inside the group.
        TArray<UNiagaraNode*> Pending;
        Pending.Add(&Module);
        while (Pending.Num() > 0)
        {
            UNiagaraNode* Node = Pending.Pop(EAllowShrinking::No);
            if (!Node || OutNodes.Contains(Node))
            {
                continue;
            }
            OutNodes.Add(Node);
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin && Pin->Direction == EGPD_Input)
                {
                    for (UEdGraphPin* Linked : Pin->LinkedTo)
                    {
                        Pending.Add(Linked ? Cast<UNiagaraNode>(Linked->GetOwningNode()) : nullptr);
                    }
                }
            }
        }
    }

    bool FindModule(FNiagaraSystemEditContext& Context, const TSharedPtr<FJsonObject>& Op, FNiagaraStackStage& OutStage, UNiagaraNodeFunctionCall*& OutModule)
    {
        FString ModuleName;
        if (!Op->TryGetStringField(TEXT("Module"), ModuleName))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Op has no Module field"));
            return false;
        }
        if (!NiagaraSystemEdit::ResolveStage(Context, Op, OutStage))
        {
            return false;
        }

        TArray<UNiagaraNodeFunctionCall*> Modules;
        NiagaraStackJson::GetOrderedModules(*OutStage.OutputNode, Modules);

        const FString* IdName = Context.FunctionNamesById.Find(ModuleName);
        const FString& Name = IdName ? *IdName : ModuleName;
        TArray<FString> Names;
        for (UNiagaraNodeFunctionCall* Module : Modules)
        {
            if (Module->GetFunctionName() == Name)
            {
                OutModule = Module;
                return true;
            }
            Names.Add(Module->GetFunctionName());
        }

        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Module '%s' not found in stage %s. Modules: %s"), *ModuleName, *OutStage.Name, *FString::Join(Names, TEXT(", ")));
        return false;
    }

    // Op "Add" | "Delete" | "Enable" | "Move". Index counts modules within the stage, absent appends.
    class FNiagaraModuleWriter : public INiagaraSystemWriter
    {
    public:
        virtual const TCHAR* GetSpecKey() const override
        {
            return TEXT("Modules");
        }

        virtual bool Apply(FNiagaraSystemEditContext& Context, const TSharedPtr<FJsonValue>& Section) override
        {
            return NiagaraSystemEdit::ForEachOp(Section, GetSpecKey(), [&](const TSharedPtr<FJsonObject>& Op)
            {
                FString OpName;
                Op->TryGetStringField(TEXT("Op"), OpName);

                if (OpName == TEXT("Add"))
                {
                    return ApplyAdd(Context, Op);
                }
                if (OpName == TEXT("Delete"))
                {
                    return ApplyDelete(Context, Op);
                }
                if (OpName == TEXT("Enable"))
                {
                    return ApplyEnable(Context, Op);
                }
                if (OpName == TEXT("Move"))
                {
                    return ApplyMove(Context, Op);
                }

                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Modules op '%s' unknown. Expected Add, Delete, Enable, Move"), *OpName);
                return false;
            });
        }

    private:
        bool ApplyAdd(FNiagaraSystemEditContext& Context, const TSharedPtr<FJsonObject>& Op)
        {
            FNiagaraStackStage Stage;
            if (!NiagaraSystemEdit::ResolveStage(Context, Op, Stage))
            {
                return false;
            }

            FString ScriptPath;
            if (!Op->TryGetStringField(TEXT("Script"), ScriptPath))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Add needs a Script asset path"));
                return false;
            }
            UNiagaraScript* Script = LoadObject<UNiagaraScript>(nullptr, *ScriptPath);
            if (!Script)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to load module script: %s"), *ScriptPath);
                return false;
            }

            int32 Index = INDEX_NONE;
            Op->TryGetNumberField(TEXT("Index"), Index);

            UNiagaraNodeFunctionCall* Module = FNiagaraStackGraphUtilities::AddScriptModuleToStack(Script, *Stage.OutputNode, Index);
            if (!Module)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to add %s to stage %s"), *ScriptPath, *Stage.Name);
                return false;
            }

            FString Id;
            if (Op->TryGetStringField(TEXT("Id"), Id))
            {
                Context.FunctionNamesById.Add(Id, Module->GetFunctionName());
            }

            ++Context.Ops;
            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Added module %s to %s"), *Module->GetFunctionName(), *Stage.Name);
            return true;
        }

        bool ApplyDelete(FNiagaraSystemEditContext& Context, const TSharedPtr<FJsonObject>& Op)
        {
            FNiagaraStackStage Stage;
            UNiagaraNodeFunctionCall* Module = nullptr;
            if (!FindModule(Context, Op, Stage, Module))
            {
                return false;
            }

            FModuleGroup Group;
            if (!FindModuleGroup(*Module, Group))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s is not on the parameter map chain of %s"), *Module->GetFunctionName(), *Stage.Name);
                return false;
            }

            const FString Name = Module->GetFunctionName();
            DisconnectGroup(*Module, Group);

            TArray<UNiagaraNode*> GroupNodes;
            CollectGroupNodes(*Module, GroupNodes);
            UNiagaraGraph* Graph = Module->GetNiagaraGraph();
            Graph->Modify();
            for (UNiagaraNode* Node : GroupNodes)
            {
                Node->Modify();
                Graph->RemoveNode(Node);
            }
            NiagaraSystemEdit::MarkChanged(Stage.OutputNode, TEXT("Module removed"));

            ++Context.Ops;
            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Deleted module %s from %s (%d node(s))"), *Name, *Stage.Name, GroupNodes.Num());
            return true;
        }

        bool ApplyEnable(FNiagaraSystemEditContext& Context, const TSharedPtr<FJsonObject>& Op)
        {
            FNiagaraStackStage Stage;
            UNiagaraNodeFunctionCall* Module = nullptr;
            if (!FindModule(Context, Op, Stage, Module))
            {
                return false;
            }

            bool bEnabled = true;
            if (!Op->TryGetBoolField(TEXT("Enabled"), bEnabled))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Enable needs an Enabled bool"));
                return false;
            }

            if (Module->IsNodeEnabled() == bEnabled)
            {
                ++Context.Unchanged;
                return true;
            }

            FNiagaraStackGraphUtilities::SetModuleIsEnabled(*Module, bEnabled);
            ++Context.Ops;
            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s module %s in %s"), bEnabled ? TEXT("Enabled") : TEXT("Disabled"), *Module->GetFunctionName(), *Stage.Name);
            return true;
        }

        bool ApplyMove(FNiagaraSystemEditContext& Context, const TSharedPtr<FJsonObject>& Op)
        {
            FNiagaraStackStage Stage;
            UNiagaraNodeFunctionCall* Module = nullptr;
            if (!FindModule(Context, Op, Stage, Module))
            {
                return false;
            }

            int32 Index = INDEX_NONE;
            if (!Op->TryGetNumberField(TEXT("Index"), Index) || Index < 0)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Move needs a non-negative Index"));
                return false;
            }

            FModuleGroup Group;
            if (!FindModuleGroup(*Module, Group))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s is not on the parameter map chain of %s"), *Module->GetFunctionName(), *Stage.Name);
                return false;
            }
            DisconnectGroup(*Module, Group);

            // Index counts the stage without the moved module, the same way the stack drop target does.
            TArray<UNiagaraNodeFunctionCall*> Remaining;
            NiagaraStackJson::GetOrderedModules(*Stage.OutputNode, Remaining);
            const int32 Clamped = FMath::Min(Index, Remaining.Num());
            UNiagaraNode* NewPrevious = Clamped == 0 ? NiagaraStackJson::FindStageStartNode(*Stage.OutputNode) : Remaining[Clamped - 1];

            TArray<UNiagaraNode*> NewNext;
            for (UEdGraphPin* Linked : NiagaraStackJson::GetParameterMapOutputPin(*NewPrevious)->LinkedTo)
            {
                NewNext.Add(CastChecked<UNiagaraNode>(Linked->GetOwningNode()));
            }

            BreakMapOutput(*NewPrevious);
            LinkMapChain(*NewPrevious, Group.StartNodes);
            LinkMapChain(*Module, NewNext);
            NiagaraSystemEdit::MarkChanged(Module, TEXT("Module moved"));

            ++Context.Ops;
            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Moved module %s to index %d in %s"), *Module->GetFunctionName(), Clamped, *Stage.Name);
            return true;
        }
    };
}

TUniquePtr<INiagaraSystemWriter> MakeNiagaraModuleWriter()
{
    return MakeUnique<FNiagaraModuleWriter>();
}
