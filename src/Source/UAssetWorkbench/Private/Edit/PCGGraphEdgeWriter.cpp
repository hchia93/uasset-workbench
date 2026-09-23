#include "Edit/PCGGraphWriter.h"
#include "UAssetWorkbenchModule.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "PCGEdge.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"

namespace
{
    struct FEdgeEnds
    {
        UPCGNode* From = nullptr;
        UPCGNode* To = nullptr;
        FString FromPin;
        FString ToPin;
    };

    // An omitted label means the data pin. Every node also carries Execution Dependency, Overrides and one
    // pin per overridable parameter, so "the only pin" is never literally true.
    FString PrimaryPinLabel(const TArray<TObjectPtr<UPCGPin>>& Pins)
    {
        FString Label;
        int32 Candidates = 0;
        for (const UPCGPin* Pin : Pins)
        {
            if (!Pin || Pin->Properties.IsDatalessPin() || Pin->Properties.IsAdvancedPin())
            {
                continue;
            }
            ++Candidates;
            if (Label.IsEmpty())
            {
                Label = Pin->Properties.Label.ToString();
            }
        }
        return Candidates == 1 ? Label : FString();
    }

    bool ReadEnds(const FPCGGraphEditContext& Context, const TSharedPtr<FJsonObject>& Desc, FEdgeEnds& OutEnds)
    {
        FString FromKey;
        FString ToKey;
        if (!Desc->TryGetStringField(TEXT("From"), FromKey) || !Desc->TryGetStringField(TEXT("To"), ToKey))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Edge needs From and To"));
            return false;
        }
        Desc->TryGetStringField(TEXT("FromPin"), OutEnds.FromPin);
        Desc->TryGetStringField(TEXT("ToPin"), OutEnds.ToPin);

        OutEnds.From = PCGGraphEdit::ResolveNode(Context, FromKey);
        OutEnds.To = PCGGraphEdit::ResolveNode(Context, ToKey);
        if (!OutEnds.From || !OutEnds.To)
        {
            return false;
        }

