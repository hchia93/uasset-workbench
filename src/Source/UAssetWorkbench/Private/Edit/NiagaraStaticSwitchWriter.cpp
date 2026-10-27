#include "Edit/NiagaraSystemWriter.h"
#include "Export/NiagaraStackJsonSerializer.h"
#include "UAssetWorkbenchModule.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphPin.h"

#include "NiagaraNodeFunctionCall.h"

namespace
{
    // One op per module, Switches keyed by the switch name that NiagaraSystemExport prints under StaticSwitches.
    class FNiagaraStaticSwitchWriter : public INiagaraSystemWriter
    {
    public:
        virtual const TCHAR* GetSpecKey() const override
        {
            return TEXT("StaticSwitches");
        }

        virtual bool Apply(FNiagaraSystemEditContext& Context, const TSharedPtr<FJsonValue>& Section) override
        {
            return NiagaraSystemEdit::ForEachOp(Section, GetSpecKey(), [&](const TSharedPtr<FJsonObject>& Op)
            {
                FNiagaraStackStage Stage;
                if (!NiagaraSystemEdit::ResolveStage(Context, Op, Stage))
                {
                    return false;
                }

                FString ModuleName;
                const TSharedPtr<FJsonObject>* Switches = nullptr;
                if (!Op->TryGetStringField(TEXT("Module"), ModuleName) || !Op->TryGetObjectField(TEXT("Switches"), Switches))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("StaticSwitches op needs Module and a Switches object"));
                    return false;
                }

                UNiagaraNodeFunctionCall* FunctionCall = NiagaraSystemEdit::ResolveFunctionCall(Context, Stage, ModuleName);
                if (!FunctionCall)
                {
                    return false;
                }

                for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Switches)->Values)
                {
                    if (!ApplySwitch(Context, Stage, *FunctionCall, Pair.Key, Pair.Value->AsString()))
                    {
                        return false;
                    }
                }
                return true;
            });
        }

    private:
        static bool ApplySwitch(FNiagaraSystemEditContext& Context, const FNiagaraStackStage& Stage, UNiagaraNodeFunctionCall& FunctionCall, const FString& SwitchName, const FString& Text)
        {
            // Collected per switch: setting one switch can reveal another in the same op.
            TArray<UEdGraphPin*> Pins;
            NiagaraStackJson::CollectStaticSwitchPins(Stage, FunctionCall, Pins);

            TArray<FString> Names;
            for (UEdGraphPin* Pin : Pins)
            {
                if (Pin->GetName() != SwitchName)
                {
                    Names.Add(Pin->GetName());
                    continue;
                }

                FString PinValue;
                if (!NiagaraStackJson::ResolveStaticSwitchValue(Pin, Text, PinValue))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("'%s' is not a value of switch %s on %s"), *Text, *SwitchName, *FunctionCall.GetFunctionName());
                    return false;
                }

                if (Pin->DefaultValue == PinValue)
                {
                    ++Context.Unchanged;
                    return true;
                }

                FunctionCall.Modify();
                Pin->Modify();
                Pin->DefaultValue = PinValue;
                NiagaraSystemEdit::MarkChanged(&FunctionCall, TEXT("Static switch changed"));
                ++Context.Ops;
                UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Set switch %s.%s = %s"), *FunctionCall.GetFunctionName(), *SwitchName, *Text);
                return true;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Static switch '%s' not visible on %s. Switches: %s"), *SwitchName, *FunctionCall.GetFunctionName(), *FString::Join(Names, TEXT(", ")));
            return false;
        }
    };
}

TUniquePtr<INiagaraSystemWriter> MakeNiagaraStaticSwitchWriter()
{
    return MakeUnique<FNiagaraStaticSwitchWriter>();
}
