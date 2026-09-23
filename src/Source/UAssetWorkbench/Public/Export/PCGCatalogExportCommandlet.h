#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "PCGCatalogExportCommandlet.generated.h"

// Exports the node dictionary a PCG graph can be built from: every native settings class with its pins and
// writable properties, plus the graphs, settings assets, Blueprint elements and data assets under -scandir.
// Takes no asset list, the output lands at Intermediate/UAssetExport/PCGCatalogExport_r*.json.
//   UnrealEditor-Cmd.exe Project.uproject -run=PCGCatalogExport [-filter=Spawner] [-scandir=/Game]
// Contract: Docs/Export.md
UCLASS()
class UPCGCatalogExportCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    UPCGCatalogExportCommandlet();

    virtual int32 Main(const FString& Params) override;
};
