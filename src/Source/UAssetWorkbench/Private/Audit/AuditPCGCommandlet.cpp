#include "Audit/AuditPCGCommandlet.h"
#include "Export/PCGGraphJsonSerializer.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/Parse.h"
#include "PCGComponent.h"
#include "PCGEdge.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "PCGSubgraph.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/UnrealType.h"

namespace
{
    const TCHAR* kSeverityError = TEXT("Error");
    const TCHAR* kSeverityWarning = TEXT("Warning");
    const TCHAR* kSeverityInfo = TEXT("Info");

    struct FFinding
    {
        FString Asset;
        FString Rule;
        FString Severity;
        FString Node;
        FString Current;
        FString Expected;
        FString Context;
    };

    struct FAuditState
    {
        TArray<FFinding> Findings;
        int32 GraphsScanned = 0;
        int32 LevelsScanned = 0;
        int32 ComponentsScanned = 0;
    };

    void Report(FAuditState& State, const FString& Asset, const TCHAR* Rule, const TCHAR* Severity, const FString& Node, const FString& Current, const FString& Expected, const FString& Context)
    {
        FFinding Finding;
        Finding.Asset = Asset;
        Finding.Rule = Rule;
        Finding.Severity = Severity;
        Finding.Node = Node;
        Finding.Current = Current;
        Finding.Expected = Expected;
        Finding.Context = Context;
        State.Findings.Add(MoveTemp(Finding));
    }

    FString NodeLabel(const UPCGNode* Node)
    {
        return FString::Printf(TEXT("%s (%s)"), *PCGGraphJson::NodeId(Node), *Node->GetNodeTitle(EPCGNodeTitleType::ListView).ToString());
    }

    bool AnyPinConnected(const TArray<TObjectPtr<UPCGPin>>& Pins)
    {
        for (const UPCGPin* Pin : Pins)
        {
            if (Pin && Pin->IsConnected())
            {
                return true;
            }
        }
        return false;
    }

    bool ReadBool(const UObject* Object, const TCHAR* Name, bool bDefault)
    {
        const FBoolProperty* Prop = FindFProperty<FBoolProperty>(Object->GetClass(), Name);
        return Prop ? Prop->GetPropertyValue_InContainer(Object) : bDefault;
    }

    FString ReadText(const UObject* Object, const TCHAR* Name)
    {
        const FProperty* Prop = FindFProperty<FProperty>(Object->GetClass(), Name);
        FString Value;
        if (Prop)
        {
            Prop->ExportTextItem_InContainer(Value, Object, nullptr, const_cast<UObject*>(Object), PPF_None);
        }
        return Value;
    }

    const UObject* ReadObject(const UObject* Object, const TCHAR* Name)
    {
        const FObjectPropertyBase* Prop = FindFProperty<FObjectPropertyBase>(Object->GetClass(), Name);
        return Prop ? Prop->GetObjectPropertyValue_InContainer(Object) : nullptr;
    }

    int32 ReadArrayNum(const UObject* Object, const TCHAR* Name)
    {
        const FArrayProperty* Prop = FindFProperty<FArrayProperty>(Object->GetClass(), Name);
        if (!Prop)
        {
            return INDEX_NONE;
        }
        FScriptArrayHelper Helper(Prop, Prop->ContainerPtrToValuePtr<void>(Object));
        return Helper.Num();
    }

    // P1 required input not fed, P2 orphan, P3 disabled, P4 subgraph missing or recursive.
    void AuditNodeShape(FAuditState& State, const FString& Asset, const UPCGGraph* Graph, const UPCGNode* Node)
    {
        const bool bIsEndpoint = Node == Graph->GetInputNode() || Node == Graph->GetOutputNode();

        // The input node's pins are fed by the caller, a subgraph shows them unconnected by design.
        const bool bIsInputNode = Node == Graph->GetInputNode();
        for (const UPCGPin* Pin : Node->GetInputPins())
        {
            if (!bIsInputNode && Pin && Pin->Properties.IsRequiredPin() && !Pin->IsConnected())
            {
                Report(State, Asset, TEXT("P1"), kSeverityError, NodeLabel(Node), TEXT("unconnected"), TEXT("connected"), FString::Printf(TEXT("required input pin '%s' (%s) has no edge"), *Pin->Properties.Label.ToString(), *Pin->Properties.AllowedTypes.ToString()));
            }
        }

        if (!bIsEndpoint && !AnyPinConnected(Node->GetInputPins()) && !AnyPinConnected(Node->GetOutputPins()))
        {
            Report(State, Asset, TEXT("P2"), kSeverityWarning, NodeLabel(Node), TEXT("orphan"), TEXT("wired or deleted"), TEXT("no edge on any pin"));
        }

        const UPCGSettingsInterface* SettingsInterface = Node->GetSettingsInterface();
        if (SettingsInterface && !SettingsInterface->bEnabled)
        {
            Report(State, Asset, TEXT("P3"), kSeverityWarning, NodeLabel(Node), TEXT("disabled"), TEXT("enabled or deleted"), TEXT("node is bypassed, data passes through untouched"));
        }

        if (const UPCGBaseSubgraphSettings* SubgraphSettings = Cast<UPCGBaseSubgraphSettings>(Node->GetSettings()))
        {
            const UPCGGraph* Subgraph = SubgraphSettings->GetSubgraph();
            if (!Subgraph)
            {
                Report(State, Asset, TEXT("P4"), kSeverityError, NodeLabel(Node), TEXT("None"), TEXT("a graph asset"), TEXT("subgraph node points at nothing"));
            }
            else if (Subgraph == Graph)
            {
                Report(State, Asset, TEXT("P4"), kSeverityError, NodeLabel(Node), Subgraph->GetPathName(), TEXT("another graph"), TEXT("subgraph node calls its own graph"));
            }
        }
    }

