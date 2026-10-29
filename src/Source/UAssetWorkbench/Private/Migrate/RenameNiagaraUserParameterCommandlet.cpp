#include "Migrate/RenameNiagaraUserParameterCommandlet.h"

// Editor-only by design: renames Niagara graph parameters and editor data. Trap any Runtime-type drift early.
static_assert(WITH_EDITOR, "UAssetWorkbench commandlets are editor-only, keep the uplugin Module Type=Editor.");

#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/Parse.h"
#include "NiagaraCommon.h"
#include "NiagaraComponent.h"
#include "NiagaraDataInterfaceArrayFunctionLibrary.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraGraph.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemEditorData.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"

#include "Migrate/ParameterCallRewriter.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

namespace
{
    // System to the BP that holds it, and one more hop for the BP that spawns that holder.
    constexpr int32 kBlueprintSearchDepth = 2;

    const TCHAR* kUserPrefix = TEXT("User.");

    struct FComponentOverride
    {
        UBlueprint* Blueprint = nullptr;
        UNiagaraComponent* Template = nullptr;
        FName Component;
        FNiagaraVariant Value;
    };

    FString StripUserPrefix(const FString& Name)
    {
        return Name.StartsWith(kUserPrefix) ? Name.RightChop(FCString::Strlen(kUserPrefix)) : Name;
    }

    // Keyed by the owner the log names, the system itself or an emitter handle.
    TArray<TPair<FString, UNiagaraGraph*>> CollectGraphs(UNiagaraSystem* System)
    {
        TArray<TPair<FString, UNiagaraGraph*>> Graphs;
        auto AddSource = [&Graphs](const FString& Owner, UNiagaraScriptSourceBase* SourceBase)
        {
            UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(SourceBase);
            if (Source && Source->NodeGraph)
            {
                Graphs.Add({ Owner, Source->NodeGraph });
            }
        };

        if (UNiagaraScript* SpawnScript = System->GetSystemSpawnScript())
        {
            AddSource(TEXT("system"), SpawnScript->GetLatestSource());
        }

        for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
        {
            if (FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData())
            {
                AddSource(FString::Printf(TEXT("emitter %s"), *Handle.GetName().ToString()), Data->GraphSource);
            }
        }

        return Graphs;
    }

    int32 CountPinReferences(const UNiagaraGraph* Graph, FName Name)
    {
        int32 Count = 0;
        for (const UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node)
            {
                continue;
            }

            for (const UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin && !Pin->bOrphanedPin && Pin->PinName == Name)
                {
                    ++Count;
                }
            }
        }

        return Count;
    }

    int32 RenameBindingsInValue(const FProperty* Property, void* Value, FName From, FName To, bool bApply);

    // Walks structs and arrays only, the way the engine's own reference replace finds user parameter bindings.
    int32 RenameBindings(const UStruct* Struct, void* Data, FName From, FName To, bool bApply)
    {
        int32 Count = 0;
        for (TFieldIterator<FProperty> It(Struct); It; ++It)
        {
            if (It->HasAnyPropertyFlags(CPF_Transient))
            {
                continue;
            }

            for (int32 Index = 0; Index < It->ArrayDim; ++Index)
            {
                Count += RenameBindingsInValue(*It, It->ContainerPtrToValuePtr<void>(Data, Index), From, To, bApply);
            }
        }

        return Count;
    }

    int32 RenameBindingsInValue(const FProperty* Property, void* Value, FName From, FName To, bool bApply)
    {
        if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
        {
            if (StructProperty->Struct == FNiagaraUserParameterBinding::StaticStruct())
            {
                FNiagaraUserParameterBinding* Binding = static_cast<FNiagaraUserParameterBinding*>(Value);
                if (Binding->Parameter.GetName() != From)
                {
                    return 0;
                }

                if (bApply)
                {
                    Binding->Parameter.SetName(To);
                }
                return 1;
            }

            return RenameBindings(StructProperty->Struct, Value, From, To, bApply);
        }

        if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
        {
            if (!ArrayProperty->Inner->IsA<FStructProperty>())
            {
                return 0;
            }

            int32 Count = 0;
            FScriptArrayHelper Helper(ArrayProperty, Value);
            for (int32 Index = 0; Index < Helper.Num(); ++Index)
            {
                Count += RenameBindingsInValue(ArrayProperty->Inner, Helper.GetRawPtr(Index), From, To, bApply);
            }
            return Count;
        }

        return 0;
    }

    int32 RenameBindingsInPackage(UNiagaraSystem* System, FName From, FName To, bool bApply)
    {
        int32 Count = 0;
        ForEachObjectWithPackage(System->GetPackage(), [&](UObject* Object)
        {
            const int32 Found = RenameBindings(Object->GetClass(), Object, From, To, false);
            if (Found > 0 && bApply)
            {
                Object->Modify();
                RenameBindings(Object->GetClass(), Object, From, To, true);
            }
            Count += Found;
            return true;
        });

        return Count;
    }

    bool ReportCompileErrors(const UNiagaraScript* Script, const FString& Owner)
    {
        if (!Script || Script->GetLastCompileStatus() != ENiagaraScriptCompileStatus::NCS_Error)
        {
            return true;
        }
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("%s script %s failed to compile: %s"), *Owner, *Script->GetName(), *Script->GetVMExecutableData().ErrorMsg);
        return false;
    }

    bool CompileSystem(UNiagaraSystem* System)
    {
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

    void CollectComponentOverrides(UBlueprint* Blueprint, UNiagaraSystem* System, const FNiagaraVariableBase& Variable, TArray<FComponentOverride>& OutOverrides)
    {
        if (!Blueprint->SimpleConstructionScript)
        {
            return;
        }

        for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
        {
            UNiagaraComponent* Template = Node ? Cast<UNiagaraComponent>(Node->ComponentTemplate) : nullptr;
            if (Template && Template->GetAsset() == System && Template->HasParameterOverride(Variable))
            {
                OutOverrides.Add({ Blueprint, Template, Node->GetVariableName(), Template->GetCurrentParameterValue(Variable) });
            }
        }
    }
}

