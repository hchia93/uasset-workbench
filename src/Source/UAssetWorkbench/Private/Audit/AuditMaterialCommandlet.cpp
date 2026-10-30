#include "Audit/AuditMaterialCommandlet.h"

// Editor-only by design: drives the editor Asset Registry. Trap any Runtime-type drift early.
static_assert(WITH_EDITOR, "UAssetWorkbench commandlets are editor-only, keep the uplugin Module Type=Editor.");

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/MeshComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "MaterialCachedData.h"
#include "MaterialDomain.h"
#include "MaterialEditingLibrary.h"
#include "MaterialShared.h"
#include "MaterialStatsCommon.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionNamedReroute.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameters.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraSystem.h"
#include "RHIDefinitions.h"
#include "RenderingThread.h"
#include "Shader.h"
#include "ShaderCompiler.h"
#include "StaticParameterSet.h"
#include "VertexFactory.h"
#include "UObject/UObjectGlobals.h"

#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

namespace
{
    // Material to instance to mesh to blueprint is the longest chain worth classifying.
    constexpr int32 kConsumerWalkDepth = 4;
    constexpr int32 kDependencyWalkDepth = 6;

    // Compiling representative shaders costs minutes per material, so a scan only stats the head of the list.
    constexpr int32 kStatsScanCap = 20;

    // PIE warmup thresholds. Permutation size is usage flags times quality levels times switch combos.
    constexpr int32 kPermutationSuspectSize = 8;
    constexpr int32 kShaderCountSuspect = 200;
    constexpr int32 kInstructionCountSuspect = 400;

    constexpr EShaderPlatform kStatsShaderPlatform = SP_PCD3D_SM6;

    const TCHAR* kAllRules[] =
    {
        TEXT("N1"), TEXT("N2"), TEXT("N3"), TEXT("N4"), TEXT("N5"),
        TEXT("N6"), TEXT("N7"), TEXT("N8"), TEXT("N9"),
        TEXT("U1"), TEXT("U2"), TEXT("U3"), TEXT("U4"),
        TEXT("G1")
    };

    // Project config, so the plugin carries the mechanism and the project names its own function outputs.
    // A file of its own keeps it out of DefaultEditor.ini, which projects often park as local-only.
    const TCHAR* kConfigFileName = TEXT("DefaultUAssetWorkbench.ini");
    const TCHAR* kConfigSection = TEXT("AuditMaterial");
    const TCHAR* kForbiddenMaskOutputsKey = TEXT("OpacityMaskForbiddenOutputs");

    const TCHAR* kSeverityError = TEXT("Error");
    const TCHAR* kSeverityWarning = TEXT("Warning");
    const TCHAR* kSeverityInfo = TEXT("Info");

    // UMaterial::GetUsageName is not exported, so the flag property names are mirrored here. Order and
    // count follow EMaterialUsage, a new engine usage lands as a compile error on the static_assert below.
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

    struct FFinding
    {
        FString Asset;
        FString Rule;
        FString Severity;
        FString Property;
        FString Current;
        FString Expected;
        FString Context;
    };

    struct FMaterialCost
    {
        FString Domain;
        FString BlendMode;
        FString ShadingModel;
        bool bTwoSided = false;
        bool bIsSky = false;
        bool bTessellation = false;
        bool bAutomaticUsage = false;
        float OpacityMaskClipValue = 0.0f;
        int32 NumCustomizedUVs = 0;

        bool bWorldPositionOffsetConnected = false;
        bool bPixelDepthOffsetConnected = false;
        bool bDisplacementConnected = false;

        bool bHasVertexInterpolator = false;
        bool bHasPerInstanceRandom = false;
        bool bHasPerInstanceCustomData = false;
        bool bHasCustomizedUVs = false;
        bool bHasSceneColor = false;
        bool bHasWorldPosition = false;
        bool bHasMaterialLayers = false;
        bool bHasRuntimeVirtualTextureOutput = false;

        int32 ReferencedTextureCount = 0;
        int32 ExpressionCount = 0;
        int32 FunctionCount = 0;

        bool bStatsResolved = false;
        int32 SamplerUsage = 0;
        int32 TextureSamplesVS = 0;
        int32 TextureSamplesPS = 0;
        int32 VirtualTextureLookups = 0;
        int32 VirtualTextureStacks = 0;
        int32 UsedUVScalars = 0;
        int32 UsedCustomInterpolatorScalars = 0;
        int32 LWCFuncUsagesVS = 0;
        int32 LWCFuncUsagesPS = 0;
        int32 LWCFuncUsagesCS = 0;
        int32 ShaderCount = 0;
        int32 MaxInstructionCount = 0;
        TArray<TPair<FString, int32>> Instructions;
    };

    struct FPermutation
    {
        int32 UsageFlagCount = 0;
        int32 QualityLevels = 1;
        int32 StaticSwitchCount = 0;
        int32 StaticSwitchCombosUsed = 1;
        int32 ChildInstanceCount = 0;

        int32 GetSize() const
        {
            return FMath::Max(1, UsageFlagCount) * FMath::Max(1, QualityLevels) * FMath::Max(1, StaticSwitchCombosUsed);
        }
    };

    struct FMaterialRecord
    {
        FString AssetPath;
        FName PackageName;
        UMaterial* Material = nullptr;
        bool bNamedEntry = false;

        TMap<int32, FString> AppliedUsages;
        TArray<FString> NaniteMeshes;
        bool bOnNaniteSkeletalMesh = false;

        FMaterialCost Cost;
        FPermutation Permutation;
        TSharedPtr<FJsonObject> EditorStats;
    };

    struct FAuditOptions
    {
        TArray<FString> EntryAssets;
        FString ScanDir;
        FString ReportPath;
        TSet<FString> Rules;
        bool bStats = false;

        // "<function package>:<output name>" entries that must never reach OpacityMask in a Masked material.
        TSet<FString> ForbiddenMaskOutputs;
    };

    struct FAuditState
    {
        TMap<FString, FMaterialRecord> Records;
        TArray<FString> RecordOrder;
        TArray<FFinding> Findings;
        TSet<FString> SeenFindings;
        TArray<FString> EntryLevels;
        TSet<FName> ScannedMeshes;
        int32 MaterialInterfacesInScope = 0;
    };

    bool IsRuleActive(const FAuditOptions& Options, const TCHAR* Rule)
    {
        return Options.Rules.IsEmpty() || Options.Rules.Contains(Rule);
    }

    const TCHAR* BoolText(bool bValue)
    {
        return bValue ? TEXT("true") : TEXT("false");
    }

    FString EnumName(const UEnum* Enum, int64 Value)
    {
        return Enum ? Enum->GetNameStringByValue(Value) : FString::FromInt(static_cast<int32>(Value));
    }

    FString UsageName(int32 Usage)
    {
        return kUsagePropertyNames[Usage];
    }

    FString ToPackageName(const FString& AssetPath)
    {
        FString PackageName = AssetPath;
        int32 ObjectDelimiter = INDEX_NONE;
        if (PackageName.FindChar(TEXT('.'), ObjectDelimiter))
        {
            PackageName.LeftInline(ObjectDelimiter);
        }

        return PackageName;
    }

    bool IsMaterialAsset(const FAssetData& Asset)
    {
        const FString ClassName = Asset.AssetClassPath.GetAssetName().ToString();
        return ClassName == TEXT("Material") || ClassName.StartsWith(TEXT("MaterialInstance"));
    }

    bool IsMaterialInstanceAsset(const FAssetData& Asset)
    {
        return Asset.AssetClassPath.GetAssetName().ToString().StartsWith(TEXT("MaterialInstance"));
    }

    bool IsLevelAsset(const FAssetData& Asset)
    {
        return Asset.AssetClassPath.GetAssetName() == FName(TEXT("World"));
    }

    bool ReadBoolTag(const FAssetData& Asset, const TCHAR* TagName, bool& OutValue)
    {
        FString Raw;
        if (!Asset.GetTagValue(FName(TagName), Raw))
        {
            return false;
        }

        OutValue = Raw.ToBool();
        return true;
    }

    // Domain and the special-engine escape hatch decide whether a missing flag actually costs anything,
    // this mirrors UMaterial::NeedsSetMaterialUsage_Concurrent without its side effects.
    bool NeedsUsageFlag(const UMaterial* Material, int32 Usage)
    {
        const bool bSurface = Material->MaterialDomain == MD_Surface;
        const bool bDecal = Material->MaterialDomain == MD_DeferredDecal;
        const bool bVolume = Material->MaterialDomain == MD_Volume;
        const bool bDomainRelevant = bSurface || bDecal || bVolume;
        return bDomainRelevant && !Material->GetUsageByFlag(static_cast<EMaterialUsage>(Usage)) && !Material->bUsedAsSpecialEngineMaterial;
    }

