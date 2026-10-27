#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "EditNiagaraSystemCommandlet.generated.h"

class FJsonObject;
class UNiagaraSystem;

// Edits a Niagara system through one writer per facet: System, Emitters, Renderers, Modules, StaticSwitches,
// Inputs. One target loads the system once, runs every writer the spec names in that order, compiles and
// saves once. A writer that fails aborts the target, so a system never lands half-edited.
//   UnrealEditor-Cmd.exe Project.uproject -run=EditNiagaraSystem -spec="C:/path/spec.json" [-apply]
// Contract: Docs/Edit.md
UCLASS()
class UEditNiagaraSystemCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:
    UEditNiagaraSystemCommandlet();

    virtual int32 Main(const FString& Params) override;

private:
    // Returns false once any writer fails, leaving the system unsaved.
    bool ApplyTarget(const TSharedPtr<FJsonObject>& Entry, TSet<UNiagaraSystem*>& OutTouched, int32& OutOps) const;

    // A system whose scripts fail to compile is not saved, the editor would open it broken.
    bool CompileSystem(UNiagaraSystem* System) const;
};
