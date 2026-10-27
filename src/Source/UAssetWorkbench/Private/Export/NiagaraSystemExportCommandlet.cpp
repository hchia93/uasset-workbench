#include "Export/NiagaraSystemExportCommandlet.h"
#include "Export/NiagaraStackJsonSerializer.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/StaticMesh.h"
#include "Misc/Paths.h"

#include "NiagaraDataInterface.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraParameterStore.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraSystem.h"

namespace
{
    // Stack content and bookkeeping that the Stages / Renderers / UserParameters sections already cover,
    // or that only versioning and the editor UI read.
    const TSet<FName>& SystemSkippedProperties()
    {
        static const TSet<FName> Names = { TEXT("EmitterHandles"), TEXT("ExposedParameters"), TEXT("SystemSpawnScript"), TEXT("SystemUpdateScript"), TEXT("EditorData"), TEXT("EditorParameters"), TEXT("ScratchPadScripts"), TEXT("ThumbnailImage"), TEXT("UpdateScriptExecution"), TEXT("PythonUpdateScript"), TEXT("ScriptAsset"), TEXT("SystemCompiledData"), TEXT("EmitterCompiledData"), TEXT("ParameterDefinitionsSubscriptions"), TEXT("BakerSettings"), TEXT("BakerGeneratedSettings") };
        return Names;
    }

    const TSet<FName>& EmitterSkippedProperties()
    {
        static const TSet<FName> Names = { TEXT("Version"), TEXT("VersionChangeDescription"), TEXT("UpdateScriptExecution"), TEXT("PythonUpdateScript"), TEXT("ScriptAsset"), TEXT("EventHandlerScriptProps"), TEXT("EmitterDependencies"), TEXT("UpdateScriptProps"), TEXT("SpawnScriptProps"), TEXT("RendererBindings"), TEXT("RendererBindingsExternalObjects"), TEXT("ResolvedDIBindings"), TEXT("EmitterSpawnScriptProps"), TEXT("EmitterUpdateScriptProps"), TEXT("GraphSource"), TEXT("ScratchPads"), TEXT("ParentScratchPads"), TEXT("VersionedParentAtLastMerge"), TEXT("RendererProperties"), TEXT("SimulationStages"), TEXT("SimStageExecutionLoops"), TEXT("SimStageExecutionLoopEditorData"), TEXT("GPUComputeScript"), TEXT("SharedEventGeneratorIds"), TEXT("CurrentScalabilitySettings"), TEXT("EditorData"), TEXT("EditorParameters") };
        return Names;
    }

    TSharedRef<FJsonObject> MakeStageAddress(const FString& EmitterName, const FString& StageName, const FString& ModuleName)
    {
        TSharedRef<FJsonObject> Op = MakeShared<FJsonObject>();
        if (!EmitterName.IsEmpty())
        {
            Op->SetStringField(TEXT("Emitter"), EmitterName);
        }
        Op->SetStringField(TEXT("Stage"), StageName);
        Op->SetStringField(TEXT("Module"), ModuleName);
        return Op;
    }