    bool IsNaniteMaskingAllowed()
    {
        static IConsoleVariable* AllowMasked = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Nanite.AllowMaskedMaterials"));
        return !AllowMasked || AllowMasked->GetInt() != 0;
    }

    void AddFinding(FAuditState& State, const FString& DedupKey, FFinding&& Finding)
    {
        if (State.SeenFindings.Contains(DedupKey))
        {
            return;
        }

        State.SeenFindings.Add(DedupKey);
        State.Findings.Add(MoveTemp(Finding));
    }

    void AddMaterialFinding(FAuditState& State, const FString& AssetPath, const TCHAR* Rule, const TCHAR* Severity, const FString& Property, const FString& Current, const FString& Expected, const FString& Context)
    {
        FFinding Finding;
        Finding.Asset = AssetPath;
        Finding.Rule = Rule;
        Finding.Severity = Severity;
        Finding.Property = Property;
        Finding.Current = Current;
        Finding.Expected = Expected;
        Finding.Context = Context;

        AddFinding(State, FString::Printf(TEXT("%s|%s|%s"), *AssetPath, Rule, *Property), MoveTemp(Finding));
    }
}

namespace
{
    bool ParseOptions(const FString& Params, FAuditOptions& OutOptions)
    {
        OutOptions.EntryAssets = UAssetWorkbench::ParseAssetPaths(Params);

        // A hand-rolled -assets= with an empty value makes FParse::Value swallow the next flag.
        OutOptions.EntryAssets.RemoveAll([](const FString& Path) { return Path.IsEmpty() || Path.StartsWith(TEXT("-")); });

        if (!FParse::Value(*Params, TEXT("-scandir="), OutOptions.ScanDir, false) || OutOptions.ScanDir.IsEmpty())
        {
            OutOptions.ScanDir = TEXT("/Game");
        }
        OutOptions.ScanDir.TrimQuotesInline();

        if (!FParse::Value(*Params, TEXT("-report="), OutOptions.ReportPath, false) || OutOptions.ReportPath.IsEmpty())
        {
            OutOptions.ReportPath = FPaths::Combine(FPaths::ProjectDir(), TEXT("Intermediate"), TEXT("AuditMaterial"), TEXT("report.json"));
        }
        OutOptions.ReportPath.TrimQuotesInline();

        OutOptions.bStats = FParse::Param(*Params, TEXT("stats"));

        for (const FString& Rule : UAssetWorkbench::ParsePathList(Params, TEXT("-rules=")))
        {
            const FString Upper = Rule.ToUpper();
            bool bKnown = false;
            for (const TCHAR* Known : kAllRules)
            {
                if (Upper == Known)
                {
                    bKnown = true;
                    break;
                }
            }

            if (!bKnown)
            {
                UE_LOG(LogUAssetWorkbenchAuditor, Error, TEXT("Unknown rule %s. Accepted: N1 N2 N3 N4 N5 N6 N7 N8 N9 U1 U2 U3 U4 G1"), *Rule);
                return false;
            }

            OutOptions.Rules.Add(Upper);
        }

        // An object path and a package path name the same function, both spellings land on the package.
        const FString ConfigPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectConfigDir(), kConfigFileName));
        // Combine, not Read. Read keeps "+Key" as a literal key name instead of appending to Key.
        FConfigFile ConfigFile;
        ConfigFile.Combine(ConfigPath);

        TArray<FString> ForbiddenEntries;
        ConfigFile.GetArray(kConfigSection, kForbiddenMaskOutputsKey, ForbiddenEntries);
        for (const FString& Entry : ForbiddenEntries)
        {
            FString FunctionPath;
            FString OutputName;
            if (!Entry.Split(TEXT(":"), &FunctionPath, &OutputName, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
            {
                UE_LOG(LogUAssetWorkbenchAuditor, Error, TEXT("%s [%s] %s entry '%s' is not <function path>:<output name>"), kConfigFileName, kConfigSection, kForbiddenMaskOutputsKey, *Entry);
                return false;
            }

            const FString PackageName = FPackageName::ObjectPathToPackageName(FunctionPath.TrimStartAndEnd());
            OutOptions.ForbiddenMaskOutputs.Add(PackageName + TEXT(":") + OutputName.TrimStartAndEnd());
        }
        UE_LOG(LogUAssetWorkbenchAuditor, Display, TEXT("AuditMaterial: G1 forbids %d OpacityMask source(s) from %s"), OutOptions.ForbiddenMaskOutputs.Num(), *ConfigPath);

        return true;
    }

    // Entry assets are walked forward through the dependency graph, a scan dir is a plain registry filter.
    bool CollectCandidateAssets(const FAuditOptions& Options, IAssetRegistry& Registry, FAuditState& State, TArray<FAssetData>& OutAssets, TSet<FString>& OutNamedPaths)
    {
        if (Options.EntryAssets.IsEmpty())
        {
            FARFilter Filter;
            Filter.ClassPaths.Add(UMaterialInterface::StaticClass()->GetClassPathName());
            Filter.bRecursiveClasses = true;
            Filter.PackagePaths.Add(FName(*Options.ScanDir));
            Filter.bRecursivePaths = true;

            Registry.GetAssets(Filter, OutAssets);
            return true;
        }

        TSet<FName> Visited;
        TArray<FName> Frontier;

        for (const FString& Entry : Options.EntryAssets)
        {
            const FString EntryPackage = ToPackageName(Entry);
            if (!FPackageName::IsValidLongPackageName(EntryPackage) || !FPackageName::DoesPackageExist(EntryPackage))
            {
                UE_LOG(LogUAssetWorkbenchAuditor, Error, TEXT("Unknown asset path: %s"), *Entry);
                return false;
            }

            const FName EntryName(*EntryPackage);
            Visited.Add(EntryName);
            Frontier.Add(EntryName);

            TArray<FAssetData> EntryAssets;
            Registry.GetAssetsByPackageName(EntryName, EntryAssets, true);
            for (const FAssetData& Asset : EntryAssets)
            {
                if (IsLevelAsset(Asset))
                {
                    State.EntryLevels.AddUnique(Asset.GetObjectPathString());
                }
                if (IsMaterialAsset(Asset))
                {
                    OutNamedPaths.Add(Asset.GetObjectPathString());
                }
            }
        }

        for (int32 Depth = 0; Depth < kDependencyWalkDepth && Frontier.Num() > 0; ++Depth)
        {
            TArray<FName> NextFrontier;

            for (FName Current : Frontier)
            {
                TArray<FAssetData> Assets;
                Registry.GetAssetsByPackageName(Current, Assets, true);

                bool bTerminal = false;
                for (const FAssetData& Asset : Assets)
                {
                    if (IsMaterialAsset(Asset))
                    {
                        OutAssets.Add(Asset);
                    }

                    // A base material's own dependencies are textures and functions, nothing below it is a
                    // consumer. An instance still has to reach its parent, so only the base ends the walk.
                    bTerminal |= Asset.AssetClassPath.GetAssetName() == FName(TEXT("Material"));
                    bTerminal |= Asset.AssetClassPath.GetAssetName() == FName(TEXT("Texture2D"));
                }

                if (bTerminal)
                {
                    continue;
                }

                TArray<FName> Dependencies;
                Registry.GetDependencies(Current, Dependencies, UE::AssetRegistry::EDependencyCategory::Package);

                for (FName Dependency : Dependencies)
                {
                    const FString DependencyName = Dependency.ToString();
                    if (DependencyName.StartsWith(TEXT("/Script/")) || Visited.Contains(Dependency))
                    {
                        continue;
                    }

                    Visited.Add(Dependency);
                    NextFrontier.Add(Dependency);
                }

            }

            Frontier = MoveTemp(NextFrontier);
        }

        return true;
    }

    FMaterialRecord& FindOrAddRecord(FAuditState& State, UMaterial* Material)
    {
        const FString AssetPath = Material->GetPathName();
        if (FMaterialRecord* Existing = State.Records.Find(AssetPath))
        {
            return *Existing;
        }

        FMaterialRecord Record;
        Record.AssetPath = AssetPath;
        Record.PackageName = Material->GetOutermost()->GetFName();
        Record.Material = Material;
        Material->AddToRoot();

        State.RecordOrder.Add(AssetPath);
        return State.Records.Add(AssetPath, MoveTemp(Record));
    }

    // Every instance below the base material, so usage can be attributed to the base and the static switch
    // combinations that actually ship can be counted.
    void CollectInstanceTree(IAssetRegistry& Registry, const FString& BasePath, FName BasePackage, TSet<FString>& OutInterfacePaths, TSet<FName>& OutPackages, TSet<FString>& OutSwitchCombos, int32& OutInstanceCount)
    {
        OutInterfacePaths.Add(BasePath);
        OutPackages.Add(BasePackage);

        TArray<FName> Frontier;
        Frontier.Add(BasePackage);

        for (int32 Depth = 0; Depth < kConsumerWalkDepth && Frontier.Num() > 0; ++Depth)
        {
            TArray<FName> NextFrontier;

            for (FName Current : Frontier)
            {
                TArray<FName> Referencers;
                Registry.GetReferencers(Current, Referencers, UE::AssetRegistry::EDependencyCategory::Package);

                for (FName Referencer : Referencers)
                {
                    if (OutPackages.Contains(Referencer))
                    {
                        continue;
                    }

                    TArray<FAssetData> Assets;
                    Registry.GetAssetsByPackageName(Referencer, Assets, true);

                    for (const FAssetData& Asset : Assets)
                    {
                        if (!IsMaterialInstanceAsset(Asset))
                        {
                            continue;
                        }

                        UMaterialInstance* Instance = LoadObject<UMaterialInstance>(nullptr, *Asset.GetObjectPathString());
                        if (!Instance || !Instance->Parent || !OutInterfacePaths.Contains(Instance->Parent->GetPathName()))
                        {
                            continue;
                        }

                        OutPackages.Add(Referencer);
                        OutInterfacePaths.Add(Asset.GetObjectPathString());
                        NextFrontier.Add(Referencer);
                        ++OutInstanceCount;

                        TArray<FString> Overrides;
                        for (const FStaticSwitchParameter& Switch : Instance->GetStaticParameters().StaticSwitchParameters)
                        {
                            if (Switch.bOverride)
                            {
                                Overrides.Add(FString::Printf(TEXT("%s=%s"), *Switch.ParameterInfo.Name.ToString(), BoolText(Switch.Value)));
                            }
                        }

                        Overrides.Sort();
                        OutSwitchCombos.Add(FString::Join(Overrides, TEXT(",")));
                    }
                }
            }

            Frontier = MoveTemp(NextFrontier);
        }
    }

    void RecordUsage(FMaterialRecord& Record, int32 Usage, const FString& Context)
    {
        if (!Record.AppliedUsages.Contains(Usage))
        {
            Record.AppliedUsages.Add(Usage, Context);
        }
    }

    void ScanSkeletalMeshConsumer(FAuditState& State, FMaterialRecord& Record, const TSet<FString>& InterfacePaths, const FAssetData& Asset)
    {
        USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *Asset.GetObjectPathString());
        if (!Mesh)
        {
            UE_LOG(LogUAssetWorkbenchAuditor, Warning, TEXT("Failed to load skeletal mesh: %s"), *Asset.GetObjectPathString());
            return;
        }

        const FString MeshPath = Mesh->GetPathName();
        const bool bNanite = Mesh->IsNaniteEnabled();

        bool bUsesMaterial = false;
        int32 EmptySlot = INDEX_NONE;
        const TArray<FSkeletalMaterial>& Materials = Mesh->GetMaterials();
        for (int32 Index = 0; Index < Materials.Num(); ++Index)
        {
            UMaterialInterface* Slot = Materials[Index].MaterialInterface;
            if (!Slot)
            {
                EmptySlot = Index;
                continue;
            }

            if (InterfacePaths.Contains(Slot->GetPathName()))
            {
                bUsesMaterial = true;
            }
        }

        if (!bUsesMaterial)
        {
            return;
        }

        RecordUsage(Record, MATUSAGE_SkeletalMesh, MeshPath);

        if (Mesh->GetMorphTargets().Num() > 0)
        {
            RecordUsage(Record, MATUSAGE_MorphTargets, FString::Printf(TEXT("%s has %d morph target(s)"), *MeshPath, Mesh->GetMorphTargets().Num()));
        }

        if (Mesh->GetMeshClothingAssets().Num() > 0)
        {
            RecordUsage(Record, MATUSAGE_Clothing, FString::Printf(TEXT("%s has %d clothing asset(s)"), *MeshPath, Mesh->GetMeshClothingAssets().Num()));
        }

        if (bNanite)
        {
            Record.NaniteMeshes.AddUnique(MeshPath);
            Record.bOnNaniteSkeletalMesh = true;
            RecordUsage(Record, MATUSAGE_Nanite, MeshPath);

            if (EmptySlot != INDEX_NONE && !State.ScannedMeshes.Contains(Asset.PackageName))
            {
                AddMaterialFinding(State, MeshPath, TEXT("N9"), kSeverityWarning, FString::Printf(TEXT("Materials[%d]"), EmptySlot), TEXT("None"), FString(), TEXT("empty slot on a nanite skeletal mesh"));
            }
        }

        State.ScannedMeshes.Add(Asset.PackageName);
    }

    void ScanStaticMeshConsumer(FAuditState& State, FMaterialRecord& Record, const TSet<FString>& InterfacePaths, const FAssetData& Asset)
    {
        bool bNanite = false;
        ReadBoolTag(Asset, TEXT("NaniteEnabled"), bNanite);

        UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *Asset.GetObjectPathString());
        if (!Mesh)
        {
            UE_LOG(LogUAssetWorkbenchAuditor, Warning, TEXT("Failed to load static mesh: %s"), *Asset.GetObjectPathString());
            return;
        }

        const FString MeshPath = Mesh->GetPathName();
        bNanite = bNanite || Mesh->IsNaniteEnabled();

        bool bUsesMaterial = false;
        int32 EmptySlot = INDEX_NONE;
        const TArray<FStaticMaterial>& Materials = Mesh->GetStaticMaterials();
        for (int32 Index = 0; Index < Materials.Num(); ++Index)
        {
            UMaterialInterface* Slot = Materials[Index].MaterialInterface;
            if (!Slot)
            {
                EmptySlot = Index;
                continue;
            }

            if (InterfacePaths.Contains(Slot->GetPathName()))
            {
                bUsesMaterial = true;
            }
        }

        if (!bUsesMaterial)
        {
            return;
        }

        RecordUsage(Record, MATUSAGE_StaticMesh, MeshPath);

        if (bNanite)
        {
            Record.NaniteMeshes.AddUnique(MeshPath);
            RecordUsage(Record, MATUSAGE_Nanite, MeshPath);

            if (EmptySlot != INDEX_NONE && !State.ScannedMeshes.Contains(Asset.PackageName))
            {
                AddMaterialFinding(State, MeshPath, TEXT("N9"), kSeverityWarning, FString::Printf(TEXT("StaticMaterials[%d]"), EmptySlot), TEXT("None"), FString(), TEXT("empty slot on a nanite static mesh"));
            }
        }

        State.ScannedMeshes.Add(Asset.PackageName);
    }

    void ScanNiagaraConsumer(FMaterialRecord& Record, const TSet<FString>& InterfacePaths, const FAssetData& Asset)
    {
        UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *Asset.GetObjectPathString());
        if (!System)
        {
            return;
        }

        const FString SystemPath = System->GetPathName();

        for (const FNiagaraEmitterHandle& EmitterHandle : System->GetEmitterHandles())
        {
            FVersionedNiagaraEmitterData* EmitterData = EmitterHandle.GetEmitterData();
            if (!EmitterData)
            {
                continue;
            }

            for (UNiagaraRendererProperties* Renderer : EmitterData->GetRenderers())
            {
                if (!Renderer)
                {
                    continue;
                }

                TArray<UMaterialInterface*> Used;
                Renderer->GetUsedMaterials(nullptr, Used);

                bool bUsesMaterial = false;
                for (UMaterialInterface* Interface : Used)
                {
                    if (Interface && InterfacePaths.Contains(Interface->GetPathName()))
                    {
                        bUsesMaterial = true;
                        break;
                    }
                }

                if (!bUsesMaterial)
                {
                    continue;
                }

                const FString RendererClass = Renderer->GetClass()->GetName();
                const FString Context = FString::Printf(TEXT("%s : %s"), *SystemPath, *RendererClass);

                if (RendererClass.Contains(TEXT("Sprite")))
                {
                    RecordUsage(Record, MATUSAGE_NiagaraSprites, Context);
                }
                else if (RendererClass.Contains(TEXT("Ribbon")))
                {
                    RecordUsage(Record, MATUSAGE_NiagaraRibbons, Context);
                }
                else if (RendererClass.Contains(TEXT("Mesh")))
                {
                    RecordUsage(Record, MATUSAGE_NiagaraMeshParticles, Context);
                }
            }
        }
    }

    // Placed components are only reachable through a level, so this runs for entry levels and nothing else.
    void ScanEntryLevels(FAuditState& State)
    {
        for (const FString& LevelPath : State.EntryLevels)
        {
            UWorld* World = LoadObject<UWorld>(nullptr, *LevelPath);
            if (!World)
            {
                UE_LOG(LogUAssetWorkbenchAuditor, Warning, TEXT("Failed to load level: %s"), *LevelPath);
                continue;
            }

            for (TActorIterator<AActor> It(World); It; ++It)
            {
                TArray<UMeshComponent*> Components;
                It->GetComponents(Components);

                for (UMeshComponent* Component : Components)
                {
                    const bool bInstanced = Component->IsA<UInstancedStaticMeshComponent>();
                    const bool bSpline = Component->IsA<USplineMeshComponent>();
                    if (!bInstanced && !bSpline)
                    {
                        continue;
                    }

                    TArray<UMaterialInterface*> Used;
                    Component->GetUsedMaterials(Used);

                    for (UMaterialInterface* Interface : Used)
                    {
                        if (!Interface)
                        {
                            continue;
                        }

                        UMaterial* Base = Interface->GetMaterial();
                        FMaterialRecord* Record = Base ? State.Records.Find(Base->GetPathName()) : nullptr;
                        if (!Record)
                        {
                            continue;
                        }

                        const FString Context = FString::Printf(TEXT("%s : %s"), *LevelPath, *Component->GetPathName());
                        RecordUsage(*Record, bInstanced ? MATUSAGE_InstancedStaticMeshes : MATUSAGE_SplineMesh, Context);
                    }
                }
            }
        }
    }

    void CollectConsumers(IAssetRegistry& Registry, FAuditState& State, FMaterialRecord& Record)
    {
        TSet<FString> InterfacePaths;
        TSet<FName> InterfacePackages;
        TSet<FString> SwitchCombos;
        SwitchCombos.Add(FString());

        CollectInstanceTree(Registry, Record.AssetPath, Record.PackageName, InterfacePaths, InterfacePackages, SwitchCombos, Record.Permutation.ChildInstanceCount);
        Record.Permutation.StaticSwitchCombosUsed = SwitchCombos.Num();

        for (FName InterfacePackage : InterfacePackages)
        {
            TArray<FName> Referencers;
            Registry.GetReferencers(InterfacePackage, Referencers, UE::AssetRegistry::EDependencyCategory::Package);

            for (FName Referencer : Referencers)
            {
                if (InterfacePackages.Contains(Referencer))
                {
                    continue;
                }

                TArray<FAssetData> Assets;
                Registry.GetAssetsByPackageName(Referencer, Assets, true);

                for (const FAssetData& Asset : Assets)
                {
                    const FString ClassName = Asset.AssetClassPath.GetAssetName().ToString();

                    if (ClassName == TEXT("SkeletalMesh"))
                    {
                        ScanSkeletalMeshConsumer(State, Record, InterfacePaths, Asset);
                    }
                    else if (ClassName == TEXT("StaticMesh"))
                    {
                        ScanStaticMeshConsumer(State, Record, InterfacePaths, Asset);
                    }
                    else if (ClassName == TEXT("NiagaraSystem"))
                    {
                        ScanNiagaraConsumer(Record, InterfacePaths, Asset);
                    }
                }
            }
        }
    }

    void GatherTier1(FMaterialRecord& Record)
    {
        UMaterial* Material = Record.Material;
        FMaterialCost& Cost = Record.Cost;

        Cost.Domain = EnumName(StaticEnum<EMaterialDomain>(), Material->MaterialDomain.GetValue());
        Cost.BlendMode = EnumName(StaticEnum<EBlendMode>(), Material->GetBlendMode());
        Cost.ShadingModel = EnumName(StaticEnum<EMaterialShadingModel>(), Material->GetShadingModels().GetFirstShadingModel());
        Cost.bTwoSided = Material->IsTwoSided();
        Cost.bIsSky = Material->bIsSky != 0;
        Cost.bTessellation = Material->IsTessellationEnabled();
        Cost.bAutomaticUsage = Material->bAutomaticallySetUsageInEditor != 0;
        Cost.OpacityMaskClipValue = Material->GetOpacityMaskClipValue();
        Cost.NumCustomizedUVs = Material->NumCustomizedUVs;

        Cost.bWorldPositionOffsetConnected = Material->HasVertexPositionOffsetConnected();
        Cost.bPixelDepthOffsetConnected = Material->HasPixelDepthOffsetConnected();
        Cost.bDisplacementConnected = Material->HasDisplacementConnected();

        const FMaterialCachedExpressionData& CachedData = Material->GetCachedExpressionData();
        Cost.bHasVertexInterpolator = CachedData.bHasVertexInterpolator != 0;
        Cost.bHasPerInstanceRandom = CachedData.bHasPerInstanceRandom != 0;
        Cost.bHasPerInstanceCustomData = CachedData.bHasPerInstanceCustomData != 0;
        Cost.bHasCustomizedUVs = CachedData.bHasCustomizedUVs != 0;
        Cost.bHasSceneColor = CachedData.bHasSceneColor != 0;
        Cost.bHasWorldPosition = CachedData.bHasWorldPosition != 0;
        Cost.bHasMaterialLayers = CachedData.bHasMaterialLayers != 0;
        Cost.bHasRuntimeVirtualTextureOutput = CachedData.bHasRuntimeVirtualTextureOutput != 0;
        Cost.FunctionCount = CachedData.FunctionInfos.Num();

        Cost.ReferencedTextureCount = Material->GetReferencedTextures().Num();
        Cost.ExpressionCount = Material->GetExpressions().Num();

        int32 QualityLevels = 0;
        for (bool bUsed : CachedData.QualityLevelsUsed)
        {
            QualityLevels += bUsed ? 1 : 0;
        }
        Record.Permutation.QualityLevels = FMath::Max(1, QualityLevels);

        int32 UsageFlagCount = 0;
        for (int32 Usage = 0; Usage < MATUSAGE_MAX; ++Usage)
        {
            UsageFlagCount += Material->GetUsageByFlag(static_cast<EMaterialUsage>(Usage)) ? 1 : 0;
        }
        Record.Permutation.UsageFlagCount = UsageFlagCount;

        TArray<FMaterialParameterInfo> SwitchInfos;
        TArray<FGuid> SwitchIds;
        Material->GetAllParameterInfoOfType(EMaterialParameterType::StaticSwitch, SwitchInfos, SwitchIds);
        Record.Permutation.StaticSwitchCount = SwitchInfos.Num();
    }

    // Compiles just the representative shader types and reads the numbers off the finished map, which is
    // the DumpMaterialInfo path. Minutes per material, never on by default.
    void GatherTier2(FMaterialRecord& Record)
    {
        UMaterial* Material = Record.Material;

        TArray<FMaterialResource*> ResourcesToCache;
        FMaterialResource* Resource = FindOrCreateMaterialResource(ResourcesToCache, Material, nullptr, kStatsShaderPlatform, EMaterialQualityLevel::High);
        if (!Resource)
        {
            UE_LOG(LogUAssetWorkbenchAuditor, Warning, TEXT("AuditMaterial: no material resource for %s"), *Record.AssetPath);
            return;
        }

        TMap<FName, TArray<FMaterialStatsUtils::FRepresentativeShaderInfo>> Descriptions;
        FMaterialStatsUtils::GetRepresentativeShaderTypesAndDescriptions(Descriptions, Resource);

        TArray<const FVertexFactoryType*> VFTypes;
        TArray<const FShaderPipelineType*> PipelineTypes;
        TArray<const FShaderType*> ShaderTypes;

        for (const TPair<FName, TArray<FMaterialStatsUtils::FRepresentativeShaderInfo>>& Pair : Descriptions)
        {
            const FVertexFactoryType* VFType = FindVertexFactoryType(Pair.Key);
            if (!VFType)
            {
                continue;
            }

            for (const FMaterialStatsUtils::FRepresentativeShaderInfo& ShaderInfo : Pair.Value)
            {
                const FShaderType* ShaderType = FindShaderTypeByName(ShaderInfo.ShaderName);
                if (!ShaderType)
                {
                    continue;
                }

                VFTypes.Add(VFType);
                ShaderTypes.Add(ShaderType);
                PipelineTypes.Add(nullptr);
            }
        }

        if (Resource->CacheShaders(EMaterialShaderPrecompileMode::None))
        {
            Resource->CacheGivenTypes(VFTypes, PipelineTypes, ShaderTypes);
        }

        while (GShaderCompilingManager->IsCompiling())
        {
            GShaderCompilingManager->ProcessAsyncResults(false, false);
            FlushRenderingCommands();
        }

        if (!Resource->IsGameThreadShaderMapComplete())
        {
            UE_LOG(LogUAssetWorkbenchAuditor, Warning, TEXT("AuditMaterial: incomplete shader map for %s, stats are partial"), *Record.AssetPath);
        }

        FMaterialCost& Cost = Record.Cost;
        Cost.bStatsResolved = true;
        Cost.SamplerUsage = Resource->GetSamplerUsage();

        uint32 SamplesVS = 0;
        uint32 SamplesPS = 0;
        Resource->GetEstimatedNumTextureSamples(SamplesVS, SamplesPS);
        Cost.TextureSamplesVS = static_cast<int32>(SamplesVS);
        Cost.TextureSamplesPS = static_cast<int32>(SamplesPS);

        Cost.VirtualTextureLookups = static_cast<int32>(Resource->GetEstimatedNumVirtualTextureLookups());
        Cost.VirtualTextureStacks = static_cast<int32>(Resource->GetNumVirtualTextureStacks());

        uint32 UVScalars = 0;
        uint32 CustomInterpolatorScalars = 0;
        Resource->GetUserInterpolatorUsage(UVScalars, CustomInterpolatorScalars);
        Cost.UsedUVScalars = static_cast<int32>(UVScalars);
        Cost.UsedCustomInterpolatorScalars = static_cast<int32>(CustomInterpolatorScalars);

        FMaterialResource::FLWCUsagesArray UsagesVS;
        FMaterialResource::FLWCUsagesArray UsagesPS;
        FMaterialResource::FLWCUsagesArray UsagesCS;
        Resource->GetEstimatedLWCFuncUsages(UsagesVS, UsagesPS, UsagesCS);
        for (int32 Index = 0; Index < UsagesVS.Num(); ++Index)
        {
            Cost.LWCFuncUsagesVS += UsagesVS[Index];
            Cost.LWCFuncUsagesPS += UsagesPS[Index];
            Cost.LWCFuncUsagesCS += UsagesCS[Index];
        }

        const FMaterialShaderMap* ShaderMap = Resource->GetGameThreadShaderMap();
        if (ShaderMap)
        {
            Cost.ShaderCount = static_cast<int32>(ShaderMap->GetShaderNum());

            // FMaterialStatsUtils::GetRepresentativeInstructionCounts is not exported, this is its body
            // reduced to the mesh shader map lookup the editor stats panel reads.
            for (const TPair<FName, TArray<FMaterialStatsUtils::FRepresentativeShaderInfo>>& Pair : Descriptions)
            {
                const FVertexFactoryType* VFType = FindVertexFactoryType(Pair.Key);
                const FMeshMaterialShaderMap* MeshShaderMap = VFType ? ShaderMap->GetMeshShaderMap(VFType) : nullptr;

                for (const FMaterialStatsUtils::FRepresentativeShaderInfo& ShaderInfo : Pair.Value)
                {
                    FShaderType* ShaderType = FindShaderTypeByName(ShaderInfo.ShaderName);
                    if (!ShaderType)
                    {
                        continue;
                    }

                    const uint32 Count = MeshShaderMap ? MeshShaderMap->GetMaxNumInstructionsForShader(*ShaderMap, ShaderType) : ShaderMap->GetMaxNumInstructionsForShader(ShaderType);
                    if (Count == 0)
                    {
                        continue;
                    }

                    Cost.Instructions.Emplace(ShaderInfo.ShaderDescription, static_cast<int32>(Count));
                    Cost.MaxInstructionCount = FMath::Max(Cost.MaxInstructionCount, static_cast<int32>(Count));
                }
            }
        }

        FMaterial::DeferredDeleteArray(ResourcesToCache);
    }

    // Only reachable on the in-editor queue path, the editor statistics call needs a live RHI.
    void GatherEditorStats(FMaterialRecord& Record)
    {
        const FMaterialStatistics Statistics = UMaterialEditingLibrary::GetStatistics(Record.Material);

        TSharedRef<FJsonObject> Stats = MakeShared<FJsonObject>();
        Stats->SetNumberField(TEXT("NumPixelShaderInstructions"), Statistics.NumPixelShaderInstructions);
        Stats->SetNumberField(TEXT("NumVertexShaderInstructions"), Statistics.NumVertexShaderInstructions);
        Stats->SetNumberField(TEXT("NumSamplers"), Statistics.NumSamplers);
        Stats->SetNumberField(TEXT("NumVertexTextureSamples"), Statistics.NumVertexTextureSamples);
        Stats->SetNumberField(TEXT("NumPixelTextureSamples"), Statistics.NumPixelTextureSamples);
        Stats->SetNumberField(TEXT("NumVirtualTextureSamples"), Statistics.NumVirtualTextureSamples);
        Stats->SetNumberField(TEXT("NumUVScalars"), Statistics.NumUVScalars);
        Stats->SetNumberField(TEXT("NumInterpolatorScalars"), Statistics.NumInterpolatorScalars);

        Record.EditorStats = Stats;
    }

    void RunUsageRules(const FAuditOptions& Options, FAuditState& State, FMaterialRecord& Record)
    {
        UMaterial* Material = Record.Material;

        for (const TPair<int32, FString>& Applied : Record.AppliedUsages)
        {
            const int32 Usage = Applied.Key;
            if (!NeedsUsageFlag(Material, Usage))
            {
                continue;
            }

            const FString Flag = UsageName(Usage);

            if (IsRuleActive(Options, TEXT("U1")))
            {
                AddMaterialFinding(State, Record.AssetPath, TEXT("U1"), kSeverityError, Flag, TEXT("false"), TEXT("true"), FString::Printf(TEXT("applied by %s"), *Applied.Value));
            }

            if (Record.Cost.bAutomaticUsage && IsRuleActive(Options, TEXT("U2")))
            {
                AddMaterialFinding(State, Record.AssetPath, TEXT("U2"), kSeverityError, TEXT("bAutomaticallySetUsageInEditor"), TEXT("true"), FString(), FString::Printf(TEXT("%s missing, synchronous recompile every editor launch until resaved"), *Flag));
            }

            if (!Record.Cost.bAutomaticUsage && IsRuleActive(Options, TEXT("U4")))
            {
                AddMaterialFinding(State, Record.AssetPath, TEXT("U4"), kSeverityError, TEXT("bAutomaticallySetUsageInEditor"), TEXT("false"), FString(), FString::Printf(TEXT("%s missing, editor cannot self heal, default material used in game"), *Flag));
            }
        }

        if (!IsRuleActive(Options, TEXT("U3")))
        {
            return;
        }

        for (int32 Usage = 0; Usage < MATUSAGE_MAX; ++Usage)
        {
            const bool bSet = Material->GetUsageByFlag(static_cast<EMaterialUsage>(Usage));
            if (!bSet || Record.AppliedUsages.Contains(Usage))
            {
                continue;
            }

            AddMaterialFinding(State, Record.AssetPath, TEXT("U3"), kSeverityWarning, UsageName(Usage), TEXT("true"), FString(), TEXT("no consumer found in scope"));
        }
    }

    // Upstream walk from a pin. A function call checks the output it is reached through, then continues
    // inside the function from that output. A function input inside leads back to the caller's pin.
    struct FForbiddenSourceWalk
    {
        const TSet<FString>& Forbidden;
        TSet<TTuple<const UMaterialExpression*, int32, const UMaterialExpression*>> Visited;
        TArray<UMaterialExpressionMaterialFunctionCall*> Calls;
        FString Hit;

        explicit FForbiddenSourceWalk(const TSet<FString>& InForbidden)
            : Forbidden(InForbidden)
        {
        }

        bool Walk(const FExpressionInput& Input)
        {
            UMaterialExpression* Expression = Input.Expression;
            UMaterialExpressionMaterialFunctionCall* Caller = Calls.IsEmpty() ? nullptr : Calls.Last();
            if (!Expression || Visited.Contains(MakeTuple(Expression, Input.OutputIndex, Caller)))
            {
                return false;
            }
            Visited.Add(MakeTuple(Expression, Input.OutputIndex, Caller));

            if (UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
            {
                if (!Call->MaterialFunction || !Call->FunctionOutputs.IsValidIndex(Input.OutputIndex))
                {
                    return false;
                }

                const FFunctionExpressionOutput& Output = Call->FunctionOutputs[Input.OutputIndex];
                const FString Key = Call->MaterialFunction->GetOutermost()->GetName() + TEXT(":") + Output.Output.OutputName.ToString();
                if (Forbidden.Contains(Key))
                {
                    Hit = Key;
                    return true;
                }
                if (!Output.ExpressionOutput)
                {
                    return false;
                }

                Calls.Push(Call);
                const bool bFound = Walk(Output.ExpressionOutput->A);
                Calls.Pop(EAllowShrinking::No);
                return bFound;
            }

            if (const UMaterialExpressionFunctionInput* FunctionInput = Cast<UMaterialExpressionFunctionInput>(Expression))
            {
                if (!Caller)
                {
                    return false;
                }

                for (const FFunctionExpressionInput& CallerInput : Caller->FunctionInputs)
                {
                    if (CallerInput.ExpressionInput == FunctionInput)
                    {
                        Calls.Pop(EAllowShrinking::No);
                        const bool bFound = Walk(CallerInput.Input);
                        Calls.Push(Caller);
                        return bFound;
                    }
                }
                return false;
            }

            if (const UMaterialExpressionNamedRerouteUsage* Usage = Cast<UMaterialExpressionNamedRerouteUsage>(Expression))
            {
                return Usage->Declaration && Walk(Usage->Declaration->Input);
            }

            for (FExpressionInputIterator It(Expression); It; ++It)
            {
                if (Walk(*It.Input))
                {
                    return true;
                }
            }
            return false;
        }
    };

    void RunGraphRules(const FAuditOptions& Options, FAuditState& State, FMaterialRecord& Record)
    {
        UMaterial* Material = Record.Material;
        const UMaterialEditorOnlyData* EditorOnlyData = Material->GetEditorOnlyData();
        const bool bMasked = Material->GetBlendMode() == BLEND_Masked;
        if (!IsRuleActive(Options, TEXT("G1")) || Options.ForbiddenMaskOutputs.IsEmpty() || !bMasked || !EditorOnlyData)
        {
            return;
        }

        FForbiddenSourceWalk Walk(Options.ForbiddenMaskOutputs);
        if (Walk.Walk(EditorOnlyData->OpacityMask))
        {
            AddMaterialFinding(State, Record.AssetPath, TEXT("G1"), kSeverityError, TEXT("OpacityMask"), Walk.Hit, FString(), FString::Printf(TEXT("listed in %s [%s] %s"), kConfigFileName, kConfigSection, kForbiddenMaskOutputsKey));
        }
    }

    void RunNaniteRules(const FAuditOptions& Options, FAuditState& State, FMaterialRecord& Record)
    {
        if (Record.NaniteMeshes.IsEmpty())
        {
            return;
        }

        UMaterial* Material = Record.Material;
        const FString MeshContext = FString::Join(Record.NaniteMeshes, TEXT(" "));
        const EBlendMode BlendMode = Material->GetBlendMode();
        const bool bSupportedBlendMode = IsOpaqueOrMaskedBlendMode(BlendMode);

        if (IsRuleActive(Options, TEXT("N1")) && !bSupportedBlendMode)
        {
            AddMaterialFinding(State, Record.AssetPath, TEXT("N1"), kSeverityError, TEXT("BlendMode"), Record.Cost.BlendMode, FString(), FString::Printf(TEXT("nanite mesh %s, only Opaque and Masked render"), *MeshContext));
        }

        if (IsRuleActive(Options, TEXT("N2")) && Material->GetShadingModels().HasShadingModel(MSM_SingleLayerWater))
        {
            AddMaterialFinding(State, Record.AssetPath, TEXT("N2"), kSeverityError, TEXT("ShadingModel"), TEXT("MSM_SingleLayerWater"), FString(), FString::Printf(TEXT("nanite mesh %s"), *MeshContext));
        }

        if (IsRuleActive(Options, TEXT("N3")) && NeedsUsageFlag(Material, MATUSAGE_Nanite))
        {
            AddMaterialFinding(State, Record.AssetPath, TEXT("N3"), kSeverityError, TEXT("bUsedWithNanite"), TEXT("false"), TEXT("true"), FString::Printf(TEXT("nanite mesh %s, silent fallback to WorldGridMaterial"), *MeshContext));
        }

        if (IsRuleActive(Options, TEXT("N4")) && Record.bOnNaniteSkeletalMesh && NeedsUsageFlag(Material, MATUSAGE_SkeletalMesh))
        {
            AddMaterialFinding(State, Record.AssetPath, TEXT("N4"), kSeverityError, TEXT("bUsedWithSkeletalMesh"), TEXT("false"), TEXT("true"), FString::Printf(TEXT("nanite skeletal mesh %s"), *MeshContext));
        }

        if (IsRuleActive(Options, TEXT("N5")) && Record.Cost.bIsSky)
        {
            AddMaterialFinding(State, Record.AssetPath, TEXT("N5"), kSeverityWarning, TEXT("bIsSky"), TEXT("true"), FString(), FString::Printf(TEXT("nanite mesh %s skips sky materials"), *MeshContext));
        }

        if (IsRuleActive(Options, TEXT("N6")) && BlendMode == BLEND_Masked && !IsNaniteMaskingAllowed())
        {
            AddMaterialFinding(State, Record.AssetPath, TEXT("N6"), kSeverityWarning, TEXT("BlendMode"), TEXT("BLEND_Masked"), FString(), TEXT("r.Nanite.AllowMaskedMaterials is 0"));
        }

        if (IsRuleActive(Options, TEXT("N7")) && Record.Cost.bWorldPositionOffsetConnected)
        {
            AddMaterialFinding(State, Record.AssetPath, TEXT("N7"), kSeverityInfo, TEXT("WorldPositionOffset"), TEXT("connected"), FString(), FString::Printf(TEXT("nanite mesh %s"), *MeshContext));
        }

        if (IsRuleActive(Options, TEXT("N8")) && Record.Cost.bPixelDepthOffsetConnected)
        {
            AddMaterialFinding(State, Record.AssetPath, TEXT("N8"), kSeverityInfo, TEXT("PixelDepthOffset"), TEXT("connected"), FString(), FString::Printf(TEXT("nanite mesh %s"), *MeshContext));
        }
    }

    TSharedRef<FJsonObject> BuildCostJson(const FMaterialCost& Cost)
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("MaterialDomain"), Cost.Domain);
        Json->SetStringField(TEXT("BlendMode"), Cost.BlendMode);
        Json->SetStringField(TEXT("ShadingModel"), Cost.ShadingModel);
        Json->SetBoolField(TEXT("TwoSided"), Cost.bTwoSided);
        Json->SetBoolField(TEXT("bIsSky"), Cost.bIsSky);
        Json->SetBoolField(TEXT("bEnableTessellation"), Cost.bTessellation);
        Json->SetBoolField(TEXT("bAutomaticallySetUsageInEditor"), Cost.bAutomaticUsage);
        Json->SetNumberField(TEXT("OpacityMaskClipValue"), Cost.OpacityMaskClipValue);
        Json->SetNumberField(TEXT("NumCustomizedUVs"), Cost.NumCustomizedUVs);
        Json->SetBoolField(TEXT("WorldPositionOffsetConnected"), Cost.bWorldPositionOffsetConnected);
        Json->SetBoolField(TEXT("PixelDepthOffsetConnected"), Cost.bPixelDepthOffsetConnected);
        Json->SetBoolField(TEXT("DisplacementConnected"), Cost.bDisplacementConnected);
        Json->SetBoolField(TEXT("bHasVertexInterpolator"), Cost.bHasVertexInterpolator);
        Json->SetBoolField(TEXT("bHasPerInstanceRandom"), Cost.bHasPerInstanceRandom);
        Json->SetBoolField(TEXT("bHasPerInstanceCustomData"), Cost.bHasPerInstanceCustomData);
        Json->SetBoolField(TEXT("bHasCustomizedUVs"), Cost.bHasCustomizedUVs);
        Json->SetBoolField(TEXT("bHasSceneColor"), Cost.bHasSceneColor);
        Json->SetBoolField(TEXT("bHasWorldPosition"), Cost.bHasWorldPosition);
        Json->SetBoolField(TEXT("bHasMaterialLayers"), Cost.bHasMaterialLayers);
        Json->SetBoolField(TEXT("bHasRuntimeVirtualTextureOutput"), Cost.bHasRuntimeVirtualTextureOutput);
        Json->SetNumberField(TEXT("ReferencedTextureCount"), Cost.ReferencedTextureCount);
        Json->SetNumberField(TEXT("ExpressionCount"), Cost.ExpressionCount);
        Json->SetNumberField(TEXT("FunctionCount"), Cost.FunctionCount);

        if (!Cost.bStatsResolved)
        {
            return Json;
        }

        Json->SetNumberField(TEXT("SamplerUsage"), Cost.SamplerUsage);
        Json->SetNumberField(TEXT("EstimatedTextureSamplesVS"), Cost.TextureSamplesVS);
        Json->SetNumberField(TEXT("EstimatedTextureSamplesPS"), Cost.TextureSamplesPS);
        Json->SetNumberField(TEXT("EstimatedVirtualTextureLookups"), Cost.VirtualTextureLookups);
        Json->SetNumberField(TEXT("VirtualTextureStacks"), Cost.VirtualTextureStacks);
        Json->SetNumberField(TEXT("UsedUVScalars"), Cost.UsedUVScalars);
        Json->SetNumberField(TEXT("UsedCustomInterpolatorScalars"), Cost.UsedCustomInterpolatorScalars);
        Json->SetNumberField(TEXT("LWCFuncUsagesVS"), Cost.LWCFuncUsagesVS);
        Json->SetNumberField(TEXT("LWCFuncUsagesPS"), Cost.LWCFuncUsagesPS);
        Json->SetNumberField(TEXT("LWCFuncUsagesCS"), Cost.LWCFuncUsagesCS);
        Json->SetNumberField(TEXT("ShaderCount"), Cost.ShaderCount);
        Json->SetNumberField(TEXT("MaxInstructionCount"), Cost.MaxInstructionCount);

        TArray<TSharedPtr<FJsonValue>> InstructionsJson;
        for (const TPair<FString, int32>& Entry : Cost.Instructions)
        {
            TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("Shader"), Entry.Key);
            Item->SetNumberField(TEXT("Instructions"), Entry.Value);
            InstructionsJson.Add(MakeShared<FJsonValueObject>(Item));
        }
        Json->SetArrayField(TEXT("Instructions"), InstructionsJson);

        return Json;
    }

    TSharedRef<FJsonObject> BuildPermutationJson(const FPermutation& Permutation)
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetNumberField(TEXT("UsageFlagCount"), Permutation.UsageFlagCount);
        Json->SetNumberField(TEXT("QualityLevels"), Permutation.QualityLevels);
        Json->SetNumberField(TEXT("StaticSwitchCount"), Permutation.StaticSwitchCount);
        Json->SetNumberField(TEXT("StaticSwitchCombosUsed"), Permutation.StaticSwitchCombosUsed);
        Json->SetNumberField(TEXT("ChildInstanceCount"), Permutation.ChildInstanceCount);
        Json->SetNumberField(TEXT("Size"), Permutation.GetSize());
        return Json;
    }

    // Ranks what would make a material arrive late in PIE, using only numbers this run measured.
    TSharedRef<FJsonObject> BuildPIEWarmupJson(const FAuditOptions& Options, const FAuditState& State)
    {
        struct FSuspect
        {
            FString Material;
            FString Reason;
            FString Evidence;
            int32 Score = 0;
        };

        TArray<FSuspect> Suspects;

        for (const FString& AssetPath : State.RecordOrder)
        {
            const FMaterialRecord& Record = State.Records[AssetPath];

            bool bSynchronousRecompile = false;
            for (const FFinding& Finding : State.Findings)
            {
                if (Finding.Asset == AssetPath && Finding.Rule == TEXT("U2"))
                {
                    bSynchronousRecompile = true;
                    break;
                }
            }

            const int32 PermutationSize = Record.Permutation.GetSize();

            if (bSynchronousRecompile)
            {
                FSuspect Suspect;
                Suspect.Material = AssetPath;
                Suspect.Reason = TEXT("SynchronousUsageRecompile");
                Suspect.Evidence = FString::Printf(TEXT("U2, bAutomaticallySetUsageInEditor=true with a missing usage flag, permutation size %d"), PermutationSize);
                Suspect.Score = 1000 + PermutationSize;
                Suspects.Add(MoveTemp(Suspect));
                continue;
            }

            if (PermutationSize >= kPermutationSuspectSize)
            {
                FSuspect Suspect;
                Suspect.Material = AssetPath;
                Suspect.Reason = TEXT("PermutationScale");
                Suspect.Evidence = FString::Printf(TEXT("UsageFlagCount %d x QualityLevels %d x StaticSwitchCombosUsed %d = %d"), Record.Permutation.UsageFlagCount, Record.Permutation.QualityLevels, Record.Permutation.StaticSwitchCombosUsed, PermutationSize);
                Suspect.Score = 100 + PermutationSize;
                Suspects.Add(MoveTemp(Suspect));
                continue;
            }

            const bool bShaderVolume = Record.Cost.bStatsResolved && (Record.Cost.ShaderCount >= kShaderCountSuspect || Record.Cost.MaxInstructionCount >= kInstructionCountSuspect);
            if (bShaderVolume)
            {
                FSuspect Suspect;
                Suspect.Material = AssetPath;
                Suspect.Reason = TEXT("ShaderVolume");
                Suspect.Evidence = FString::Printf(TEXT("ShaderCount %d, MaxInstructionCount %d"), Record.Cost.ShaderCount, Record.Cost.MaxInstructionCount);
                Suspect.Score = Record.Cost.ShaderCount + Record.Cost.MaxInstructionCount;
                Suspects.Add(MoveTemp(Suspect));
            }
        }

        Suspects.Sort([](const FSuspect& Left, const FSuspect& Right) { return Left.Score > Right.Score; });

        TArray<TSharedPtr<FJsonValue>> SuspectsJson;
        for (const FSuspect& Suspect : Suspects)
        {
            TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("Material"), Suspect.Material);
            Item->SetStringField(TEXT("Reason"), Suspect.Reason);
            Item->SetStringField(TEXT("Evidence"), Suspect.Evidence);
            SuspectsJson.Add(MakeShared<FJsonValueObject>(Item));
        }

        FString Notes;
        if (Suspects.IsEmpty())
        {
            Notes = FString::Printf(TEXT("No material in scope shows a synchronous usage recompile, a permutation size at or above %d, or an outlier shader count."), kPermutationSuspectSize);
        }
        else
        {
            Notes = FString::Printf(TEXT("%d material(s) ranked. SynchronousUsageRecompile outranks PermutationScale, which outranks ShaderVolume."), Suspects.Num());
        }

        if (!Options.bStats)
        {
            Notes += TEXT(" Shader counts and instruction counts are absent, the run had no -stats.");
        }

        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetArrayField(TEXT("Suspects"), SuspectsJson);
        Json->SetStringField(TEXT("Notes"), Notes);
        return Json;
    }

    bool WriteReport(const FAuditOptions& Options, const FAuditState& State, int32& OutErrors, int32& OutWarnings, int32& OutInfos)
    {
        TArray<TSharedPtr<FJsonValue>> FindingsJson;
        TArray<FString> SpecOrder;
        TMap<FString, TSharedPtr<FJsonObject>> SpecProperties;
        TMap<FString, int32> RuleCounts;

        for (const FFinding& Finding : State.Findings)
        {
            if (Finding.Severity == kSeverityError)
            {
                ++OutErrors;
            }
            else if (Finding.Severity == kSeverityWarning)
            {
                ++OutWarnings;
            }
            else
            {
                ++OutInfos;
            }

            RuleCounts.FindOrAdd(Finding.Rule)++;

            TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("Asset"), Finding.Asset);
            Item->SetStringField(TEXT("Rule"), Finding.Rule);
            Item->SetStringField(TEXT("Severity"), Finding.Severity);
            Item->SetStringField(TEXT("Property"), Finding.Property);
            Item->SetStringField(TEXT("Current"), Finding.Current);
            Item->SetStringField(TEXT("Expected"), Finding.Expected);
            Item->SetStringField(TEXT("Context"), Finding.Context);
            FindingsJson.Add(MakeShared<FJsonValueObject>(Item));

            // Info carries no fix, and a rule without a deterministic Expected cannot be handed to a writer.
            const bool bActionable = Finding.Severity != kSeverityInfo && !Finding.Property.IsEmpty() && !Finding.Expected.IsEmpty();
            if (!bActionable)
            {
                continue;
            }

            TSharedPtr<FJsonObject>& Properties = SpecProperties.FindOrAdd(Finding.Asset);
            if (!Properties.IsValid())
            {
                Properties = MakeShared<FJsonObject>();
                SpecOrder.Add(Finding.Asset);
            }

            if (!Properties->HasField(Finding.Property))
            {
                Properties->SetStringField(Finding.Property, Finding.Expected);
            }
        }

        TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
        Summary->SetNumberField(TEXT("Error"), OutErrors);
        Summary->SetNumberField(TEXT("Warning"), OutWarnings);
        Summary->SetNumberField(TEXT("Info"), OutInfos);

        TSharedRef<FJsonObject> Rules = MakeShared<FJsonObject>();
        for (const TCHAR* Rule : kAllRules)
        {
            const int32* Count = RuleCounts.Find(Rule);
            Rules->SetNumberField(Rule, Count ? *Count : 0);
        }
        Summary->SetObjectField(TEXT("Rules"), Rules);

        TArray<TSharedPtr<FJsonValue>> MaterialsJson;
        for (const FString& AssetPath : State.RecordOrder)
        {
            const FMaterialRecord& Record = State.Records[AssetPath];

            TArray<TSharedPtr<FJsonValue>> AppliedJson;
            for (const TPair<int32, FString>& Applied : Record.AppliedUsages)
            {
                TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
                Item->SetStringField(TEXT("Flag"), UsageName(Applied.Key));
                Item->SetStringField(TEXT("Context"), Applied.Value);
                AppliedJson.Add(MakeShared<FJsonValueObject>(Item));
            }

            TArray<TSharedPtr<FJsonValue>> FlagsJson;
            for (int32 Usage = 0; Usage < MATUSAGE_MAX; ++Usage)
            {
                if (Record.Material->GetUsageByFlag(static_cast<EMaterialUsage>(Usage)))
                {
                    FlagsJson.Add(MakeShared<FJsonValueString>(UsageName(Usage)));
                }
            }

            TArray<TSharedPtr<FJsonValue>> NaniteJson;
            for (const FString& Mesh : Record.NaniteMeshes)
            {
                NaniteJson.Add(MakeShared<FJsonValueString>(Mesh));
            }

            TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("Asset"), AssetPath);
            Item->SetStringField(TEXT("Class"), Record.Material->GetClass()->GetName());
            Item->SetArrayField(TEXT("UsageFlags"), FlagsJson);
            Item->SetArrayField(TEXT("AppliedUsages"), AppliedJson);
            Item->SetArrayField(TEXT("NaniteMeshes"), NaniteJson);
            Item->SetObjectField(TEXT("Cost"), BuildCostJson(Record.Cost));
            Item->SetObjectField(TEXT("Permutation"), BuildPermutationJson(Record.Permutation));
            if (Record.EditorStats.IsValid())
            {
                Item->SetObjectField(TEXT("EditorStats"), Record.EditorStats.ToSharedRef());
            }
            MaterialsJson.Add(MakeShared<FJsonValueObject>(Item));
        }

        TArray<TSharedPtr<FJsonValue>> TargetsJson;
        for (const FString& AssetPath : SpecOrder)
        {
            TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
            Target->SetStringField(TEXT("AssetPath"), AssetPath);
            Target->SetObjectField(TEXT("Properties"), SpecProperties[AssetPath]);
            TargetsJson.Add(MakeShared<FJsonValueObject>(Target));
        }

        TSharedRef<FJsonObject> Spec = MakeShared<FJsonObject>();
        Spec->SetArrayField(TEXT("Targets"), TargetsJson);

        TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetStringField(TEXT("RunName"), TEXT("AuditMaterial"));
        Root->SetNumberField(TEXT("Scanned"), State.RecordOrder.Num());
        Root->SetNumberField(TEXT("MaterialInterfacesInScope"), State.MaterialInterfacesInScope);
        Root->SetBoolField(TEXT("Stats"), Options.bStats);
        Root->SetArrayField(TEXT("Findings"), FindingsJson);
        Root->SetObjectField(TEXT("Summary"), Summary);
        Root->SetArrayField(TEXT("Materials"), MaterialsJson);
        Root->SetObjectField(TEXT("PIEWarmup"), BuildPIEWarmupJson(Options, State));
        Root->SetObjectField(TEXT("Spec"), Spec);

        if (!UAssetWorkbench::SaveJsonToFile(Root, Options.ReportPath))
        {
            UE_LOG(LogUAssetWorkbenchAuditor, Error, TEXT("Failed to write report: %s"), *Options.ReportPath);
            return false;
        }

        UE_LOG(LogUAssetWorkbenchAuditor, Display, TEXT("AuditMaterial: report written: %s"), *Options.ReportPath);
        return true;
    }
}

