#include "Edit/EditBlueprintCommandlet.h"
#include "Edit/BlueprintWriter.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
    // Fixed order, not spec key order: Graph reads components and variables the earlier writers made,
    // and Layout reads the node Ids Graph registered.
    TArray<TUniquePtr<IBlueprintWriter>> MakeWriters()
    {
        TArray<TUniquePtr<IBlueprintWriter>> Writers;
        Writers.Add(MakeBlueprintComponentWriter());
        Writers.Add(MakeBlueprintWidgetWriter());
        Writers.Add(MakeBlueprintWidgetAnimationWriter());
        Writers.Add(MakeBlueprintVariableWriter());
        Writers.Add(MakeBlueprintDefaultsWriter());
        Writers.Add(MakeBlueprintFunctionWriter());
        Writers.Add(MakeBlueprintDispatcherWriter());
        Writers.Add(MakeBlueprintInterfaceWriter());
        Writers.Add(MakeBlueprintStateMachineWriter());
        Writers.Add(MakeBlueprintGraphWriter());
        Writers.Add(MakeBlueprintLayoutWriter());
        return Writers;
    }
}

UEditBlueprintCommandlet::UEditBlueprintCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UEditBlueprintCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("UAssetWorkbench v%s - EditBlueprint"), UASSET_WORKBENCH_VERSION_STRING);

    FString SpecPath;
    if (!FParse::Value(*Params, TEXT("spec="), SpecPath))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("No spec specified. Usage: -spec=\"C:/path/spec.json\" [-apply]"));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    SpecPath = SpecPath.TrimQuotes();
    const bool bApply = FParse::Param(*Params, TEXT("apply"));

    // Writers mutate either way, only compile and save read bApply, so a dry run relies on the process
    // exiting to throw the mutation away. In-editor there is no such exit.
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

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("EditBlueprint: %d target(s) %s"), Targets->Num(), bApply ? TEXT("[APPLY]") : TEXT("[DRY RUN]"));

    // Value is whether anything structural ran, so a layout-only edit skips the recompile.
    TMap<UBlueprint*, bool> Touched;
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
        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Done. %d operation(s) staged across %d blueprint(s) (dry run, not saved)."), Ops, Touched.Num());
        return ToExitCode(EUAssetWorkbenchExitType::Success);
    }

    for (const TPair<UBlueprint*, bool>& Pair : Touched)
    {
        if (!UAssetWorkbench::CompileAndSavePackage(Pair.Key, Pair.Value))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to save package for %s"), *Pair.Key->GetPathName());
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }
    }

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Done. %d operation(s) across %d blueprint(s) (saved)."), Ops, Touched.Num());
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}

bool UEditBlueprintCommandlet::ApplyTarget(const TSharedPtr<FJsonObject>& Entry, TMap<UBlueprint*, bool>& OutTouched, int32& OutOps) const
{
    FString AssetPath;
    if (!Entry->TryGetStringField(TEXT("AssetPath"), AssetPath))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Target has no AssetPath field"));
        return false;
    }

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
    if (!Blueprint)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to load Blueprint: %s"), *AssetPath);
        return false;
    }

    FBlueprintEditContext Context;
    Context.Blueprint = Blueprint;
    Context.AssetPath = AssetPath;

    TArray<TUniquePtr<IBlueprintWriter>> Writers = MakeWriters();

    int32 Matched = 0;
    for (const TUniquePtr<IBlueprintWriter>& Writer : Writers)
    {
        const TSharedPtr<FJsonValue> Section = Entry->TryGetField(Writer->GetSpecKey());
        if (!Section.IsValid())
        {
            continue;
        }

        ++Matched;
        if (!Writer->Apply(Context, Section))
        {
            return false;
        }
    }

    if (Matched == 0)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s writes nothing. Expected one of Components, Widgets, WidgetAnimations, Variables, Defaults, Functions, Dispatchers, Interfaces, StateMachines, Graph, Layout"), *AssetPath);
        return false;
    }

    OutOps += Context.Ops;

    bool& NeedsRecompile = OutTouched.FindOrAdd(Blueprint, false);
    NeedsRecompile |= Context.bNeedsStructuralRecompile;
    return true;
}
