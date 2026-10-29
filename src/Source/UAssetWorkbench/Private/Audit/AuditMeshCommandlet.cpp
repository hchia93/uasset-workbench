#include "Audit/AuditMeshCommandlet.h"

// Editor-only by design: drives the editor Asset Registry and reads source models. Trap any Runtime-type drift early.
static_assert(WITH_EDITOR, "UAssetWorkbench commandlets are editor-only, keep the uplugin Module Type=Editor.");

#include "AssetCompilingManager.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PhysicsEngine/BodySetup.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"

#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

namespace
{
    constexpr int32 kLODRequiredTriangles = 2000;
    constexpr int32 kNaniteSuggestedTriangles = 50000;
    constexpr int32 kComplexAsSimpleMaxTriangles = 1000;
    constexpr int32 kSuggestedLODCount = 4;

    // Mesh to material is the only hop a mesh audit needs, a level or BP entry reaches its meshes well within this.
    constexpr int32 kDependencyWalkDepth = 6;

    const TCHAR* kAllRules[] =
    {
        TEXT("M1"), TEXT("M2"), TEXT("M3"), TEXT("M4"), TEXT("M5")
    };

    const TCHAR* kSeverityError = TEXT("Error");
    const TCHAR* kSeverityWarning = TEXT("Warning");

    struct FMaterialSlot
    {
        int32 Index = 0;
        FName SlotName;
        UMaterialInterface* Material = nullptr;
    };

    struct FMeshRecord
    {
        FString AssetPath;
        bool bStatic = false;
        bool bNanite = false;
        int32 LODCount = 0;
        int32 LOD0Triangles = 0;
        bool bHasBodySetup = false;
        ECollisionTraceFlag TraceFlag = CTF_UseDefault;
        int32 SimpleShapes = 0;
        TArray<FMaterialSlot> Slots;
    };

    struct FFinding
    {
        FString Asset;
        FString Rule;
        FString Severity;
        FString Property;
        FString Current;
        FString Expected;
        FString Context;

        // The slice of an EditMeshAsset target this finding fixes. Null when the fix is not deterministic.
        TSharedPtr<FJsonObject> SpecPatch;
    };

    struct FAuditOptions
    {
        TArray<FString> EntryAssets;
        FString ScanDir;
        FString ReportPath;
        TSet<FString> Rules;
    };

    bool IsRuleActive(const FAuditOptions& Options, const TCHAR* Rule)
    {
        return Options.Rules.IsEmpty() || Options.Rules.Contains(Rule);
    }

    FString ToPackageName(const FString& AssetPath)
    {
        FString PackageName = AssetPath;
        int32 DotIndex = INDEX_NONE;
        if (PackageName.FindChar(TEXT('.'), DotIndex))
        {
            PackageName.LeftInline(DotIndex);
        }

        return PackageName;
    }

    bool IsMeshAsset(const FAssetData& Asset)
    {
        const UClass* Class = Asset.GetClass();
        return Class && (Class->IsChildOf(UStaticMesh::StaticClass()) || Class->IsChildOf(USkeletalMesh::StaticClass()));
    }

    bool IsNaniteBlendMode(EBlendMode BlendMode)
    {
        return BlendMode == BLEND_Opaque || BlendMode == BLEND_Masked;
    }

    FString BlendModeName(EBlendMode BlendMode)
    {
        return StaticEnum<EBlendMode>()->GetNameStringByValue(static_cast<int64>(BlendMode));
    }

    FString TraceFlagName(ECollisionTraceFlag Flag)
    {
        return StaticEnum<ECollisionTraceFlag>()->GetNameStringByValue(static_cast<int64>(Flag));
    }

    TSharedRef<FJsonObject> MakePatch(const TCHAR* Section, const TCHAR* Field, const FString& Value)
    {
        TSharedRef<FJsonObject> Inner = MakeShared<FJsonObject>();
        Inner->SetStringField(Field, Value);

        TSharedRef<FJsonObject> Patch = MakeShared<FJsonObject>();
        Patch->SetObjectField(Section, Inner);
        return Patch;
    }

