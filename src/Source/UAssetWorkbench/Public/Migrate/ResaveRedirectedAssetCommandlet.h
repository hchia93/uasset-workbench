#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "ResaveRedirectedAssetCommandlet.generated.h"

// Finds packages whose name table still holds an old name from the project's [CoreRedirects] and resaves them, so
// the redirect can be dropped. Maps and their external actor packages are scanned too, instanced subobjects live there.
//   -run=ResaveRedirectedAsset [-scandir=/Game] [-oldnames="OldActor,/Game/Old/BP_A"] [-apply]
// Contract: Docs/Migrate.md
UCLASS()
class UResaveRedirectedAssetCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:
    UResaveRedirectedAssetCommandlet();

    virtual int32 Main(const FString& Params) override;
};
