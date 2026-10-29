#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "RenameMaterialParameterCommandlet.generated.h"

// Renames a parameter on a base material, follows it into every instance override under that material,
// and rewrites Blueprint calls that set it by literal name. Calls fed by a link are listed, never changed.
//   -run=RenameMaterialParameter -material="/Game/M_Base" -from="Old" -to="New" [-blueprints="/Game/BP_A"] [-apply]
// Contract: Docs/Migrate.md
UCLASS()
class URenameMaterialParameterCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    URenameMaterialParameterCommandlet();

    virtual int32 Main(const FString& Params) override;
};
