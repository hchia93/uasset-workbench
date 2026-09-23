#include "Edit/PCGGraphWriter.h"
#include "UAssetWorkbenchModule.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "PCGGraph.h"
#include "StructUtils/PropertyBag.h"

namespace
{
    bool ParseBagType(const FString& TypeName, EPropertyBagPropertyType& OutType)
    {
        const UEnum* Enum = StaticEnum<EPropertyBagPropertyType>();
        const int64 Value = Enum ? Enum->GetValueByNameString(TypeName) : INDEX_NONE;
        if (Value == INDEX_NONE)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Unknown parameter Type '%s'. Expected Bool, Byte, Int32, Int64, Float, Double, Name, String, Text, Enum, Struct, Object, SoftObject, Class, SoftClass"), *TypeName);
            return false;
        }
        OutType = static_cast<EPropertyBagPropertyType>(Value);
        return true;
    }

    bool SetValue(FPCGGraphEditContext& Context, const FName Name, const FString& Value)
    {
        EPropertyBagResult Result = EPropertyBagResult::PropertyNotFound;
        Context.Graph->UpdateUserParametersStruct([&Name, &Value, &Result](FInstancedPropertyBag& Bag)
        {
            Result = Bag.SetValueSerializedString(Name, Value);
        });

        if (Result != EPropertyBagResult::Success)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to set parameter %s = %s (%s)"), *Name.ToString(), *Value, *StaticEnum<EPropertyBagResult>()->GetNameStringByValue(static_cast<int64>(Result)));
            return false;
        }
        return true;
    }

    bool AddParameter(FPCGGraphEditContext& Context, const TSharedPtr<FJsonObject>& Desc, const FName Name)
    {
        FString TypeName = TEXT("Double");
        Desc->TryGetStringField(TEXT("Type"), TypeName);
        EPropertyBagPropertyType Type = EPropertyBagPropertyType::Double;
        if (!ParseBagType(TypeName, Type))
        {
            return false;
        }

        // Enum, Struct, Object and Class types carry the enum / struct / class they hold.
        const UObject* TypeObject = nullptr;
        FString TypeObjectPath;
        if (Desc->TryGetStringField(TEXT("TypeObject"), TypeObjectPath))
        {
            TypeObject = LoadObject<UObject>(nullptr, *TypeObjectPath);
            if (!TypeObject)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("TypeObject not found: %s"), *TypeObjectPath);
                return false;
            }
        }

        TArray<FPropertyBagPropertyDesc> Descs;
        Descs.Add(FPropertyBagPropertyDesc(Name, Type, TypeObject));
        const EPropertyBagAlterationResult Result = Context.Graph->AddUserParameters(Descs);
        if (Result != EPropertyBagAlterationResult::Success)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to add parameter %s (%s)"), *Name.ToString(), *StaticEnum<EPropertyBagAlterationResult>()->GetNameStringByValue(static_cast<int64>(Result)));
            return false;
        }

        FString Value;
        if (Desc->TryGetStringField(TEXT("Value"), Value) && !SetValue(Context, Name, Value))
        {
            return false;
        }

        ++Context.Ops;
        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Added parameter %s (%s)"), *Name.ToString(), *TypeName);
        return true;
    }

    bool RemoveParameter(FPCGGraphEditContext& Context, const FName Name)
    {
        EPropertyBagAlterationResult Result = EPropertyBagAlterationResult::TargetPropertyNotFound;
        Context.Graph->UpdateUserParametersStruct([&Name, &Result](FInstancedPropertyBag& Bag)
        {
            Result = Bag.RemovePropertyByName(Name);
        });

        if (Result != EPropertyBagAlterationResult::Success)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to remove parameter %s (%s)"), *Name.ToString(), *StaticEnum<EPropertyBagAlterationResult>()->GetNameStringByValue(static_cast<int64>(Result)));
            return false;
        }

        ++Context.Ops;
        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Removed parameter %s"), *Name.ToString());
        return true;
    }

    bool RenameParameter(FPCGGraphEditContext& Context, const FName Name, const FName NewName)
    {
        const EPropertyBagAlterationResult Result = Context.Graph->RenameUserParameter(Name, NewName);
        if (Result != EPropertyBagAlterationResult::Success)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to rename parameter %s -> %s (%s)"), *Name.ToString(), *NewName.ToString(), *StaticEnum<EPropertyBagAlterationResult>()->GetNameStringByValue(static_cast<int64>(Result)));
            return false;
        }

        ++Context.Ops;
        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Renamed parameter %s -> %s"), *Name.ToString(), *NewName.ToString());
        return true;
    }

    // Runs first so a parameter getter node added in the same spec finds its parameter.
    class FPCGGraphParameterWriter : public IPCGGraphWriter
    {
    public:
        virtual const TCHAR* GetSpecKey() const override
        {
            return TEXT("Parameters");
        }

        virtual bool Apply(FPCGGraphEditContext& Context, const TSharedPtr<FJsonValue>& Section) override
        {
            const TArray<TSharedPtr<FJsonValue>>* Ops = nullptr;
            if (!Section->TryGetArray(Ops))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Parameters must be an array of operations"));
                return false;
            }

            for (const TSharedPtr<FJsonValue>& Value : *Ops)
            {
                const TSharedPtr<FJsonObject> Desc = Value->AsObject();
                FString NameString;
                if (!Desc.IsValid() || !Desc->TryGetStringField(TEXT("Name"), NameString))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Parameters entry needs a Name"));
                    return false;
                }
                const FName Name(*NameString);

                FString Op = TEXT("Set");
                Desc->TryGetStringField(TEXT("Op"), Op);

                if (Op == TEXT("Add"))
                {
                    if (!AddParameter(Context, Desc, Name))
                    {
                        return false;
                    }
                }
                else if (Op == TEXT("Set"))
                {
                    FString ValueString;
                    if (!Desc->TryGetStringField(TEXT("Value"), ValueString))
                    {
                        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Set on parameter %s needs a Value"), *NameString);
                        return false;
                    }
                    if (!SetValue(Context, Name, ValueString))
                    {
                        return false;
                    }
                    ++Context.Ops;
                    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Set parameter %s = %s"), *NameString, *ValueString);
                }
                else if (Op == TEXT("Remove"))
                {
                    if (!RemoveParameter(Context, Name))
                    {
                        return false;
                    }
                }
                else if (Op == TEXT("Rename"))
                {
                    FString NewName;
                    if (!Desc->TryGetStringField(TEXT("NewName"), NewName))
                    {
                        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Rename on parameter %s needs a NewName"), *NameString);
                        return false;
                    }
                    if (!RenameParameter(Context, Name, FName(*NewName)))
                    {
                        return false;
                    }
                }
                else
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Unknown Parameters op '%s'. Expected Add, Set, Remove, Rename"), *Op);
                    return false;
                }
            }
            return true;
        }
    };
}

TUniquePtr<IPCGGraphWriter> MakePCGGraphParameterWriter()
{
    return MakeUnique<FPCGGraphParameterWriter>();
}