    // First writer of a field wins, the same rule AuditTexture applies when two findings name one property.
    void MergePatch(const TSharedRef<FJsonObject>& Target, const TSharedPtr<FJsonObject>& Patch)
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Patch->Values)
        {
            const TSharedPtr<FJsonObject>* PatchSection = nullptr;
            const TSharedPtr<FJsonObject>* TargetSection = nullptr;
            if (Pair.Value->TryGetObject(PatchSection) && Target->TryGetObjectField(Pair.Key, TargetSection))
            {
                for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : (*PatchSection)->Values)
                {
                    if (!(*TargetSection)->HasField(Field.Key))
                    {
                        (*TargetSection)->SetField(Field.Key, Field.Value);
                    }
                }
                continue;
            }

            if (!Target->HasField(Pair.Key))
            {
                Target->SetField(Pair.Key, Pair.Value);
            }
        }
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
            OutOptions.ReportPath = FPaths::Combine(FPaths::ProjectDir(), TEXT("Intermediate"), TEXT("AuditMesh"), TEXT("report.json"));
        }
        OutOptions.ReportPath.TrimQuotesInline();

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
                UE_LOG(LogUAssetWorkbenchAuditor, Error, TEXT("Unknown rule %s. Accepted: M1 M2 M3 M4 M5"), *Rule);
                return false;
            }

            OutOptions.Rules.Add(Upper);
        }

        return true;
    }

    // Entry assets are walked forward through the dependency graph, a scan dir is a plain registry filter.
    bool CollectCandidateAssets(const FAuditOptions& Options, IAssetRegistry& Registry, TArray<FAssetData>& OutAssets)
    {
        if (Options.EntryAssets.IsEmpty())
        {
            FARFilter Filter;
            Filter.ClassPaths.Add(UStaticMesh::StaticClass()->GetClassPathName());
            Filter.ClassPaths.Add(USkeletalMesh::StaticClass()->GetClassPathName());
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
        }

        for (int32 Depth = 0; Depth < kDependencyWalkDepth && Frontier.Num() > 0; ++Depth)
        {
            TArray<FName> NextFrontier;

            for (FName Current : Frontier)
            {
                TArray<FAssetData> Assets;
                Registry.GetAssetsByPackageName(Current, Assets, true);

                bool bIsMesh = false;
                for (const FAssetData& Asset : Assets)
                {
                    if (IsMeshAsset(Asset))
                    {
                        bIsMesh = true;
                        OutAssets.Add(Asset);
                    }
                }

                if (bIsMesh)
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

    bool ReadStaticMesh(UStaticMesh* Mesh, FMeshRecord& OutRecord)
    {
        OutRecord.bStatic = true;
        OutRecord.bNanite = Mesh->IsNaniteEnabled();
        OutRecord.LODCount = Mesh->GetNumSourceModels();
        OutRecord.LOD0Triangles = Mesh->GetNumLODs() > 0 ? Mesh->GetNumTriangles(0) : 0;

        if (const UBodySetup* BodySetup = Mesh->GetBodySetup())
        {
            OutRecord.bHasBodySetup = true;
            OutRecord.TraceFlag = BodySetup->CollisionTraceFlag;
            OutRecord.SimpleShapes = BodySetup->AggGeom.GetElementCount();
        }

        const TArray<FStaticMaterial>& Materials = Mesh->GetStaticMaterials();
        for (int32 Index = 0; Index < Materials.Num(); ++Index)
        {
            OutRecord.Slots.Add({ Index, Materials[Index].MaterialSlotName, Materials[Index].MaterialInterface });
        }

        return true;
    }

    bool ReadSkeletalMesh(USkeletalMesh* Mesh, FMeshRecord& OutRecord)
    {
        OutRecord.bStatic = false;
        OutRecord.bNanite = Mesh->IsNaniteEnabled();
        OutRecord.LODCount = Mesh->GetLODNum();

        const FSkeletalMeshModel* ImportedModel = Mesh->GetImportedModel();
        if (ImportedModel && ImportedModel->LODModels.Num() > 0)
        {
            for (const FSkelMeshSection& Section : ImportedModel->LODModels[0].Sections)
            {
                OutRecord.LOD0Triangles += Section.NumTriangles;
            }
        }

        const TArray<FSkeletalMaterial>& Materials = Mesh->GetMaterials();
        for (int32 Index = 0; Index < Materials.Num(); ++Index)
        {
            OutRecord.Slots.Add({ Index, Materials[Index].MaterialSlotName, Materials[Index].MaterialInterface });
        }

        return true;
    }

    void AddFinding(TArray<FFinding>& Findings, const FMeshRecord& Record, const TCHAR* Rule, const TCHAR* Severity, const FString& Property, const FString& Current, const FString& Expected, const FString& Context, TSharedPtr<FJsonObject> SpecPatch = nullptr)
    {
        FFinding Finding;
        Finding.Asset = Record.AssetPath;
        Finding.Rule = Rule;
        Finding.Severity = Severity;
        Finding.Property = Property;
        Finding.Current = Current;
        Finding.Expected = Expected;
        Finding.Context = Context;
        Finding.SpecPatch = SpecPatch;
        Findings.Add(MoveTemp(Finding));
    }

    void RunMeshRules(const FAuditOptions& Options, const FMeshRecord& Record, TArray<FFinding>& Findings)
    {
        TArray<FString> NonNaniteSlots;
        bool bHasEmptySlot = false;
        for (const FMaterialSlot& Slot : Record.Slots)
        {
            if (!Slot.Material)
            {
                bHasEmptySlot = true;
                continue;
            }

            const EBlendMode BlendMode = Slot.Material->GetBlendMode();
            if (!IsNaniteBlendMode(BlendMode))
            {
                NonNaniteSlots.Add(FString::Printf(TEXT("slot %d %s %s %s"), Slot.Index, *Slot.SlotName.ToString(), *Slot.Material->GetPathName(), *BlendModeName(BlendMode)));
            }
        }

        const FString Triangles = FString::Printf(TEXT("LOD0 %d triangles"), Record.LOD0Triangles);

        // A mesh that M3 would move onto Nanite needs no LOD chain, so M1 stops where M3 starts.
        const bool bNeedsLODs = !Record.bNanite && Record.LODCount <= 1 && Record.LOD0Triangles >= kLODRequiredTriangles;
        const bool bNaniteCandidate = Record.bStatic && Record.LOD0Triangles >= kNaniteSuggestedTriangles;
        if (IsRuleActive(Options, TEXT("M1")) && bNeedsLODs && !bNaniteCandidate)
        {
            if (Record.bStatic)
            {
                TSharedRef<FJsonObject> Patch = MakeShared<FJsonObject>();
                Patch->SetNumberField(TEXT("NumLODs"), kSuggestedLODCount);
                AddFinding(Findings, Record, TEXT("M1"), kSeverityWarning, TEXT("NumLODs"), FString::FromInt(Record.LODCount), FString::FromInt(kSuggestedLODCount), Triangles, Patch);
            }
            else
            {
                AddFinding(Findings, Record, TEXT("M1"), kSeverityWarning, TEXT("NumLODs"), FString::FromInt(Record.LODCount), FString(), Triangles + TEXT(", skeletal LODs are not generated by EditMeshAsset"));
            }
        }

        if (IsRuleActive(Options, TEXT("M2")) && Record.bNanite && NonNaniteSlots.Num() > 0)
        {
            AddFinding(Findings, Record, TEXT("M2"), kSeverityError, TEXT("NaniteSettings.bEnabled"), TEXT("True"), TEXT("False"), FString::Join(NonNaniteSlots, TEXT("; ")), MakePatch(TEXT("Properties"), TEXT("NaniteSettings.bEnabled"), TEXT("False")));
        }

        // Every slot must render under Nanite, an empty one falls to the default material and M5 reports it.
        const bool bAllSlotsNanite = Record.Slots.Num() > 0 && NonNaniteSlots.IsEmpty() && !bHasEmptySlot;
        if (IsRuleActive(Options, TEXT("M3")) && bNaniteCandidate && !Record.bNanite && bAllSlotsNanite)
        {
            AddFinding(Findings, Record, TEXT("M3"), kSeverityWarning, TEXT("NaniteSettings.bEnabled"), TEXT("False"), TEXT("True"), Triangles, MakePatch(TEXT("Properties"), TEXT("NaniteSettings.bEnabled"), TEXT("True")));
        }

        const bool bComplexAsSimple = Record.bStatic && Record.bHasBodySetup && Record.TraceFlag == CTF_UseComplexAsSimple;
        if (IsRuleActive(Options, TEXT("M4")) && bComplexAsSimple && Record.LOD0Triangles >= kComplexAsSimpleMaxTriangles)
        {
            const FString DefaultFlag = TraceFlagName(CTF_UseDefault);
            TSharedRef<FJsonObject> Patch = MakePatch(TEXT("Collision"), TEXT("CollisionTraceFlag"), DefaultFlag);
            FString Expected = DefaultFlag;
            if (Record.SimpleShapes == 0)
            {
                Patch->GetObjectField(TEXT("Collision"))->SetStringField(TEXT("SimpleCollision"), TEXT("Box"));
                Expected += TEXT(" + SimpleCollision Box");
            }

            const FString Context = FString::Printf(TEXT("%s, %d simple shapes"), *Triangles, Record.SimpleShapes);
            AddFinding(Findings, Record, TEXT("M4"), kSeverityWarning, TEXT("Collision.CollisionTraceFlag"), TraceFlagName(Record.TraceFlag), Expected, Context, Patch);
        }

        if (IsRuleActive(Options, TEXT("M5")))
        {
            for (const FMaterialSlot& Slot : Record.Slots)
            {
                if (!Slot.Material)
                {
                    const FString Property = FString::Printf(TEXT("MaterialSlots[%d]"), Slot.Index);
                    AddFinding(Findings, Record, TEXT("M5"), kSeverityError, Property, TEXT("None"), FString(), FString::Printf(TEXT("slot %s"), *Slot.SlotName.ToString()));
                }
            }
        }
    }

    bool WriteReport(const FAuditOptions& Options, const TArray<FFinding>& Findings, int32 Scanned, int32& OutErrors, int32& OutWarnings, int32& OutInfos)
    {
        TArray<TSharedPtr<FJsonValue>> FindingsJson;
        TArray<FString> SpecOrder;
        TMap<FString, TSharedRef<FJsonObject>> SpecTargets;

        for (const FFinding& Finding : Findings)
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

            TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("Asset"), Finding.Asset);
            Item->SetStringField(TEXT("Rule"), Finding.Rule);
            Item->SetStringField(TEXT("Severity"), Finding.Severity);
            Item->SetStringField(TEXT("Property"), Finding.Property);
            Item->SetStringField(TEXT("Current"), Finding.Current);
            Item->SetStringField(TEXT("Expected"), Finding.Expected);
            Item->SetStringField(TEXT("Context"), Finding.Context);
            FindingsJson.Add(MakeShared<FJsonValueObject>(Item));

            if (!Finding.SpecPatch.IsValid())
            {
                continue;
            }

            TSharedRef<FJsonObject>* Target = SpecTargets.Find(Finding.Asset);
            if (!Target)
            {
                TSharedRef<FJsonObject> NewTarget = MakeShared<FJsonObject>();
                NewTarget->SetStringField(TEXT("AssetPath"), Finding.Asset);
                Target = &SpecTargets.Add(Finding.Asset, NewTarget);
                SpecOrder.Add(Finding.Asset);
            }

            MergePatch(*Target, Finding.SpecPatch);
        }

        TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
        Summary->SetNumberField(TEXT("Error"), OutErrors);
        Summary->SetNumberField(TEXT("Warning"), OutWarnings);
        Summary->SetNumberField(TEXT("Info"), OutInfos);

        TArray<TSharedPtr<FJsonValue>> TargetsJson;
        for (const FString& AssetPath : SpecOrder)
        {
            TargetsJson.Add(MakeShared<FJsonValueObject>(SpecTargets[AssetPath]));
        }

        TSharedRef<FJsonObject> Spec = MakeShared<FJsonObject>();
        Spec->SetArrayField(TEXT("Targets"), TargetsJson);

        TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetStringField(TEXT("RunName"), TEXT("AuditMesh"));
        Root->SetNumberField(TEXT("Scanned"), Scanned);
        Root->SetArrayField(TEXT("Findings"), FindingsJson);
        Root->SetObjectField(TEXT("Summary"), Summary);
        Root->SetObjectField(TEXT("Spec"), Spec);

        if (!UAssetWorkbench::SaveJsonToFile(Root, Options.ReportPath))
        {
            UE_LOG(LogUAssetWorkbenchAuditor, Error, TEXT("Failed to write report: %s"), *Options.ReportPath);
            return false;
        }

        UE_LOG(LogUAssetWorkbenchAuditor, Display, TEXT("AuditMesh: report written: %s"), *Options.ReportPath);
        return true;
    }
}

