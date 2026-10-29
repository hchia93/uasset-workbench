#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "AuditNiagaraCommandlet.generated.h"

// Audits Niagara systems against fixed rules FX1-FX5: GPU dynamic bounds, scalability, warmup, emitter count,
// renderers the sim target cannot drive. The report carries a Spec block EditNiagaraSystem consumes unchanged.
//   UnrealEditor-Cmd.exe Project.uproject -run=AuditNiagara [-assets=] [-scandir=] [-report=] [-rules=]
// Contract: Docs/Audit.md
UCLASS()
class UAuditNiagaraCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    UAuditNiagaraCommandlet();

    virtual int32 Main(const FString& Params) override;
};
