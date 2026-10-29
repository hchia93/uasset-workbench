#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "MeshExportCommandlet.generated.h"

class FJsonObject;
class UStaticMesh;
class USkeletalMesh;

// Exports StaticMesh / SkeletalMesh to JSON: per-LOD triangle counts, Nanite settings, collision, material slots, sockets.
//   UnrealEditor-Cmd.exe Project.uproject -run=MeshExport -assets="/Game/Path/SM_A,/Game/Path/SK_B"
// Contract: Docs/Export.md
UCLASS()
class UMeshExportCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    UMeshExportCommandlet();

    virtual int32 Main(const FString& Params) override;

private:

    TSharedPtr<FJsonObject> ExportStaticMesh(UStaticMesh* Mesh) const;
    TSharedPtr<FJsonObject> ExportSkeletalMesh(USkeletalMesh* Mesh) const;
};