UAuditMeshCommandlet::UAuditMeshCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UAuditMeshCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchAuditor, Display, TEXT("UAssetWorkbench v%s - AuditMesh"), UASSET_WORKBENCH_VERSION_STRING);

    FAuditOptions Options;
    if (!ParseOptions(Params, Options))
    {
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    UE_LOG(LogUAssetWorkbenchAuditor, Display, TEXT("AuditMesh: scanning asset registry..."));
    AssetRegistry.SearchAllAssets(true);

    TArray<FAssetData> Candidates;
    if (!CollectCandidateAssets(Options, AssetRegistry, Candidates))
    {
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    if (Candidates.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchAuditor, Warning, TEXT("AuditMesh: no meshes in scope (assets=%d scandir=%s)"), Options.EntryAssets.Num(), *Options.ScanDir);
    }

    TArray<FFinding> Findings;
    TSet<FString> Seen;
    int32 Scanned = 0;
    for (const FAssetData& Candidate : Candidates)
    {
        const FString AssetPath = Candidate.GetObjectPathString();
        if (Seen.Contains(AssetPath))
        {
            continue;
        }
        Seen.Add(AssetPath);

        UObject* Asset = Candidate.GetAsset();

        // Triangle counts come from render data and imported LOD models, both build async on load.
        FAssetCompilingManager::Get().FinishAllCompilation();

        FMeshRecord Record;
        Record.AssetPath = AssetPath;
        if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset))
        {
            ReadStaticMesh(StaticMesh, Record);
        }
        else if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Asset))
        {
            ReadSkeletalMesh(SkeletalMesh, Record);
        }
        else
        {
            UE_LOG(LogUAssetWorkbenchAuditor, Warning, TEXT("AuditMesh: failed to load %s"), *AssetPath);
            continue;
        }

        ++Scanned;
        RunMeshRules(Options, Record, Findings);
    }

    int32 Errors = 0;
    int32 Warnings = 0;
    int32 Infos = 0;
    if (!WriteReport(Options, Findings, Scanned, Errors, Warnings, Infos))
    {
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    for (const FFinding& Finding : Findings)
    {
        if (Finding.Severity == kSeverityError || Finding.Severity == kSeverityWarning)
        {
            UE_LOG(LogUAssetWorkbenchAuditor, Warning, TEXT("[%s] %s %s %s: %s -> %s (%s)"), *Finding.Severity, *Finding.Rule, *Finding.Asset, *Finding.Property, *Finding.Current, *Finding.Expected, *Finding.Context);
        }
    }

    UE_LOG(LogUAssetWorkbenchAuditor, Display, TEXT("AuditMesh: complete. scanned=%d errors=%d warnings=%d infos=%d"), Scanned, Errors, Warnings, Infos);

    const bool bIssues = Errors > 0 || Warnings > 0;
    return ToExitCode(bIssues ? EUAssetWorkbenchExitType::IssuesFound : EUAssetWorkbenchExitType::Success);
}
