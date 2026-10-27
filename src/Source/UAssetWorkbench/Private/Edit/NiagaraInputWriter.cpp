#include "Edit/NiagaraSystemWriter.h"
#include "Export/NiagaraStackJsonSerializer.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphPin.h"

#include "NiagaraDataInterface.h"
#include "NiagaraDataInterfaceCurveBase.h"
#include "NiagaraNode.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraScript.h"
#include "ViewModels/Stack/NiagaraParameterHandle.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"

namespace
{
    struct FInputTarget
    {
        FNiagaraStackStage Stage;
        UNiagaraNodeFunctionCall* FunctionCall = nullptr;
        FNiagaraStackInput Input;
    };

    void RemoveRapidIterationValue(const FInputTarget& Target)
    {
        const FNiagaraVariable Parameter = NiagaraStackJson::MakeRapidIterationVariable(Target.Stage, Target.Input);
        TArray<UNiagaraScript*> Scripts;
        NiagaraStackJson::GetAffectedScripts(Target.Stage.System, Target.Stage, Scripts);
        for (UNiagaraScript* Script : Scripts)
        {
            if (Script && Script->RapidIterationParameters.IndexOf(Parameter) != INDEX_NONE)
            {
                Script->Modify();
                Script->RapidIterationParameters.RemoveParameter(Parameter);
            }
        }
    }

    void WriteRapidIterationValue(const FInputTarget& Target, const FNiagaraVariable& Value)
    {
        const FNiagaraVariable Parameter = NiagaraStackJson::MakeRapidIterationVariable(Target.Stage, Target.Input);
        TArray<UNiagaraScript*> Scripts;
        NiagaraStackJson::GetAffectedScripts(Target.Stage.System, Target.Stage, Scripts);
        for (UNiagaraScript* Script : Scripts)
        {
            if (Script)
            {
                Script->Modify();
                Script->RapidIterationParameters.SetParameterData(Value.GetData(), Parameter, /* bAdd */ true);
            }
        }
    }

    // Drops whatever currently drives the input. bKeepPin leaves an empty override pin for the next value mode.
    void ClearOverride(const FInputTarget& Target, bool bKeepPin)
    {
        UEdGraphPin* OverridePin = NiagaraStackJson::FindOverridePin(*Target.FunctionCall, Target.Input.AliasedName);
        if (OverridePin)
        {
            NiagaraSystemEdit::RemoveNodesFeedingPin(*OverridePin);
            if (!bKeepPin)
            {
                UEdGraphNode* OverrideNode = OverridePin->GetOwningNode();
                OverrideNode->Modify();
                OverrideNode->RemovePin(OverridePin);
                NiagaraSystemEdit::MarkChanged(Cast<UNiagaraNode>(OverrideNode), TEXT("Override pin removed"));
            }
        }
        if (Target.Input.bRapidIteration)
        {
            RemoveRapidIterationValue(Target);
        }
    }

    UEdGraphPin& GetEmptyOverridePin(const FInputTarget& Target)
    {
        ClearOverride(Target, /* bKeepPin */ true);
        const FNiagaraParameterHandle Handle(Target.Input.AliasedName);
        return FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(*Target.FunctionCall, Handle, Target.Input.Variable.GetType(), FGuid(), FGuid());
    }

    // Exactly one value field per op: Value, Link, DynamicInput, DataInterface, Object, or Reset.
    class FNiagaraInputWriter : public INiagaraSystemWriter
    {
    public:
        virtual const TCHAR* GetSpecKey() const override
        {
            return TEXT("Inputs");
        }

