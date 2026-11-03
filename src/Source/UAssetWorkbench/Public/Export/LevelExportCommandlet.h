#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "LevelExportCommandlet.generated.h"

// Exports level contents to JSON. Actors and components carry only their delta against the archetype,
// which is what the umap itself persists. Collision, mesh and mobility are hoisted to top-level keys for
// grep, and an instanced component past the dump threshold records count, bounds and a few samples.
//   UnrealEditor-Cmd.exe Project.uproject -run=LevelExport -assets="/Game/Maps/L_A,/Game/Maps/L_B"
// Contract: Docs/Export.md
UCLASS()
class ULevelExportCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    ULevelExportCommandlet();

    virtual int32 Main(const FString& Params) override;

private:

    static constexpr int32 kInstanceDumpThreshold = 200;
    static constexpr int32 kInstanceSampleCount = 5;

    TSharedPtr<FJsonObject> ExportLevel(class UWorld* World, const FString& LevelPath) const;
    TSharedPtr<FJsonObject> ExportActor(class AActor* Actor) const;
    TSharedPtr<FJsonObject> ExportComponent(class UActorComponent* Component) const;
    TSharedPtr<FJsonObject> ExportWorldSettings(class AWorldSettings* WorldSettings) const;
    TSharedPtr<FJsonObject> ExportDeltaProperties(UObject* Object, UObject* Archetype) const;
    TSharedPtr<FJsonObject> ExportInstancedSubobject(UObject* SubObject) const;

    void AddInstancedComponentData(class UInstancedStaticMeshComponent* ISMComp, TSharedPtr<FJsonObject>& OutJson) const;
    void AddTransformField(const FTransform& Transform, const FString& FieldName, TSharedPtr<FJsonObject>& OutJson) const;

};
