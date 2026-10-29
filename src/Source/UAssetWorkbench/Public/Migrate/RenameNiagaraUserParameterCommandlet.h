#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "RenameNiagaraUserParameterCommandlet.generated.h"

// Renames a Niagara system user parameter with every link, binding and renderer inside the system, then
// follows it into Blueprint calls by literal name and Niagara component overrides. Calls fed by a link are listed, never changed.
//   -run=RenameNiagaraUserParameter -system="/Game/NS_Foo" -from="Old" -to="New" [-blueprints="/Game/BP_A"] [-apply]
// Contract: Docs/Migrate.md
UCLASS()
class URenameNiagaraUserParameterCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    URenameNiagaraUserParameterCommandlet();

    virtual int32 Main(const FString& Params) override;
};
