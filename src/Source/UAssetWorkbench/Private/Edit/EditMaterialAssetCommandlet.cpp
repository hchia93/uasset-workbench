#include "Edit/EditMaterialAssetCommandlet.h"

// Editor-only by design: writes editor-only material settings. Trap any Runtime-type drift early.
static_assert(WITH_EDITOR, "UAssetWorkbench commandlets are editor-only, keep the uplugin Module Type=Editor.");

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Edit/MaterialGraphWriter.h"
#include "Editor.h"
#include "Engine/Texture.h"
#include "MaterialEditingLibrary.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionStaticBoolParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialFunctionInstance.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceBasePropertyOverrides.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameters.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "RHIShaderPlatform.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "StaticParameterSet.h"
#include "Subsystems/AssetEditorSubsystem.h"

#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

namespace
{
    // Same order as EMaterialUsage, so a flag name maps back to the enum value by index.
    const TCHAR* kUsagePropertyNames[] =
    {
        TEXT("bUsedWithSkeletalMesh"),
        TEXT("bUsedWithParticleSprites"),
        TEXT("bUsedWithBeamTrails"),
        TEXT("bUsedWithMeshParticles"),
        TEXT("bUsedWithStaticLighting"),
        TEXT("bUsedWithMorphTargets"),
        TEXT("bUsedWithSplineMeshes"),
        TEXT("bUsedWithInstancedStaticMeshes"),
        TEXT("bUsedWithGeometryCollections"),
        TEXT("bUsedWithClothing"),
        TEXT("bUsedWithNiagaraSprites"),
        TEXT("bUsedWithNiagaraRibbons"),
        TEXT("bUsedWithNiagaraMeshParticles"),
        TEXT("bUsedWithGeometryCache"),
        TEXT("bUsedWithWater"),
        TEXT("bUsedWithHairStrands"),
        TEXT("bUsedWithLidarPointCloud"),
        TEXT("bUsedWithVirtualHeightfieldMesh"),
        TEXT("bUsedWithNanite"),
        TEXT("bUsedWithVoxels"),
        TEXT("bUsedWithVolumetricCloud"),
        TEXT("bUsedWithHeterogeneousVolumes"),
        TEXT("bUsedWithStaticMesh")
    };
    static_assert(UE_ARRAY_COUNT(kUsagePropertyNames) == MATUSAGE_MAX, "EMaterialUsage changed, update kUsagePropertyNames.");

    const TCHAR* kInstanceSections[] =
    {
        TEXT("Parent"),
        TEXT("ScalarParameters"),
        TEXT("VectorParameters"),
        TEXT("TextureParameters"),
        TEXT("StaticSwitchParameters"),
        TEXT("BasePropertyOverrides")
    };

    const TCHAR* kFunctionInstanceSections[] =
    {
        TEXT("Parent"),
        TEXT("ScalarParameters"),
        TEXT("VectorParameters"),
        TEXT("TextureParameters"),
        TEXT("StaticSwitchParameters")
    };

    const TCHAR* kOverrideKeys[] =
    {
        TEXT("BlendMode"),
        TEXT("TwoSided"),
        TEXT("ShadingModel"),
        TEXT("OpacityMaskClipValue")
    };

    // Graph writers run in this order: Edges and NodeProperties read the Ids Nodes registered, Layout
    // runs on the finished graph.
    const TCHAR* kGraphKeys[] =
    {
        TEXT("Nodes"),
        TEXT("NodeProperties"),
        TEXT("Edges"),
        TEXT("Layout")
    };

    const TCHAR* kTargetKeys[] =
    {
        TEXT("AssetPath"),
        TEXT("Properties"),
        TEXT("Parameters"),
        TEXT("Nodes"),
        TEXT("NodeProperties"),
        TEXT("Edges"),
        TEXT("Layout")
    };

    FString JoinNames(const TCHAR* const* Names, int32 Count)
    {
        TArray<FString> List;
        for (int32 Index = 0; Index < Count; ++Index)
        {
            List.Add(Names[Index]);
        }

        return FString::Join(List, TEXT(" "));
    }

    bool ContainsName(const TCHAR* const* Names, int32 Count, const FString& Name)
    {
        for (int32 Index = 0; Index < Count; ++Index)
        {
            if (Name == Names[Index])
            {
                return true;
            }
        }

        return false;
    }

    int32 FindUsageFlag(const FString& Name)
    {
        for (int32 Index = 0; Index < UE_ARRAY_COUNT(kUsagePropertyNames); ++Index)
        {
            if (Name == kUsagePropertyNames[Index])
            {
                return Index;
            }
        }

        return INDEX_NONE;
    }

    bool IsInstanceSection(const FString& Name)
    {
        return ContainsName(kInstanceSections, UE_ARRAY_COUNT(kInstanceSections), Name);
    }

    bool HasGraphSection(const TSharedPtr<FJsonObject>& Entry)
    {
        for (const TCHAR* Key : kGraphKeys)
        {
            if (Entry->HasField(Key))
            {
                return true;
            }
        }

        return false;
    }

    bool ReadBool(const TSharedPtr<FJsonValue>& Value, bool& OutValue)
    {
        if (Value->TryGetBool(OutValue))
        {
            return true;
        }

        FString Text;
        if (!Value->TryGetString(Text))
        {
            return false;
        }

        Text.TrimStartAndEndInline();
        if (Text.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Text == TEXT("1"))
        {
            OutValue = true;
            return true;
        }
        if (Text.Equals(TEXT("false"), ESearchCase::IgnoreCase) || Text == TEXT("0"))
        {
            OutValue = false;
            return true;
        }

        return false;
    }

