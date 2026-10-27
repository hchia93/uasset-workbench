#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "RenameDefaultSubobjectCommandlet.generated.h"

// Renames a C++ default subobject on every Blueprint and placed actor that serialized it, so changing the
// CreateDefaultSubobject name leaves no orphan component. Runs before the C++ rename, ResaveAsset follows it.
//   -run=RenameDefaultSubobject -classes="AMyActor,UMyComponent" -oldname=Old -newname=New [-scandir=/Game] [-apply]
// Contract: Docs/Migrate.md
UCLASS()
class URenameDefaultSubobjectCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:
    URenameDefaultSubobjectCommandlet();

    virtual int32 Main(const FString& Params) override;
};