    // Dynamic inputs are not re-set: that would rebuild the node under a new name and orphan the nested ops.
    // Their own inputs are addressed by the existing function name instead. Default and Expression carry nothing to write.
    void AppendInputOps(const FString& EmitterName, const FString& StageName, const TSharedPtr<FJsonObject>& FunctionCall, TArray<TSharedPtr<FJsonValue>>& OutOps)
    {
        const FString ModuleName = FunctionCall->GetStringField(TEXT("Name"));
        const TSharedPtr<FJsonObject>* Inputs = nullptr;
        if (!FunctionCall->TryGetObjectField(TEXT("Inputs"), Inputs))
        {
            return;
        }

        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Inputs)->Values)
        {
            const TSharedPtr<FJsonObject> Input = Pair.Value->AsObject();
            const FString Mode = Input->GetStringField(TEXT("Mode"));

            const TSharedPtr<FJsonObject>* Nested = nullptr;
            if (Mode == TEXT("DynamicInput") && Input->TryGetObjectField(TEXT("DynamicInput"), Nested))
            {
                AppendInputOps(EmitterName, StageName, *Nested, OutOps);
                continue;
            }

            TSharedRef<FJsonObject> Op = MakeStageAddress(EmitterName, StageName, ModuleName);
            Op->SetStringField(TEXT("Input"), Pair.Key);

            FString Text;
            const TSharedPtr<FJsonObject>* DataInterface = nullptr;
            const TSharedPtr<FJsonObject>* Properties = nullptr;
            if (Mode == TEXT("Local") && Input->TryGetStringField(TEXT("Value"), Text))
            {
                Op->SetStringField(TEXT("Value"), Text);
            }
            else if (Mode == TEXT("Linked") && Input->TryGetStringField(TEXT("Link"), Text))
            {
                Op->SetStringField(TEXT("Link"), Text);
            }
            else if (Mode == TEXT("ObjectAsset") && Input->TryGetStringField(TEXT("Object"), Text))
            {
                Op->SetStringField(TEXT("Object"), Text);
            }
            else if (Mode == TEXT("DataInterface") && Input->TryGetObjectField(TEXT("DataInterface"), DataInterface) && (*DataInterface)->TryGetObjectField(TEXT("Properties"), Properties))
            {
                TSharedRef<FJsonObject> Spec = MakeShared<FJsonObject>();
                Spec->SetObjectField(TEXT("Properties"), *Properties);
                Op->SetObjectField(TEXT("DataInterface"), Spec);
            }
            else
            {
                continue;
            }
            OutOps.Add(MakeShared<FJsonValueObject>(Op));
        }
    }

    void AppendStageOps(const FString& EmitterName, const TArray<TSharedPtr<FJsonValue>>& Stages, TArray<TSharedPtr<FJsonValue>>& OutModules, TArray<TSharedPtr<FJsonValue>>& OutSwitches, TArray<TSharedPtr<FJsonValue>>& OutInputs)
    {
        for (const TSharedPtr<FJsonValue>& StageValue : Stages)
        {
            const TSharedPtr<FJsonObject> Stage = StageValue->AsObject();
            const FString StageName = Stage->GetStringField(TEXT("Name"));

            for (const TSharedPtr<FJsonValue>& ModuleValue : Stage->GetArrayField(TEXT("Modules")))
            {
                const TSharedPtr<FJsonObject> Module = ModuleValue->AsObject();
                const FString ModuleName = Module->GetStringField(TEXT("Name"));

                TSharedRef<FJsonObject> EnableOp = MakeStageAddress(EmitterName, StageName, ModuleName);
                EnableOp->SetStringField(TEXT("Op"), TEXT("Enable"));
                EnableOp->SetBoolField(TEXT("Enabled"), Module->GetBoolField(TEXT("Enabled")));
                OutModules.Add(MakeShared<FJsonValueObject>(EnableOp));

                const TSharedPtr<FJsonObject>* Switches = nullptr;
                if (Module->TryGetObjectField(TEXT("StaticSwitches"), Switches))
                {
                    TSharedRef<FJsonObject> SwitchOp = MakeStageAddress(EmitterName, StageName, ModuleName);
                    SwitchOp->SetObjectField(TEXT("Switches"), *Switches);
                    OutSwitches.Add(MakeShared<FJsonValueObject>(SwitchOp));
                }

                AppendInputOps(EmitterName, StageName, Module, OutInputs);
            }
        }
    }
}

UNiagaraSystemExportCommandlet::UNiagaraSystemExportCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UNiagaraSystemExportCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("UAssetWorkbench v%s - NiagaraSystemExport"), UASSET_WORKBENCH_VERSION_STRING);

    TArray<FString> AssetPaths = UAssetWorkbench::ParseAssetPaths(Params);

    if (AssetPaths.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchExporter, Error, TEXT("No assets specified. Usage: -assets=\"/Game/Path/NS_A,/Game/Path/NS_B\""));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    int32 ExportedCount = 0;

    for (const FString& AssetPath : AssetPaths)
    {
        UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *AssetPath);
        if (!System)
        {
            UE_LOG(LogUAssetWorkbenchExporter, Warning, TEXT("Failed to load NiagaraSystem: %s"), *AssetPath);
            continue;
        }

        TSharedPtr<FJsonObject> JsonObject = ExportNiagaraSystem(System);
        if (!JsonObject.IsValid())
        {
            UE_LOG(LogUAssetWorkbenchExporter, Warning, TEXT("Failed to export NiagaraSystem: %s"), *AssetPath);
            continue;
        }

        UAssetWorkbench::FExportTarget ExportTarget(AssetPath);
        if (ExportTarget.Save(JsonObject.ToSharedRef()))
        {
            UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Exported: %s -> %s"), *AssetPath, *ExportTarget.GetPath());
            ExportedCount++;

            // Saved after the stamp so it shares the export's revision and time.
            const FString SpecPath = FPaths::ChangeExtension(ExportTarget.GetPath(), TEXT("spec.json"));
            if (UAssetWorkbench::SaveJsonToFile(BuildEditSpec(JsonObject.ToSharedRef()), SpecPath))
            {
                UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Edit spec: %s"), *SpecPath);
            }
        }
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Export complete. %d/%d Niagara systems exported."), ExportedCount, AssetPaths.Num());
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}

