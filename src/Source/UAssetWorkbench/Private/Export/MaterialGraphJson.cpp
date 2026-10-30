#include "Export/MaterialGraphJson.h"

#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionNamedReroute.h"
#include "Materials/MaterialFunction.h"
#include "UObject/UnrealType.h"

namespace
{
    // FMaterialInput is a reflection mirror that does not derive from the ExpressionInput mirror, while the
    // C++ types do derive from FExpressionInput. Either struct marks a pin.
    bool IsPinStruct(const UScriptStruct* Struct)
    {
        static const UScriptStruct* ExpressionInputStruct = FindObject<UScriptStruct>(nullptr, TEXT("/Script/Engine.ExpressionInput"));
        static const UScriptStruct* MaterialInputStruct = FindObject<UScriptStruct>(nullptr, TEXT("/Script/Engine.MaterialInput"));

        const bool bExpressionInput = ExpressionInputStruct && Struct->IsChildOf(ExpressionInputStruct);
        const bool bMaterialInput = MaterialInputStruct && Struct->IsChildOf(MaterialInputStruct);
        return bExpressionInput || bMaterialInput;
    }

    FString StripTypeHint(const FString& Name)
    {
        int32 Open = INDEX_NONE;
        if (Name.EndsWith(TEXT(")")) && Name.FindLastChar(TEXT('('), Open) && Open > 0 && Name[Open - 1] == TEXT(' '))
        {
            return Name.Left(Open - 1);
        }
        return Name;
    }
}

bool MaterialGraphJson::IsPinProperty(const FProperty* Property)
{
    const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
    return StructProperty && IsPinStruct(StructProperty->Struct);
}

void MaterialGraphJson::ForEachMaterialInput(UMaterial* Material, TFunctionRef<void(const FString& PinName, FExpressionInput& Input)> Visitor)
{
    UMaterialEditorOnlyData* EditorOnlyData = Material ? Material->GetEditorOnlyData() : nullptr;
    if (!EditorOnlyData)
    {
        return;
    }

    for (TFieldIterator<FStructProperty> It(EditorOnlyData->GetClass()); It; ++It)
    {
        if (It->HasAnyPropertyFlags(CPF_Deprecated) || !IsPinStruct(It->Struct))
        {
            continue;
        }

        for (int32 ArrayIndex = 0; ArrayIndex < It->ArrayDim; ++ArrayIndex)
        {
            const FString PinName = It->ArrayDim > 1 ? FString::Printf(TEXT("%s[%d]"), *It->GetName(), ArrayIndex) : It->GetName();
            FExpressionInput* Input = It->ContainerPtrToValuePtr<FExpressionInput>(EditorOnlyData, ArrayIndex);
            Visitor(PinName, *Input);
        }
    }
}

FExpressionInput* MaterialGraphJson::FindMaterialInput(UMaterial* Material, const FString& PinName)
{
    FExpressionInput* Found = nullptr;
    ForEachMaterialInput(Material, [&](const FString& Name, FExpressionInput& Input)
    {
        if (!Found && Name.Equals(PinName, ESearchCase::IgnoreCase))
        {
            Found = &Input;
        }
    });
    return Found;
}

FString MaterialGraphJson::DescribeMaterialInputs(UMaterial* Material)
{
    TArray<FString> Names;
    ForEachMaterialInput(Material, [&Names](const FString& Name, FExpressionInput& Input)
    {
        Names.Add(Name);
    });
    return FString::Join(Names, TEXT(", "));
}

TConstArrayView<TObjectPtr<UMaterialExpression>> MaterialGraphJson::GetExpressions(const UMaterial* Material, const UMaterialFunction* Function)
{
    if (Material)
    {
        return Material->GetExpressions();
    }
    if (Function)
    {
        return Function->GetExpressions();
    }
    return {};
}

TConstArrayView<TObjectPtr<UMaterialExpressionComment>> MaterialGraphJson::GetComments(const UMaterial* Material, const UMaterialFunction* Function)
{
    if (Material)
    {
        return Material->GetEditorComments();
    }
    if (Function)
    {
        return Function->GetEditorComments();
    }
    return {};
}

FString MaterialGraphJson::NodeLabel(const UMaterialExpression* Expression)
{
    if (!Expression)
    {
        return FString();
    }
    if (Expression->HasAParameterName())
    {
        return Expression->GetParameterName().ToString();
    }
    if (const UMaterialExpressionFunctionInput* Input = Cast<UMaterialExpressionFunctionInput>(Expression))
    {
        return Input->InputName.ToString();
    }
    if (const UMaterialExpressionFunctionOutput* Output = Cast<UMaterialExpressionFunctionOutput>(Expression))
    {
        return Output->OutputName.ToString();
    }
    if (const UMaterialExpressionNamedRerouteDeclaration* Declaration = Cast<UMaterialExpressionNamedRerouteDeclaration>(Expression))
    {
        return Declaration->Name.ToString();
    }
    return FString();
}

int32 MaterialGraphJson::FindInputIndex(UMaterialExpression* Expression, const FString& PinName)
{
    TArray<FString> Names;
    for (FExpressionInputIterator It(Expression); It; ++It)
    {
        Names.Add(Expression->GetInputName(It.Index).ToString());
    }

    if (PinName.IsEmpty())
    {
        return Names.Num() == 1 ? 0 : INDEX_NONE;
    }

    for (int32 Index = 0; Index < Names.Num(); ++Index)
    {
        if (Names[Index].Equals(PinName, ESearchCase::IgnoreCase))
        {
            return Index;
        }
    }
    for (int32 Index = 0; Index < Names.Num(); ++Index)
    {
        if (StripTypeHint(Names[Index]).Equals(PinName, ESearchCase::IgnoreCase))
        {
            return Index;
        }
    }
    return INDEX_NONE;
}

int32 MaterialGraphJson::FindOutputIndex(UMaterialExpression* Expression, const FString& PinName)
{
    const TArray<FExpressionOutput>& Outputs = Expression->GetOutputs();
    if (PinName.IsEmpty())
    {
        return Outputs.Num() > 0 ? 0 : INDEX_NONE;
    }

    if (PinName.IsNumeric())
    {
        const int32 Index = FCString::Atoi(*PinName);
        return Outputs.IsValidIndex(Index) ? Index : INDEX_NONE;
    }

    for (int32 Index = 0; Index < Outputs.Num(); ++Index)
    {
        if (Outputs[Index].OutputName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
        {
            return Index;
        }
    }
    return INDEX_NONE;
}

FString MaterialGraphJson::DescribePins(UMaterialExpression* Expression)
{
    TArray<FString> Inputs;
    for (FExpressionInputIterator It(Expression); It; ++It)
    {
        Inputs.Add(FString::Printf(TEXT("'%s'"), *Expression->GetInputName(It.Index).ToString()));
    }

    TArray<FString> Outputs;
    const TArray<FExpressionOutput>& ExpressionOutputs = Expression->GetOutputs();
    for (int32 Index = 0; Index < ExpressionOutputs.Num(); ++Index)
    {
        Outputs.Add(FString::Printf(TEXT("%d:'%s'"), Index, *ExpressionOutputs[Index].OutputName.ToString()));
    }

    return FString::Printf(TEXT("in [%s] out [%s]"), *FString::Join(Inputs, TEXT(", ")), *FString::Join(Outputs, TEXT(", ")));
}
