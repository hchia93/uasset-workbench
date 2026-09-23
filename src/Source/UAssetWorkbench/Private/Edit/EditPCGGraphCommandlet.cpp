#include "Edit/EditPCGGraphCommandlet.h"
#include "Edit/PCGGraphWriter.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "PCGGraph.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
    // Fixed order, not spec key order: a parameter getter node needs its parameter first, Properties and
    // Edges read the Ids Nodes registered, and Layout runs on the finished graph.
    TArray<TUniquePtr<IPCGGraphWriter>> MakeWriters()
    {
        TArray<TUniquePtr<IPCGGraphWriter>> Writers;
        Writers.Add(MakePCGGraphParameterWriter());
        Writers.Add(MakePCGGraphSettingsWriter());
        Writers.Add(MakePCGGraphNodeWriter());
        Writers.Add(MakePCGGraphPropertyWriter());
        Writers.Add(MakePCGGraphEdgeWriter());
        Writers.Add(MakePCGGraphLayoutWriter());
        return Writers;
    }
}

UEditPCGGraphCommandlet::UEditPCGGraphCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UEditPCGGraphCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("UAssetWorkbench v%s - EditPCGGraph"), UASSET_WORKBENCH_VERSION_STRING);

    FString SpecPath;
    if (!FParse::Value(*Params, TEXT("spec="), SpecPath))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("No spec specified. Usage: -spec=\"C:/path/spec.json\" [-apply]"));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    SpecPath = SpecPath.TrimQuotes();
    const bool bApply = FParse::Param(*Params, TEXT("apply"));

    // Writers mutate either way, only save reads bApply, so a dry run relies on the process exiting to
    // throw the mutation away. In-editor there is no such exit.
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

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("EditPCGGraph: %d target(s) %s"), Targets->Num(), bApply ? TEXT("[APPLY]") : TEXT("[DRY RUN]"));

    TSet<UPCGGraph*> Touched;
    int32 Ops = 0;
    for (const TSharedPtr<FJsonValue>& Value : *Targets)
    {
        const TSharedPtr<FJsonObject>& Entry = Value->AsObject();
        if (!Entry.IsValid() || !ApplyTarget(Entry, Touched, Ops))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Target failed, nothing saved"));
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }
    }

    if (!bApply)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Done. %d operation(s) staged across %d graph(s) (dry run, not saved)."), Ops, Touched.Num());
        return ToExitCode(EUAssetWorkbenchExitType::Success);
    }

    for (UPCGGraph* Graph : Touched)
    {
        // A graph has no compile step, but an open PCG editor mirrors the runtime model and only rebuilds
        // its view on a graph change broadcast.
        Graph->ForceNotificationForEditor();
        if (!UAssetWorkbench::CompileAndSavePackage(Graph, /* bCompileBlueprint */ false))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to save package for %s"), *Graph->GetPathName());
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }
    }

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Done. %d operation(s) across %d graph(s) (saved)."), Ops, Touched.Num());
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}

bool UEditPCGGraphCommandlet::ApplyTarget(const TSharedPtr<FJsonObject>& Entry, TSet<UPCGGraph*>& OutTouched, int32& OutOps) const
{
    FString AssetPath;
    if (!Entry->TryGetStringField(TEXT("AssetPath"), AssetPath))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Target has no AssetPath field"));
        return false;
    }

    UPCGGraph* Graph = LoadObject<UPCGGraph>(nullptr, *AssetPath);
    if (!Graph)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to load PCG graph: %s"), *AssetPath);
        return false;
    }

    FPCGGraphEditContext Context;
    Context.Graph = Graph;
    Context.AssetPath = AssetPath;
    PCGGraphEdit::RegisterExistingNodes(Graph, Context.NodesById);

    TArray<TUniquePtr<IPCGGraphWriter>> Writers = MakeWriters();

    // One structural broadcast at the end instead of one per node touched.
    Graph->DisableNotificationsForEditor();

    int32 Matched = 0;
    bool bOk = true;
    for (const TUniquePtr<IPCGGraphWriter>& Writer : Writers)
    {
        const TSharedPtr<FJsonValue> Section = Entry->TryGetField(Writer->GetSpecKey());
        if (!Section.IsValid())
        {
            continue;
        }

        ++Matched;
        if (!Writer->Apply(Context, Section))
        {
            bOk = false;
            break;
        }
    }

    Graph->EnableNotificationsForEditor();

    if (!bOk)
    {
        return false;
    }

    if (Matched == 0)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s writes nothing. Expected one of Parameters, GraphSettings, Nodes, Properties, Edges, Layout"), *AssetPath);
        return false;
    }

    OutOps += Context.Ops;
    OutTouched.Add(Graph);
    return true;
}