TSharedPtr<FJsonObject> UNiagaraSystemExportCommandlet::ExportNiagaraSystem(UNiagaraSystem* System) const
{
    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

    Root->SetStringField(TEXT("ExporterVersion"), UASSET_WORKBENCH_VERSION_STRING);
    Root->SetStringField(TEXT("ExportType"), TEXT("NiagaraSystem"));
    Root->SetStringField(TEXT("SystemName"), System->GetName());
    Root->SetStringField(TEXT("AssetPath"), System->GetPathName());
    Root->SetStringField(TEXT("ExportTimestamp"), FDateTime::Now().ToString());

    TSharedPtr<FJsonObject> Properties = NiagaraStackJson::ExportPropertyDelta(System, UObject::StaticClass(), SystemSkippedProperties());
    if (Properties->Values.Num() > 0)
    {
        Root->SetObjectField(TEXT("Properties"), Properties);
    }

    Root->SetObjectField(TEXT("UserParameters"), ExportUserParameters(System->GetExposedParameters()));
    Root->SetArrayField(TEXT("Stages"), ExportStages(System, nullptr));

    TArray<TSharedPtr<FJsonValue>> EmittersArray;
    for (const FNiagaraEmitterHandle& EmitterHandle : System->GetEmitterHandles())
    {
        EmittersArray.Add(MakeShared<FJsonValueObject>(ExportEmitter(System, EmitterHandle)));
    }
    Root->SetArrayField(TEXT("Emitters"), EmittersArray);

    return Root;
}

