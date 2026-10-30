#include "Edit/MaterialGraphWriter.h"
#include "Export/MaterialGraphJson.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionNamedReroute.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialFunctionInterface.h"
#include "UObject/UnrealType.h"

const TCHAR* const MaterialGraphEdit::kMaterialNodeKey = TEXT("Material");

UObject* MaterialGraphEdit::GetOwner(const FMaterialGraphEditContext& Context)
{
    if (Context.Material)
    {
        return Context.Material;
    }
    return Context.Function;
}

void MaterialGraphEdit::RegisterExistingNodes(FMaterialGraphEditContext& Context)
{
    for (UMaterialExpression* Expression : MaterialGraphJson::GetExpressions(Context.Material, Context.Function))
    {
        if (Expression)
        {
            Context.NodesById.Add(Expression->GetName(), Expression);
        }
    }
    for (UMaterialExpressionComment* Comment : MaterialGraphJson::GetComments(Context.Material, Context.Function))
    {
        if (Comment)
        {
            Context.NodesById.Add(Comment->GetName(), Comment);
        }
    }
}

UMaterialExpression* MaterialGraphEdit::ResolveNode(const FMaterialGraphEditContext& Context, const FString& NodeKey)
{
    if (UMaterialExpression* const* Found = Context.NodesById.Find(NodeKey))
    {
        return *Found;
    }

    if (NodeKey == kMaterialNodeKey)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("'%s' is the material output node, only Edges can target it"), kMaterialNodeKey);
        return nullptr;
    }

    // Labels are names a person chose, so they usually are unique, but only a single hit resolves.
    UMaterialExpression* ByLabel = nullptr;
    int32 LabelHits = 0;
    for (const TPair<FString, UMaterialExpression*>& Pair : Context.NodesById)
    {
        if (Pair.Value && MaterialGraphJson::NodeLabel(Pair.Value) == NodeKey && ByLabel != Pair.Value)
        {
            ByLabel = Pair.Value;
            ++LabelHits;
        }
    }
    if (LabelHits == 1)
    {
        return ByLabel;
    }

    if (LabelHits > 1)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Label '%s' matches %d nodes, use the Name. Nodes: %s"), *NodeKey, LabelHits, *DescribeNodes(Context));
    }
    else
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Node '%s' not found. Nodes: %s"), *NodeKey, *DescribeNodes(Context));
    }
    return nullptr;
}

FString MaterialGraphEdit::DescribeNodes(const FMaterialGraphEditContext& Context)
{
    TArray<FString> Entries;
    for (const TPair<FString, UMaterialExpression*>& Pair : Context.NodesById)
    {
        if (!Pair.Value)
        {
            continue;
        }

        const FString Label = MaterialGraphJson::NodeLabel(Pair.Value);
        Entries.Add(Label.IsEmpty() ? Pair.Key : FString::Printf(TEXT("%s (%s)"), *Pair.Key, *Label));
    }
    return FString::Join(Entries, TEXT(", "));
}

bool MaterialGraphEdit::ApplyNodeProperties(FMaterialGraphEditContext& Context, UMaterialExpression* Expression, const TSharedPtr<FJsonObject>& Properties, int32& OutApplied)
{
    TSharedPtr<FJsonObject> Plain = MakeShared<FJsonObject>();
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
    {
        if (Pair.Key == TEXT("MaterialFunction"))
        {
            UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression);
            FString FunctionPath;
            if (!Call || !Pair.Value->TryGetString(FunctionPath))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("MaterialFunction takes a function path and only applies to a MaterialFunctionCall, %s is a %s"), *Expression->GetName(), *Expression->GetClass()->GetName());
                return false;
            }

            UMaterialFunctionInterface* NewFunction = LoadObject<UMaterialFunctionInterface>(nullptr, *FunctionPath);
            if (!NewFunction)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to load material function: %s"), *FunctionPath);
                return false;
            }
            if (NewFunction == Context.Function)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s cannot call itself"), *Context.AssetPath);
                return false;
            }
            if (!Call->SetMaterialFunction(NewFunction))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s rejected %s, a call cycle or an incompatible function usage"), *Expression->GetName(), *FunctionPath);
                return false;
            }

            ++OutApplied;
            continue;
        }

        if (Pair.Key == TEXT("Declaration"))
        {
            UMaterialExpressionNamedRerouteUsage* Usage = Cast<UMaterialExpressionNamedRerouteUsage>(Expression);
            FString DeclarationKey;
            if (!Usage || !Pair.Value->TryGetString(DeclarationKey))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Declaration takes a node key and only applies to a NamedRerouteUsage, %s is a %s"), *Expression->GetName(), *Expression->GetClass()->GetName());
                return false;
            }

            UMaterialExpressionNamedRerouteDeclaration* Declaration = Cast<UMaterialExpressionNamedRerouteDeclaration>(ResolveNode(Context, DeclarationKey));
            if (!Declaration)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Declaration '%s' of %s is not a NamedRerouteDeclaration"), *DeclarationKey, *Expression->GetName());
                return false;
            }

            // The guid is what survives a reload, the pointer is rebuilt from it.
            Usage->Declaration = Declaration;
            Usage->DeclarationGuid = Declaration->VariableGuid;
            ++OutApplied;
            continue;
        }

        Plain->SetField(Pair.Key, Pair.Value);
    }

    if (Plain->Values.IsEmpty())
    {
        return true;
    }

    // Handlers such as a parameter rename only run on a change event that names the property.
    TArray<FProperty*> Roots;
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Plain->Values)
    {
        FString Root = Pair.Key;
        int32 Cut = INDEX_NONE;
        if (Root.FindChar(TEXT('.'), Cut))
        {
            Root.LeftInline(Cut);
        }
        if (Root.FindChar(TEXT('['), Cut))
        {
            Root.LeftInline(Cut);
        }
        if (FProperty* Property = FindFProperty<FProperty>(Expression->GetClass(), *Root))
        {
            Roots.AddUnique(Property);
        }
    }

    Expression->Modify();
    for (FProperty* Property : Roots)
    {
        Expression->PreEditChange(Property);
    }

    int32 Failures = 0;
    const int32 Applied = UAssetWorkbench::ApplyProperties(Expression, Plain, Failures);

    for (FProperty* Property : Roots)
    {
        FPropertyChangedEvent Event(Property);
        Expression->PostEditChangeProperty(Event);
    }

    if (Failures > 0)
    {
        return false;
    }

    if (Context.Material && Expression->HasAParameterName())
    {
        Context.Material->UpdateExpressionParameterName(Expression);
    }

    OutApplied += Applied;
    return true;
}