    bool ReadFloat(const TSharedPtr<FJsonValue>& Value, float& OutValue)
    {
        double Number = 0.0;
        if (Value->TryGetNumber(Number))
        {
            OutValue = static_cast<float>(Number);
            return true;
        }

        FString Text;
        if (!Value->TryGetString(Text) || !Text.IsNumeric())
        {
            return false;
        }

        OutValue = FCString::Atof(*Text);
        return true;
    }

    bool ReadColor(const TSharedPtr<FJsonValue>& Value, FLinearColor& OutValue)
    {
        FString Text;
        return Value->TryGetString(Text) && OutValue.InitFromString(Text);
    }

    template <typename TEnumType>
    bool ReadEnum(const TSharedPtr<FJsonValue>& Value, const TCHAR* EnumLabel, TEnumType& OutValue)
    {
        FString Text;
        if (!Value->TryGetString(Text))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Expected a %s enum name"), EnumLabel);
            return false;
        }

        Text.TrimStartAndEndInline();
        const UEnum* Enum = StaticEnum<TEnumType>();
        const int64 Resolved = Enum ? Enum->GetValueByNameString(Text) : INDEX_NONE;
        if (Resolved == INDEX_NONE)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s is not a valid %s value"), *Text, EnumLabel);
            return false;
        }

        OutValue = static_cast<TEnumType>(Resolved);
        return true;
    }

    // Any reflected property by path, logged before and after so the run shows what moved.
    bool ApplyGenericProperties(UObject* Target, const TSharedPtr<FJsonObject>& Properties, int32& OutOps)
    {
        if (Properties->Values.IsEmpty())
        {
            return true;
        }

        TMap<FString, FString> Before;
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
        {
            Before.Add(Pair.Key, UAssetWorkbench::ReadPropertyPathText(Target, Pair.Key));
        }

        int32 Failures = 0;
        const int32 Applied = UAssetWorkbench::ApplyProperties(Target, Properties, Failures);
        if (Failures > 0)
        {
            return false;
        }

        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s %s: %s -> %s"), *Target->GetPathName(), *Pair.Key, *Before[Pair.Key], *UAssetWorkbench::ReadPropertyPathText(Target, Pair.Key));
        }

        OutOps += Applied;
        return true;
    }

    // The material and function editors edit a copy and write it back on Apply, so an editor left open
    // would later overwrite what this run saved.
    void CloseAssetEditors(UObject* Asset)
    {
        if (!GEditor || IsRunningCommandlet())
        {
            return;
        }

        UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
        if (Subsystem && Subsystem->FindEditorForAsset(Asset, false))
        {
            Subsystem->CloseAllEditorsForAsset(Asset);
            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Closed the open editor of %s"), *Asset->GetPathName());
        }
    }

    TArray<TUniquePtr<IMaterialGraphWriter>> MakeGraphWriters()
    {
        TArray<TUniquePtr<IMaterialGraphWriter>> Writers;
        Writers.Add(MakeMaterialGraphNodeWriter());
        Writers.Add(MakeMaterialGraphNodePropertyWriter());
        Writers.Add(MakeMaterialGraphEdgeWriter());
        Writers.Add(MakeMaterialGraphLayoutWriter());
        return Writers;
    }

    bool RunGraphWriters(UMaterial* Material, UMaterialFunction* Function, const FString& AssetPath, const TSharedPtr<FJsonObject>& Entry, int32& OutOps)
    {
        FMaterialGraphEditContext Context;
        Context.Material = Material;
        Context.Function = Function;
        Context.AssetPath = AssetPath;
        MaterialGraphEdit::RegisterExistingNodes(Context);

        for (const TUniquePtr<IMaterialGraphWriter>& Writer : MakeGraphWriters())
        {
            const TSharedPtr<FJsonValue> Section = Entry->TryGetField(Writer->GetSpecKey());
            if (Section.IsValid() && !Writer->Apply(Context, Section))
            {
                return false;
            }
        }

        OutOps += Context.Ops;
        return true;
    }

    // Translation errors land on the resource the PostEditChange recompile created. A commandlet runs
    // with -nullrhi and creates none, so the check only bites in the editor.
    bool ValidateMaterialCompiles(UMaterial* Material)
    {
        FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform);
        if (!Resource)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Warning, TEXT("%s compile check skipped, this process renders nothing. Run with the editor open to check it."), *Material->GetPathName());
            return true;
        }

        Resource->FinishCompilation();
        const TArray<FString>& Errors = Resource->GetCompileErrors();
        for (const FString& Error : Errors)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s compile error: %s"), *Material->GetPathName(), *Error);
        }
        return Errors.IsEmpty();
    }

    // An output with nothing behind it fails in every caller, far from the function that broke.
    bool ValidateFunctionOutputs(UMaterialFunction* Function)
    {
        int32 OutputCount = 0;
        for (const UMaterialExpression* Expression : Function->GetExpressions())
        {
            const UMaterialExpressionFunctionOutput* Output = Cast<UMaterialExpressionFunctionOutput>(Expression);
            if (!Output)
            {
                continue;
            }

            ++OutputCount;
            if (!Output->A.Expression)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s output '%s' has nothing connected"), *Function->GetPathName(), *Output->OutputName.ToString());
                return false;
            }
        }

        if (OutputCount == 0)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Warning, TEXT("%s has no FunctionOutput, a caller gets no pins"), *Function->GetPathName());
        }
        return true;
    }

    // The parent decides which parameters exist, a name it does not carry never reaches the instance.
    bool ResolveParentParameter(UMaterialInstanceConstant* Instance, EMaterialParameterType Type, const FString& Name, const TCHAR* TypeLabel, FMaterialParameterInfo& OutInfo)
    {
        UMaterialInterface* Parent = Instance->Parent;
        if (!Parent)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no parent, parameter %s cannot be resolved"), *Instance->GetPathName(), *Name);
            return false;
        }

        TArray<FMaterialParameterInfo> Infos;
        TArray<FGuid> Ids;
        Parent->GetAllParameterInfoOfType(Type, Infos, Ids);

        TArray<FString> Available;
        for (const FMaterialParameterInfo& Info : Infos)
        {
            if (Info.Name == FName(*Name))
            {
                OutInfo = Info;
                return true;
            }

            Available.Add(Info.Name.ToString());
        }

        Available.Sort();
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no %s parameter %s. Parent %s carries: %s"), *Instance->GetPathName(), TypeLabel, *Name, *Parent->GetPathName(), Available.IsEmpty() ? TEXT("none") : *FString::Join(Available, TEXT(" ")));
        return false;
    }

    // Same rule for a function instance: the base function's parameter nodes decide which names exist.
    UMaterialExpression* ResolveBaseParameter(UMaterialFunctionInstance* Instance, const UClass* ExpressionClass, const FString& Name, const TCHAR* TypeLabel)
    {
        UMaterialFunction* Base = Instance->GetBaseFunction();
        if (!Base)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no base function, parameter %s cannot be resolved"), *Instance->GetPathName(), *Name);
            return nullptr;
        }

        TArray<FString> Available;
        for (UMaterialExpression* Expression : Base->GetExpressions())
        {
            if (!Expression || !Expression->IsA(ExpressionClass) || !Expression->HasAParameterName())
            {
                continue;
            }

            const FString ParameterName = Expression->GetParameterName().ToString();
            if (ParameterName == Name)
            {
                return Expression;
            }
            Available.AddUnique(ParameterName);
        }

        Available.Sort();
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no %s parameter %s. Base %s carries: %s"), *Instance->GetPathName(), TypeLabel, *Name, *Base->GetPathName(), Available.IsEmpty() ? TEXT("none") : *FString::Join(Available, TEXT(" ")));
        return nullptr;
    }

    // Returns the existing override or a new one bound to the base parameter node.
    template <typename TValue>
    TValue& FindOrAddOverride(TArray<TValue>& Values, const FName Name, UMaterialExpression* BaseParameter)
    {
        if (TValue* Existing = Values.FindByPredicate([Name](const TValue& Value) { return Value.ParameterInfo.Name == Name; }))
        {
            return *Existing;
        }

        TValue& Added = Values.AddDefaulted_GetRef();
        Added.ParameterInfo = FMaterialParameterInfo(Name);
        Added.ExpressionGUID = BaseParameter->GetParameterExpressionId();
        return Added;
    }

    FString DescribeCollection(const UMaterialParameterCollection* Collection)
    {
        TArray<FString> Names;
        for (const FCollectionScalarParameter& Parameter : Collection->ScalarParameters)
        {
            Names.Add(FString::Printf(TEXT("%s (Scalar)"), *Parameter.ParameterName.ToString()));
        }
        for (const FCollectionVectorParameter& Parameter : Collection->VectorParameters)
        {
            Names.Add(FString::Printf(TEXT("%s (Vector)"), *Parameter.ParameterName.ToString()));
        }
        return Names.IsEmpty() ? TEXT("none") : FString::Join(Names, TEXT(", "));
    }
}