TSharedRef<FJsonObject> UNiagaraSystemExportCommandlet::BuildEditSpec(const TSharedRef<FJsonObject>& Export) const
{
    TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
    Target->SetStringField(TEXT("AssetPath"), Export->GetStringField(TEXT("AssetPath")));

    TSharedRef<FJsonObject> SystemSection = MakeShared<FJsonObject>();
    const TSharedPtr<FJsonObject>* SystemProperties = nullptr;
    if (Export->TryGetObjectField(TEXT("Properties"), SystemProperties))
    {
        SystemSection->SetObjectField(TEXT("Properties"), *SystemProperties);
    }

    // Data interface user parameters are not writable through UserParameters, they stay out.
    TSharedRef<FJsonObject> UserParameters = MakeShared<FJsonObject>();
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Export->GetObjectField(TEXT("UserParameters"))->Values)
    {
        FString Text;
        const TSharedPtr<FJsonObject> Parameter = Pair.Value->AsObject();
        if (Parameter->TryGetStringField(TEXT("Value"), Text) || Parameter->TryGetStringField(TEXT("Object"), Text))
        {
            UserParameters->SetStringField(Pair.Key, Text);
        }
    }
    if (UserParameters->Values.Num() > 0)
    {
        SystemSection->SetObjectField(TEXT("UserParameters"), UserParameters);
    }
    if (SystemSection->Values.Num() > 0)
    {
        Target->SetObjectField(TEXT("System"), SystemSection);
    }

    TArray<TSharedPtr<FJsonValue>> EmitterOps;
    TArray<TSharedPtr<FJsonValue>> RendererOps;
    TArray<TSharedPtr<FJsonValue>> ModuleOps;
    TArray<TSharedPtr<FJsonValue>> SwitchOps;
    TArray<TSharedPtr<FJsonValue>> InputOps;

    AppendStageOps(FString(), Export->GetArrayField(TEXT("Stages")), ModuleOps, SwitchOps, InputOps);

    for (const TSharedPtr<FJsonValue>& EmitterValue : Export->GetArrayField(TEXT("Emitters")))
    {
        const TSharedPtr<FJsonObject> Emitter = EmitterValue->AsObject();
        const FString EmitterName = Emitter->GetStringField(TEXT("Name"));

        TSharedRef<FJsonObject> EmitterOp = MakeShared<FJsonObject>();
        EmitterOp->SetStringField(TEXT("Emitter"), EmitterName);
        EmitterOp->SetBoolField(TEXT("Enabled"), Emitter->GetBoolField(TEXT("Enabled")));
        const TSharedPtr<FJsonObject>* EmitterProperties = nullptr;
        if (Emitter->TryGetObjectField(TEXT("Properties"), EmitterProperties))
        {
            EmitterOp->SetObjectField(TEXT("Properties"), *EmitterProperties);
        }
        EmitterOps.Add(MakeShared<FJsonValueObject>(EmitterOp));

        const TArray<TSharedPtr<FJsonValue>>* Renderers = nullptr;
        if (Emitter->TryGetArrayField(TEXT("Renderers"), Renderers))
        {
            for (const TSharedPtr<FJsonValue>& RendererValue : *Renderers)
            {
                const TSharedPtr<FJsonObject> Renderer = RendererValue->AsObject();
                const TSharedPtr<FJsonObject>* RendererProperties = nullptr;
                if (!Renderer->TryGetObjectField(TEXT("Properties"), RendererProperties))
                {
                    continue;
                }
                TSharedRef<FJsonObject> RendererOp = MakeShared<FJsonObject>();
                RendererOp->SetStringField(TEXT("Emitter"), EmitterName);
                RendererOp->SetNumberField(TEXT("Index"), Renderer->GetNumberField(TEXT("Index")));
                RendererOp->SetObjectField(TEXT("Properties"), *RendererProperties);
                RendererOps.Add(MakeShared<FJsonValueObject>(RendererOp));
            }
        }

        const TArray<TSharedPtr<FJsonValue>>* Stages = nullptr;
        if (Emitter->TryGetArrayField(TEXT("Stages"), Stages))
        {
            AppendStageOps(EmitterName, *Stages, ModuleOps, SwitchOps, InputOps);
        }
    }

    auto SetIfAny = [&Target](const TCHAR* Key, const TArray<TSharedPtr<FJsonValue>>& Ops)
    {
        if (Ops.Num() > 0)
        {
            Target->SetArrayField(Key, Ops);
        }
    };
    SetIfAny(TEXT("Emitters"), EmitterOps);
    SetIfAny(TEXT("Renderers"), RendererOps);
    SetIfAny(TEXT("Modules"), ModuleOps);
    SetIfAny(TEXT("StaticSwitches"), SwitchOps);
    SetIfAny(TEXT("Inputs"), InputOps);

    TSharedRef<FJsonObject> Spec = MakeShared<FJsonObject>();
    Spec->SetArrayField(TEXT("Targets"), { MakeShared<FJsonValueObject>(Target) });
    return Spec;
}

TSharedPtr<FJsonObject> UNiagaraSystemExportCommandlet::ExportEmitter(UNiagaraSystem* System, const FNiagaraEmitterHandle& EmitterHandle) const
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

    Obj->SetStringField(TEXT("Name"), EmitterHandle.GetName().ToString());
    Obj->SetBoolField(TEXT("Enabled"), EmitterHandle.GetIsEnabled());
    Obj->SetStringField(TEXT("UniqueInstanceName"), EmitterHandle.GetUniqueInstanceName());

    FVersionedNiagaraEmitterData* EmitterData = EmitterHandle.GetEmitterData();
    if (!EmitterData)
    {
        return Obj;
    }

    TSharedPtr<FJsonObject> Properties = NiagaraStackJson::ExportStructDelta(FVersionedNiagaraEmitterData::StaticStruct(), EmitterData, EmitterHandle.GetInstance().Emitter, EmitterSkippedProperties());
    if (Properties->Values.Num() > 0)
    {
        Obj->SetObjectField(TEXT("Properties"), Properties);
    }

    Obj->SetArrayField(TEXT("Stages"), ExportStages(System, &EmitterHandle));

    TArray<TSharedPtr<FJsonValue>> RenderersArray;
    const TArray<UNiagaraRendererProperties*>& Renderers = EmitterData->GetRenderers();
    for (int32 Index = 0; Index < Renderers.Num(); ++Index)
    {
        if (Renderers[Index])
        {
            RenderersArray.Add(MakeShared<FJsonValueObject>(ExportRendererProperties(Renderers[Index], Index)));
        }
    }
    Obj->SetArrayField(TEXT("Renderers"), RenderersArray);

    return Obj;
}

