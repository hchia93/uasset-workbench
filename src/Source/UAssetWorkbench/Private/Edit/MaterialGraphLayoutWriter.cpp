#include "Edit/MaterialGraphWriter.h"
#include "Export/MaterialGraphJson.h"
#include "UAssetWorkbenchModule.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionNamedReroute.h"
#include "Materials/MaterialFunction.h"

namespace
{
    struct FLayoutParams
    {
        int32 PosX = 0;
        int32 PosY = 0;
        int32 Spacing = 80;
        int32 RowSpacing = 40;
    };

    constexpr int32 kTitleHeight = 36;
    constexpr int32 kPinRowHeight = 24;
    constexpr int32 kPreviewHeight = 110;

    // A consumer with no node is the material output node.
    struct FConsumer
    {
        UMaterialExpression* Node = nullptr;
        int32 PinIndex = 0;
    };

    struct FCommentGroup
    {
        UMaterialExpressionComment* Comment = nullptr;
        TArray<UMaterialExpression*> Members;
    };

    int32 InputCount(UMaterialExpression* Expression)
    {
        int32 Count = 0;
        for (FExpressionInputIterator It(Expression); It; ++It)
        {
            ++Count;
        }
        return Count;
    }

    // Graph nodes are measured by Slate on draw, a headless run never sees them. Caption and pin labels
    // set the width, pin rows and the preview thumbnail the height. UMaterialExpression::GetWidth and
    // GetHeight describe the legacy thumbnail, not the graph node.
    int32 EstimateWidth(UMaterialExpression* Expression)
    {
        TArray<FString> Captions;
        Expression->GetCaption(Captions);
        int32 Caption = 0;
        for (const FString& Line : Captions)
        {
            Caption = FMath::Max(Caption, Line.Len());
        }

        int32 LongestInput = 0;
        for (FExpressionInputIterator It(Expression); It; ++It)
        {
            LongestInput = FMath::Max(LongestInput, Expression->GetInputName(It.Index).ToString().Len());
        }
        int32 LongestOutput = 0;
        for (const FExpressionOutput& Output : Expression->GetOutputs())
        {
            LongestOutput = FMath::Max(LongestOutput, Output.OutputName.ToString().Len());
        }

        const int32 CaptionWidth = Caption * 8 + 48;
        const int32 PinWidth = (LongestInput + LongestOutput) * 7 + 72;
        const int32 PreviewWidth = Expression->bCollapsed ? 0 : 140;
        return FMath::Clamp(FMath::Max3(CaptionWidth, PinWidth, PreviewWidth), 120, 480);
    }

    int32 EstimateHeight(UMaterialExpression* Expression)
    {
        const int32 Rows = FMath::Max3(InputCount(Expression), Expression->GetOutputs().Num(), 1);
        return kTitleHeight + kPinRowHeight * Rows + (Expression->bCollapsed ? 0 : kPreviewHeight);
    }

    TMap<UMaterialExpression*, TArray<FConsumer>> CollectConsumers(FMaterialGraphEditContext& Context, const TArray<UMaterialExpression*>& Nodes)
    {
        TMap<UMaterialExpression*, TArray<FConsumer>> Consumers;
        for (UMaterialExpression* Node : Nodes)
        {
            for (FExpressionInputIterator It(Node); It; ++It)
            {
                if (It->Expression)
                {
                    Consumers.FindOrAdd(It->Expression).Add({ Node, It.Index });
                }
            }

            // A declaration has no wire to its usages, but it reads best left of them.
            const UMaterialExpressionNamedRerouteUsage* Usage = Cast<UMaterialExpressionNamedRerouteUsage>(Node);
            if (Usage && Usage->Declaration)
            {
                Consumers.FindOrAdd(Usage->Declaration).Add({ Node, 0 });
            }
        }

        int32 RootPin = 0;
        MaterialGraphJson::ForEachMaterialInput(Context.Material, [&Consumers, &RootPin](const FString& PinName, FExpressionInput& Input)
        {
            if (Input.Expression)
            {
                Consumers.FindOrAdd(Input.Expression).Add({ nullptr, RootPin });
            }
            ++RootPin;
        });
        return Consumers;
    }

