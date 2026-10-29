#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "EditBehaviorTreeCommandlet.generated.h"

class FJsonObject;

// Edits a Behavior Tree through its editor graph: adds, deletes and moves composites and tasks, adds,
// deletes and reorders decorators and services, writes node properties and blackboard key selectors, sets
// the blackboard asset. Keys on the blackboard itself belong to EditBlackboard. The runtime tree is rebuilt from the graph. Dry run does everything except the save.
//   UnrealEditor-Cmd.exe Project.uproject -run=EditBehaviorTree -spec="C:/path/spec.json" [-apply]
// Contract: Docs/Edit.md
UCLASS()
class UEditBehaviorTreeCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    UEditBehaviorTreeCommandlet();

    virtual int32 Main(const FString& Params) override;

private:

    // Returns false once anything is rejected, which aborts the run before any package is saved.
    bool ApplyTarget(const TSharedPtr<FJsonObject>& Entry, TSet<UObject*>& OutTouched, int32& OutOps) const;
};
