#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "RenameAssetCommandlet.generated.h"

// Renames or moves assets and repoints every referencer, hard and soft. Reports the referencer count per
// asset before and after, so a rename that silently dropped a reference is visible instead of assumed.
// A folder source moves every asset under it. Redirectors left behind are fixed up and deleted unless -keepredirectors.
//   -run=RenameAsset -pairs="/Game/A>/Game/Sub/B,/Game/Folder>/Game/NewFolder" [-apply] [-keepredirectors]
// Contract: Docs/Migrate.md
UCLASS()
class URenameAssetCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:
    URenameAssetCommandlet();

    virtual int32 Main(const FString& Params) override;
};