URenameNiagaraUserParameterCommandlet::URenameNiagaraUserParameterCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 URenameNiagaraUserParameterCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("UAssetWorkbench v%s - RenameNiagaraUserParameter"), UASSET_WORKBENCH_VERSION_STRING);

    FString SystemPath;
    FString FromText;
    FString ToText;
    FParse::Value(*Params, TEXT("system="), SystemPath);
    FParse::Value(*Params, TEXT("from="), FromText);
    FParse::Value(*Params, TEXT("to="), ToText);
    const bool bApply = FParse::Param(*Params, TEXT("apply"));

    if (SystemPath.IsEmpty() || FromText.IsEmpty() || ToText.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Usage: -run=RenameNiagaraUserParameter -system=\"/Game/NS_Foo\" -from=\"Old\" -to=\"New\" [-blueprints=\"/Game/BP_A\"] [-apply]"));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *SystemPath);
    if (!System)
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Failed to load Niagara system: %s"), *SystemPath);
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    // An earlier compile still in flight would race the graph edits below.
    System->WaitForCompilationComplete(/* bIncludingGPUShaders */ false, /* bShowProgress */ false);

    const FString BareFrom = StripUserPrefix(FromText);
    const FString BareTo = StripUserPrefix(ToText);
    const FName FullFrom(*(FString(kUserPrefix) + BareFrom));
    const FName FullTo(*(FString(kUserPrefix) + BareTo));

    TArray<FNiagaraVariable> UserParameters;
    System->GetExposedParameters().GetParameters(UserParameters);

    const FNiagaraVariable* Found = UserParameters.FindByPredicate([FullFrom](const FNiagaraVariable& Variable) { return Variable.GetName() == FullFrom; });
    if (!Found)
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("%s has no user parameter %s"), *System->GetPathName(), *FullFrom.ToString());
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    if (UserParameters.ContainsByPredicate([FullTo](const FNiagaraVariable& Variable) { return Variable.GetName() == FullTo; }))
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("%s already has a user parameter %s"), *System->GetPathName(), *FullTo.ToString());
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    const FNiagaraVariable Variable = *Found;
    const FNiagaraVariable NewVariable(Variable.GetType(), FullTo);

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("RenameNiagaraUserParameter: %s %s -> %s %s"), *System->GetPathName(), *FullFrom.ToString(), *FullTo.ToString(), bApply ? TEXT("[APPLY]") : TEXT("[DRY RUN]"));

    TArray<UNiagaraGraph*> ReferencingGraphs;
    for (const TPair<FString, UNiagaraGraph*>& Entry : CollectGraphs(System))
    {
        const int32 Pins = CountPinReferences(Entry.Value, FullFrom);
        if (Pins > 0)
        {
            ReferencingGraphs.AddUnique(Entry.Value);
            UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("  %s graph: %d pin(s)"), *Entry.Key, Pins);
        }
    }

    const int32 Bindings = RenameBindingsInPackage(System, FullFrom, FullTo, false);
    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("  %d user parameter binding(s) in renderers and data interfaces"), Bindings);

    TArray<FString> BlueprintPaths = UAssetWorkbench::ParsePathList(Params, TEXT("-blueprints="));
    if (BlueprintPaths.IsEmpty())
    {
        BlueprintPaths = UAssetWorkbench::CollectReferencingBlueprints({ System->GetPackage()->GetFName() }, kBlueprintSearchDepth);
    }

    // Component setters take the bare name or the User. form, both resolve to the same parameter.
    UAssetWorkbench::FParameterCallMatcher Matcher;
    Matcher.OwnerClasses = { UNiagaraComponent::StaticClass(), UNiagaraFunctionLibrary::StaticClass(), UNiagaraDataInterfaceArrayFunctionLibrary::StaticClass() };
    Matcher.PinNames = { TEXT("InVariableName"), TEXT("OverrideName"), TEXT("VariableName"), TEXT("ParameterName") };
    Matcher.Rewrite = [&BareFrom, &BareTo, FullFrom, FullTo](const FString& Literal, FString& OutRewritten)
    {
        if (Literal == FullFrom.ToString())
        {
            OutRewritten = FullTo.ToString();
            return true;
        }

        if (Literal == BareFrom)
        {
            OutRewritten = BareTo;
            return true;
        }

        return false;
    };

    TArray<UBlueprint*> TouchedBlueprints;
    TArray<UAssetWorkbench::FParameterCallSite> Sites;
    TArray<FComponentOverride> Overrides;
    for (const FString& Path : BlueprintPaths)
    {
        UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *Path);
        if (!Blueprint)
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Warning, TEXT("Failed to load Blueprint: %s"), *Path);
            continue;
        }

        const int32 OverridesBefore = Overrides.Num();
        CollectComponentOverrides(Blueprint, System, Variable, Overrides);

        const bool bRewritesCalls = UAssetWorkbench::RewriteParameterCalls(Blueprint, Matcher, bApply, Sites) > 0;
        if (bRewritesCalls || Overrides.Num() > OverridesBefore)
        {
            TouchedBlueprints.Add(Blueprint);
        }
    }

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("  %d Blueprint call(s), %d component override(s) in %d of %d Blueprint(s) in scope"), Sites.Num(), Overrides.Num(), TouchedBlueprints.Num(), BlueprintPaths.Num());
    UAssetWorkbench::LogParameterCallSites(Sites);
    for (const FComponentOverride& Override : Overrides)
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("  %s component %s: override %s"), *Override.Blueprint->GetPathName(), *Override.Component.ToString(), *FullFrom.ToString());
    }

    if (!bApply)
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("Done. Dry run, nothing written. Pass -apply to rename."));
        return ToExitCode(EUAssetWorkbenchExitType::Success);
    }

    // Same order as the editor's own rename: script variable and store first, then every reference.
    System->Modify();
    if (UNiagaraSystemEditorData* EditorData = Cast<UNiagaraSystemEditorData>(System->GetEditorData()))
    {
        EditorData->Modify();
        EditorData->RenameUserScriptVariable(Variable, FullTo);
    }
    System->GetExposedParameters().RenameParameter(Variable, FullTo);

    for (UNiagaraGraph* Graph : ReferencingGraphs)
    {
        Graph->RenameParameter(Variable, FullTo);
    }

    for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
    {
        FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
        if (!Data)
        {
            continue;
        }

        for (UNiagaraRendererProperties* Renderer : Data->GetRenderers())
        {
            if (Renderer)
            {
                Renderer->Modify();
                Renderer->RenameVariable(Variable, NewVariable, Handle.GetInstance().ToBase());
            }
        }
    }

    RenameBindingsInPackage(System, FullFrom, FullTo, true);

    if (!CompileSystem(System))
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("%s failed to compile after the rename, nothing saved"), *System->GetPathName());
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    bool bSaveFailed = !UAssetWorkbench::CompileAndSavePackage(System, false);

    for (const FComponentOverride& Override : Overrides)
    {
        Override.Template->Modify();
        Override.Template->RemoveParameterOverride(Variable);
        Override.Template->SetParameterOverride(NewVariable, Override.Value);
    }

    for (UBlueprint* Blueprint : TouchedBlueprints)
    {
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        bSaveFailed |= !UAssetWorkbench::CompileAndSavePackage(Blueprint);
    }

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("Done. Renamed %s -> %s: system, %d graph(s), %d Blueprint(s) %s"), *FullFrom.ToString(), *FullTo.ToString(), ReferencingGraphs.Num(), TouchedBlueprints.Num(), bSaveFailed ? TEXT("(SAVE FAILED)") : TEXT("(saved)"));
    UAssetWorkbench::WarnIfWrittenOutsideEditor();
    return ToExitCode(bSaveFailed ? EUAssetWorkbenchExitType::Failed : EUAssetWorkbenchExitType::Success);
}
