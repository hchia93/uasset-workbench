#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "MaterialExportCommandlet.generated.h"

class UMaterialExpression;
class UMaterialExpressionComment;

/*
 * Exports Material node graph and MaterialInstance parameter overrides to JSON.
 *
 * Usage:
 *   UnrealEditor-Cmd.exe Project.uproject -run=MaterialExport -assets="/Game/Path/M_A,/Game/Path/MI_B" [-full]
 *
 * Accepts UMaterial, UMaterialInstance, UMaterialFunction with its layer types, UMaterialFunctionInstance
 * and UMaterialParameterCollection assets.
 *
 * Output:
 *   <ProjectDir>/Intermediate/UAssetExport/<AssetPath>.json
 */
UCLASS()
class UMaterialExportCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    UMaterialExportCommandlet();

    virtual int32 Main(const FString& Params) override;

private:

    TSharedPtr<FJsonObject> ExportMaterial(class UMaterial* Material) const;
    TSharedPtr<FJsonObject> ExportMaterialInstance(class UMaterialInstance* MaterialInstance) const;
    TSharedPtr<FJsonObject> ExportMaterialFunction(class UMaterialFunction* Function) const;
    TSharedPtr<FJsonObject> ExportMaterialFunctionInstance(class UMaterialFunctionInstance* Instance) const;
    TSharedPtr<FJsonObject> ExportParameterCollection(class UMaterialParameterCollection* Collection) const;
    TSharedPtr<FJsonObject> ExportExpression(UMaterialExpression* Expression) const;
    TSharedPtr<FJsonObject> ExportComment(const UMaterialExpressionComment* Comment) const;

    // Every editable property instead of the delta against the class default.
    bool m_bFull = false;

};
