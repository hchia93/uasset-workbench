#include "Migrate/ParameterCallRewriter.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "Modules/ModuleManager.h"

#include "UAssetWorkbenchModule.h"

TArray<FString> UAssetWorkbench::CollectReferencingBlueprints(const TArray<FName>& SeedPackages, int32 Depth)
{
    IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

    TSet<FName> Visited(SeedPackages);
    TArray<FName> Frontier = SeedPackages;
    TArray<FString> Blueprints;

    for (int32 Hop = 0; Hop < Depth && Frontier.Num() > 0; ++Hop)
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
                NextFrontier.Add(Referencer);

                TArray<FAssetData> Assets;
                Registry.GetAssetsByPackageName(Referencer, Assets, true);
                for (const FAssetData& Asset : Assets)
                {
                    const UClass* Class = Asset.GetClass();
                    if (Class && Class->IsChildOf(UBlueprint::StaticClass()))
                    {
                        Blueprints.Add(Asset.GetObjectPathString());
                    }
                }
            }
        }

        Frontier = MoveTemp(NextFrontier);
    }

    Blueprints.Sort();
    return Blueprints;
}

int32 UAssetWorkbench::RewriteParameterCalls(UBlueprint* Blueprint, const FParameterCallMatcher& Matcher, bool bApply, TArray<FParameterCallSite>& OutSites)
{
    TArray<UEdGraph*> Graphs;
    Blueprint->GetAllGraphs(Graphs);

    int32 Rewritten = 0;
    for (UEdGraph* Graph : Graphs)
    {
        if (!Graph)
        {
            continue;
        }

        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
            const UFunction* Function = Call ? Call->GetTargetFunction() : nullptr;
            if (!Function)
            {
                continue;
            }

            const UClass* Owner = Function->GetOwnerClass();
            const bool bOwnerMatches = Matcher.OwnerClasses.ContainsByPredicate([Owner](const UClass* Class) { return Owner && Owner->IsChildOf(Class); });
            if (!bOwnerMatches)
            {
                continue;
            }

            for (FName PinName : Matcher.PinNames)
            {
                UEdGraphPin* Pin = Call->FindPin(PinName, EGPD_Input);
                const bool bNamePin = Pin && (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Name || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_String);
                if (!bNamePin)
                {
                    continue;
                }

                FParameterCallSite Site;
                Site.Blueprint = Blueprint->GetPathName();
                Site.Graph = Graph->GetName();
                Site.Function = Function->GetName();

                if (Pin->LinkedTo.Num() > 0)
                {
                    Site.bFromLink = true;
                    OutSites.Add(MoveTemp(Site));
                    continue;
                }

                Site.Literal = Pin->DefaultValue;
                if (!Matcher.Rewrite(Site.Literal, Site.Rewritten))
                {
                    continue;
                }

                if (bApply)
                {
                    Pin->Modify();
                    Pin->GetSchema()->TrySetDefaultValue(*Pin, Site.Rewritten);
                }

                ++Rewritten;
                OutSites.Add(MoveTemp(Site));
            }
        }
    }

    return Rewritten;
}

void UAssetWorkbench::LogParameterCallSites(const TArray<FParameterCallSite>& Sites)
{
    for (const FParameterCallSite& Site : Sites)
    {
        if (Site.bFromLink)
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Warning, TEXT("  %s %s %s: name comes from a link, check it by hand"), *Site.Blueprint, *Site.Graph, *Site.Function);
        }
        else
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("  %s %s %s: \"%s\" -> \"%s\""), *Site.Blueprint, *Site.Graph, *Site.Function, *Site.Literal, *Site.Rewritten);
        }
    }
}
