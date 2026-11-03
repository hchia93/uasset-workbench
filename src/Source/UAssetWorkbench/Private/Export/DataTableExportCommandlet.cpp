#include "Export/DataTableExportCommandlet.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/DataTable.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

UDataTableExportCommandlet::UDataTableExportCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UDataTableExportCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("UAssetWorkbench v%s - DataTableExport"), UASSET_WORKBENCH_VERSION_STRING);

    TArray<FString> AssetPaths = UAssetWorkbench::ParseAssetPaths(Params);

    if (AssetPaths.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchExporter, Error, TEXT("No assets specified. Usage: -assets=\"/Game/Path/DT_A,/Game/Path/DT_B\""));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    int32 ExportedCount = 0;

    for (const FString& AssetPath : AssetPaths)
    {
        UDataTable* DataTable = LoadObject<UDataTable>(nullptr, *AssetPath);
        if (!DataTable)
        {
            UE_LOG(LogUAssetWorkbenchExporter, Warning, TEXT("Failed to load DataTable: %s"), *AssetPath);
            continue;
        }

        TSharedPtr<FJsonObject> JsonObject = ExportDataTable(DataTable);
        if (!JsonObject.IsValid())
        {
            UE_LOG(LogUAssetWorkbenchExporter, Warning, TEXT("Failed to export DataTable: %s"), *AssetPath);
            continue;
        }

        UAssetWorkbench::FExportTarget ExportTarget(AssetPath);
        if (ExportTarget.Save(JsonObject.ToSharedRef()))
        {
            UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Exported: %s -> %s"), *AssetPath, *ExportTarget.GetPath());
            ExportedCount++;
        }
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Export complete. %d/%d data tables exported."), ExportedCount, AssetPaths.Num());
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}

TSharedPtr<FJsonObject> UDataTableExportCommandlet::ExportDataTable(UDataTable* DataTable) const
{
    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

    Root->SetStringField(TEXT("ExporterVersion"), UASSET_WORKBENCH_VERSION_STRING);
    Root->SetStringField(TEXT("ExportType"), TEXT("DataTable"));
    Root->SetStringField(TEXT("DataTableName"), DataTable->GetName());
    Root->SetStringField(TEXT("AssetPath"), DataTable->GetPathName());
    Root->SetStringField(TEXT("ExportTimestamp"), FDateTime::Now().ToString());

    const UScriptStruct* RowStruct = DataTable->GetRowStruct();
    if (!RowStruct)
    {
        UE_LOG(LogUAssetWorkbenchExporter, Warning, TEXT("DataTable has no row struct: %s"), *DataTable->GetName());
        return Root;
    }

    Root->SetStringField(TEXT("RowStruct"), RowStruct->GetName());
    Root->SetNumberField(TEXT("RowCount"), DataTable->GetRowMap().Num());

    TSharedPtr<FJsonObject> RowsObject = MakeShared<FJsonObject>();

    for (const auto& Pair : DataTable->GetRowMap())
    {
        FString RowName = Pair.Key.ToString();
        const void* RowData = Pair.Value;

        TSharedPtr<FJsonObject> RowObj = ExportRow(RowStruct, RowData);
        if (RowObj.IsValid())
        {
            RowsObject->SetObjectField(RowName, RowObj);
        }
    }

    Root->SetObjectField(TEXT("Rows"), RowsObject);

    return Root;
}

TSharedPtr<FJsonObject> UDataTableExportCommandlet::ExportRow(const UScriptStruct* RowStruct, const void* RowData) const
{
    TSharedPtr<FJsonObject> RowObj = MakeShared<FJsonObject>();

    for (TFieldIterator<FProperty> PropIt(RowStruct); PropIt; ++PropIt)
    {
        FProperty* Prop = *PropIt;

        FString Value;
        const void* PropertyValue = Prop->ContainerPtrToValuePtr<void>(RowData);
        Value += UAssetWorkbench::ExportPropertyText(Prop, PropertyValue, nullptr);

        if (!Value.IsEmpty())
        {
            RowObj->SetStringField(Prop->GetName(), Value);
        }
    }

    return RowObj;
}