    // Depth is the column count left of the sinks, the material node or the function outputs. Longest
    // path wins. Relaxation is bounded by node count so a feedback loop cannot spin it forever.
    TMap<UMaterialExpression*, int32> ComputeDepths(const TArray<UMaterialExpression*>& Nodes, const TMap<UMaterialExpression*, TArray<FConsumer>>& Consumers)
    {
        TMap<UMaterialExpression*, int32> Depth;
        for (UMaterialExpression* Node : Nodes)
        {
            if (Node->IsA<UMaterialExpressionFunctionOutput>())
            {
                Depth.Add(Node, 0);
            }
        }

        for (int32 Pass = 0; Pass <= Nodes.Num(); ++Pass)
        {
            bool bChanged = false;
            for (UMaterialExpression* Node : Nodes)
            {
                const TArray<FConsumer>* List = Consumers.Find(Node);
                if (!List || Node->IsA<UMaterialExpressionFunctionOutput>())
                {
                    continue;
                }

                int32 Wanted = INDEX_NONE;
                for (const FConsumer& Consumer : *List)
                {
                    const int32* ConsumerDepth = Consumer.Node ? Depth.Find(Consumer.Node) : nullptr;
                    if (!Consumer.Node)
                    {
                        Wanted = FMath::Max(Wanted, 1);
                    }
                    else if (ConsumerDepth)
                    {
                        Wanted = FMath::Max(Wanted, *ConsumerDepth + 1);
                    }
                }

                int32* Current = Depth.Find(Node);
                if (Wanted != INDEX_NONE && (!Current || *Current < Wanted))
                {
                    Depth.Add(Node, Wanted);
                    bChanged = true;
                }
            }
            if (!bChanged)
            {
                break;
            }
        }

        // Nothing consumes it: one column left of the sinks, where a stray node is easy to spot.
        for (UMaterialExpression* Node : Nodes)
        {
            if (!Depth.Contains(Node))
            {
                Depth.Add(Node, 1);
            }
        }
        return Depth;
    }

    TArray<FCommentGroup> CollectCommentGroups(FMaterialGraphEditContext& Context, const TArray<UMaterialExpression*>& Nodes)
    {
        TArray<FCommentGroup> Groups;
        for (UMaterialExpressionComment* Comment : MaterialGraphJson::GetComments(Context.Material, Context.Function))
        {
            if (!Comment)
            {
                continue;
            }

            FCommentGroup Group;
            Group.Comment = Comment;
            for (UMaterialExpression* Node : Nodes)
            {
                const bool bInsideX = Node->MaterialExpressionEditorX >= Comment->MaterialExpressionEditorX && Node->MaterialExpressionEditorX <= Comment->MaterialExpressionEditorX + Comment->SizeX;
                const bool bInsideY = Node->MaterialExpressionEditorY >= Comment->MaterialExpressionEditorY && Node->MaterialExpressionEditorY <= Comment->MaterialExpressionEditorY + Comment->SizeY;
                if (bInsideX && bInsideY)
                {
                    Group.Members.Add(Node);
                }
            }
            Groups.Add(MoveTemp(Group));
        }
        return Groups;
    }