UEditMaterialAssetCommandlet::UEditMaterialAssetCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UEditMaterialAssetCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("UAssetWorkbench v%s - EditMaterialAsset"), UASSET_WORKBENCH_VERSION_STRING);

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

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("EditMaterialAsset: %d target(s) %s"), Targets->Num(), bApply ? TEXT("[APPLY]") : TEXT("[DRY RUN]"));

    TSet<UObject*> Touched;
    int32 Ops = 0;
    bool bFailed = false;

    // One update context for the whole run, its destructor is what pushes the recompile to the renderer.
    {
        FMaterialUpdateContext UpdateContext;

        for (const TSharedPtr<FJsonValue>& Value : *Targets)
        {
            const TSharedPtr<FJsonObject>& Entry = Value->AsObject();
            UAssetWorkbench::FFailureReasonCapture FailureReason;
            if (!Entry.IsValid() || !ApplyTarget(Entry, UpdateContext, Touched, Ops))
            {
                UAssetWorkbench::ReportTargetFailure(TEXT("EditMaterialAsset"), UAssetWorkbench::DescribeSpecTarget(Entry), FailureReason.GetReason());
                bFailed = true;
                break;
            }
        }
    }

    if (bFailed)
    {
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    if (!bApply)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Done. %d write(s) staged across %d asset(s) (dry run, not saved)."), Ops, Touched.Num());
        return ToExitCode(EUAssetWorkbenchExitType::Success);
    }

    for (UObject* Asset : Touched)
    {
        UAssetWorkbench::FFailureReasonCapture FailureReason;
        if (!UAssetWorkbench::CompileAndSavePackage(Asset, false))
        {
            UAssetWorkbench::ReportTargetFailure(TEXT("EditMaterialAsset"), Asset->GetPathName(), FailureReason.GetReason());
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }
    }

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Done. %d write(s) across %d asset(s) (saved)."), Ops, Touched.Num());
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}