        virtual bool Apply(FNiagaraSystemEditContext& Context, const TSharedPtr<FJsonValue>& Section) override
        {
            return NiagaraSystemEdit::ForEachOp(Section, GetSpecKey(), [&](const TSharedPtr<FJsonObject>& Op)
            {
                FInputTarget Target;
                if (!ResolveTarget(Context, Op, Target))
                {
                    return false;
                }

                // Relinking or rewriting an identical value would rebuild nodes for nothing, a fed-back export stays quiet.
                if (IsUnchanged(Target, Op))
                {
                    ++Context.Unchanged;
                    return true;
                }

                bool bOk = false;
                FString Text;
                const TSharedPtr<FJsonObject>* DataInterfaceSpec = nullptr;
                bool bReset = false;
                if (Op->TryGetStringField(TEXT("Value"), Text))
                {
                    bOk = ApplyValue(Target, Text);
                }
                else if (Op->TryGetStringField(TEXT("Link"), Text))
                {
                    bOk = ApplyLink(Target, Text);
                }
                else if (Op->TryGetStringField(TEXT("DynamicInput"), Text))
                {
                    bOk = ApplyDynamicInput(Context, Op, Target, Text);
                }
                else if (Op->TryGetObjectField(TEXT("DataInterface"), DataInterfaceSpec))
                {
                    bool bWrote = false;
                    bOk = ApplyDataInterface(Context, Target, *DataInterfaceSpec, bWrote);
                    if (bOk && !bWrote)
                    {
                        return true;
                    }
                }
                else if (Op->TryGetStringField(TEXT("Object"), Text))
                {
                    bOk = ApplyObject(Target, Text);
                }
                else if (Op->TryGetBoolField(TEXT("Reset"), bReset) && bReset)
                {
                    ClearOverride(Target, /* bKeepPin */ false);
                    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Reset %s.%s to the module default"), *Target.FunctionCall->GetFunctionName(), *Target.Input.DisplayName);
                    bOk = true;
                }
                else
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Inputs op for %s needs one of Value, Link, DynamicInput, DataInterface, Object, Reset"), *Target.Input.DisplayName);
                }

                if (bOk)
                {
                    NiagaraSystemEdit::MarkChanged(Target.FunctionCall, TEXT("Module input changed"));
                    ++Context.Ops;
                }
                return bOk;
            });
        }

