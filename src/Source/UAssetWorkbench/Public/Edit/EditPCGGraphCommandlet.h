#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "EditPCGGraphCommandlet.generated.h"

class FJsonObject;
class UPCGGraph;

// Edits a PCG graph through one writer per facet: Parameters, GraphSettings, Nodes, Properties, Edges,
// Layout. One target loads the graph once, runs every writer the spec names in that order, then saves
// once. A writer that fails aborts the target, so a graph never lands half-edited.
//   UnrealEditor-Cmd.exe Project.uproject -run=EditPCGGraph -spec="C:/path/spec.json" [-apply]
// Contract: Docs/Edit.md
UCLASS()
class UEditPCGGraphCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:
    UEditPCGGraphCommandlet();

    virtual int32 Main(const FString& Params) override;

private:
    // Returns false once any writer fails, leaving the graph unsaved.
    bool ApplyTarget(const TSharedPtr<FJsonObject>& Entry, TSet<UPCGGraph*>& OutTouched, int32& OutOps) const;
};