bool UEditMaterialAssetCommandlet::ApplyTarget(const TSharedPtr<FJsonObject>& Entry, FMaterialUpdateContext& UpdateContext, TSet<UObject*>& OutTouched, int32& OutOps) const
{
    FString AssetPath;
    if (!Entry->TryGetStringField(TEXT("AssetPath"), AssetPath))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Target has no AssetPath field"));
        return false;
    }

    // A misspelled section would otherwise be skipped and the run would still report success.
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Entry->Values)
    {
        if (!ContainsName(kTargetKeys, UE_ARRAY_COUNT(kTargetKeys), Pair.Key))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s is not a target key. Accepted: %s"), *Pair.Key, *JoinNames(kTargetKeys, UE_ARRAY_COUNT(kTargetKeys)));
            return false;
        }
    }

    TSharedPtr<FJsonObject> Properties;
    const TSharedPtr<FJsonObject>* PropertiesField = nullptr;
    if (Entry->TryGetObjectField(TEXT("Properties"), PropertiesField) && !(*PropertiesField)->Values.IsEmpty())
    {
        Properties = *PropertiesField;
    }
    const TSharedPtr<FJsonValue> Parameters = Entry->TryGetField(TEXT("Parameters"));
    const bool bGraph = HasGraphSection(Entry);

    if (!Properties.IsValid() && !Parameters.IsValid() && !bGraph)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s writes nothing. Expected Properties, Parameters, %s"), *AssetPath, *JoinNames(kGraphKeys, UE_ARRAY_COUNT(kGraphKeys)));
        return false;
    }

    UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
    if (!Asset)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to load: %s"), *AssetPath);
        return false;
    }

    const bool bGraphOwner = Asset->IsA<UMaterial>() || Asset->IsA<UMaterialFunction>();
    if (bGraph && !bGraphOwner)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s is a %s, only Material and MaterialFunction have a node graph"), *AssetPath, *Asset->GetClass()->GetName());
        return false;
    }

    const bool bCollection = Asset->IsA<UMaterialParameterCollection>();
    if (bCollection && (!Parameters.IsValid() || Properties.IsValid()))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s is a MaterialParameterCollection, it takes Parameters and nothing else"), *AssetPath);
        return false;
    }
    if (!bCollection && Parameters.IsValid())
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s is a %s, Parameters is for MaterialParameterCollection only"), *AssetPath, *Asset->GetClass()->GetName());
        return false;
    }

    if (UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Asset))
    {
        if (!ApplyMaterialInstance(Instance, Properties, OutOps))
        {
            return false;
        }

        OutTouched.Add(Instance);
        return true;
    }

    if (UMaterial* Material = Cast<UMaterial>(Asset))
    {
        CloseAssetEditors(Material);
        Material->PreEditChange(nullptr);

        if (Properties.IsValid() && !ApplyMaterial(Material, Properties, OutOps))
        {
            return false;
        }
        if (bGraph && !RunGraphWriters(Material, nullptr, AssetPath, Entry, OutOps))
        {
            return false;
        }

        // UMaterialEditingLibrary::RecompileMaterialInternal is private in 5.7, this is its body. One context
        // for the whole run means one renderer update no matter how many materials the spec names.
        UpdateContext.AddMaterial(Material);
        Material->PostEditChange();
        Material->MarkPackageDirty();

        if (bGraph && !ValidateMaterialCompiles(Material))
        {
            return false;
        }

        OutTouched.Add(Material);
        return true;
    }

    if (UMaterialFunction* Function = Cast<UMaterialFunction>(Asset))
    {
        CloseAssetEditors(Function);
        Function->PreEditChange(nullptr);

        if (Properties.IsValid() && !ApplyMaterialFunction(Function, Properties, OutOps))
        {
            return false;
        }
        if (bGraph && !RunGraphWriters(nullptr, Function, AssetPath, Entry, OutOps))
        {
            return false;
        }
        if (bGraph && !ValidateFunctionOutputs(Function))
        {
            return false;
        }

        Function->PostEditChange();

        // Recompiles every loaded material that calls the function, the refresh the function editor's Apply runs.
        UMaterialEditingLibrary::UpdateMaterialFunction(Function, nullptr);
        Function->MarkPackageDirty();

        OutTouched.Add(Function);
        return true;
    }

    if (UMaterialFunctionInstance* FunctionInstance = Cast<UMaterialFunctionInstance>(Asset))
    {
        if (!ApplyMaterialFunctionInstance(FunctionInstance, Properties, OutOps))
        {
            return false;
        }

        OutTouched.Add(FunctionInstance);
        return true;
    }

    if (UMaterialParameterCollection* Collection = Cast<UMaterialParameterCollection>(Asset))
    {
        if (!ApplyParameterCollection(Collection, Parameters, OutOps))
        {
            return false;
        }

        OutTouched.Add(Collection);
        return true;
    }

    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s is a %s, editable: Material, MaterialInstanceConstant, MaterialFunction, MaterialFunctionInstance, MaterialParameterCollection"), *AssetPath, *Asset->GetClass()->GetName());
    return false;
}

