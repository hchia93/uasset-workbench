#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "SoundExportCommandlet.generated.h"

class FJsonObject;
class USoundBase;
class USoundNode;

// Exports sounds to JSON: sound class, submix, attenuation, concurrency and voice settings for any USoundBase,
// plus the node graph of a SoundCue.
//   UnrealEditor-Cmd.exe Project.uproject -run=SoundExport -assets="/Game/Path/SC_A,/Game/Path/MS_B"
// Contract: Docs/Export.md
UCLASS()
class USoundExportCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    USoundExportCommandlet();

    virtual int32 Main(const FString& Params) override;

private:

    TSharedPtr<FJsonObject> ExportSound(USoundBase* Sound) const;
    TSharedPtr<FJsonObject> ExportCueNode(USoundNode* Node, TSet<USoundNode*>& Visited) const;
};
