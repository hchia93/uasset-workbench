#include "Audit/AuditSoundCommandlet.h"

// Editor-only by design: drives the editor Asset Registry and reads the editor-only SoundCue node list. Trap any Runtime-type drift early.
static_assert(WITH_EDITOR, "UAssetWorkbench commandlets are editor-only, keep the uplugin Module Type=Editor.");

#include "Animation/AnimSequenceBase.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNodeAttenuation.h"
#include "Sound/SoundWave.h"

#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

namespace
{
    // Sound to BP to the level that places it is the longest chain worth classifying.
    constexpr int32 kReferencerWalkDepth = 3;
    constexpr int32 kDependencyWalkDepth = 6;

    const TCHAR* kAllRules[] =
    {
        TEXT("S1"), TEXT("S2"), TEXT("S3")
    };

    const TCHAR* kSeverityWarning = TEXT("Warning");

    // Plugin and editor classes this module does not link, matched by class path.
    const TCHAR* kWidgetBlueprintClass = TEXT("/Script/UMGEditor.WidgetBlueprint");
    const TCHAR* kWorldClass = TEXT("/Script/Engine.World");
    const TCHAR* kNiagaraSystemClass = TEXT("/Script/Niagara.NiagaraSystem");
    const TCHAR* kMetaSoundPackagePrefix = TEXT("/Script/MetasoundEngine.");

    enum class ESoundUsage : uint8
    {
        Unknown,
        Interface,
        Spatial
    };

    struct FFinding
    {
        FString Asset;
        FString Rule;
        FString Severity;
        FString Property;
        FString Current;
        FString Context;
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

    bool IsSoundAsset(const FAssetData& Asset)
    {
        const UClass* Class = Asset.GetClass();
        return Class && Class->IsChildOf(USoundBase::StaticClass());
    }

    // A SoundCue or MetaSound that wraps the wave owns its playback settings.
    bool IsOuterSound(const FAssetData& Asset)
    {
        const FString ClassPath = Asset.AssetClassPath.ToString();
        const UClass* Class = Asset.GetClass();
        return ClassPath.StartsWith(kMetaSoundPackagePrefix) || (Class && Class->IsChildOf(USoundCue::StaticClass()));
    }

    ESoundUsage ClassifyReferencer(const FAssetData& Asset)
    {
        const FString ClassPath = Asset.AssetClassPath.ToString();
        if (ClassPath == kWidgetBlueprintClass)
        {
            return ESoundUsage::Interface;
        }

        if (ClassPath == kWorldClass || ClassPath == kNiagaraSystemClass)
        {
            return ESoundUsage::Spatial;
        }

        const UClass* Class = Asset.GetClass();
        if (Class && (Class->IsChildOf(UBlueprint::StaticClass()) || Class->IsChildOf(UAnimSequenceBase::StaticClass())))
        {
            return ESoundUsage::Spatial;
        }

        return ESoundUsage::Unknown;
    }

    // Spatial wins over interface: one placed or attached use is enough to need attenuation.
    ESoundUsage ClassifyUsage(IAssetRegistry& Registry, FName PackageName, FString& OutEvidence)
    {
        ESoundUsage Usage = ESoundUsage::Unknown;
        TSet<FName> Visited = { PackageName };
        TArray<FName> Frontier = { PackageName };

        for (int32 Depth = 0; Depth < kReferencerWalkDepth && Frontier.Num() > 0; ++Depth)
        {
            TArray<FName> NextFrontier;
            for (FName Package : Frontier)
            {
                TArray<FName> Referencers;
                Registry.GetReferencers(Package, Referencers, UE::AssetRegistry::EDependencyCategory::Package);

                for (FName Referencer : Referencers)
                {
                    if (Visited.Contains(Referencer))
                    {
                        continue;
                    }
                    Visited.Add(Referencer);

                    TArray<FAssetData> Assets;
                    Registry.GetAssetsByPackageName(Referencer, Assets, true);

                    bool bClassified = false;
                    for (const FAssetData& Asset : Assets)
                    {
                        const ESoundUsage Found = ClassifyReferencer(Asset);
                        if (Found == ESoundUsage::Spatial)
                        {
                            OutEvidence = Asset.GetObjectPathString();
                            return ESoundUsage::Spatial;
                        }

                        if (Found == ESoundUsage::Interface)
                        {
                            Usage = ESoundUsage::Interface;
                            bClassified = true;
                        }
                    }

                    if (!bClassified)
                    {
                        NextFrontier.Add(Referencer);
                    }
                }
            }

            Frontier = MoveTemp(NextFrontier);
        }

        return Usage;
    }