bool UEditMaterialAssetCommandlet::ApplyMaterial(UMaterial* Material, const TSharedPtr<FJsonObject>& Properties, int32& OutOps) const
{
    const FString AssetPath = Material->GetPathName();

    // bAutomaticallySetUsageInEditor is read by SetMaterialUsage, so a spec that turns it off in the same
    // pass must land before the flags it governs.
    const TSharedPtr<FJsonValue>* AutomaticField = Properties->Values.Find(TEXT("bAutomaticallySetUsageInEditor"));
    if (AutomaticField)
    {
        bool bAutomatic = false;
        if (!ReadBool(*AutomaticField, bAutomatic))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("bAutomaticallySetUsageInEditor expects a bool"));
            return false;
        }

        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s bAutomaticallySetUsageInEditor: %s -> %s"), *AssetPath, Material->bAutomaticallySetUsageInEditor ? TEXT("true") : TEXT("false"), bAutomatic ? TEXT("true") : TEXT("false"));
        Material->bAutomaticallySetUsageInEditor = bAutomatic;
        ++OutOps;
    }

    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
    {
        const int32 Usage = FindUsageFlag(Pair.Key);
        if (Usage == INDEX_NONE)
        {
            continue;
        }

        bool bWanted = false;
        if (!ReadBool(Pair.Value, bWanted))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s expects a bool"), *Pair.Key);
            return false;
        }

        const bool bBefore = Material->GetUsageByFlag(static_cast<EMaterialUsage>(Usage));

        // SetMaterialUsage is a no-op while bAutomaticallySetUsageInEditor is false, the bitfield is the
        // only way in then. The engine takes the same route in FbxSkeletalMeshImport.
        if (Material->bAutomaticallySetUsageInEditor)
        {
            bool bNeedsRecompile = false;
            Material->SetMaterialUsage(bNeedsRecompile, static_cast<EMaterialUsage>(Usage));
        }

        if (Material->GetUsageByFlag(static_cast<EMaterialUsage>(Usage)) != bWanted)
        {
            FBoolProperty* Property = FindFProperty<FBoolProperty>(UMaterial::StaticClass(), *Pair.Key);
            if (!Property)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("UMaterial has no property %s"), *Pair.Key);
                return false;
            }

            Property->SetPropertyValue_InContainer(Material, bWanted);
        }

        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s %s: %s -> %s"), *AssetPath, *Pair.Key, bBefore ? TEXT("true") : TEXT("false"), Material->GetUsageByFlag(static_cast<EMaterialUsage>(Usage)) ? TEXT("true") : TEXT("false"));
        ++OutOps;
    }

    // Anything without a dedicated setter below goes through reflection, the same path Settings exports.
    TSharedPtr<FJsonObject> Generic = MakeShared<FJsonObject>();

    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
    {
        if (Pair.Key == TEXT("bAutomaticallySetUsageInEditor") || FindUsageFlag(Pair.Key) != INDEX_NONE)
        {
            continue;
        }

        if (Pair.Key == TEXT("BlendMode"))
        {
            EBlendMode BlendMode = BLEND_Opaque;
            if (!ReadEnum(Pair.Value, TEXT("EBlendMode"), BlendMode))
            {
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s BlendMode: %s -> %s"), *AssetPath, *StaticEnum<EBlendMode>()->GetNameStringByValue(Material->BlendMode.GetValue()), *StaticEnum<EBlendMode>()->GetNameStringByValue(BlendMode));
            Material->BlendMode = BlendMode;
        }
        else if (Pair.Key == TEXT("MaterialDomain"))
        {
            EMaterialDomain Domain = MD_Surface;
            if (!ReadEnum(Pair.Value, TEXT("EMaterialDomain"), Domain))
            {
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s MaterialDomain: %s -> %s"), *AssetPath, *StaticEnum<EMaterialDomain>()->GetNameStringByValue(Material->MaterialDomain.GetValue()), *StaticEnum<EMaterialDomain>()->GetNameStringByValue(Domain));
            Material->MaterialDomain = Domain;
        }
        else if (Pair.Key == TEXT("ShadingModel"))
        {
            EMaterialShadingModel ShadingModel = MSM_DefaultLit;
            if (!ReadEnum(Pair.Value, TEXT("EMaterialShadingModel"), ShadingModel))
            {
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s ShadingModel: %s -> %s"), *AssetPath, *StaticEnum<EMaterialShadingModel>()->GetNameStringByValue(Material->GetShadingModels().GetFirstShadingModel()), *StaticEnum<EMaterialShadingModel>()->GetNameStringByValue(ShadingModel));
            Material->SetShadingModel(ShadingModel);
        }
        else if (Pair.Key == TEXT("TwoSided"))
        {
            bool bTwoSided = false;
            if (!ReadBool(Pair.Value, bTwoSided))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("TwoSided expects a bool"));
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s TwoSided: %s -> %s"), *AssetPath, Material->TwoSided ? TEXT("true") : TEXT("false"), bTwoSided ? TEXT("true") : TEXT("false"));
            Material->TwoSided = bTwoSided;
        }
        else if (Pair.Key == TEXT("OpacityMaskClipValue"))
        {
            float ClipValue = 0.0f;
            if (!ReadFloat(Pair.Value, ClipValue))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("OpacityMaskClipValue expects a number"));
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s OpacityMaskClipValue: %f -> %f"), *AssetPath, Material->OpacityMaskClipValue, ClipValue);
            Material->OpacityMaskClipValue = ClipValue;
        }
        else
        {
            Generic->SetField(Pair.Key, Pair.Value);
            continue;
        }

        ++OutOps;
    }

    return ApplyGenericProperties(Material, Generic, OutOps);
}

