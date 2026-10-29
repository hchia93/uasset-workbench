#include "Audit/AuditNiagaraCommandlet.h"

// Editor-only by design: drives the editor Asset Registry and reads versioned emitter data. Trap any Runtime-type drift early.
static_assert(WITH_EDITOR, "UAssetWorkbench commandlets are editor-only, keep the uplugin Module Type=Editor.");

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraSystem.h"

#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

namespace
{
    constexpr float kMaxWarmupSeconds = 2.0f;
    constexpr int32 kMaxEnabledEmitters = 8;

    // A level or BP entry reaches its systems well within this.
    constexpr int32 kDependencyWalkDepth = 6;

    const TCHAR* kAllRules[] =
    {
        TEXT("FX1"), TEXT("FX2"), TEXT("FX3"), TEXT("FX4"), TEXT("FX5")
    };

    const TCHAR* kSeverityError = TEXT("Error");
    const TCHAR* kSeverityWarning = TEXT("Warning");

    // Where a finding's fix lands in an EditNiagaraSystem target.
    enum class ESpecSection : uint8
    {
        None,
        System,
        Emitter
    };

    struct FSpecWrite
    {
        FString Key;
        FString Value;
    };

    struct FFinding
    {
        FString Asset;
        FString Rule;
        FString Severity;
        FString Emitter;
        FString Property;
        FString Current;
        FString Expected;
        FString Context;

        ESpecSection Section = ESpecSection::None;
        TArray<FSpecWrite> Writes;
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

    bool IsSystemAsset(const FAssetData& Asset)
    {
        const UClass* Class = Asset.GetClass();
        return Class && Class->IsChildOf(UNiagaraSystem::StaticClass());
    }

    FString SimTargetName(ENiagaraSimTarget Target)
    {
        return StaticEnum<ENiagaraSimTarget>()->GetNameStringByValue(static_cast<int64>(Target));
    }

