#include "Edit/EditNiagaraSystemCommandlet.h"
#include "Edit/NiagaraSystemWriter.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"

namespace
{
    // Fixed order, not spec key order: emitters must exist before their renderers and stages, modules before
    // their switches, and switches before the inputs they reveal.
    TArray<TUniquePtr<INiagaraSystemWriter>> MakeWriters()
    {
        TArray<TUniquePtr<INiagaraSystemWriter>> Writers;
        Writers.Add(MakeNiagaraSystemSettingsWriter());
        Writers.Add(MakeNiagaraEmitterWriter());
        Writers.Add(MakeNiagaraRendererWriter());
        Writers.Add(MakeNiagaraModuleWriter());
        Writers.Add(MakeNiagaraStaticSwitchWriter());
        Writers.Add(MakeNiagaraInputWriter());
        return Writers;
    }

    bool ReportCompileErrors(const UNiagaraScript* Script, const FString& Owner)
    {
        if (!Script || Script->GetLastCompileStatus() != ENiagaraScriptCompileStatus::NCS_Error)
        {
            return true;
        }
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s script %s failed to compile: %s"), *Owner, *Script->GetName(), *Script->GetVMExecutableData().ErrorMsg);
        return false;
    }
}

UEditNiagaraSystemCommandlet::UEditNiagaraSystemCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UEditNiagaraSystemCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("UAssetWorkbench v%s - EditNiagaraSystem"), UASSET_WORKBENCH_VERSION_STRING);

    FString SpecPath;
    if (!FParse::Value(*Params, TEXT("spec="), SpecPath))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("No spec specified. Usage: -spec=\"C:/path/spec.json\" [-apply]"));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    SpecPath = SpecPath.TrimQuotes();
    const bool bApply = FParse::Param(*Params, TEXT("apply"));

    // Writers mutate either way, only save reads bApply, so a dry run relies on the process exiting to
    // throw the mutation away. In-editor there is no such exit.
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

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("EditNiagaraSystem: %d target(s) %s"), Targets->Num(), bApply ? TEXT("[APPLY]") : TEXT("[DRY RUN]"));

    TSet<UNiagaraSystem*> Touched;
    int32 Ops = 0;
    for (const TSharedPtr<FJsonValue>& Value : *Targets)
    {
        const TSharedPtr<FJsonObject>& Entry = Value->AsObject();
        UAssetWorkbench::FFailureReasonCapture FailureReason;
        if (!Entry.IsValid() || !ApplyTarget(Entry, Touched, Ops))
        {
            UAssetWorkbench::ReportTargetFailure(TEXT("EditNiagaraSystem"), UAssetWorkbench::DescribeSpecTarget(Entry), FailureReason.GetReason());
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }
    }

    if (!bApply)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Done. %d operation(s) staged across %d system(s) (dry run, not saved)."), Ops, Touched.Num());
        return ToExitCode(EUAssetWorkbenchExitType::Success);
    }

    for (UNiagaraSystem* System : Touched)
    {
        UAssetWorkbench::FFailureReasonCapture FailureReason;
        if (!CompileSystem(System) || !UAssetWorkbench::CompileAndSavePackage(System, /* bCompileBlueprint */ false))
        {
            UAssetWorkbench::ReportTargetFailure(TEXT("EditNiagaraSystem"), System->GetPathName(), FailureReason.GetReason());
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }
    }

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Done. %d operation(s) across %d system(s) (saved)."), Ops, Touched.Num());
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}

bool UEditNiagaraSystemCommandlet::ApplyTarget(const TSharedPtr<FJsonObject>& Entry, TSet<UNiagaraSystem*>& OutTouched, int32& OutOps) const
{
    FString AssetPath;
    if (!Entry->TryGetStringField(TEXT("AssetPath"), AssetPath))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Target has no AssetPath field"));
        return false;
    }

    UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *AssetPath);
    if (!System)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to load Niagara system: %s"), *AssetPath);
        return false;
    }

    // An earlier compile still in flight would race the graph edits below.
    System->WaitForCompilationComplete(/* bIncludingGPUShaders */ false, /* bShowProgress */ false);

    FNiagaraSystemEditContext Context;
    Context.System = System;
    Context.AssetPath = AssetPath;

    int32 Matched = 0;
    for (const TUniquePtr<INiagaraSystemWriter>& Writer : MakeWriters())
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
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s writes nothing. Expected one of System, Emitters, Renderers, Modules, StaticSwitches, Inputs"), *AssetPath);
        return false;
    }

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s: %d change(s), %d entr%s already held the value"), *AssetPath, Context.Ops, Context.Unchanged, Context.Unchanged == 1 ? TEXT("y") : TEXT("ies"));

    // Nothing differed, so there is nothing to compile or save.
    if (Context.Ops == 0)
    {
        return true;
    }

    OutOps += Context.Ops;
    OutTouched.Add(System);
    return true;
}

bool UEditNiagaraSystemCommandlet::CompileSystem(UNiagaraSystem* System) const
{
    // Forced: a rapid iteration value change leaves the graph change id untouched, an unforced request skips it.
    System->RequestCompile(/* bForce */ true);
    System->WaitForCompilationComplete(/* bIncludingGPUShaders */ false, /* bShowProgress */ false);

    bool bOk = ReportCompileErrors(System->GetSystemSpawnScript(), System->GetName());
    bOk &= ReportCompileErrors(System->GetSystemUpdateScript(), System->GetName());
    for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
    {
        FVersionedNiagaraEmitterData* EmitterData = Handle.GetIsEnabled() ? Handle.GetEmitterData() : nullptr;
        if (!EmitterData)
        {
            continue;
        }
        TArray<UNiagaraScript*> Scripts;
        EmitterData->GetScripts(Scripts, /* bCompilableOnly */ true);
        for (const UNiagaraScript* Script : Scripts)
        {
            bOk &= ReportCompileErrors(Script, Handle.GetName().ToString());
        }
    }
    return bOk;
}
