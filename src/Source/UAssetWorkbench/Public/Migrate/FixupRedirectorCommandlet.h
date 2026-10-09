#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FixupRedirectorCommandlet.generated.h"

// Resaves every referencer of a redirector so it points at the destination, then deletes the redirector.
// Takes redirector packages or folders, a folder is searched recursively.
//   -run=FixupRedirector -assets="/Game/Audio,/Game/UI/WBP_Old" [-apply]
// Contract: Docs/Migrate.md
UCLASS()
class UFixupRedirectorCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:
    UFixupRedirectorCommandlet();

    virtual int32 Main(const FString& Params) override;
};
