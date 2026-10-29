#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "EditMeshAssetCommandlet.generated.h"

class FJsonObject;
class UStaticMesh;
class USkeletalMesh;

// Writes mesh settings from a spec: LOD count and per-LOD reduction, Nanite, collision, material slot
// assignment. Spec shape matches the AuditMesh report Spec block, so an audit can be fed straight back.
// Dry run does everything except the save.
//   UnrealEditor-Cmd.exe Project.uproject -run=EditMeshAsset -spec="C:/path/spec.json" [-apply]
// Contract: Docs/Edit.md
UCLASS()
class UEditMeshAssetCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    UEditMeshAssetCommandlet();

    virtual int32 Main(const FString& Params) override;

private:

    // Returns false once anything is rejected, which aborts the run before any package is saved.
    bool ApplyTarget(const TSharedPtr<FJsonObject>& Entry, TSet<UObject*>& OutTouched, int32& OutOps) const;

    bool ApplyStaticMesh(UStaticMesh* Mesh, const TSharedPtr<FJsonObject>& Entry, int32& OutOps) const;
    bool ApplySkeletalMesh(USkeletalMesh* Mesh, const TSharedPtr<FJsonObject>& Entry, int32& OutOps) const;
};
