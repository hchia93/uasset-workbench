#include "Edit/PCGGraphWriter.h"
#include "Export/PCGGraphJsonSerializer.h"
#include "UAssetWorkbenchModule.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "PCGEdge.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"

namespace
{
    struct FLayoutParams
    {
        int32 PosX = 0;
        int32 PosY = 0;
        int32 Spacing = 120;
        int32 RowSpacing = 60;
        int32 NodeWidth = 280;
    };

    // PCG nodes carry no size, the editor measures them on draw. Title row plus one line per pin.
    int32 HeightOf(const UPCGNode* Node)
    {
        const int32 PinRows = FMath::Max(Node->GetInputPins().Num(), Node->GetOutputPins().Num());
        return 48 + 22 * FMath::Max(1, PinRows);
    }

    void CollectDownstream(const UPCGNode* Node, TArray<const UPCGNode*>& OutNodes)
    {
        for (const UPCGPin* Pin : Node->GetOutputPins())
        {
            if (!Pin)
            {
                continue;
            }
            for (const UPCGEdge* Edge : Pin->Edges)
            {
                const UPCGNode* Downstream = Edge && Edge->OutputPin ? Edge->OutputPin->Node.Get() : nullptr;
                if (Downstream)
                {
                    OutNodes.AddUnique(Downstream);
                }
            }
        }
    }

    void CollectUpstream(const UPCGNode* Node, TArray<const UPCGNode*>& OutNodes)
    {
        for (const UPCGPin* Pin : Node->GetInputPins())
        {
            if (!Pin)
            {
                continue;
            }
            for (const UPCGEdge* Edge : Pin->Edges)
            {
                const UPCGNode* Upstream = Edge && Edge->InputPin ? Edge->InputPin->Node.Get() : nullptr;
                if (Upstream)
                {
                    OutNodes.AddUnique(Upstream);
                }
            }
        }
    }

    // Longest path from any source decides the column. Relaxation bounded by node count so a feedback
    // loop cannot spin it forever.
    void ComputeLayers(const UPCGGraph* Graph, const TArray<const UPCGNode*>& Nodes, TMap<const UPCGNode*, int32>& OutLayer)
    {
        for (const UPCGNode* Node : Nodes)
        {
            OutLayer.Add(Node, 0);
        }

        for (int32 Pass = 0; Pass < Nodes.Num(); ++Pass)
        {
            bool bChanged = false;
            for (const UPCGNode* Node : Nodes)
            {
                TArray<const UPCGNode*> Downstream;
                CollectDownstream(Node, Downstream);
                for (const UPCGNode* Next : Downstream)
                {
                    int32& NextLayer = OutLayer.FindOrAdd(Next);
                    const int32 Wanted = OutLayer[Node] + 1;
                    if (NextLayer < Wanted)
                    {
                        NextLayer = Wanted;
                        bChanged = true;
                    }
                }
            }
            if (!bChanged)
            {
                break;
            }
        }

        // The output node closes every chain, so it sits in the last column even when nothing reaches it.
        int32 MaxLayer = 0;
        for (const TPair<const UPCGNode*, int32>& Pair : OutLayer)
        {
            MaxLayer = FMath::Max(MaxLayer, Pair.Value);
        }
        if (const UPCGNode* OutputNode = Graph->GetOutputNode())
        {
            OutLayer.FindOrAdd(OutputNode) = MaxLayer;
        }
    }

    int32 Arrange(FPCGGraphEditContext& Context, const FLayoutParams& Params)
    {
        TArray<const UPCGNode*> Nodes;
        PCGGraphJson::CollectNodes(Context.Graph, Nodes);

        TMap<const UPCGNode*, int32> Layer;
        ComputeLayers(Context.Graph, Nodes, Layer);

        TMap<int32, TArray<const UPCGNode*>> Columns;
        for (const UPCGNode* Node : Nodes)
        {
            Columns.FindOrAdd(Layer[Node]).Add(Node);
        }

        TArray<int32> LayerIndices;
        Columns.GetKeys(LayerIndices);
        LayerIndices.Sort();

        // Rows follow the average row of the feeders so wires run mostly straight. Ties keep the authored order.
        TMap<const UPCGNode*, int32> Row;
        int32 Moved = 0;
        for (const int32 LayerIndex : LayerIndices)
        {
            TArray<const UPCGNode*>& Column = Columns[LayerIndex];

            TMap<const UPCGNode*, float> Barycenter;
            for (const UPCGNode* Node : Column)
            {
                TArray<const UPCGNode*> Upstream;
                CollectUpstream(Node, Upstream);
                float Sum = 0.0f;
                int32 Count = 0;
                for (const UPCGNode* Feeder : Upstream)
                {
                    if (const int32* FeederRow = Row.Find(Feeder))
                    {
                        Sum += static_cast<float>(*FeederRow);
                        ++Count;
                    }
                }

                int32 PositionX = 0;
                int32 PositionY = 0;
                Node->GetNodePosition(PositionX, PositionY);
                Barycenter.Add(Node, Count > 0 ? Sum / Count : static_cast<float>(PositionY));
            }
            Column.StableSort([&Barycenter](const UPCGNode& A, const UPCGNode& B)
            {
                return Barycenter[&A] < Barycenter[&B];
            });

            const int32 ColumnX = Params.PosX + LayerIndex * (Params.NodeWidth + Params.Spacing);
            int32 RowY = Params.PosY;
            for (int32 Index = 0; Index < Column.Num(); ++Index)
            {
                UPCGNode* Node = const_cast<UPCGNode*>(Column[Index]);
                Node->SetNodePosition(ColumnX, RowY);
                Row.Add(Node, Index);
                RowY += HeightOf(Node) + Params.RowSpacing;
                ++Moved;
            }
        }
        return Moved;
    }

    class FPCGGraphLayoutWriter : public IPCGGraphWriter
    {
    public:
        virtual const TCHAR* GetSpecKey() const override
        {
            return TEXT("Layout");
        }

        virtual bool Apply(FPCGGraphEditContext& Context, const TSharedPtr<FJsonValue>& Section) override
        {
            const TSharedPtr<FJsonObject>* Desc = nullptr;
            if (!Section->TryGetObject(Desc))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Layout must be an object"));
                return false;
            }

            FString Op = TEXT("Arrange");
            (*Desc)->TryGetStringField(TEXT("Op"), Op);
            if (Op != TEXT("Arrange"))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Unknown Layout op '%s'. Expected Arrange"), *Op);
                return false;
            }

            FLayoutParams Params;
            double Number = 0.0;
            if ((*Desc)->TryGetNumberField(TEXT("PosX"), Number))
            {
                Params.PosX = static_cast<int32>(Number);
            }
            if ((*Desc)->TryGetNumberField(TEXT("PosY"), Number))
            {
                Params.PosY = static_cast<int32>(Number);
            }
            if ((*Desc)->TryGetNumberField(TEXT("Spacing"), Number))
            {
                Params.Spacing = static_cast<int32>(Number);
            }
            if ((*Desc)->TryGetNumberField(TEXT("RowSpacing"), Number))
            {
                Params.RowSpacing = static_cast<int32>(Number);
            }
            if ((*Desc)->TryGetNumberField(TEXT("NodeWidth"), Number))
            {
                Params.NodeWidth = static_cast<int32>(Number);
            }

            const int32 Moved = Arrange(Context, Params);
            Context.Ops += Moved;
            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Arranged %d node(s)"), Moved);
            return true;
        }
    };
}

TUniquePtr<IPCGGraphWriter> MakePCGGraphLayoutWriter()
{
    return MakeUnique<FPCGGraphLayoutWriter>();
}