    // P5: a graph parameter nothing reads. Getter nodes are the only consumers, an instance override of an
    // unread parameter changes nothing.
    void AuditParameters(FAuditState& State, const FString& Asset, const UPCGGraph* Graph, const TArray<const UPCGNode*>& Nodes)
    {
        const FInstancedPropertyBag* Bag = Graph->GetUserParametersStruct();
        const UPropertyBag* BagStruct = Bag ? Bag->GetPropertyBagStruct() : nullptr;
        if (!BagStruct)
        {
            return;
        }

        TSet<FName> Used;
        for (const UPCGNode* Node : Nodes)
        {
            const UPCGSettings* Settings = Node->GetSettings();
            if (Settings && Settings->GetClass()->GetName().Contains(TEXT("UserParameterGet")))
            {
                if (const FNameProperty* Prop = FindFProperty<FNameProperty>(Settings->GetClass(), TEXT("PropertyName")))
                {
                    Used.Add(Prop->GetPropertyValue_InContainer(Settings));
                }
            }
        }

        for (const FPropertyBagPropertyDesc& Desc : BagStruct->GetPropertyDescs())
        {
            if (!Used.Contains(Desc.Name))
            {
                Report(State, Asset, TEXT("P5"), kSeverityWarning, FString(), Desc.Name.ToString(), TEXT("read by a Get Graph Parameter node"), TEXT("graph parameter has no consumer"));
            }
        }
    }

    // P6: a spawner with nothing to spawn. Only the two common spawners have a static answer, an attribute
    // driven selector resolves at generation time and is left alone.
    void AuditSpawner(FAuditState& State, const FString& Asset, const UPCGNode* Node)
    {
        const UPCGSettings* Settings = Node->GetSettings();
        if (!Settings || Settings->GetType() != EPCGSettingsType::Spawner)
        {
            return;
        }

        const FString ClassName = Settings->GetClass()->GetName();
        if (ClassName == TEXT("PCGStaticMeshSpawnerSettings"))
        {
            const UObject* Selector = ReadObject(Settings, TEXT("MeshSelectorParameters"));
            if (Selector && Selector->GetClass()->GetName().Contains(TEXT("Weighted")))
            {
                const int32 Entries = ReadArrayNum(Selector, TEXT("MeshEntries"));
                if (Entries == 0)
                {
                    Report(State, Asset, TEXT("P6"), kSeverityError, NodeLabel(Node), TEXT("0 mesh entries"), TEXT("at least one mesh"), TEXT("weighted mesh selector has nothing to spawn"));
                }
            }
        }
        else if (ClassName == TEXT("PCGSpawnActorSettings"))
        {
            const UPCGPin* OverridePin = Node->GetInputPin(TEXT("TemplateActorClass"));
            const bool bOverridden = OverridePin && OverridePin->IsConnected();
            if (!bOverridden && !ReadObject(Settings, TEXT("TemplateActorClass")))
            {
                Report(State, Asset, TEXT("P6"), kSeverityError, NodeLabel(Node), TEXT("None"), TEXT("an actor class"), TEXT("spawn actor has no template class and no override pin"));
            }
        }
    }

