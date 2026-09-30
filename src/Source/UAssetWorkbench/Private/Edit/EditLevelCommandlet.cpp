#include "Edit/EditLevelCommandlet.h"

// Editor-only by design: actor labels are editor-only data. Trap any Runtime-type drift early.
static_assert(WITH_EDITOR, "UAssetWorkbench commandlets are editor-only, keep the uplugin Module Type=Editor.");

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

namespace
{
    // Takes the package path or the full object path of a map.
    FString ToWorldObjectPath(const FString& AssetPath)
    {
        if (AssetPath.Contains(TEXT(".")))
        {
            return AssetPath;
        }

        return AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
    }

    TArray<AActor*> FindActorsByLabel(const UWorld* World, const FString& Label)
    {
        TArray<AActor*> Matches;
        if (!World->PersistentLevel)
        {
            return Matches;
        }

        for (AActor* Actor : World->PersistentLevel->Actors)
        {
            if (Actor && Actor->GetActorLabel() == Label)
            {
                Matches.Add(Actor);
            }
        }

        return Matches;
    }
}

UEditLevelCommandlet::UEditLevelCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UEditLevelCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("UAssetWorkbench v%s - EditLevel"), UASSET_WORKBENCH_VERSION_STRING);

    FString SpecPath;
    if (!FParse::Value(*Params, TEXT("spec="), SpecPath))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("No spec specified. Usage: -spec=\"C:/path/spec.json\" [-apply]"));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    SpecPath = SpecPath.TrimQuotes();
    const bool bApply = FParse::Param(*Params, TEXT("apply"));

    // Writes happen either way, only save reads bApply. In-editor there is no process exit to discard them.
    if (!bApply && !IsRunningCommandlet())
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Dry run needs its own process to discard the in-memory edit. Close the editor and run the commandlet, or pass -apply."));
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    FString SpecText;
    if (!FFileHelper::LoadFileToString(SpecText, *SpecPath))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to read spec: %s"), *SpecPath);
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    TSharedPtr<FJsonObject> Spec;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(SpecText);
    if (!FJsonSerializer::Deserialize(Reader, Spec) || !Spec.IsValid())
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Spec is not valid JSON: %s"), *SpecPath);
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    const TArray<TSharedPtr<FJsonValue>>* Targets = nullptr;
    if (!Spec->TryGetArrayField(TEXT("Targets"), Targets))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Spec has no Targets array"));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("EditLevel: %d target(s) %s"), Targets->Num(), bApply ? TEXT("[APPLY]") : TEXT("[DRY RUN]"));

    TSet<UWorld*> Touched;
    int32 Ops = 0;
    for (const TSharedPtr<FJsonValue>& Value : *Targets)
    {
        const TSharedPtr<FJsonObject>& Entry = Value->AsObject();
        if (!Entry.IsValid() || !ApplyTarget(Entry, Touched, Ops))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Target failed, nothing saved"));
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }
    }

    if (!bApply)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Done. %d property write(s) staged across %d level(s) (dry run, not saved)."), Ops, Touched.Num());
        return ToExitCode(EUAssetWorkbenchExitType::Success);
    }

    for (UWorld* World : Touched)
    {
        if (!UAssetWorkbench::CompileAndSavePackage(World, false))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to save package for %s"), *World->GetPathName());
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }
    }

    UAssetWorkbench::WarnIfWrittenOutsideEditor();

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Done. %d property write(s) across %d level(s) (saved)."), Ops, Touched.Num());
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}

bool UEditLevelCommandlet::ApplyTarget(const TSharedPtr<FJsonObject>& Entry, TSet<UWorld*>& OutTouched, int32& OutOps) const
{
    FString AssetPath;
    if (!Entry->TryGetStringField(TEXT("AssetPath"), AssetPath))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Target has no AssetPath field"));
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
    if (!Entry->TryGetArrayField(TEXT("Actors"), Actors) || Actors->IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s writes nothing. Expected Actors"), *AssetPath);
        return false;
    }

    UWorld* World = LoadObject<UWorld>(nullptr, *ToWorldObjectPath(AssetPath));
    if (!World)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to load level: %s"), *AssetPath);
        return false;
    }

    for (const TSharedPtr<FJsonValue>& ActorValue : *Actors)
    {
        const TSharedPtr<FJsonObject> ActorEntry = ActorValue->AsObject();
        FString Label;
        if (!ActorEntry.IsValid() || !ActorEntry->TryGetStringField(TEXT("Label"), Label))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: actor entry has no Label field"), *AssetPath);
            return false;
        }

        const TSharedPtr<FJsonObject>* PropertiesField = nullptr;
        if (!ActorEntry->TryGetObjectField(TEXT("Properties"), PropertiesField) || (*PropertiesField)->Values.IsEmpty())
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s %s writes nothing. Expected Properties"), *AssetPath, *Label);
            return false;
        }
        const TSharedPtr<FJsonObject>& Properties = *PropertiesField;

        // Labels are not unique in the engine, a second match would make the write land on a guess.
        const TArray<AActor*> Matches = FindActorsByLabel(World, Label);
        if (Matches.Num() != 1)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has %d actor(s) labelled %s, expected exactly one"), *AssetPath, Matches.Num(), *Label);
            return false;
        }
        AActor* Actor = Matches[0];

        TMap<FString, FString> Before;
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
        {
            Before.Add(Pair.Key, UAssetWorkbench::ReadPropertyPathText(Actor, Pair.Key));
        }

        Actor->Modify();

        int32 Failures = 0;
        const int32 Written = UAssetWorkbench::ApplyProperties(Actor, Properties, Failures);
        if (Failures > 0)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%d propert(ies) rejected on %s %s, nothing saved"), Failures, *AssetPath, *Label);
            return false;
        }

        for (const TPair<FString, FString>& Pair : Before)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s %s %s: %s -> %s"), *AssetPath, *Label, *Pair.Key, *Pair.Value, *UAssetWorkbench::ReadPropertyPathText(Actor, Pair.Key));
        }

        OutOps += Written;
    }

    OutTouched.Add(World);
    return true;
}