    private:
        static bool ResolveTarget(const FNiagaraSystemEditContext& Context, const TSharedPtr<FJsonObject>& Op, FInputTarget& OutTarget)
        {
            FString ModuleName;
            FString InputName;
            if (!Op->TryGetStringField(TEXT("Module"), ModuleName) || !Op->TryGetStringField(TEXT("Input"), InputName))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Inputs op needs Module and Input"));
                return false;
            }
            if (!NiagaraSystemEdit::ResolveStage(Context, Op, OutTarget.Stage))
            {
                return false;
            }
            OutTarget.FunctionCall = NiagaraSystemEdit::ResolveFunctionCall(Context, OutTarget.Stage, ModuleName);
            return OutTarget.FunctionCall && NiagaraSystemEdit::ResolveInput(OutTarget.Stage, *OutTarget.FunctionCall, InputName, OutTarget.Input);
        }

        static bool ApplyValue(const FInputTarget& Target, const FString& Text)
        {
            FNiagaraVariable Value(Target.Input.Variable.GetType(), Target.Input.AliasedName);
            Value.AllocateData();
            if (!NiagaraStackJson::StringToValue(Text, Value))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("'%s' does not parse as %s for %s"), *Text, *Value.GetType().GetName(), *Target.Input.DisplayName);
                return false;
            }

            if (Target.Input.bRapidIteration)
            {
                ClearOverride(Target, /* bKeepPin */ false);
                WriteRapidIterationValue(Target, Value);
            }
            else
            {
                // Bools and enums are not rapid iteration types, the value lives as the override pin default.
                UEdGraphPin& OverridePin = GetEmptyOverridePin(Target);
                OverridePin.Modify();
                OverridePin.DefaultValue = NiagaraStackJson::ValueToString(Value);
                NiagaraSystemEdit::MarkChanged(Cast<UNiagaraNode>(OverridePin.GetOwningNode()), TEXT("Override pin value changed"));
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Set %s.%s = %s"), *Target.FunctionCall->GetFunctionName(), *Target.Input.DisplayName, *Text);
            return true;
        }

        static bool ApplyLink(const FInputTarget& Target, const FString& ParameterName)
        {
            const FNiagaraVariableBase Linked(Target.Input.Variable.GetType(), FName(*ParameterName));
            TSet<FNiagaraVariableBase> KnownParameters;
            KnownParameters.Add(Linked);

            UEdGraphPin& OverridePin = GetEmptyOverridePin(Target);
            FNiagaraStackGraphUtilities::SetLinkedParameterValueForFunctionInput(OverridePin, Linked, KnownParameters);

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Linked %s.%s to %s"), *Target.FunctionCall->GetFunctionName(), *Target.Input.DisplayName, *ParameterName);
            return true;
        }

        static bool ApplyDynamicInput(FNiagaraSystemEditContext& Context, const TSharedPtr<FJsonObject>& Op, const FInputTarget& Target, const FString& ScriptPath)
        {
            UNiagaraScript* Script = LoadObject<UNiagaraScript>(nullptr, *ScriptPath);
            if (!Script || Script->GetUsage() != ENiagaraScriptUsage::DynamicInput)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s is not a dynamic input script"), *ScriptPath);
                return false;
            }

            UEdGraphPin& OverridePin = GetEmptyOverridePin(Target);
            UNiagaraNodeFunctionCall* DynamicInput = nullptr;
            FNiagaraStackGraphUtilities::SetDynamicInputForFunctionInput(OverridePin, Script, DynamicInput);
            if (!DynamicInput)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to set dynamic input %s on %s"), *ScriptPath, *Target.Input.DisplayName);
                return false;
            }

            FString Id;
            if (Op->TryGetStringField(TEXT("Id"), Id))
            {
                Context.FunctionNamesById.Add(Id, DynamicInput->GetFunctionName());
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Set %s.%s to dynamic input %s"), *Target.FunctionCall->GetFunctionName(), *Target.Input.DisplayName, *DynamicInput->GetFunctionName());
            return true;
        }

        static bool IsUnchanged(const FInputTarget& Target, const TSharedPtr<FJsonObject>& Op)
        {
            UEdGraphPin* OverridePin = NiagaraStackJson::FindOverridePin(*Target.FunctionCall, Target.Input.AliasedName);
            const UEdGraphPin* Linked = OverridePin && OverridePin->LinkedTo.Num() == 1 ? OverridePin->LinkedTo[0] : nullptr;
            const UNiagaraNode* LinkedNode = Linked ? Cast<UNiagaraNode>(Linked->GetOwningNode()) : nullptr;

            FString Text;
            if (Op->TryGetStringField(TEXT("Value"), Text))
            {
                FNiagaraVariable Value(Target.Input.Variable.GetType(), Target.Input.AliasedName);
                Value.AllocateData();
                FString Current;
                const bool bParsed = NiagaraStackJson::StringToValue(Text, Value);
                return bParsed && NiagaraStackJson::ReadLocalValue(Target.Stage, *Target.FunctionCall, Target.Input, Current) && Current == NiagaraStackJson::ValueToString(Value);
            }
            if (Op->TryGetStringField(TEXT("Link"), Text))
            {
                return NiagaraStackJson::IsNodeOfClass(LinkedNode, TEXT("NiagaraNodeParameterMapGet")) && Linked->GetName() == Text;
            }
            if (Op->TryGetStringField(TEXT("Object"), Text))
            {
                const UObject* Current = NiagaraStackJson::GetObjectAsset(LinkedNode);
                return Current && Current->GetPathName() == Text;
            }
            bool bReset = false;
            if (Op->TryGetBoolField(TEXT("Reset"), bReset) && bReset)
            {
                return NiagaraStackJson::ResolveInputMode(Target.Stage, *Target.FunctionCall, Target.Input) == ENiagaraStackInputMode::Default;
            }
            return false;
        }

        static bool ApplyDataInterface(FNiagaraSystemEditContext& Context, const FInputTarget& Target, const TSharedPtr<FJsonObject>& Spec, bool& bOutWrote)
        {
            bOutWrote = true;
            UNiagaraDataInterface* DataInterface = nullptr;

            FString ClassName;
            if (Spec->TryGetStringField(TEXT("Class"), ClassName))
            {
                UClass* Class = UAssetWorkbench::ResolveClass(ClassName);
                if (!Class || !Class->IsChildOf(UNiagaraDataInterface::StaticClass()))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s is not a data interface class"), *ClassName);
                    return false;
                }
                UEdGraphPin& OverridePin = GetEmptyOverridePin(Target);
                FNiagaraStackGraphUtilities::SetDataInterfaceValueForFunctionInput(OverridePin, Class, Target.Input.AliasedName.ToString(), DataInterface);
            }
            else
            {
                // No Class edits the data interface already on the input.
                UEdGraphPin* OverridePin = NiagaraStackJson::FindOverridePin(*Target.FunctionCall, Target.Input.AliasedName);
                const UEdGraphPin* Linked = OverridePin && OverridePin->LinkedTo.Num() == 1 ? OverridePin->LinkedTo[0] : nullptr;
                DataInterface = Linked ? NiagaraStackJson::GetDataInterface(Cast<UNiagaraNode>(Linked->GetOwningNode())) : nullptr;
            }

            if (!DataInterface)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no data interface value. Give a Class to create one"), *Target.Input.DisplayName);
                return false;
            }

            const TSharedPtr<FJsonObject>* Properties = nullptr;
            if (Spec->TryGetObjectField(TEXT("Properties"), Properties))
            {
                const TSharedPtr<FJsonObject> Changed = NiagaraSystemEdit::DropUnchangedProperties(DataInterface, *Properties, Context.Unchanged);
                if (Changed->Values.Num() == 0 && !Spec->HasField(TEXT("Class")))
                {
                    bOutWrote = false;
                    return true;
                }

                int32 Failures = 0;
                DataInterface->Modify();
                UAssetWorkbench::ApplyProperties(DataInterface, Changed, Failures);
                if (Failures > 0)
                {
                    return false;
                }
            }

            // PostEditChange without a property leaves the curve lookup table on the old keys until the next load.
            if (UNiagaraDataInterfaceCurveBase* Curve = Cast<UNiagaraDataInterfaceCurveBase>(DataInterface))
            {
                Curve->UpdateLUT();
            }
            DataInterface->PostEditChange();
            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Wrote data interface %s on %s.%s"), *DataInterface->GetClass()->GetName(), *Target.FunctionCall->GetFunctionName(), *Target.Input.DisplayName);
            return true;
        }

        static bool ApplyObject(const FInputTarget& Target, const FString& AssetPath)
        {
            UClass* ObjectClass = Target.Input.Variable.GetType().GetClass();
            UObject* Asset = ObjectClass ? StaticLoadObject(ObjectClass, nullptr, *AssetPath) : nullptr;
            if (!Asset)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to load %s as the object type of %s"), *AssetPath, *Target.Input.DisplayName);
                return false;
            }

            UEdGraphPin& OverridePin = GetEmptyOverridePin(Target);
            FNiagaraStackGraphUtilities::SetObjectAssetValueForFunctionInput(OverridePin, ObjectClass, Target.Input.AliasedName.ToString(), Asset);
            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Set %s.%s to %s"), *Target.FunctionCall->GetFunctionName(), *Target.Input.DisplayName, *AssetPath);
            return true;
        }
    };
}

TUniquePtr<INiagaraSystemWriter> MakeNiagaraInputWriter()
{
    return MakeUnique<FNiagaraInputWriter>();
}