UAuditMaterialCommandlet::UAuditMaterialCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UAuditMaterialCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchAuditor, Display, TEXT("UAssetWorkbench v%s - AuditMaterial"), UASSET_WORKBENCH_VERSION_STRING);

    FAuditOptions Options;
    if (!ParseOptions(Params, Options))
    {
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    UE_LOG(LogUAssetWorkbenchAuditor, Display, TEXT("AuditMaterial: scanning asset registry..."));
    AssetRegistry.SearchAllAssets(true);

    FAuditState State;
    TArray<FAssetData> Candidates;
    TSet<FString> NamedPaths;
    if (!CollectCandidateAssets(Options, AssetRegistry, State, Candidates, NamedPaths))
    {
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    State.MaterialInterfacesInScope = Candidates.Num();

    for (const FAssetData& Asset : Candidates)
    {
        UMaterialInterface* Interface = LoadObject<UMaterialInterface>(nullptr, *Asset.GetObjectPathString());
        if (!Interface)
        {
            UE_LOG(LogUAssetWorkbenchAuditor, Warning, TEXT("Failed to load material: %s"), *Asset.GetObjectPathString());
            continue;
        }

        UMaterial* Base = Interface->GetMaterial();
        if (!Base)
        {
            continue;
        }

        FMaterialRecord& Record = FindOrAddRecord(State, Base);
        Record.bNamedEntry |= NamedPaths.Contains(Asset.GetObjectPathString());
    }

    if (State.RecordOrder.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchAuditor, Warning, TEXT("AuditMaterial: no materials in scope (assets=%d scandir=%s)"), Options.EntryAssets.Num(), *Options.ScanDir);
    }

    UE_LOG(LogUAssetWorkbenchAuditor, Display, TEXT("AuditMaterial: %d base material(s) from %d material interface(s)"), State.RecordOrder.Num(), State.MaterialInterfacesInScope);

    for (const FString& AssetPath : State.RecordOrder)
    {
        FMaterialRecord& Record = State.Records[AssetPath];
        GatherTier1(Record);
        CollectConsumers(AssetRegistry, State, Record);
    }

    ScanEntryLevels(State);

    if (Options.bStats)
    {
        int32 StatsRun = 0;
        int32 StatsSkipped = 0;

        for (const FString& AssetPath : State.RecordOrder)
        {
            FMaterialRecord& Record = State.Records[AssetPath];
            if (!Record.bNamedEntry && StatsRun >= kStatsScanCap)
            {
                ++StatsSkipped;
                continue;
            }

            UE_LOG(LogUAssetWorkbenchAuditor, Display, TEXT("AuditMaterial: compiling representative shaders for %s"), *AssetPath);
            GatherTier2(Record);
            ++StatsRun;
        }

        if (StatsSkipped > 0)
        {
            UE_LOG(LogUAssetWorkbenchAuditor, Warning, TEXT("AuditMaterial: -stats capped at %d material(s), %d skipped. Name them in -assets= to force stats."), kStatsScanCap, StatsSkipped);
        }
    }

    // Tier 3 needs a live RHI, which only the in-editor queue path has.
    if (!IsRunningCommandlet())
    {
        for (const FString& AssetPath : State.RecordOrder)
        {
            GatherEditorStats(State.Records[AssetPath]);
        }
    }

    for (const FString& AssetPath : State.RecordOrder)
    {
        FMaterialRecord& Record = State.Records[AssetPath];
        RunUsageRules(Options, State, Record);
        RunNaniteRules(Options, State, Record);
        RunGraphRules(Options, State, Record);
    }

    int32 Errors = 0;
    int32 Warnings = 0;
    int32 Infos = 0;
    const bool bWritten = WriteReport(Options, State, Errors, Warnings, Infos);

    for (const FString& AssetPath : State.RecordOrder)
    {
        State.Records[AssetPath].Material->RemoveFromRoot();
    }

    if (!bWritten)
    {
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    for (const FFinding& Finding : State.Findings)
    {
        if (Finding.Severity == kSeverityError || Finding.Severity == kSeverityWarning)
        {
            UE_LOG(LogUAssetWorkbenchAuditor, Warning, TEXT("[%s] %s %s %s: %s -> %s (%s)"), *Finding.Severity, *Finding.Rule, *Finding.Asset, *Finding.Property, *Finding.Current, *Finding.Expected, *Finding.Context);
        }
    }

    UE_LOG(LogUAssetWorkbenchAuditor, Display, TEXT("AuditMaterial: complete. scanned=%d errors=%d warnings=%d infos=%d"), State.RecordOrder.Num(), Errors, Warnings, Infos);

    const bool bIssues = Errors > 0 || Warnings > 0;
    return ToExitCode(bIssues ? EUAssetWorkbenchExitType::IssuesFound : EUAssetWorkbenchExitType::Success);
}
