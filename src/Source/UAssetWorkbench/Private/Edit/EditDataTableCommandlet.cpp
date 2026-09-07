#include "Edit/EditDataTableCommandlet.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/DataTable.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
    FString DescribeRows(const UDataTable* Table)
    {
        TArray<FString> Names;
        for (const TPair<FName, uint8*>& Row : Table->GetRowMap())
        {
            Names.Add(Row.Key.ToString());
        }
        return FString::Join(Names, TEXT(", "));
    }
}

UEditDataTableCommandlet::UEditDataTableCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UEditDataTableCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("UAssetWorkbench v%s - EditDataTable"), UASSET_WORKBENCH_VERSION_STRING);

    FString SpecPath;
    if (!FParse::Value(*Params, TEXT("spec="), SpecPath))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("No spec specified. Usage: -spec=\"C:/path/spec.json\" [-apply]"));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    SpecPath = SpecPath.TrimQuotes();
    const bool bApply = FParse::Param(*Params, TEXT("apply"));

    // Rows are mutated either way, only saving reads bApply, so a dry run relies on the process exiting
    // to throw the edit away. In-editor there is no such exit.
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

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("EditDataTable: %d target(s) %s"), Targets->Num(), bApply ? TEXT("[APPLY]") : TEXT("[DRY RUN]"));

    TArray<UDataTable*> Touched;
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
        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Done. %d row write(s) staged across %d table(s) (dry run, not saved)."), Ops, Touched.Num());
        return ToExitCode(EUAssetWorkbenchExitType::Success);
    }

    for (UDataTable* Table : Touched)
    {
        // Rebuilds the cached row handles and stamps the asset registry, without it the editor keeps
        // showing the old values until a reload.
        Table->HandleDataTableChanged();

        if (!UAssetWorkbench::CompileAndSavePackage(Table, /* bCompileBlueprint */ false))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to save package for %s"), *Table->GetPathName());
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }
    }

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Done. %d row write(s) across %d table(s) (saved)."), Ops, Touched.Num());
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}

bool UEditDataTableCommandlet::ApplyTarget(const TSharedPtr<FJsonObject>& Entry, TArray<UDataTable*>& OutTouched, int32& OutOps) const
{
    FString AssetPath;
    if (!Entry->TryGetStringField(TEXT("AssetPath"), AssetPath))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Target has no AssetPath field"));
        return false;
    }

    UDataTable* Table = LoadObject<UDataTable>(nullptr, *AssetPath);
    if (!Table)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to load DataTable: %s"), *AssetPath);
        return false;
    }

    UScriptStruct* RowStruct = const_cast<UScriptStruct*>(Table->GetRowStruct());
    if (!RowStruct)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no row struct, it cannot be edited"), *AssetPath);
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
    if (!Entry->TryGetArrayField(TEXT("Rows"), Operations))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s writes nothing. Expected a Rows array"), *AssetPath);
        return false;
    }

    Table->Modify();

    for (const TSharedPtr<FJsonValue>& Value : *Operations)
    {
        const TSharedPtr<FJsonObject>& Desc = Value->AsObject();
        FString Op;
        FString RowName;
        if (!Desc.IsValid() || !Desc->TryGetStringField(TEXT("Op"), Op) || !Desc->TryGetStringField(TEXT("Row"), RowName))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: row operation needs Op and Row"), *AssetPath);
            return false;
        }

        if (Op != TEXT("Modify"))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: unknown row Op '%s'. Accepted: Modify"), *AssetPath, *Op);
            return false;
        }

        const TSharedPtr<FJsonObject>* Properties = nullptr;
        if (!Desc->TryGetObjectField(TEXT("Properties"), Properties))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: row Modify needs Properties"), *AssetPath);
            return false;
        }

        uint8* RowData = Table->FindRowUnchecked(FName(*RowName));
        if (!RowData)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no row named '%s'. Table has: %s"), *AssetPath, *RowName, *DescribeRows(Table));
            return false;
        }

        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: row %s %s (%d propert(ies))"), *Table->GetName(), *Op, *RowName, (*Properties)->Values.Num());

        int32 Failures = 0;
        OutOps += UAssetWorkbench::ApplyStructProperties(RowStruct, RowData, Table, *Properties, Failures);
        if (Failures > 0)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: %d property write(s) failed on row '%s'"), *AssetPath, Failures, *RowName);
            return false;
        }
    }

    OutTouched.AddUnique(Table);
    return true;
}