TArray<TSharedPtr<FJsonValue>> UNiagaraSystemExportCommandlet::ExportStages(UNiagaraSystem* System, const FNiagaraEmitterHandle* EmitterHandle) const
{
    TArray<FNiagaraStackStage> Stages;
    NiagaraStackJson::CollectStages(System, EmitterHandle, Stages);

    TArray<TSharedPtr<FJsonValue>> StagesArray;
    for (const FNiagaraStackStage& Stage : Stages)
    {
        StagesArray.Add(MakeShared<FJsonValueObject>(NiagaraStackJson::SerializeStage(Stage)));
    }
    return StagesArray;
}

TSharedPtr<FJsonObject> UNiagaraSystemExportCommandlet::ExportUserParameters(const FNiagaraParameterStore& Store) const
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

    for (const FNiagaraVariableWithOffset& Var : Store.ReadParameterVariables())
    {
        const FNiagaraTypeDefinition& Type = Var.GetType();

        TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
        ParamObj->SetStringField(TEXT("Type"), Type.GetName());

        if (Type.IsDataInterface())
        {
            if (UNiagaraDataInterface* DataInterface = Store.GetDataInterface(Var))
            {
                ParamObj->SetObjectField(TEXT("DataInterface"), NiagaraStackJson::SerializeDataInterface(DataInterface));
            }
        }
        else if (Type.IsUObject())
        {
            UObject* Object = Store.GetUObject(Var);
            ParamObj->SetStringField(TEXT("Object"), Object ? Object->GetPathName() : FString());
        }
        else if (const uint8* Data = Store.GetParameterData(Var))
        {
            FNiagaraVariable Value(Var);
            Value.SetData(Data);
            ParamObj->SetStringField(TEXT("Value"), NiagaraStackJson::ValueToString(Value));
        }

        Obj->SetObjectField(Var.GetName().ToString(), ParamObj);
    }

    return Obj;
}

TSharedPtr<FJsonObject> UNiagaraSystemExportCommandlet::ExportRendererProperties(UNiagaraRendererProperties* Renderer, int32 Index) const
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

    Obj->SetNumberField(TEXT("Index"), Index);
    Obj->SetStringField(TEXT("RendererClass"), Renderer->GetClass()->GetName());

    // Up to UObject so the base class fields (enabled, sort order, platforms) are part of the delta.
    static const TSet<FName> Bookkeeping = { TEXT("MergeId"), TEXT("OuterEmitterVersion") };
    TSharedPtr<FJsonObject> Props = NiagaraStackJson::ExportPropertyDelta(Renderer, UObject::StaticClass(), Bookkeeping);
    if (Props->Values.Num() > 0)
    {
        Obj->SetObjectField(TEXT("Properties"), Props);
    }

    // Resolve mesh references to native size so "mesh too big" is measurable
    if (UNiagaraMeshRendererProperties* MeshRenderer = Cast<UNiagaraMeshRendererProperties>(Renderer))
    {
        TArray<TSharedPtr<FJsonValue>> MeshArray;
        for (const FNiagaraMeshRendererMeshProperties& MeshProps : MeshRenderer->Meshes)
        {
            TSharedPtr<FJsonObject> MeshObj = MakeShared<FJsonObject>();
            if (MeshProps.Mesh)
            {
                const FBoxSphereBounds Bounds = MeshProps.Mesh->GetBounds();
                MeshObj->SetStringField(TEXT("Mesh"), MeshProps.Mesh->GetPathName());
                MeshObj->SetStringField(TEXT("NativeBoundsExtent"), Bounds.BoxExtent.ToString());
                MeshObj->SetNumberField(TEXT("NativeBoundsRadius"), Bounds.SphereRadius);
            }
            MeshObj->SetStringField(TEXT("Scale"), MeshProps.Scale.ToString());
            MeshObj->SetStringField(TEXT("PivotOffset"), MeshProps.PivotOffset.ToString());
            MeshArray.Add(MakeShared<FJsonValueObject>(MeshObj));
        }
        if (MeshArray.Num() > 0)
        {
            Obj->SetArrayField(TEXT("MeshDetails"), MeshArray);
        }
    }

    return Obj;
}