    // P7: HiGen intent and grid size nodes disagree.
    void AuditHiGen(FAuditState& State, const FString& Asset, const UPCGGraph* Graph, const TArray<const UPCGNode*>& Nodes)
    {
        const bool bHiGen = ReadBool(Graph, TEXT("bUseHierarchicalGeneration"), false);

        int32 GridSizeNodes = 0;
        for (const UPCGNode* Node : Nodes)
        {
            const UPCGSettings* Settings = Node->GetSettings();
            if (Settings && Settings->GetClass()->GetName() == TEXT("PCGHiGenGridSizeSettings"))
            {
                ++GridSizeNodes;
            }
        }

        if (!bHiGen && GridSizeNodes > 0)
        {
            Report(State, Asset, TEXT("P7"), kSeverityWarning, FString(), TEXT("bUseHierarchicalGeneration=False"), TEXT("True, or delete the grid size nodes"), FString::Printf(TEXT("%d Grid Size node(s) in a graph without hierarchical generation"), GridSizeNodes));
        }
        else if (bHiGen && GridSizeNodes == 0)
        {
            Report(State, Asset, TEXT("P7"), kSeverityInfo, FString(), TEXT("bUseHierarchicalGeneration=True"), TEXT("a Grid Size node per grid"), TEXT("hierarchical generation on but every node runs on the default grid"));
        }
    }

    void AuditGraph(FAuditState& State, const UPCGGraph* Graph)
    {
        const FString Asset = Graph->GetPathName();
        ++State.GraphsScanned;

        TArray<const UPCGNode*> Nodes;
        PCGGraphJson::CollectNodes(Graph, Nodes);

        for (const UPCGNode* Node : Nodes)
        {
            AuditNodeShape(State, Asset, Graph, Node);
            AuditSpawner(State, Asset, Node);
        }
        AuditParameters(State, Asset, Graph, Nodes);
        AuditHiGen(State, Asset, Graph, Nodes);
    }

    // P8: component configuration a level ships with.
    void AuditComponent(FAuditState& State, const FString& LevelPackage, const AActor* Actor, const UPCGComponent* Component)
    {
        ++State.ComponentsScanned;
        const FString Asset = FString::Printf(TEXT("%s:%s.%s"), *LevelPackage, *Actor->GetActorNameOrLabel(), *Component->GetName());

        if (!Component->GetGraph())
        {
            Report(State, Asset, TEXT("P8"), kSeverityError, FString(), TEXT("Graph=None"), TEXT("a graph"), TEXT("PCG component has no graph, it generates nothing"));
        }

        const FString Trigger = ReadText(Component, TEXT("GenerationTrigger"));
        if (Trigger == TEXT("GenerateAtRuntime") && !ReadObject(Component, TEXT("SchedulingPolicy")))
        {
            Report(State, Asset, TEXT("P8"), kSeverityWarning, FString(), TEXT("SchedulingPolicy=None"), TEXT("a scheduling policy"), TEXT("runtime generation with no scheduling policy never schedules"));
        }

        if (!ReadBool(Component, TEXT("bActivated"), true))
        {
            Report(State, Asset, TEXT("P8"), kSeverityInfo, FString(), TEXT("bActivated=False"), TEXT("True"), TEXT("component is deactivated"));
        }
    }

    void AuditLevel(FAuditState& State, const FName LevelPackage)
    {
        UPackage* Package = LoadPackage(nullptr, *LevelPackage.ToString(), LOAD_None);
        UWorld* World = Package ? UWorld::FindWorldInPackage(Package) : nullptr;
        if (!World || !World->PersistentLevel)
        {
            UE_LOG(LogUAssetWorkbenchAuditor, Warning, TEXT("AuditPCG: no UWorld in %s"), *LevelPackage.ToString());
            return;
        }

        ++State.LevelsScanned;
        for (const AActor* Actor : World->PersistentLevel->Actors)
        {
            if (!Actor)
            {
                continue;
            }
            TArray<UPCGComponent*> Components;
            Actor->GetComponents<UPCGComponent>(Components);
            for (const UPCGComponent* Component : Components)
            {
                AuditComponent(State, LevelPackage.ToString(), Actor, Component);
            }
        }
    }

    void CollectGraphs(const TArray<FString>& EntryAssets, const FString& ScanDir, TArray<FString>& OutGraphPaths)
    {
        if (!EntryAssets.IsEmpty())
        {
            OutGraphPaths = EntryAssets;
            return;
        }

        IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        Registry.ScanPathsSynchronous({ ScanDir }, /* bForceRescan */ true);

        FARFilter Filter;
        Filter.PackagePaths.Add(*ScanDir);
        Filter.bRecursivePaths = true;
        Filter.ClassPaths.Add(UPCGGraph::StaticClass()->GetClassPathName());

        TArray<FAssetData> Assets;
        Registry.GetAssets(Filter, Assets);
        for (const FAssetData& Asset : Assets)
        {
            OutGraphPaths.Add(Asset.GetObjectPathString());
        }
        OutGraphPaths.Sort();
    }

