#include "Edit/MaterialGraphWriter.h"
#include "Export/MaterialGraphJson.h"
#include "UAssetWorkbenchModule.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionNamedReroute.h"

namespace
{
    FString DescribeEnd(const FString& NodeKey, const FString& PinName)
    {
        return PinName.IsEmpty() ? NodeKey : FString::Printf(TEXT("%s.%s"), *NodeKey, *PinName);
    }

    void ClearInput(FExpressionInput& Input)
    {
        Input.Expression = nullptr;
        Input.OutputIndex = 0;
        Input.Mask = 0;
        Input.MaskR = 0;
        Input.MaskG = 0;
        Input.MaskB = 0;
        Input.MaskA = 0;
    }

    // Upstream walk from From. A reroute usage continues at its declaration, the wire the graph draws.
    bool DependsOn(UMaterialExpression* From, const UMaterialExpression* Target)
    {
        TArray<UMaterialExpression*> Stack = { From };
        TSet<UMaterialExpression*> Seen;
        while (Stack.Num() > 0)
        {
            UMaterialExpression* Current = Stack.Pop(EAllowShrinking::No);
            if (Current == Target)
            {
                return true;
            }
            if (!Current || Seen.Contains(Current))
            {
                continue;
            }
            Seen.Add(Current);

            for (FExpressionInputIterator It(Current); It; ++It)
            {
                Stack.Add(It->Expression);
            }
            if (const UMaterialExpressionNamedRerouteUsage* Usage = Cast<UMaterialExpressionNamedRerouteUsage>(Current))
            {
                Stack.Add(Usage->Declaration);
            }
        }
        return false;
    }

    // To is a node or the material output node. Returns the one input the edge lands on.
    FExpressionInput* ResolveTargetInput(FMaterialGraphEditContext& Context, const FString& ToKey, const FString& ToPin, UMaterialExpression*& OutTo)
    {
        OutTo = nullptr;
        if (ToKey == MaterialGraphEdit::kMaterialNodeKey)
        {
            if (!Context.Material)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("A material function has no '%s' node, connect to a FunctionOutput instead"), MaterialGraphEdit::kMaterialNodeKey);
                return nullptr;
            }

