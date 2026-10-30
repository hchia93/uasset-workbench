#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "EditMaterialAssetCommandlet.generated.h"

class FJsonObject;
class FJsonValue;
class FMaterialUpdateContext;
class UMaterial;
class UMaterialFunction;
class UMaterialFunctionInstance;
class UMaterialInstanceConstant;
class UMaterialParameterCollection;

// Writes material assets from a spec. Material and MaterialFunction take settings plus their node graph
// through the Nodes, NodeProperties, Edges and Layout writers, the two instance types take parent and
// parameter overrides, a parameter collection takes its parameters. Settings match the AuditMaterial
// report Spec block. Dry run skips only the save.
//   UnrealEditor-Cmd.exe Project.uproject -run=EditMaterialAsset -spec="C:/path/spec.json" [-apply]
// See Docs/Edit.md.
UCLASS()
class UEditMaterialAssetCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    UEditMaterialAssetCommandlet();

    virtual int32 Main(const FString& Params) override;

private:

    // Returns false once anything is rejected, which aborts the run before any package is saved.
    bool ApplyTarget(const TSharedPtr<FJsonObject>& Entry, FMaterialUpdateContext& UpdateContext, TSet<UObject*>& OutTouched, int32& OutOps) const;

    bool ApplyMaterial(UMaterial* Material, const TSharedPtr<FJsonObject>& Properties, int32& OutOps) const;
    bool ApplyMaterialInstance(UMaterialInstanceConstant* Instance, const TSharedPtr<FJsonObject>& Properties, int32& OutOps) const;
    bool ApplyMaterialFunction(UMaterialFunction* Function, const TSharedPtr<FJsonObject>& Properties, int32& OutOps) const;
    bool ApplyMaterialFunctionInstance(UMaterialFunctionInstance* Instance, const TSharedPtr<FJsonObject>& Properties, int32& OutOps) const;
    bool ApplyParameterCollection(UMaterialParameterCollection* Collection, const TSharedPtr<FJsonValue>& Parameters, int32& OutOps) const;
};