    bool WriteReport(const FString& ReportPath, const FAuditState& State, int32& OutErrors, int32& OutWarnings, int32& OutInfos)
    {
        TArray<TSharedPtr<FJsonValue>> FindingsJson;
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

            TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("Asset"), Finding.Asset);
            Item->SetStringField(TEXT("Rule"), Finding.Rule);
            Item->SetStringField(TEXT("Severity"), Finding.Severity);
            Item->SetStringField(TEXT("Node"), Finding.Node);
            Item->SetStringField(TEXT("Current"), Finding.Current);
            Item->SetStringField(TEXT("Expected"), Finding.Expected);
            Item->SetStringField(TEXT("Context"), Finding.Context);
            FindingsJson.Add(MakeShared<FJsonValueObject>(Item));
        }

        TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
        Summary->SetNumberField(TEXT("Error"), OutErrors);
        Summary->SetNumberField(TEXT("Warning"), OutWarnings);
        Summary->SetNumberField(TEXT("Info"), OutInfos);

        TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetStringField(TEXT("RunName"), TEXT("AuditPCG"));
        Root->SetStringField(TEXT("ExporterVersion"), UASSET_WORKBENCH_VERSION_STRING);
        Root->SetNumberField(TEXT("GraphsScanned"), State.GraphsScanned);
        Root->SetNumberField(TEXT("LevelsScanned"), State.LevelsScanned);
        Root->SetNumberField(TEXT("ComponentsScanned"), State.ComponentsScanned);
        Root->SetObjectField(TEXT("Summary"), Summary);
        Root->SetArrayField(TEXT("Findings"), FindingsJson);

        if (!UAssetWorkbench::SaveJsonToFile(Root, ReportPath))
        {
            UE_LOG(LogUAssetWorkbenchAuditor, Error, TEXT("Failed to write report: %s"), *ReportPath);
            return false;
        }

        UE_LOG(LogUAssetWorkbenchAuditor, Display, TEXT("AuditPCG: report written: %s"), *ReportPath);
        return true;
    }
}

UAuditPCGCommandlet::UAuditPCGCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UAuditPCGCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchAuditor, Display, TEXT("UAssetWorkbench v%s - AuditPCG"), UASSET_WORKBENCH_VERSION_STRING);

    const UAssetWorkbench::FLevelScanOptions Options = UAssetWorkbench::ParseLevelScanOptions(Params, TEXT("AuditPCG"));
    const TArray<FString> EntryAssets = UAssetWorkbench::ParseAssetPaths(Params);

    TArray<FString> GraphPaths;
    CollectGraphs(EntryAssets, Options.ScanDir, GraphPaths);

    FAuditState State;
    for (const FString& GraphPath : GraphPaths)
    {
        const UPCGGraph* Graph = LoadObject<UPCGGraph>(nullptr, *GraphPath);
        if (!Graph)
        {
            UE_LOG(LogUAssetWorkbenchAuditor, Warning, TEXT("AuditPCG: failed to load graph: %s"), *GraphPath);
            continue;
        }
        AuditGraph(State, Graph);
    }

    // Levels load every actor, so the scan is opt-in. Named levels always run.
    TArray<FName> LevelPackages;
    if (!Options.LevelPaths.IsEmpty() || FParse::Param(*Params, TEXT("withlevels")))
    {
        UAssetWorkbench::CollectLevelPackages(Options, LevelPackages);
    }
    for (const FName& LevelPackage : LevelPackages)
    {
        AuditLevel(State, LevelPackage);
    }

    if (State.GraphsScanned == 0 && State.LevelsScanned == 0)
    {
        UE_LOG(LogUAssetWorkbenchAuditor, Warning, TEXT("AuditPCG: nothing in scope (assets=%d scandir=%s levels=%d)"), EntryAssets.Num(), *Options.ScanDir, LevelPackages.Num());
    }

    int32 Errors = 0;
    int32 Warnings = 0;
    int32 Infos = 0;
    if (!WriteReport(Options.ReportPath, State, Errors, Warnings, Infos))
    {
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    for (const FFinding& Finding : State.Findings)
    {
        if (Finding.Severity != kSeverityInfo)
        {
            UE_LOG(LogUAssetWorkbenchAuditor, Warning, TEXT("[%s] %s %s %s: %s -> %s (%s)"), *Finding.Severity, *Finding.Rule, *Finding.Asset, *Finding.Node, *Finding.Current, *Finding.Expected, *Finding.Context);
        }
    }

    UE_LOG(LogUAssetWorkbenchAuditor, Display, TEXT("AuditPCG: complete. graphs=%d levels=%d components=%d errors=%d warnings=%d infos=%d"), State.GraphsScanned, State.LevelsScanned, State.ComponentsScanned, Errors, Warnings, Infos);

    const bool bIssues = Errors > 0 || Warnings > 0;
    return ToExitCode(bIssues ? EUAssetWorkbenchExitType::IssuesFound : EUAssetWorkbenchExitType::Success);
}