    FString BoundsModeName(ENiagaraEmitterCalculateBoundMode Mode)
    {
        return StaticEnum<ENiagaraEmitterCalculateBoundMode>()->GetNameStringByValue(static_cast<int64>(Mode));
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
            OutOptions.ReportPath = FPaths::Combine(FPaths::ProjectDir(), TEXT("Intermediate"), TEXT("AuditNiagara"), TEXT("report.json"));
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
                UE_LOG(LogUAssetWorkbenchAuditor, Error, TEXT("Unknown rule %s. Accepted: FX1 FX2 FX3 FX4 FX5"), *Rule);
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
            Filter.ClassPaths.Add(UNiagaraSystem::StaticClass()->GetClassPathName());
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

                bool bIsSystem = false;
                for (const FAssetData& Asset : Assets)
                {
                    if (IsSystemAsset(Asset))
                    {
                        bIsSystem = true;
                        OutAssets.Add(Asset);
                    }
                }

                if (bIsSystem)
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

    FFinding MakeFinding(const FString& Asset, const TCHAR* Rule, const TCHAR* Severity, const FString& Emitter, const FString& Property, const FString& Current, const FString& Expected, const FString& Context)
    {
        FFinding Finding;
        Finding.Asset = Asset;
        Finding.Rule = Rule;
        Finding.Severity = Severity;
        Finding.Emitter = Emitter;
        Finding.Property = Property;
        Finding.Current = Current;
        Finding.Expected = Expected;
        Finding.Context = Context;
        return Finding;
    }

    void RunEmitterRules(const FAuditOptions& Options, const FString& AssetPath, const UNiagaraSystem* System, const FNiagaraEmitterHandle& Handle, TArray<FFinding>& Findings)
    {
        const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
        if (!Data)
        {
            return;
        }

        const FString Emitter = Handle.GetName().ToString();
        const bool bDynamicBounds = Data->CalculateBoundsMode == ENiagaraEmitterCalculateBoundMode::Dynamic;

        const ENiagaraSimTarget OtherTarget = Data->SimTarget == ENiagaraSimTarget::CPUSim ? ENiagaraSimTarget::GPUComputeSim : ENiagaraSimTarget::CPUSim;
        TArray<FString> Unsupported;
        bool bOtherTargetFitsAll = true;
        for (const UNiagaraRendererProperties* Renderer : Data->GetRenderers())
        {
            if (!Renderer || !Renderer->GetIsEnabled())
            {
                continue;
            }

            if (!Renderer->IsSimTargetSupported(Data->SimTarget))
            {
                Unsupported.Add(Renderer->GetClass()->GetName());
            }

            if (!Renderer->IsSimTargetSupported(OtherTarget))
            {
                bOtherTargetFitsAll = false;
            }
        }

        const bool bReportsSimTarget = IsRuleActive(Options, TEXT("FX5")) && !Unsupported.IsEmpty();
        const bool bMovesToCPU = bReportsSimTarget && bOtherTargetFitsAll && OtherTarget == ENiagaraSimTarget::CPUSim;

        // Same test the engine's own validation rule runs. System fixed bounds cover every emitter.
        // An emitter FX5 moves back to CPU keeps dynamic bounds legally, so FX1 stays quiet for it.
        const bool bGPUDynamicBounds = Data->SimTarget == ENiagaraSimTarget::GPUComputeSim && bDynamicBounds && !System->bFixedBounds;
        if (IsRuleActive(Options, TEXT("FX1")) && bGPUDynamicBounds && !bMovesToCPU)
        {
            FFinding Finding = MakeFinding(AssetPath, TEXT("FX1"), kSeverityError, Emitter, TEXT("CalculateBoundsMode"), BoundsModeName(Data->CalculateBoundsMode), BoundsModeName(ENiagaraEmitterCalculateBoundMode::Fixed), FString::Printf(TEXT("GPU emitter, FixedBounds %s"), *Data->FixedBounds.ToString()));
            Finding.Section = ESpecSection::Emitter;
            Finding.Writes.Add({ TEXT("CalculateBoundsMode"), BoundsModeName(ENiagaraEmitterCalculateBoundMode::Fixed) });
            Findings.Add(MoveTemp(Finding));
        }

        if (!bReportsSimTarget)
        {
            return;
        }

        const FString Context = FString::Printf(TEXT("renderers without %s support: %s"), *SimTargetName(Data->SimTarget), *FString::Join(Unsupported, TEXT(", ")));
        FFinding Finding = MakeFinding(AssetPath, TEXT("FX5"), kSeverityError, Emitter, TEXT("SimTarget"), SimTargetName(Data->SimTarget), bOtherTargetFitsAll ? SimTargetName(OtherTarget) : FString(), Context);
        if (bOtherTargetFitsAll)
        {
            Finding.Section = ESpecSection::Emitter;
            Finding.Writes.Add({ TEXT("SimTarget"), SimTargetName(OtherTarget) });

            // Moving onto GPU with dynamic bounds would trade this finding for FX1. Fix both in one write.
            const bool bLandsOnGPUDynamic = OtherTarget == ENiagaraSimTarget::GPUComputeSim && bDynamicBounds && !System->bFixedBounds;
            if (bLandsOnGPUDynamic)
            {
                Finding.Writes.Add({ TEXT("CalculateBoundsMode"), BoundsModeName(ENiagaraEmitterCalculateBoundMode::Fixed) });
            }
        }
        Findings.Add(MoveTemp(Finding));
    }

    void RunSystemRules(const FAuditOptions& Options, const FString& AssetPath, UNiagaraSystem* System, TArray<FFinding>& Findings)
    {
        if (IsRuleActive(Options, TEXT("FX2")) && !System->GetEffectType())
        {
            Findings.Add(MakeFinding(AssetPath, TEXT("FX2"), kSeverityWarning, FString(), TEXT("EffectType"), TEXT("None"), FString(), TEXT("no effect type, the system runs without scalability or culling")));
        }

        const float Warmup = System->GetWarmupTime();
        if (IsRuleActive(Options, TEXT("FX3")) && Warmup > kMaxWarmupSeconds)
        {
            const FString Expected = FString::SanitizeFloat(kMaxWarmupSeconds);
            FFinding Finding = MakeFinding(AssetPath, TEXT("FX3"), kSeverityWarning, FString(), TEXT("WarmupTime"), FString::SanitizeFloat(Warmup), Expected, FString::Printf(TEXT("warmup simulates on every activation, budget %ss"), *Expected));
            Finding.Section = ESpecSection::System;
            Finding.Writes.Add({ TEXT("WarmupTime"), Expected });
            Findings.Add(MoveTemp(Finding));
        }

        int32 EnabledEmitters = 0;
        for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
        {
            if (!Handle.GetIsEnabled())
            {
                continue;
            }

            ++EnabledEmitters;
            RunEmitterRules(Options, AssetPath, System, Handle, Findings);
        }

        if (IsRuleActive(Options, TEXT("FX4")) && EnabledEmitters > kMaxEnabledEmitters)
        {
            Findings.Add(MakeFinding(AssetPath, TEXT("FX4"), kSeverityWarning, FString(), TEXT("Emitters"), FString::FromInt(EnabledEmitters), FString(), FString::Printf(TEXT("more than %d enabled emitters"), kMaxEnabledEmitters)));
        }
    }

    // First writer of a key wins, the same rule the other audits apply when two findings name one property.
    void AddSpecWrites(const TSharedRef<FJsonObject>& Target, const FFinding& Finding)
    {
        TSharedPtr<FJsonObject> Properties;
        if (Finding.Section == ESpecSection::System)
        {
            const TSharedPtr<FJsonObject>* SystemField = nullptr;
            if (!Target->TryGetObjectField(TEXT("System"), SystemField))
            {
                TSharedRef<FJsonObject> SystemSection = MakeShared<FJsonObject>();
                SystemSection->SetObjectField(TEXT("Properties"), MakeShared<FJsonObject>());
                Target->SetObjectField(TEXT("System"), SystemSection);
                Target->TryGetObjectField(TEXT("System"), SystemField);
            }
            Properties = (*SystemField)->GetObjectField(TEXT("Properties"));
        }
        else
        {
            const TArray<TSharedPtr<FJsonValue>>* Ops = nullptr;
            if (!Target->TryGetArrayField(TEXT("Emitters"), Ops))
            {
                Target->SetArrayField(TEXT("Emitters"), {});
                Target->TryGetArrayField(TEXT("Emitters"), Ops);
            }

            for (const TSharedPtr<FJsonValue>& Op : *Ops)
            {
                if (Op->AsObject()->GetStringField(TEXT("Emitter")) == Finding.Emitter)
                {
                    Properties = Op->AsObject()->GetObjectField(TEXT("Properties"));
                    break;
                }
            }

            if (!Properties.IsValid())
            {
                Properties = MakeShared<FJsonObject>();
                TSharedRef<FJsonObject> Op = MakeShared<FJsonObject>();
                Op->SetStringField(TEXT("Op"), TEXT("Modify"));
                Op->SetStringField(TEXT("Emitter"), Finding.Emitter);
                Op->SetObjectField(TEXT("Properties"), Properties);

                TArray<TSharedPtr<FJsonValue>> NewOps = *Ops;
                NewOps.Add(MakeShared<FJsonValueObject>(Op));
                Target->SetArrayField(TEXT("Emitters"), NewOps);
            }
        }

        for (const FSpecWrite& Write : Finding.Writes)
        {
            if (!Properties->HasField(Write.Key))
            {
                Properties->SetStringField(Write.Key, Write.Value);
            }
        }
    }

    bool WriteReport(const FAuditOptions& Options, const TArray<FFinding>& Findings, int32 Scanned, int32& OutErrors, int32& OutWarnings)
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
            else
            {
                ++OutWarnings;
            }

            TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("Asset"), Finding.Asset);
            Item->SetStringField(TEXT("Rule"), Finding.Rule);
            Item->SetStringField(TEXT("Severity"), Finding.Severity);
            Item->SetStringField(TEXT("Emitter"), Finding.Emitter);
            Item->SetStringField(TEXT("Property"), Finding.Property);
            Item->SetStringField(TEXT("Current"), Finding.Current);
            Item->SetStringField(TEXT("Expected"), Finding.Expected);
            Item->SetStringField(TEXT("Context"), Finding.Context);
            FindingsJson.Add(MakeShared<FJsonValueObject>(Item));

