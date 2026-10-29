#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "AuditSoundCommandlet.generated.h"

// Audits playable sounds against fixed rules S1-S3: no concurrency, spatial use without attenuation, no sound
// class. Waves that only play through a SoundCue or MetaSound are covered by that outer sound and skipped.
//   UnrealEditor-Cmd.exe Project.uproject -run=AuditSound [-assets=] [-scandir=] [-report=] [-rules=]
// Contract: Docs/Audit.md
UCLASS()
class UAuditSoundCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    UAuditSoundCommandlet();

    virtual int32 Main(const FString& Params) override;
};
