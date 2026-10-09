#include "Edit/EditMaterialAssetCommandlet.h"

// Editor-only by design: writes editor-only material settings. Trap any Runtime-type drift early.
static_assert(WITH_EDITOR, "UAssetWorkbench commandlets are editor-only, keep the uplugin Module Type=Editor.");

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Texture.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceBasePropertyOverrides.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameters.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "StaticParameterSet.h"

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

    const TCHAR* kMaterialProperties[] =
    {
        TEXT("BlendMode"),
        TEXT("MaterialDomain"),
        TEXT("TwoSided"),
        TEXT("ShadingModel"),
        TEXT("bAutomaticallySetUsageInEditor"),
        TEXT("OpacityMaskClipValue")
    };

    const TCHAR* kInstanceSections[] =
    {
        TEXT("Parent"),
        TEXT("ScalarParameters"),
        TEXT("VectorParameters"),
        TEXT("TextureParameters"),
        TEXT("StaticSwitchParameters"),
        TEXT("BasePropertyOverrides")
    };

    const TCHAR* kOverrideKeys[] =
    {
        TEXT("BlendMode"),
        TEXT("TwoSided"),
        TEXT("ShadingModel"),
        TEXT("OpacityMaskClipValue")
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

    FString MaterialPropertyList()
    {
        return JoinNames(kMaterialProperties, UE_ARRAY_COUNT(kMaterialProperties)) + TEXT(" ") + JoinNames(kUsagePropertyNames, UE_ARRAY_COUNT(kUsagePropertyNames));
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

    bool IsMaterialProperty(const FString& Name)
    {
        for (const TCHAR* Known : kMaterialProperties)
        {
            if (Name == Known)
            {
                return true;
            }
        }

        return false;
    }

    bool IsInstanceSection(const FString& Name)
    {
        for (const TCHAR* Known : kInstanceSections)
        {
            if (Name == Known)
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

    const TSharedPtr<FJsonObject>* PropertiesField = nullptr;
    if (!Entry->TryGetObjectField(TEXT("Properties"), PropertiesField) || (*PropertiesField)->Values.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s writes nothing. Expected Properties"), *AssetPath);
        return false;
    }

    UMaterialInterface* Interface = LoadObject<UMaterialInterface>(nullptr, *AssetPath);
    if (!Interface)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to load Material or MaterialInstance: %s"), *AssetPath);
        return false;
    }

    if (UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Interface))
    {
        if (!ApplyMaterialInstance(Instance, *PropertiesField, OutOps))
        {
            return false;
        }

        OutTouched.Add(Instance);
        return true;
    }

    UMaterial* Material = Cast<UMaterial>(Interface);
    if (!Material)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s is a %s, only Material and MaterialInstanceConstant are editable"), *AssetPath, *Interface->GetClass()->GetName());
        return false;
    }

    if (!ApplyMaterial(Material, *PropertiesField, UpdateContext, OutOps))
    {
        return false;
    }

    OutTouched.Add(Material);
    return true;
}

bool UEditMaterialAssetCommandlet::ApplyMaterial(UMaterial* Material, const TSharedPtr<FJsonObject>& Properties, FMaterialUpdateContext& UpdateContext, int32& OutOps) const
{
    const FString AssetPath = Material->GetPathName();

    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
    {
        if (!IsMaterialProperty(Pair.Key) && FindUsageFlag(Pair.Key) == INDEX_NONE)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s is not an editable material property. Accepted: %s"), *Pair.Key, *MaterialPropertyList());
            return false;
        }
    }

    Material->PreEditChange(nullptr);

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

        ++OutOps;
    }

    // UMaterialEditingLibrary::RecompileMaterialInternal is private in 5.7, this is its body. One context
    // for the whole run means one renderer update no matter how many materials the spec names.
    UpdateContext.AddMaterial(Material);
    Material->PostEditChange();
    Material->MarkPackageDirty();
    return true;
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