            FExpressionInput* Input = MaterialGraphJson::FindMaterialInput(Context.Material, ToPin);
            if (!Input)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Material has no pin '%s'. Pins: %s"), *ToPin, *MaterialGraphJson::DescribeMaterialInputs(Context.Material));
            }
            return Input;
        }

        OutTo = MaterialGraphEdit::ResolveNode(Context, ToKey);
        if (!OutTo)
        {
            return nullptr;
        }

        const int32 InputIndex = MaterialGraphJson::FindInputIndex(OutTo, ToPin);
        FExpressionInput* Input = InputIndex == INDEX_NONE ? nullptr : OutTo->GetInput(InputIndex);
        if (!Input)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no input '%s'. Pins: %s"), *ToKey, *ToPin, *MaterialGraphJson::DescribePins(OutTo));
        }
        return Input;
    }

    bool Connect(FMaterialGraphEditContext& Context, const TSharedPtr<FJsonObject>& Desc)
    {
        FString FromKey;
        FString ToKey;
        FString FromPin;
        FString ToPin;
        if (!Desc->TryGetStringField(TEXT("From"), FromKey) || !Desc->TryGetStringField(TEXT("To"), ToKey))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Edges Connect needs From and To"));
            return false;
        }
        Desc->TryGetStringField(TEXT("FromPin"), FromPin);
        Desc->TryGetStringField(TEXT("ToPin"), ToPin);

        UMaterialExpression* From = MaterialGraphEdit::ResolveNode(Context, FromKey);
        if (!From)
        {
            return false;
        }

        const int32 OutputIndex = MaterialGraphJson::FindOutputIndex(From, FromPin);
        if (OutputIndex == INDEX_NONE)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no output '%s'. Pins: %s"), *FromKey, *FromPin, *MaterialGraphJson::DescribePins(From));
            return false;
        }

        UMaterialExpression* To = nullptr;
        FExpressionInput* Input = ResolveTargetInput(Context, ToKey, ToPin, To);
        if (!Input)
        {
            return false;
        }

        // The compiler would reject a loop later with no hint which wire closed it.
        if (To && DependsOn(From, To))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s -> %s closes a loop, %s already feeds %s"), *FromKey, *ToKey, *ToKey, *FromKey);
            return false;
        }

        Input->Connect(OutputIndex, From);
        ++Context.Ops;
        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Connected %s -> %s"), *DescribeEnd(FromKey, FromPin), *DescribeEnd(ToKey, ToPin));
        return true;
    }

    bool Disconnect(FMaterialGraphEditContext& Context, const TSharedPtr<FJsonObject>& Desc)
    {
        FString ToKey;
        FString ToPin;
        if (!Desc->TryGetStringField(TEXT("To"), ToKey))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Edges Disconnect needs To"));
            return false;
        }
        Desc->TryGetStringField(TEXT("ToPin"), ToPin);

        UMaterialExpression* To = nullptr;
        FExpressionInput* Input = ResolveTargetInput(Context, ToKey, ToPin, To);
        if (!Input)
        {
            return false;
        }

        ClearInput(*Input);
        ++Context.Ops;
        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Disconnected %s"), *DescribeEnd(ToKey, ToPin));
        return true;
    }

    bool DisconnectAll(FMaterialGraphEditContext& Context, const TSharedPtr<FJsonObject>& Desc)
    {
        FString NodeKey;
        if (!Desc->TryGetStringField(TEXT("Node"), NodeKey))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Edges DisconnectAll needs Node"));
            return false;
        }

        UMaterialExpression* Node = MaterialGraphEdit::ResolveNode(Context, NodeKey);
        if (!Node)
        {
            return false;
        }

        int32 Cleared = 0;
        for (FExpressionInputIterator It(Node); It; ++It)
        {
            if (It->Expression)
            {
                ClearInput(*It.Input);
                ++Cleared;
            }
        }
        for (UMaterialExpression* Other : MaterialGraphJson::GetExpressions(Context.Material, Context.Function))
        {
            if (!Other)
            {
                continue;
            }
            for (FExpressionInputIterator It(Other); It; ++It)
            {
                if (It->Expression == Node)
                {
                    ClearInput(*It.Input);
                    ++Cleared;
                }
            }
        }
        MaterialGraphJson::ForEachMaterialInput(Context.Material, [Node, &Cleared](const FString& PinName, FExpressionInput& Input)
        {
            if (Input.Expression == Node)
            {
                ClearInput(Input);
                ++Cleared;
            }
        });

        Context.Ops += Cleared;
        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Disconnected %d wire(s) at %s"), Cleared, *NodeKey);
        return true;
    }

    class FMaterialGraphEdgeWriter : public IMaterialGraphWriter
    {
    public:
        virtual const TCHAR* GetSpecKey() const override
        {
            return TEXT("Edges");
        }

        virtual bool Apply(FMaterialGraphEditContext& Context, const TSharedPtr<FJsonValue>& Section) override
        {
            const TArray<TSharedPtr<FJsonValue>>* Ops = nullptr;
            if (!Section->TryGetArray(Ops))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Edges must be an array of operations"));
                return false;
            }

            for (const TSharedPtr<FJsonValue>& Value : *Ops)
            {
                const TSharedPtr<FJsonObject> Desc = Value->AsObject();
                if (!Desc.IsValid())
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Edges entry is not an object"));
                    return false;
                }

                FString Op = TEXT("Connect");
                Desc->TryGetStringField(TEXT("Op"), Op);

                bool bOk = false;
                if (Op == TEXT("Connect"))
                {
                    bOk = Connect(Context, Desc);
                }
                else if (Op == TEXT("Disconnect"))
                {
                    bOk = Disconnect(Context, Desc);
                }
                else if (Op == TEXT("DisconnectAll"))
                {
                    bOk = DisconnectAll(Context, Desc);
                }
                else
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Unknown Edges op '%s'. Expected Connect, Disconnect, DisconnectAll"), *Op);
                }

                if (!bOk)
                {
                    return false;
                }
            }
            return true;
        }
    };
}

TUniquePtr<IMaterialGraphWriter> MakeMaterialGraphEdgeWriter()
{
    return MakeUnique<FMaterialGraphEdgeWriter>();
}
