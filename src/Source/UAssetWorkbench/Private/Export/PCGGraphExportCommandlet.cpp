#include "Export/PCGGraphExportCommandlet.h"
#include "Export/PCGGraphJsonSerializer.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

#include "Dom/JsonObject.h"
#include "PCGGraph.h"

UPCGGraphExportCommandlet::UPCGGraphExportCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UPCGGraphExportCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("UAssetWorkbench v%s - PCGGraphExport"), UASSET_WORKBENCH_VERSION_STRING);

    TArray<FString> AssetPaths = UAssetWorkbench::ParseAssetPaths(Params);
    if (AssetPaths.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchExporter, Error, TEXT("No assets specified. Usage: -assets=\"/Game/PCG/PCG_A,/Game/PCG/PCG_B\" [-full]"));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    const bool bFull = FParse::Param(*Params, TEXT("full"));

    int32 ExportedCount = 0;
    for (const FString& AssetPath : AssetPaths)
    {
        UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
        if (!Asset)
        {
            UE_LOG(LogUAssetWorkbenchExporter, Warning, TEXT("Failed to load asset: %s"), *AssetPath);
            continue;
        }

        TSharedPtr<FJsonObject> JsonObject;
        if (const UPCGGraph* Graph = Cast<UPCGGraph>(Asset))
        {
            JsonObject = PCGGraphJson::ExportGraph(Graph, bFull);
        }
        else if (const UPCGGraphInstance* Instance = Cast<UPCGGraphInstance>(Asset))
        {
            JsonObject = PCGGraphJson::ExportGraphInstance(Instance);
        }
        else
        {
            UE_LOG(LogUAssetWorkbenchExporter, Warning, TEXT("Not a PCGGraph or PCGGraphInstance: %s (%s)"), *AssetPath, *Asset->GetClass()->GetName());
            continue;
        }

        UAssetWorkbench::FExportTarget ExportTarget(AssetPath);
        if (ExportTarget.Save(JsonObject.ToSharedRef()))
        {
            UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Exported: %s -> %s"), *AssetPath, *ExportTarget.GetPath());
            ExportedCount++;
        }
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Export complete. %d/%d PCG graphs exported."), ExportedCount, AssetPaths.Num());
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}