bool UEditMaterialAssetCommandlet::ApplyMaterialInstance(UMaterialInstanceConstant* Instance, const TSharedPtr<FJsonObject>& Properties, int32& OutOps) const
{
    const FString AssetPath = Instance->GetPathName();

    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
    {
        if (!IsInstanceSection(Pair.Key))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s is not an editable material instance section. Accepted: %s"), *Pair.Key, *JoinNames(kInstanceSections, UE_ARRAY_COUNT(kInstanceSections)));
            return false;
        }
    }

    FString ParentPath;
    if (Properties->TryGetStringField(TEXT("Parent"), ParentPath))
    {
        UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, *ParentPath);
        if (!Parent)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to load parent material: %s"), *ParentPath);
            return false;
        }

        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s Parent: %s -> %s"), *AssetPath, Instance->Parent ? *Instance->Parent->GetPathName() : TEXT("None"), *Parent->GetPathName());
        Instance->SetParentEditorOnly(Parent);
        ++OutOps;
    }

    struct FPendingParameter
    {
        FMaterialParameterInfo Info;
        FMaterialParameterMetadata Meta;
        FString Label;
    };

    TArray<FPendingParameter> Pending;
    FMaterialInstanceBasePropertyOverrides Overrides = Instance->BasePropertyOverrides;
    bool bOverridesTouched = false;

    const TSharedPtr<FJsonObject>* Scalars = nullptr;
    if (Properties->TryGetObjectField(TEXT("ScalarParameters"), Scalars))
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Scalars)->Values)
        {
            float Value = 0.0f;
            if (!ReadFloat(Pair.Value, Value))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("ScalarParameters.%s expects a number"), *Pair.Key);
                return false;
            }

            FPendingParameter Parameter;
            if (!ResolveParentParameter(Instance, EMaterialParameterType::Scalar, Pair.Key, TEXT("scalar"), Parameter.Info))
            {
                return false;
            }

            Parameter.Meta = FMaterialParameterMetadata(FMaterialParameterValue(Value));
            Parameter.Label = FString::Printf(TEXT("ScalarParameters.%s = %f"), *Pair.Key, Value);
            Pending.Add(MoveTemp(Parameter));
        }
    }

    const TSharedPtr<FJsonObject>* Vectors = nullptr;
    if (Properties->TryGetObjectField(TEXT("VectorParameters"), Vectors))
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Vectors)->Values)
        {
            FString Text;
            FLinearColor Color;
            if (!Pair.Value->TryGetString(Text) || !Color.InitFromString(Text))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("VectorParameters.%s expects \"(R=,G=,B=,A=)\""), *Pair.Key);
                return false;
            }

            FPendingParameter Parameter;
            if (!ResolveParentParameter(Instance, EMaterialParameterType::Vector, Pair.Key, TEXT("vector"), Parameter.Info))
            {
                return false;
            }

            Parameter.Meta = FMaterialParameterMetadata(FMaterialParameterValue(Color));
            Parameter.Label = FString::Printf(TEXT("VectorParameters.%s = %s"), *Pair.Key, *Color.ToString());
            Pending.Add(MoveTemp(Parameter));
        }
    }

    const TSharedPtr<FJsonObject>* Textures = nullptr;
    if (Properties->TryGetObjectField(TEXT("TextureParameters"), Textures))
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Textures)->Values)
        {
            FString TexturePath;
            if (!Pair.Value->TryGetString(TexturePath))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("TextureParameters.%s expects a texture path"), *Pair.Key);
                return false;
            }

            UTexture* Texture = LoadObject<UTexture>(nullptr, *TexturePath);
            if (!Texture)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to load texture: %s"), *TexturePath);
                return false;
            }

            FPendingParameter Parameter;
            if (!ResolveParentParameter(Instance, EMaterialParameterType::Texture, Pair.Key, TEXT("texture"), Parameter.Info))
            {
                return false;
            }

            Parameter.Meta = FMaterialParameterMetadata(FMaterialParameterValue(Texture));
            Parameter.Label = FString::Printf(TEXT("TextureParameters.%s = %s"), *Pair.Key, *Texture->GetPathName());
            Pending.Add(MoveTemp(Parameter));
        }
    }

    const TSharedPtr<FJsonObject>* Switches = nullptr;
    if (Properties->TryGetObjectField(TEXT("StaticSwitchParameters"), Switches))
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Switches)->Values)
        {
            bool bValue = false;
            if (!ReadBool(Pair.Value, bValue))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("StaticSwitchParameters.%s expects a bool"), *Pair.Key);
                return false;
            }

            FPendingParameter Parameter;
            if (!ResolveParentParameter(Instance, EMaterialParameterType::StaticSwitch, Pair.Key, TEXT("static switch"), Parameter.Info))
            {
                return false;
            }

            Parameter.Meta = FMaterialParameterMetadata(FMaterialParameterValue(bValue));
            Parameter.Label = FString::Printf(TEXT("StaticSwitchParameters.%s = %s"), *Pair.Key, bValue ? TEXT("true") : TEXT("false"));
            Pending.Add(MoveTemp(Parameter));
        }
    }

    const TSharedPtr<FJsonObject>* OverrideField = nullptr;
    if (Properties->TryGetObjectField(TEXT("BasePropertyOverrides"), OverrideField))
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*OverrideField)->Values)
        {
            if (Pair.Key == TEXT("BlendMode"))
            {
                EBlendMode BlendMode = BLEND_Opaque;
                if (!ReadEnum(Pair.Value, TEXT("EBlendMode"), BlendMode))
                {
                    return false;
                }

                Overrides.bOverride_BlendMode = true;
                Overrides.BlendMode = BlendMode;
            }
            else if (Pair.Key == TEXT("ShadingModel"))
            {
                EMaterialShadingModel ShadingModel = MSM_DefaultLit;
                if (!ReadEnum(Pair.Value, TEXT("EMaterialShadingModel"), ShadingModel))
                {
                    return false;
                }

                Overrides.bOverride_ShadingModel = true;
                Overrides.ShadingModel = ShadingModel;
            }
            else if (Pair.Key == TEXT("TwoSided"))
            {
                bool bTwoSided = false;
                if (!ReadBool(Pair.Value, bTwoSided))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("BasePropertyOverrides.TwoSided expects a bool"));
                    return false;
                }

                Overrides.bOverride_TwoSided = true;
                Overrides.TwoSided = bTwoSided;
            }
            else if (Pair.Key == TEXT("OpacityMaskClipValue"))
            {
                float ClipValue = 0.0f;
                if (!ReadFloat(Pair.Value, ClipValue))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("BasePropertyOverrides.OpacityMaskClipValue expects a number"));
                    return false;
                }

                Overrides.bOverride_OpacityMaskClipValue = true;
                Overrides.OpacityMaskClipValue = ClipValue;
            }
            else
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s is not an editable base property override. Accepted: %s"), *Pair.Key, *JoinNames(kOverrideKeys, UE_ARRAY_COUNT(kOverrideKeys)));
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s BasePropertyOverrides.%s written"), *AssetPath, *Pair.Key);
            bOverridesTouched = true;
            ++OutOps;
        }
    }

    // One context per target, its destructor runs a single UpdateStaticPermutation for everything staged.
    {
        FMaterialInstanceParameterUpdateContext UpdateContext(Instance);

        for (const FPendingParameter& Parameter : Pending)
        {
            UpdateContext.SetParameterValueEditorOnly(Parameter.Info, Parameter.Meta);
            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s %s"), *AssetPath, *Parameter.Label);
            ++OutOps;
        }

        if (bOverridesTouched)
        {
            UpdateContext.SetBasePropertyOverrides(Overrides);
        }
    }

    Instance->MarkPackageDirty();
    return true;
}