        if (OutEnds.FromPin.IsEmpty())
        {
            OutEnds.FromPin = PrimaryPinLabel(OutEnds.From->GetOutputPins());
        }
        if (OutEnds.ToPin.IsEmpty())
        {
            OutEnds.ToPin = PrimaryPinLabel(OutEnds.To->GetInputPins());
        }
        return true;
    }

    bool Connect(FPCGGraphEditContext& Context, const FEdgeEnds& Ends)
    {
        UPCGPin* FromPin = PCGGraphEdit::FindOutputPin(Ends.From, Ends.FromPin);
        if (!FromPin)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("No output pin '%s' on %s: %s"), *Ends.FromPin, *Ends.From->GetName(), *PCGGraphEdit::DescribePins(Ends.From));
            return false;
        }
        UPCGPin* ToPin = PCGGraphEdit::FindInputPin(Ends.To, Ends.ToPin);
        if (!ToPin)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("No input pin '%s' on %s: %s"), *Ends.ToPin, *Ends.To->GetName(), *PCGGraphEdit::DescribePins(Ends.To));
            return false;
        }
        if (!FromPin->CanConnect(ToPin))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s.%s (%s) cannot connect to %s.%s (%s)"), *Ends.From->GetName(), *Ends.FromPin, *FromPin->GetCurrentTypesID().ToString(), *Ends.To->GetName(), *Ends.ToPin, *ToPin->Properties.AllowedTypes.ToString());
            return false;
        }

        // Return value reports whether a single-connection pin dropped an older edge, not success.
        Context.Graph->AddLabeledEdge(Ends.From, FName(*Ends.FromPin), Ends.To, FName(*Ends.ToPin));

        bool bLinked = false;
        for (const UPCGEdge* Edge : FromPin->Edges)
        {
            bLinked |= Edge && Edge->GetOtherPin(FromPin) == ToPin;
        }
        if (!bLinked)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Graph refused edge %s.%s -> %s.%s"), *Ends.From->GetName(), *Ends.FromPin, *Ends.To->GetName(), *Ends.ToPin);
            return false;
        }

        ++Context.Ops;
        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Connected %s.%s -> %s.%s"), *Ends.From->GetName(), *Ends.FromPin, *Ends.To->GetName(), *Ends.ToPin);
        return true;
    }

    bool Disconnect(FPCGGraphEditContext& Context, const FEdgeEnds& Ends)
    {
        if (!Context.Graph->RemoveEdge(Ends.From, FName(*Ends.FromPin), Ends.To, FName(*Ends.ToPin)))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("No edge %s.%s -> %s.%s"), *Ends.From->GetName(), *Ends.FromPin, *Ends.To->GetName(), *Ends.ToPin);
            return false;
        }

        ++Context.Ops;
        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Disconnected %s.%s -> %s.%s"), *Ends.From->GetName(), *Ends.FromPin, *Ends.To->GetName(), *Ends.ToPin);
        return true;
    }

    // Pin given: that pin only. Otherwise every pin on the node, both directions.
    bool DisconnectAll(FPCGGraphEditContext& Context, const TSharedPtr<FJsonObject>& Desc)
    {
        FString NodeKey;
        if (!Desc->TryGetStringField(TEXT("Node"), NodeKey))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("DisconnectAll needs a Node"));
            return false;
        }
        UPCGNode* Node = PCGGraphEdit::ResolveNode(Context, NodeKey);
        if (!Node)
        {
            return false;
        }

        TArray<UPCGPin*> Pins;
        FString Label;
        if (Desc->TryGetStringField(TEXT("Pin"), Label))
        {
            UPCGPin* Pin = PCGGraphEdit::FindInputPin(Node, Label);
            if (!Pin)
            {
                Pin = PCGGraphEdit::FindOutputPin(Node, Label);
            }
            if (!Pin)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("No pin '%s' on %s: %s"), *Label, *Node->GetName(), *PCGGraphEdit::DescribePins(Node));
                return false;
            }
            Pins.Add(Pin);
        }
        else
        {
            for (UPCGPin* Pin : Node->GetInputPins())
            {
                Pins.Add(Pin);
            }
            for (UPCGPin* Pin : Node->GetOutputPins())
            {
                Pins.Add(Pin);
            }
        }

        int32 Broken = 0;
        for (UPCGPin* Pin : Pins)
        {
            if (Pin && Pin->IsConnected())
            {
                Broken += Pin->EdgeCount();
                Pin->BreakAllEdges();
            }
        }

        Context.Ops += Broken;
        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Broke %d edge(s) on %s"), Broken, *Node->GetName());
        return true;
    }

    class FPCGGraphEdgeWriter : public IPCGGraphWriter
    {
    public:
        virtual const TCHAR* GetSpecKey() const override
        {
            return TEXT("Edges");
        }

        virtual bool Apply(FPCGGraphEditContext& Context, const TSharedPtr<FJsonValue>& Section) override
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

                if (Op == TEXT("DisconnectAll"))
                {
                    if (!DisconnectAll(Context, Desc))
                    {
                        return false;
                    }
                    continue;
                }

                FEdgeEnds Ends;
                if (!ReadEnds(Context, Desc, Ends))
                {
                    return false;
                }

                if (Op == TEXT("Connect"))
                {
                    if (!Connect(Context, Ends))
                    {
                        return false;
                    }
                }
                else if (Op == TEXT("Disconnect"))
                {
                    if (!Disconnect(Context, Ends))
                    {
                        return false;
                    }
                }
                else
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Unknown Edges op '%s'. Expected Connect, Disconnect, DisconnectAll"), *Op);
                    return false;
                }
            }
            return true;
        }
    };
}

TUniquePtr<IPCGGraphWriter> MakePCGGraphEdgeWriter()
{
    return MakeUnique<FPCGGraphEdgeWriter>();
}