    int32 RefitComments(const TArray<FCommentGroup>& Groups)
    {
        int32 Refit = 0;
        for (const FCommentGroup& Group : Groups)
        {
            if (Group.Members.IsEmpty())
            {
                continue;
            }

            int32 MinX = MAX_int32;
            int32 MinY = MAX_int32;
            int32 MaxX = MIN_int32;
            int32 MaxY = MIN_int32;
            for (UMaterialExpression* Member : Group.Members)
            {
                MinX = FMath::Min(MinX, Member->MaterialExpressionEditorX);
                MinY = FMath::Min(MinY, Member->MaterialExpressionEditorY);
                MaxX = FMath::Max(MaxX, Member->MaterialExpressionEditorX + EstimateWidth(Member));
                MaxY = FMath::Max(MaxY, Member->MaterialExpressionEditorY + EstimateHeight(Member));
            }

            // Room for the title bar above and a margin around.
            Group.Comment->MaterialExpressionEditorX = MinX - 32;
            Group.Comment->MaterialExpressionEditorY = MinY - 64;
            Group.Comment->SizeX = MaxX - MinX + 64;
            Group.Comment->SizeY = MaxY - MinY + 96;
            ++Refit;
        }
        return Refit;
    }

    // Places a column right-aligned against Right. Each node aims at the average pin height of what it
    // feeds, overlaps push down, then the whole column shifts back so it stays centered on its targets.
    void PlaceColumn(TArray<UMaterialExpression*>& Column, const TMap<UMaterialExpression*, TArray<FConsumer>>& Consumers, TMap<UMaterialExpression*, int32>& Top, const FLayoutParams& Params, int32 Right)
    {
        TMap<UMaterialExpression*, float> Desired;
        float Lowest = static_cast<float>(Params.PosY);
        TArray<UMaterialExpression*> Strays;
        for (UMaterialExpression* Node : Column)
        {
            float Sum = 0.0f;
            int32 Count = 0;
            if (const TArray<FConsumer>* List = Consumers.Find(Node))
            {
                for (const FConsumer& Consumer : *List)
                {
                    const int32* ConsumerTop = Consumer.Node ? Top.Find(Consumer.Node) : &Params.PosY;
                    if (ConsumerTop)
                    {
                        Sum += static_cast<float>(*ConsumerTop + kPinRowHeight * Consumer.PinIndex);
                        ++Count;
                    }
                }
            }

            if (Count > 0)
            {
                Desired.Add(Node, Sum / Count);
                Lowest = FMath::Max(Lowest, Sum / Count);
            }
            else
            {
                Strays.Add(Node);
            }
        }

        // Unconsumed nodes queue up under the rest of the column.
        for (int32 Index = 0; Index < Strays.Num(); ++Index)
        {
            Desired.Add(Strays[Index], Lowest + 1.0f + Index);
        }

        Column.StableSort([&Desired](const UMaterialExpression& A, const UMaterialExpression& B)
        {
            return Desired[&A] < Desired[&B];
        });

        TArray<int32> Placed;
        int32 NextFree = MIN_int32;
        float Drift = 0.0f;
        for (UMaterialExpression* Node : Column)
        {
            const int32 Y = FMath::Max(FMath::RoundToInt(Desired[Node]), NextFree);
            Placed.Add(Y);
            Drift += static_cast<float>(Y) - Desired[Node];
            NextFree = Y + EstimateHeight(Node) + Params.RowSpacing;
        }

        const int32 Shift = Column.Num() > 0 ? FMath::RoundToInt(Drift / Column.Num()) : 0;
        for (int32 Index = 0; Index < Column.Num(); ++Index)
        {
            UMaterialExpression* Node = Column[Index];
            Node->MaterialExpressionEditorX = Right - EstimateWidth(Node);
            Node->MaterialExpressionEditorY = Placed[Index] - Shift;
            Top.Add(Node, Node->MaterialExpressionEditorY);
        }
    }

