#pragma once

#include "CoreMinimal.h"

class FProperty;
class UMaterial;
class UMaterialExpression;
class UMaterialExpressionComment;
class UMaterialFunction;
struct FExpressionInput;

// Shared by MaterialExport and the EditMaterialAsset graph writers, so every name an export prints is a
// name an edit resolves. A graph owner is either a UMaterial or a UMaterialFunction, never both.
namespace MaterialGraphJson
{
    // A wire, not a setting. Property dumps skip it because the connection already prints elsewhere.
    bool IsPinProperty(const FProperty* Property);

    // Pins are reflected off the editor-only data, so a pin the engine adds needs no list kept in sync.
    // Static arrays yield one pin per element, "CustomizedUVs[3]".
    void ForEachMaterialInput(UMaterial* Material, TFunctionRef<void(const FString& PinName, FExpressionInput& Input)> Visitor);

    FExpressionInput* FindMaterialInput(UMaterial* Material, const FString& PinName);

    FString DescribeMaterialInputs(UMaterial* Material);

    TConstArrayView<TObjectPtr<UMaterialExpression>> GetExpressions(const UMaterial* Material, const UMaterialFunction* Function);

    TConstArrayView<TObjectPtr<UMaterialExpressionComment>> GetComments(const UMaterial* Material, const UMaterialFunction* Function);

    // The name a person gave the node: parameter, function input or output, reroute declaration. Empty for
    // anonymous nodes, whose captions repeat too often to address anything.
    FString NodeLabel(const UMaterialExpression* Expression);

    // "Alpha Threshold (S)" also answers to "Alpha Threshold", the suffix is the editor's type hint.
    int32 FindInputIndex(UMaterialExpression* Expression, const FString& PinName);

    // Output names, or the index as text, which is how an unnamed first output is reached.
    int32 FindOutputIndex(UMaterialExpression* Expression, const FString& PinName);

    FString DescribePins(UMaterialExpression* Expression);
}
