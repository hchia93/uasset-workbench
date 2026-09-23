#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "AuditPCGCommandlet.generated.h"

// Static checks on PCG graphs, rules P1-P7, and on the PCGComponents a level places, rule P8. Nothing
// generates, so runtime node errors are out of reach. Graphs come from -assets= or a -scandir= scan,
// levels from -levels= or -withlevels.
//   UnrealEditor-Cmd.exe Project.uproject -run=AuditPCG [-assets=] [-scandir=] [-levels=] [-withlevels] [-report=]
// Contract: Docs/Audit.md
UCLASS()
class UAuditPCGCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    UAuditPCGCommandlet();

    virtual int32 Main(const FString& Params) override;
};