bool UEditMaterialAssetCommandlet::ApplyMaterialFunction(UMaterialFunction* Function, const TSharedPtr<FJsonObject>& Properties, int32& OutOps) const
{
    // A function has no setter that does more than store, every setting goes through reflection.
    return ApplyGenericProperties(Function, Properties, OutOps);
}

bool UEditMaterialAssetCommandlet::ApplyMaterialFunctionInstance(UMaterialFunctionInstance* Instance, const TSharedPtr<FJsonObject>& Properties, int32& OutOps) const
{
    const FString AssetPath = Instance->GetPathName();

    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
    {
        if (!ContainsName(kFunctionInstanceSections, UE_ARRAY_COUNT(kFunctionInstanceSections), Pair.Key))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s is not an editable material function instance section. Accepted: %s"), *Pair.Key, *JoinNames(kFunctionInstanceSections, UE_ARRAY_COUNT(kFunctionInstanceSections)));
            return false;
        }
    }

    Instance->PreEditChange(nullptr);

    FString ParentPath;
    if (Properties->TryGetStringField(TEXT("Parent"), ParentPath))
    {
        UMaterialFunctionInterface* Parent = LoadObject<UMaterialFunctionInterface>(nullptr, *ParentPath);
        if (!Parent)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to load parent function: %s"), *ParentPath);
            return false;
        }

        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s Parent: %s -> %s"), *AssetPath, Instance->Parent ? *Instance->Parent->GetPathName() : TEXT("None"), *Parent->GetPathName());
        Instance->SetParent(Parent);
        ++OutOps;
    }

    const TSharedPtr<FJsonObject>* Scalars = nullptr;
    if (Properties->TryGetObjectField(TEXT("ScalarParameters"), Scalars))
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Scalars)->Values)
        {
            float Value = 0.0f;
            if (!ReadFloat(Pair.Value, Value))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("ScalarParameters.%s expects a number"), *Pair.Key);
                return false;
            }

            UMaterialExpression* BaseParameter = ResolveBaseParameter(Instance, UMaterialExpressionScalarParameter::StaticClass(), Pair.Key, TEXT("scalar"));
            if (!BaseParameter)
            {
                return false;
            }

            FindOrAddOverride(Instance->ScalarParameterValues, FName(*Pair.Key), BaseParameter).ParameterValue = Value;
            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s ScalarParameters.%s = %f"), *AssetPath, *Pair.Key, Value);
            ++OutOps;
        }
    }

    const TSharedPtr<FJsonObject>* Vectors = nullptr;
    if (Properties->TryGetObjectField(TEXT("VectorParameters"), Vectors))
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Vectors)->Values)
        {
            FLinearColor Color;
            if (!ReadColor(Pair.Value, Color))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("VectorParameters.%s expects \"(R=,G=,B=,A=)\""), *Pair.Key);
                return false;
            }

            UMaterialExpression* BaseParameter = ResolveBaseParameter(Instance, UMaterialExpressionVectorParameter::StaticClass(), Pair.Key, TEXT("vector"));
            if (!BaseParameter)
            {
                return false;
            }

            FindOrAddOverride(Instance->VectorParameterValues, FName(*Pair.Key), BaseParameter).ParameterValue = Color;
            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s VectorParameters.%s = %s"), *AssetPath, *Pair.Key, *Color.ToString());
            ++OutOps;
        }
    }

    const TSharedPtr<FJsonObject>* Textures = nullptr;
    if (Properties->TryGetObjectField(TEXT("TextureParameters"), Textures))
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Textures)->Values)
        {
            FString TexturePath;
            UTexture* Texture = Pair.Value->TryGetString(TexturePath) ? LoadObject<UTexture>(nullptr, *TexturePath) : nullptr;
            if (!Texture)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("TextureParameters.%s expects a loadable texture path"), *Pair.Key);
                return false;
            }

            UMaterialExpression* BaseParameter = ResolveBaseParameter(Instance, UMaterialExpressionTextureSampleParameter::StaticClass(), Pair.Key, TEXT("texture"));
            if (!BaseParameter)
            {
                return false;
            }

            FindOrAddOverride(Instance->TextureParameterValues, FName(*Pair.Key), BaseParameter).ParameterValue = Texture;
            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s TextureParameters.%s = %s"), *AssetPath, *Pair.Key, *Texture->GetPathName());
            ++OutOps;
        }
    }

    const TSharedPtr<FJsonObject>* Switches = nullptr;
    if (Properties->TryGetObjectField(TEXT("StaticSwitchParameters"), Switches))
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Switches)->Values)
        {
            bool bValue = false;
            if (!ReadBool(Pair.Value, bValue))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("StaticSwitchParameters.%s expects a bool"), *Pair.Key);
                return false;
            }

            UMaterialExpression* BaseParameter = ResolveBaseParameter(Instance, UMaterialExpressionStaticBoolParameter::StaticClass(), Pair.Key, TEXT("static switch"));
            if (!BaseParameter)
            {
                return false;
            }

            FStaticSwitchParameter& Override = FindOrAddOverride(Instance->StaticSwitchParameterValues, FName(*Pair.Key), BaseParameter);
            Override.Value = bValue;
            Override.bOverride = true;
            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s StaticSwitchParameters.%s = %s"), *AssetPath, *Pair.Key, bValue ? TEXT("true") : TEXT("false"));
            ++OutOps;
        }
    }

    Instance->UpdateParameterSet();
    Instance->PostEditChange();

    // Callers cache the function's pins and parameters, the same refresh the instance editor runs.
    UMaterialEditingLibrary::UpdateMaterialFunction(Instance, nullptr);
    Instance->MarkPackageDirty();
    return true;
}

