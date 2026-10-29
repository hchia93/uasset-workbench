#include "Export/SoundExportCommandlet.h"

// Editor-only by design: reads the editor-only SoundCue node list. Trap any Runtime-type drift early.
static_assert(WITH_EDITOR, "UAssetWorkbench commandlets are editor-only, keep the uplugin Module Type=Editor.");

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundConcurrency.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "Sound/SoundSubmix.h"
#include "Sound/SoundWave.h"

#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

namespace
{
    FString PathOf(const UObject* Object)
    {
        return Object ? Object->GetPathName() : FString();
    }

    // MetaSound lives in a plugin this module does not link. Its source derives from USoundWave, so it is checked first, by class name.
    FString SoundExportType(const USoundBase* Sound)
    {
        if (Sound->GetClass()->GetPackage()->GetName() == TEXT("/Script/MetasoundEngine"))
        {
            return Sound->GetClass()->GetName();
        }

        if (Sound->IsA<USoundCue>())
        {
            return TEXT("SoundCue");
        }

        if (Sound->IsA<USoundWave>())
        {
            return TEXT("SoundWave");
        }

        return Sound->GetClass()->GetName();
    }
}

USoundExportCommandlet::USoundExportCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 USoundExportCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("UAssetWorkbench v%s - SoundExport"), UASSET_WORKBENCH_VERSION_STRING);

    TArray<FString> AssetPaths = UAssetWorkbench::ParseAssetPaths(Params);

    if (AssetPaths.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchExporter, Error, TEXT("No assets specified. Usage: -assets=\"/Game/Path/SC_A,/Game/Path/MS_B\""));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    int32 ExportedCount = 0;

    for (const FString& AssetPath : AssetPaths)
    {
        USoundBase* Sound = LoadObject<USoundBase>(nullptr, *AssetPath);
        if (!Sound)
        {
            UE_LOG(LogUAssetWorkbenchExporter, Warning, TEXT("Failed to load sound: %s"), *AssetPath);
            continue;
        }

        TSharedPtr<FJsonObject> JsonObject = ExportSound(Sound);

        UAssetWorkbench::FExportTarget ExportTarget(AssetPath);
        if (ExportTarget.Save(JsonObject.ToSharedRef()))
        {
            UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Exported: %s -> %s"), *AssetPath, *ExportTarget.GetPath());
            ExportedCount++;
        }
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Export complete. %d/%d sounds exported."), ExportedCount, AssetPaths.Num());
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}

TSharedPtr<FJsonObject> USoundExportCommandlet::ExportSound(USoundBase* Sound) const
{
    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

    Root->SetStringField(TEXT("ExporterVersion"), UASSET_WORKBENCH_VERSION_STRING);
    Root->SetStringField(TEXT("ExportType"), SoundExportType(Sound));
    Root->SetStringField(TEXT("SoundName"), Sound->GetName());
    Root->SetStringField(TEXT("AssetPath"), Sound->GetPathName());
    Root->SetStringField(TEXT("Class"), Sound->GetClass()->GetName());
    Root->SetStringField(TEXT("ExportTimestamp"), FDateTime::Now().ToString());

    Root->SetNumberField(TEXT("Duration"), Sound->GetDuration());
    Root->SetBoolField(TEXT("Looping"), Sound->IsLooping());
    Root->SetNumberField(TEXT("MaxDistance"), Sound->GetMaxDistance());
    Root->SetNumberField(TEXT("Priority"), Sound->Priority);
    Root->SetStringField(TEXT("VirtualizationMode"), StaticEnum<EVirtualizationMode>()->GetNameStringByValue(static_cast<int64>(Sound->VirtualizationMode)));

    // The authored class, empty when the sound falls back to the project default.
    Root->SetStringField(TEXT("SoundClass"), PathOf(Sound->SoundClassObject));
    Root->SetStringField(TEXT("Submix"), PathOf(Sound->SoundSubmixObject));

    TSharedRef<FJsonObject> Attenuation = MakeShared<FJsonObject>();
    Attenuation->SetStringField(TEXT("Asset"), PathOf(Sound->AttenuationSettings));
    if (const USoundCue* Cue = Cast<USoundCue>(Sound))
    {
        Attenuation->SetBoolField(TEXT("Override"), Cue->bOverrideAttenuation);
    }
    Root->SetObjectField(TEXT("Attenuation"), Attenuation);

    TSharedRef<FJsonObject> Concurrency = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> ConcurrencyAssets;
    for (const USoundConcurrency* Asset : Sound->ConcurrencySet)
    {
        if (Asset)
        {
            ConcurrencyAssets.Add(MakeShared<FJsonValueString>(Asset->GetPathName()));
        }
    }
    Concurrency->SetArrayField(TEXT("Assets"), ConcurrencyAssets);
    Concurrency->SetBoolField(TEXT("Override"), Sound->bOverrideConcurrency);
    if (Sound->bOverrideConcurrency)
    {
        Concurrency->SetNumberField(TEXT("MaxCount"), Sound->ConcurrencyOverrides.MaxCount);
        Concurrency->SetStringField(TEXT("ResolutionRule"), StaticEnum<EMaxConcurrentResolutionRule::Type>()->GetNameStringByValue(Sound->ConcurrencyOverrides.ResolutionRule));
    }
    Root->SetObjectField(TEXT("Concurrency"), Concurrency);

    if (USoundCue* Cue = Cast<USoundCue>(Sound))
    {
        Root->SetNumberField(TEXT("VolumeMultiplier"), Cue->VolumeMultiplier);
        Root->SetNumberField(TEXT("PitchMultiplier"), Cue->PitchMultiplier);
        Root->SetNumberField(TEXT("NodeCount"), Cue->AllNodes.Num());

        TSet<USoundNode*> Visited;
        if (Cue->FirstNode)
        {
            Root->SetObjectField(TEXT("FirstNode"), ExportCueNode(Cue->FirstNode, Visited));
        }
    }

    return Root;
}

TSharedPtr<FJsonObject> USoundExportCommandlet::ExportCueNode(USoundNode* Node, TSet<USoundNode*>& Visited) const
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("Id"), Node->GetName());

    // A node fed into two parents appears in full once, later appearances only name it.
    if (Visited.Contains(Node))
    {
        Obj->SetBoolField(TEXT("Shared"), true);
        return Obj;
    }
    Visited.Add(Node);

    Obj->SetStringField(TEXT("Class"), Node->GetClass()->GetName());

    TSharedPtr<FJsonObject> Props = UAssetWorkbench::ExportSubclassProperties(Node, USoundNode::StaticClass());
    if (Props.IsValid() && Props->Values.Num() > 0)
    {
        Obj->SetObjectField(TEXT("Properties"), Props);
    }

    TArray<TSharedPtr<FJsonValue>> Children;
    for (USoundNode* Child : Node->ChildNodes)
    {
        if (Child)
        {
            Children.Add(MakeShared<FJsonValueObject>(ExportCueNode(Child, Visited)));
        }
        else
        {
            Children.Add(MakeShared<FJsonValueNull>());
        }
    }

    if (Children.Num() > 0)
    {
        Obj->SetArrayField(TEXT("Children"), Children);
    }

    return Obj;
}
