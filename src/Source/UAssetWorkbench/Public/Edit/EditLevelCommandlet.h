#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "EditLevelCommandlet.generated.h"

class FJsonObject;
class UWorld;

// Writes properties on actors placed in a level, found by their editor label. Paths reach through arrays,
// structs and instanced sub-objects, which Python refuses for EditInstanceOnly on an inline object in a map.
// Dry run does everything except the save.
//   UnrealEditor-Cmd.exe Project.uproject -run=EditLevel -spec="C:/path/spec.json" [-apply]
// Contract: Docs/Edit.md
UCLASS()
class UEditLevelCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    UEditLevelCommandlet();

    virtual int32 Main(const FString& Params) override;

private:

    // Returns false once anything is rejected, which aborts the run before any package is saved.
    bool ApplyTarget(const TSharedPtr<FJsonObject>& Entry, TSet<UWorld*>& OutTouched, int32& OutOps) const;
};