            if (Finding.Section == ESpecSection::None)
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

            AddSpecWrites(*Target, Finding);
        }

        TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
        Summary->SetNumberField(TEXT("Error"), OutErrors);
        Summary->SetNumberField(TEXT("Warning"), OutWarnings);
        Summary->SetNumberField(TEXT("Info"), 0);

        TArray<TSharedPtr<FJsonValue>> TargetsJson;
        for (const FString& AssetPath : SpecOrder)
        {
            TargetsJson.Add(MakeShared<FJsonValueObject>(SpecTargets[AssetPath]));
        }

        TSharedRef<FJsonObject> Spec = MakeShared<FJsonObject>();
        Spec->SetArrayField(TEXT("Targets"), TargetsJson);

        TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetStringField(TEXT("RunName"), TEXT("AuditNiagara"));
        Root->SetNumberField(TEXT("Scanned"), Scanned);
        Root->SetArrayField(TEXT("Findings"), FindingsJson);
        Root->SetObjectField(TEXT("Summary"), Summary);
        Root->SetObjectField(TEXT("Spec"), Spec);

        if (!UAssetWorkbench::SaveJsonToFile(Root, Options.ReportPath))
        {
            UE_LOG(LogUAssetWorkbenchAuditor, Error, TEXT("Failed to write report: %s"), *Options.ReportPath);
            return false;
        }