    bool PlaysOnlyThroughOuterSound(IAssetRegistry& Registry, FName PackageName)
    {
        TArray<FName> Referencers;
        Registry.GetReferencers(PackageName, Referencers, UE::AssetRegistry::EDependencyCategory::Package);
        if (Referencers.IsEmpty())
        {
            return false;
        }

        for (FName Referencer : Referencers)
        {
            TArray<FAssetData> Assets;
            Registry.GetAssetsByPackageName(Referencer, Assets, true);
            const bool bOuter = Assets.ContainsByPredicate([](const FAssetData& Asset) { return IsOuterSound(Asset); });
            if (!bOuter)
            {
                return false;
            }
        }

        return true;
    }

    bool HasAttenuation(const USoundBase* Sound)
    {
        if (Sound->AttenuationSettings)
        {
            return true;
        }

        const USoundCue* Cue = Cast<USoundCue>(Sound);
        if (!Cue)
        {
            return false;
        }

        if (Cue->bOverrideAttenuation)
        {
            return true;
        }

        return Cue->AllNodes.ContainsByPredicate([](const USoundNode* Node) { return Node && Node->IsA<USoundNodeAttenuation>(); });
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
            OutOptions.ReportPath = FPaths::Combine(FPaths::ProjectDir(), TEXT("Intermediate"), TEXT("AuditSound"), TEXT("report.json"));
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
                UE_LOG(LogUAssetWorkbenchAuditor, Error, TEXT("Unknown rule %s. Accepted: S1 S2 S3"), *Rule);
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
            Filter.ClassPaths.Add(USoundBase::StaticClass()->GetClassPathName());
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

                bool bIsSound = false;
                for (const FAssetData& Asset : Assets)
                {
                    if (IsSoundAsset(Asset))
                    {
                        bIsSound = true;
                        OutAssets.Add(Asset);
                    }
                }

                if (bIsSound)
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

    void RunSoundRules(const FAuditOptions& Options, IAssetRegistry& Registry, const FString& AssetPath, FName PackageName, USoundBase* Sound, TArray<FFinding>& Findings)
    {
        const bool bHasConcurrency = Sound->bOverrideConcurrency || Sound->ConcurrencySet.Num() > 0;
        if (IsRuleActive(Options, TEXT("S1")) && !bHasConcurrency)
        {
            Findings.Add({ AssetPath, TEXT("S1"), kSeverityWarning, TEXT("ConcurrencySet"), TEXT("None"), TEXT("no concurrency, every trigger plays a new voice") });
        }

        if (IsRuleActive(Options, TEXT("S2")) && !HasAttenuation(Sound))
        {
            FString Evidence;
            if (ClassifyUsage(Registry, PackageName, Evidence) == ESoundUsage::Spatial)
            {
                Findings.Add({ AssetPath, TEXT("S2"), kSeverityWarning, TEXT("AttenuationSettings"), TEXT("None"), FString::Printf(TEXT("spatial use through %s"), *Evidence) });
            }
        }

        if (IsRuleActive(Options, TEXT("S3")) && !Sound->SoundClassObject)
        {
            Findings.Add({ AssetPath, TEXT("S3"), kSeverityWarning, TEXT("SoundClassObject"), TEXT("None"), TEXT("falls back to the project default sound class, its volume follows no mix") });
        }
    }

    bool WriteReport(const FAuditOptions& Options, const TArray<FFinding>& Findings, int32 Scanned, int32 Skipped)
    {
        TArray<TSharedPtr<FJsonValue>> FindingsJson;
        for (const FFinding& Finding : Findings)
        {
            TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("Asset"), Finding.Asset);
            Item->SetStringField(TEXT("Rule"), Finding.Rule);
            Item->SetStringField(TEXT("Severity"), Finding.Severity);
            Item->SetStringField(TEXT("Property"), Finding.Property);
            Item->SetStringField(TEXT("Current"), Finding.Current);
            Item->SetStringField(TEXT("Context"), Finding.Context);
            FindingsJson.Add(MakeShared<FJsonValueObject>(Item));
        }

        TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
        Summary->SetNumberField(TEXT("Error"), 0);
        Summary->SetNumberField(TEXT("Warning"), Findings.Num());
        Summary->SetNumberField(TEXT("Info"), 0);

        TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetStringField(TEXT("RunName"), TEXT("AuditSound"));
        Root->SetNumberField(TEXT("Scanned"), Scanned);
        Root->SetNumberField(TEXT("SkippedInnerWaves"), Skipped);
        Root->SetArrayField(TEXT("Findings"), FindingsJson);
        Root->SetObjectField(TEXT("Summary"), Summary);

        if (!UAssetWorkbench::SaveJsonToFile(Root, Options.ReportPath))
        {
            UE_LOG(LogUAssetWorkbenchAuditor, Error, TEXT("Failed to write report: %s"), *Options.ReportPath);
            return false;
        }

        UE_LOG(LogUAssetWorkbenchAuditor, Display, TEXT("AuditSound: report written: %s"), *Options.ReportPath);
        return true;
    }
}

UAuditSoundCommandlet::UAuditSoundCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UAuditSoundCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchAuditor, Display, TEXT("UAssetWorkbench v%s - AuditSound"), UASSET_WORKBENCH_VERSION_STRING);

    FAuditOptions Options;
    if (!ParseOptions(Params, Options))
    {
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    UE_LOG(LogUAssetWorkbenchAuditor, Display, TEXT("AuditSound: scanning asset registry..."));
    AssetRegistry.SearchAllAssets(true);

    TArray<FAssetData> Candidates;
    if (!CollectCandidateAssets(Options, AssetRegistry, Candidates))
    {
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    if (Candidates.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchAuditor, Warning, TEXT("AuditSound: no sounds in scope (assets=%d scandir=%s)"), Options.EntryAssets.Num(), *Options.ScanDir);
    }

    TArray<FFinding> Findings;
    TSet<FString> Seen;
    int32 Scanned = 0;
    int32 Skipped = 0;
    for (const FAssetData& Candidate : Candidates)
    {
        const FString AssetPath = Candidate.GetObjectPathString();
        if (Seen.Contains(AssetPath))
        {
            continue;
        }
        Seen.Add(AssetPath);

        // A MetaSound source derives from USoundWave but is a playable sound of its own.
        const UClass* Class = Candidate.GetClass();
        const bool bMetaSound = Candidate.AssetClassPath.ToString().StartsWith(kMetaSoundPackagePrefix);
        const bool bWave = Class && Class->IsChildOf(USoundWave::StaticClass()) && !bMetaSound;
        if (bWave && PlaysOnlyThroughOuterSound(AssetRegistry, Candidate.PackageName))
        {
            ++Skipped;
            continue;
        }

        USoundBase* Sound = Cast<USoundBase>(Candidate.GetAsset());
        if (!Sound)
        {
            UE_LOG(LogUAssetWorkbenchAuditor, Warning, TEXT("AuditSound: failed to load %s"), *AssetPath);
            continue;
        }

        ++Scanned;
        RunSoundRules(Options, AssetRegistry, AssetPath, Candidate.PackageName, Sound, Findings);
    }

    if (!WriteReport(Options, Findings, Scanned, Skipped))
    {
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    for (const FFinding& Finding : Findings)
    {
        UE_LOG(LogUAssetWorkbenchAuditor, Warning, TEXT("[%s] %s %s %s: %s (%s)"), *Finding.Severity, *Finding.Rule, *Finding.Asset, *Finding.Property, *Finding.Current, *Finding.Context);
    }

    UE_LOG(LogUAssetWorkbenchAuditor, Display, TEXT("AuditSound: complete. scanned=%d skipped inner waves=%d warnings=%d"), Scanned, Skipped, Findings.Num());

    return ToExitCode(Findings.IsEmpty() ? EUAssetWorkbenchExitType::Success : EUAssetWorkbenchExitType::IssuesFound);
}
