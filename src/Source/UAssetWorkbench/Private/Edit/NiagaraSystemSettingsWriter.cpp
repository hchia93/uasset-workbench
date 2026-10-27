#include "Edit/NiagaraSystemWriter.h"
#include "Export/NiagaraStackJsonSerializer.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "NiagaraParameterStore.h"
#include "NiagaraSystem.h"

namespace
{
    // { "Properties": { UNiagaraSystem fields }, "UserParameters": { "User.Name": "value or asset path" } }
    class FNiagaraSystemSettingsWriter : public INiagaraSystemWriter
    {
    public:
        virtual const TCHAR* GetSpecKey() const override
        {
            return TEXT("System");
        }

        virtual bool Apply(FNiagaraSystemEditContext& Context, const TSharedPtr<FJsonValue>& Section) override
        {
            const TSharedPtr<FJsonObject>* Settings = nullptr;
            if (!Section->TryGetObject(Settings))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("System must be an object with Properties and / or UserParameters"));
                return false;
            }

            const TSharedPtr<FJsonObject>* Properties = nullptr;
            const TSharedPtr<FJsonObject> Changed = (*Settings)->TryGetObjectField(TEXT("Properties"), Properties) ? NiagaraSystemEdit::DropUnchangedProperties(Context.System, *Properties, Context.Unchanged) : nullptr;
            if (Changed.IsValid() && Changed->Values.Num() > 0)
            {
                int32 Failures = 0;
                Context.System->Modify();
                const int32 Applied = UAssetWorkbench::ApplyProperties(Context.System, Changed, Failures);
                if (Failures > 0)
                {
                    return false;
                }
                Context.Ops += Applied;
                UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Set %d system propert%s"), Applied, Applied == 1 ? TEXT("y") : TEXT("ies"));
            }

            const TSharedPtr<FJsonObject>* UserParameters = nullptr;
            if ((*Settings)->TryGetObjectField(TEXT("UserParameters"), UserParameters))
            {
                for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*UserParameters)->Values)
                {
                    if (!ApplyUserParameter(Context, Pair.Key, Pair.Value->AsString()))
                    {
                        return false;
                    }
                }
            }
            return true;
        }

    private:
        static bool ApplyUserParameter(FNiagaraSystemEditContext& Context, const FString& Name, const FString& Text)
        {
            FNiagaraUserRedirectionParameterStore& Store = Context.System->GetExposedParameters();

            TArray<FString> Names;
            for (const FNiagaraVariableWithOffset& Var : Store.ReadParameterVariables())
            {
                if (Var.GetName().ToString() != Name)
                {
                    Names.Add(Var.GetName().ToString());
                    continue;
                }

                const FNiagaraTypeDefinition& Type = Var.GetType();
                if (Type.IsDataInterface())
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s is a data interface user parameter, not editable through UserParameters"), *Name);
                    return false;
                }

                if (Type.IsUObject())
                {
                    UObject* Asset = Type.GetClass() ? StaticLoadObject(Type.GetClass(), nullptr, *Text) : nullptr;
                    if (!Asset)
                    {
                        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to load %s for user parameter %s"), *Text, *Name);
                        return false;
                    }
                    if (Store.GetUObject(Var) == Asset)
                    {
                        ++Context.Unchanged;
                        return true;
                    }
                    Context.System->Modify();
                    Store.SetUObject(Asset, Var);
                }
                else
                {
                    FNiagaraVariable Value(Var);
                    Value.AllocateData();
                    if (!NiagaraStackJson::StringToValue(Text, Value))
                    {
                        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("'%s' does not parse as %s for %s"), *Text, *Type.GetName(), *Name);
                        return false;
                    }

                    const uint8* CurrentData = Store.GetParameterData(Var);
                    if (CurrentData && FMemory::Memcmp(CurrentData, Value.GetData(), Type.GetSize()) == 0)
                    {
                        ++Context.Unchanged;
                        return true;
                    }
                    Context.System->Modify();
                    Store.SetParameterData(Value.GetData(), Value);
                }

                ++Context.Ops;
                UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Set user parameter %s = %s"), *Name, *Text);
                return true;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("User parameter '%s' not found. User parameters: %s"), *Name, *FString::Join(Names, TEXT(", ")));
            return false;
        }
    };
}

TUniquePtr<INiagaraSystemWriter> MakeNiagaraSystemSettingsWriter()
{
    return MakeUnique<FNiagaraSystemSettingsWriter>();
}
