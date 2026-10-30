#include "Edit/MaterialGraphWriter.h"
#include "Export/MaterialGraphJson.h"
#include "UAssetWorkbenchModule.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionNamedReroute.h"
#include "Materials/MaterialFunction.h"
#include "UObject/SoftObjectPath.h"

namespace
{
    // The export prints the bare class name. "Multiply" still resolves, with the MaterialExpression prefix
    // tried second, and a full path goes straight to the class.
    UClass* ResolveExpressionClass(const FString& ClassName)
    {
        UClass* Found = nullptr;
        if (ClassName.Contains(TEXT("/")))
        {
            Found = FSoftClassPath(ClassName).TryLoadClass<UMaterialExpression>();
        }
        else
        {
            Found = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::ExactClass);
            if (!Found)
            {
                const FString Wrapped = TEXT("MaterialExpression") + ClassName;
                Found = FindFirstObject<UClass>(*Wrapped, EFindFirstObjectOptions::ExactClass);
            }
        }

        const bool bConcreteExpression = Found && Found->IsChildOf(UMaterialExpression::StaticClass()) && !Found->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated);
        return bConcreteExpression ? Found : nullptr;
    }

    // Comments live in their own collection, CreateMaterialExpressionEx would file one as an expression.
    UMaterialExpression* CreateComment(FMaterialGraphEditContext& Context, int32 PositionX, int32 PositionY)
    {
        UMaterialExpressionComment* Comment = NewObject<UMaterialExpressionComment>(MaterialGraphEdit::GetOwner(Context), NAME_None, RF_Transactional);
        Comment->Material = Context.Material;
        Comment->Function = Context.Function;
        Comment->MaterialExpressionEditorX = PositionX;
        Comment->MaterialExpressionEditorY = PositionY;

        if (Context.Material)
        {
            Context.Material->GetExpressionCollection().AddComment(Comment);
        }
        else
        {
            Context.Function->GetExpressionCollection().AddComment(Comment);
        }
        return Comment;
    }

    bool AddNode(FMaterialGraphEditContext& Context, const TSharedPtr<FJsonObject>& Desc)
    {
        FString Id;
        FString ClassName;
        if (!Desc->TryGetStringField(TEXT("Id"), Id) || !Desc->TryGetStringField(TEXT("Class"), ClassName))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Nodes Add needs Id and Class"));
            return false;
        }
        if (Context.NodesById.Contains(Id) || Id == MaterialGraphEdit::kMaterialNodeKey)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Id '%s' already names a node"), *Id);
            return false;
        }

        UClass* ExpressionClass = ResolveExpressionClass(ClassName);
        if (!ExpressionClass)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("'%s' is not a concrete material expression class"), *ClassName);
            return false;
        }

        double PositionX = 0.0;
        double PositionY = 0.0;
        Desc->TryGetNumberField(TEXT("PositionX"), PositionX);
        Desc->TryGetNumberField(TEXT("PositionY"), PositionY);

        UMaterialExpression* Expression = nullptr;
        if (ExpressionClass->IsChildOf(UMaterialExpressionComment::StaticClass()))
        {
            Expression = CreateComment(Context, static_cast<int32>(PositionX), static_cast<int32>(PositionY));
        }
        else
        {
            Expression = UMaterialEditingLibrary::CreateMaterialExpressionEx(Context.Material, Context.Function, ExpressionClass, nullptr, static_cast<int32>(PositionX), static_cast<int32>(PositionY));
        }
        if (!Expression)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to create %s in %s"), *ExpressionClass->GetName(), *Context.AssetPath);
            return false;
        }

        Context.NodesById.Add(Id, Expression);

        FString Comment;
        if (Desc->TryGetStringField(TEXT("Comment"), Comment))
        {
            Expression->Desc = Comment;
        }

        // Function first, the call's pins only exist once it knows what it calls.
        TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
        FString FunctionPath;
        if (Desc->TryGetStringField(TEXT("Function"), FunctionPath))
        {
            Properties->SetStringField(TEXT("MaterialFunction"), FunctionPath);
        }
        const TSharedPtr<FJsonObject>* Extra = nullptr;
        if (Desc->TryGetObjectField(TEXT("Properties"), Extra))
        {
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Extra)->Values)
            {
                Properties->SetField(Pair.Key, Pair.Value);
            }
        }

        int32 Applied = 0;
        if (!MaterialGraphEdit::ApplyNodeProperties(Context, Expression, Properties, Applied))
        {
            return false;
        }

        ++Context.Ops;
        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Added %s as %s (%s), %d propert%s"), *ExpressionClass->GetName(), *Id, *Expression->GetName(), Applied, Applied == 1 ? TEXT("y") : TEXT("ies"));
        return true;
    }

    bool DeleteNode(FMaterialGraphEditContext& Context, UMaterialExpression* Expression)
    {
        // Engine delete leaves usages pointing at a dead declaration and the graph fails to compile later,
        // far from the cause. Usages go first.
        if (const UMaterialExpressionNamedRerouteDeclaration* Declaration = Cast<UMaterialExpressionNamedRerouteDeclaration>(Expression))
        {
            TArray<FString> Usages;
            for (const UMaterialExpression* Other : MaterialGraphJson::GetExpressions(Context.Material, Context.Function))
            {
                const UMaterialExpressionNamedRerouteUsage* Usage = Cast<UMaterialExpressionNamedRerouteUsage>(Other);
                if (Usage && Usage->Declaration == Declaration)
                {
                    Usages.Add(Usage->GetName());
                }
            }
            if (Usages.Num() > 0)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s still has usages, delete them first: %s"), *Expression->GetName(), *FString::Join(Usages, TEXT(", ")));
                return false;
            }
        }

        const FString Name = Expression->GetName();
        if (UMaterialExpressionComment* Comment = Cast<UMaterialExpressionComment>(Expression))
        {
            if (Context.Material)
            {
                Context.Material->GetExpressionCollection().RemoveComment(Comment);
            }
            else
            {
                Context.Function->GetExpressionCollection().RemoveComment(Comment);
            }
            Comment->MarkAsGarbage();
        }
        else if (Context.Material)
        {
            UMaterialEditingLibrary::DeleteMaterialExpression(Context.Material, Expression);
        }
        else
        {
            UMaterialEditingLibrary::DeleteMaterialExpressionInFunction(Context.Function, Expression);
        }

        for (auto It = Context.NodesById.CreateIterator(); It; ++It)
        {
            if (It->Value == Expression)
            {
                It.RemoveCurrent();
            }
        }

        ++Context.Ops;
        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Deleted %s"), *Name);
        return true;
    }

    class FMaterialGraphNodeWriter : public IMaterialGraphWriter
    {
    public:
        virtual const TCHAR* GetSpecKey() const override
        {
            return TEXT("Nodes");
        }

        virtual bool Apply(FMaterialGraphEditContext& Context, const TSharedPtr<FJsonValue>& Section) override
        {
            const TArray<TSharedPtr<FJsonValue>>* Ops = nullptr;
            if (!Section->TryGetArray(Ops))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Nodes must be an array of operations"));
                return false;
            }

            for (const TSharedPtr<FJsonValue>& Value : *Ops)
            {
                const TSharedPtr<FJsonObject> Desc = Value->AsObject();
                if (!Desc.IsValid())
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Nodes entry is not an object"));
                    return false;
                }

                FString Op;
                Desc->TryGetStringField(TEXT("Op"), Op);
                if (Op == TEXT("Add"))
                {
                    if (!AddNode(Context, Desc))
                    {
                        return false;
                    }
                    continue;
                }

                FString NodeKey;
                if (!Desc->TryGetStringField(TEXT("Node"), NodeKey))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Nodes op '%s' needs a Node"), *Op);
                    return false;
                }
                UMaterialExpression* Expression = MaterialGraphEdit::ResolveNode(Context, NodeKey);
                if (!Expression)
                {
                    return false;
                }

                if (Op == TEXT("Delete"))
                {
                    if (!DeleteNode(Context, Expression))
                    {
                        return false;
                    }
                }
                else if (Op == TEXT("Move"))
                {
                    double PositionX = 0.0;
                    double PositionY = 0.0;
                    Desc->TryGetNumberField(TEXT("PositionX"), PositionX);
                    Desc->TryGetNumberField(TEXT("PositionY"), PositionY);
                    Expression->MaterialExpressionEditorX = static_cast<int32>(PositionX);
                    Expression->MaterialExpressionEditorY = static_cast<int32>(PositionY);
                    ++Context.Ops;
                }
                else
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Unknown Nodes op '%s'. Expected Add, Delete, Move"), *Op);
                    return false;
                }
            }
            return true;
        }
    };
}

TUniquePtr<IMaterialGraphWriter> MakeMaterialGraphNodeWriter()
{
    return MakeUnique<FMaterialGraphNodeWriter>();
}
