#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "EditBlackboardCommandlet.generated.h"

class FJsonObject;

// Edits a Blackboard asset: adds, deletes and modifies its own keys, writes key type properties, sets the
// parent blackboard. Derived blackboards loaded in the run pick up the change. Dry run does everything except the save.
//   UnrealEditor-Cmd.exe Project.uproject -run=EditBlackboard -spec="C:/path/spec.json" [-apply]
// Contract: Docs/Edit.md
UCLASS()
class UEditBlackboardCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    UEditBlackboardCommandlet();

    virtual int32 Main(const FString& Params) override;

private:

    // Returns false once anything is rejected, which aborts the run before any package is saved.
    bool ApplyTarget(const TSharedPtr<FJsonObject>& Entry, TSet<UObject*>& OutTouched, int32& OutOps) const;
};
