#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "PCGGraphExportCommandlet.generated.h"

// Exports a PCG graph or graph instance to JSON: nodes, pins, edges, settings deltas, parameters, subgraphs.
//   UnrealEditor-Cmd.exe Project.uproject -run=PCGGraphExport -assets="/Game/PCG/PCG_A,/Game/PCG/PCG_B" [-full]
// Contract: Docs/Export.md
UCLASS()
class UPCGGraphExportCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    UPCGGraphExportCommandlet();

    virtual int32 Main(const FString& Params) override;
};