bool UEditMaterialAssetCommandlet::ApplyParameterCollection(UMaterialParameterCollection* Collection, const TSharedPtr<FJsonValue>& Parameters, int32& OutOps) const
{
    const FString AssetPath = Collection->GetPathName();

    const TArray<TSharedPtr<FJsonValue>>* Ops = nullptr;
    if (!Parameters->TryGetArray(Ops))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Parameters must be an array of operations"));
        return false;
    }

    auto FindScalar = [Collection](FName Name)
    {
        return Collection->ScalarParameters.IndexOfByPredicate([Name](const FCollectionScalarParameter& Parameter) { return Parameter.ParameterName == Name; });
    };
    auto FindVector = [Collection](FName Name)
    {
        return Collection->VectorParameters.IndexOfByPredicate([Name](const FCollectionVectorParameter& Parameter) { return Parameter.ParameterName == Name; });
    };

    // PreEditChange snapshots the layout, PostEditChange compares against it to decide which materials recompile.
    Collection->PreEditChange(nullptr);

    for (const TSharedPtr<FJsonValue>& Value : *Ops)
    {
        const TSharedPtr<FJsonObject> Desc = Value->AsObject();
        FString Op;
        FString NameText;
        if (!Desc.IsValid() || !Desc->TryGetStringField(TEXT("Op"), Op) || !Desc->TryGetStringField(TEXT("Name"), NameText))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Parameters entry needs Op and Name"));
            return false;
        }

        const FName Name(*NameText);
        const int32 ScalarIndex = FindScalar(Name);
        const int32 VectorIndex = FindVector(Name);
        const bool bExists = ScalarIndex != INDEX_NONE || VectorIndex != INDEX_NONE;
        const TSharedPtr<FJsonValue> ValueField = Desc->TryGetField(TEXT("Value"));

        if (Op == TEXT("Add"))
        {
            if (bExists)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s already has a parameter %s"), *AssetPath, *NameText);
                return false;
            }

            FString Type = TEXT("Scalar");
            Desc->TryGetStringField(TEXT("Type"), Type);
            if (Type == TEXT("Scalar"))
            {
                FCollectionScalarParameter Parameter;
                Parameter.ParameterName = Name;
                if (ValueField.IsValid() && !ReadFloat(ValueField, Parameter.DefaultValue))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Parameters.%s Value expects a number"), *NameText);
                    return false;
                }
                Collection->ScalarParameters.Add(Parameter);
            }
            else if (Type == TEXT("Vector"))
            {
                FCollectionVectorParameter Parameter;
                Parameter.ParameterName = Name;
                if (ValueField.IsValid() && !ReadColor(ValueField, Parameter.DefaultValue))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Parameters.%s Value expects \"(R=,G=,B=,A=)\""), *NameText);
                    return false;
                }
                Collection->VectorParameters.Add(Parameter);
            }
            else
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Parameters.%s Type '%s' is not Scalar or Vector"), *NameText, *Type);
                return false;
            }
            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s added %s parameter %s"), *AssetPath, *Type, *NameText);
        }
        else if (!bExists)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no parameter %s. Parameters: %s"), *AssetPath, *NameText, *DescribeCollection(Collection));
            return false;
        }
        else if (Op == TEXT("Set"))
        {
            const bool bRead = ScalarIndex != INDEX_NONE ? ValueField.IsValid() && ReadFloat(ValueField, Collection->ScalarParameters[ScalarIndex].DefaultValue) : ValueField.IsValid() && ReadColor(ValueField, Collection->VectorParameters[VectorIndex].DefaultValue);
            if (!bRead)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Parameters.%s Set needs a Value of the parameter's type"), *NameText);
                return false;
            }
            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s set %s"), *AssetPath, *NameText);
        }
        else if (Op == TEXT("Remove"))
        {
            if (ScalarIndex != INDEX_NONE)
            {
                Collection->ScalarParameters.RemoveAt(ScalarIndex);
            }
            else
            {
                Collection->VectorParameters.RemoveAt(VectorIndex);
            }
            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s removed %s"), *AssetPath, *NameText);
        }
        else if (Op == TEXT("Rename"))
        {
            FString NewNameText;
            const FName NewName = Desc->TryGetStringField(TEXT("NewName"), NewNameText) ? FName(*NewNameText) : NAME_None;
            if (NewName.IsNone() || FindScalar(NewName) != INDEX_NONE || FindVector(NewName) != INDEX_NONE)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Parameters.%s Rename needs a NewName not already in %s"), *NameText, *AssetPath);
                return false;
            }

            // Materials bind collection parameters by Id, so a rename keeps every reference intact.
            if (ScalarIndex != INDEX_NONE)
            {
                Collection->ScalarParameters[ScalarIndex].ParameterName = NewName;
            }
            else
            {
                Collection->VectorParameters[VectorIndex].ParameterName = NewName;
            }
            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s renamed %s -> %s"), *AssetPath, *NameText, *NewNameText);
        }
        else
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Unknown Parameters op '%s'. Expected Add, Set, Remove, Rename"), *Op);
            return false;
        }

        ++OutOps;
    }

    Collection->PostEditChange();
    Collection->MarkPackageDirty();
    return true;
}
