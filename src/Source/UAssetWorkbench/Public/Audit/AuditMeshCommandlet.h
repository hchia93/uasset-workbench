#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "AuditMeshCommandlet.generated.h"

// Audits StaticMesh / SkeletalMesh against fixed rules M1-M5: missing LODs, Nanite against its materials and
// triangle count, complex collision used as simple, empty material slots. The report carries a Spec block
// EditMeshAsset consumes unchanged.
//   UnrealEditor-Cmd.exe Project.uproject -run=AuditMesh [-assets=] [-scandir=] [-report=] [-rules=]
// Contract: Docs/Audit.md
UCLASS()
class UAuditMeshCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    UAuditMeshCommandlet();

    virtual int32 Main(const FString& Params) override;
};