    int32 Arrange(FMaterialGraphEditContext& Context, const FLayoutParams& Params, int32& OutCommentsRefit)
    {
        TArray<UMaterialExpression*> Nodes;
        for (UMaterialExpression* Expression : MaterialGraphJson::GetExpressions(Context.Material, Context.Function))
        {
            if (Expression)
            {
                Nodes.Add(Expression);
            }
        }

        const TArray<FCommentGroup> CommentGroups = CollectCommentGroups(Context, Nodes);
        const TMap<UMaterialExpression*, TArray<FConsumer>> Consumers = CollectConsumers(Context, Nodes);
        const TMap<UMaterialExpression*, int32> Depth = ComputeDepths(Nodes, Consumers);

        int32 MaxDepth = 0;
        for (const TPair<UMaterialExpression*, int32>& Pair : Depth)
        {
            MaxDepth = FMath::Max(MaxDepth, Pair.Value);
        }

        TArray<TArray<UMaterialExpression*>> Columns;
        Columns.SetNum(MaxDepth + 1);
        for (UMaterialExpression* Node : Nodes)
        {
            Columns[Depth[Node]].Add(Node);
        }

        TMap<UMaterialExpression*, int32> Top;
        int32 Moved = 0;
        int32 PreviousLeft = Params.PosX;

        if (Context.Material)
        {
            Context.Material->EditorX = Params.PosX;
            Context.Material->EditorY = Params.PosY;
        }

        // Function outputs stack in pin order at the anchor, the way a caller sees them.
        TArray<UMaterialExpression*>& Outputs = Columns[0];
        Outputs.StableSort([](const UMaterialExpression& A, const UMaterialExpression& B)
        {
            return CastChecked<UMaterialExpressionFunctionOutput>(&A)->SortPriority < CastChecked<UMaterialExpressionFunctionOutput>(&B)->SortPriority;
        });
        int32 OutputY = Params.PosY;
        for (UMaterialExpression* Output : Outputs)
        {
            Output->MaterialExpressionEditorX = Params.PosX;
            Output->MaterialExpressionEditorY = OutputY;
            Top.Add(Output, OutputY);
            OutputY += EstimateHeight(Output) + Params.RowSpacing;
            ++Moved;
        }

        for (int32 ColumnIndex = 1; ColumnIndex <= MaxDepth; ++ColumnIndex)
        {
            TArray<UMaterialExpression*>& Column = Columns[ColumnIndex];
            if (Column.IsEmpty())
            {
                continue;
            }

            int32 Width = 0;
            for (UMaterialExpression* Node : Column)
            {
                Width = FMath::Max(Width, EstimateWidth(Node));
            }

            const int32 Right = PreviousLeft - Params.Spacing;
            PlaceColumn(Column, Consumers, Top, Params, Right);
            PreviousLeft = Right - Width;
            Moved += Column.Num();
        }

        OutCommentsRefit = RefitComments(CommentGroups);
        return Moved;
    }

    class FMaterialGraphLayoutWriter : public IMaterialGraphWriter
    {
    public:
        virtual const TCHAR* GetSpecKey() const override
        {
            return TEXT("Layout");
        }

        virtual bool Apply(FMaterialGraphEditContext& Context, const TSharedPtr<FJsonValue>& Section) override
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

            // The anchor stays where the graph already is, so an arrange does not throw the view off.
            FLayoutParams Params;
            if (Context.Material)
            {
                Params.PosX = Context.Material->EditorX;
                Params.PosY = Context.Material->EditorY;
            }
            else
            {
                for (UMaterialExpression* Expression : MaterialGraphJson::GetExpressions(Context.Material, Context.Function))
                {
                    if (Expression && Expression->IsA<UMaterialExpressionFunctionOutput>())
                    {
                        Params.PosX = Expression->MaterialExpressionEditorX;
                        Params.PosY = Expression->MaterialExpressionEditorY;
                        break;
                    }
                }
            }

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

            int32 CommentsRefit = 0;
            const int32 Moved = Arrange(Context, Params, CommentsRefit);
            Context.Ops += Moved;
            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Arranged %d node(s), refit %d comment(s)"), Moved, CommentsRefit);
            return true;
        }
    };
}

TUniquePtr<IMaterialGraphWriter> MakeMaterialGraphLayoutWriter()
{
    return MakeUnique<FMaterialGraphLayoutWriter>();
}