        UE_LOG(LogUAssetWorkbenchAuditor, Display, TEXT("AuditNiagara: report written: %s"), *Options.ReportPath);
        return true;
    }
}

UAuditNiagaraCommandlet::UAuditNiagaraCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UAuditNiagaraCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchAuditor, Display, TEXT("UAssetWorkbench v%s - AuditNiagara"), UASSET_WORKBENCH_VERSION_STRING);

    FAuditOptions Options;
    if (!ParseOptions(Params, Options))
    {
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    UE_LOG(LogUAssetWorkbenchAuditor, Display, TEXT("AuditNiagara: scanning asset registry..."));
    AssetRegistry.SearchAllAssets(true);

    TArray<FAssetData> Candidates;
    if (!CollectCandidateAssets(Options, AssetRegistry, Candidates))
    {
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    if (Candidates.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchAuditor, Warning, TEXT("AuditNiagara: no systems in scope (assets=%d scandir=%s)"), Options.EntryAssets.Num(), *Options.ScanDir);
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

        UNiagaraSystem* System = Cast<UNiagaraSystem>(Candidate.GetAsset());
        if (!System)
        {
            UE_LOG(LogUAssetWorkbenchAuditor, Warning, TEXT("AuditNiagara: failed to load %s"), *AssetPath);
            continue;
        }

        ++Scanned;
        RunSystemRules(Options, AssetPath, System, Findings);
    }

    int32 Errors = 0;
    int32 Warnings = 0;
    if (!WriteReport(Options, Findings, Scanned, Errors, Warnings))
    {
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    for (const FFinding& Finding : Findings)
    {
        UE_LOG(LogUAssetWorkbenchAuditor, Warning, TEXT("[%s] %s %s %s %s: %s -> %s (%s)"), *Finding.Severity, *Finding.Rule, *Finding.Asset, *Finding.Emitter, *Finding.Property, *Finding.Current, *Finding.Expected, *Finding.Context);
    }

    UE_LOG(LogUAssetWorkbenchAuditor, Display, TEXT("AuditNiagara: complete. scanned=%d errors=%d warnings=%d"), Scanned, Errors, Warnings);

    const bool bIssues = Errors > 0 || Warnings > 0;
    return ToExitCode(bIssues ? EUAssetWorkbenchExitType::IssuesFound : EUAssetWorkbenchExitType::Success);
}
