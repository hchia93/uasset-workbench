#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "NiagaraSystemExportCommandlet.generated.h"

class UNiagaraRendererProperties;
class UNiagaraSystem;
struct FNiagaraEmitterHandle;
struct FNiagaraParameterStore;

// Exports a Niagara System as its stack: system and emitter settings, user parameters, every stage with its
// ordered modules, static switches and per-input value mode, and renderers. Addresses match EditNiagaraSystem.
//   UnrealEditor-Cmd.exe Project.uproject -run=NiagaraSystemExport -assets="/Game/Path/NS_A,/Game/Path/NS_B"
// Contract: Docs/Export.md
UCLASS()
class UNiagaraSystemExportCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    UNiagaraSystemExportCommandlet();

    virtual int32 Main(const FString& Params) override;

private:

    TSharedPtr<FJsonObject> ExportNiagaraSystem(UNiagaraSystem* System) const;
    TSharedPtr<FJsonObject> ExportEmitter(UNiagaraSystem* System, const FNiagaraEmitterHandle& EmitterHandle) const;
    TSharedPtr<FJsonObject> ExportUserParameters(const FNiagaraParameterStore& Store) const;
    TSharedPtr<FJsonObject> ExportRendererProperties(UNiagaraRendererProperties* Renderer, int32 Index) const;
    TArray<TSharedPtr<FJsonValue>> ExportStages(UNiagaraSystem* System, const FNiagaraEmitterHandle* EmitterHandle) const;

    // EditNiagaraSystem spec of the current editable state, built from the export so both stay in step.
    // Applied unchanged it rewrites every value to itself, an agent edits the lines it wants to change.
    TSharedRef<FJsonObject> BuildEditSpec(const TSharedRef<FJsonObject>& Export) const;
};
