#include "Export/PCGCatalogExportCommandlet.h"
#include "Export/PCGGraphJsonSerializer.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "PCGDataAsset.h"
#include "PCGGraph.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "Utils/PCGPreconfiguration.h"

namespace
{
    bool MatchesFilter(const FString& Filter, const TArray<FString>& Candidates)
    {
        if (Filter.IsEmpty())
        {
            return true;
        }
        for (const FString& Candidate : Candidates)
        {
            if (Candidate.Contains(Filter, ESearchCase::IgnoreCase))
            {
                return true;
            }
        }
        return false;
    }

    TArray<TSharedPtr<FJsonValue>> ExportPinList(const TArray<FPCGPinProperties>& Pins)
    {
        TArray<TSharedPtr<FJsonValue>> Array;
        for (const FPCGPinProperties& Pin : Pins)
        {
            Array.Add(MakeShared<FJsonValueObject>(PCGGraphJson::ExportPinProperties(Pin)));
        }
        return Array;
    }

    TArray<TSharedPtr<FJsonValue>> ExportEnumOptions(const UEnum* Enum)
    {
        TArray<TSharedPtr<FJsonValue>> Options;
        // Last entry is the UHT _MAX sentinel, never a real choice.
        const int32 Count = Enum->NumEnums() - 1;
        for (int32 Index = 0; Index < Count; ++Index)
        {
            if (Enum->HasMetaData(TEXT("Hidden"), Index))
            {
                continue;
            }
            Options.Add(MakeShared<FJsonValueString>(Enum->GetNameStringByIndex(Index)));
        }
        return Options;
    }

    // Subclass properties only. UPCGSettings base fields (Seed, bEnabled, bDebug) print on the node.
    TArray<TSharedPtr<FJsonValue>> ExportClassProperties(const UPCGSettings* Defaults)
    {
        TArray<TSharedPtr<FJsonValue>> Properties;
        for (TFieldIterator<FProperty> It(Defaults->GetClass()); It; ++It)
        {
            FProperty* Prop = *It;
            const UClass* OwnerClass = Prop->GetOwnerClass();
            if (OwnerClass && UPCGSettings::StaticClass()->IsChildOf(OwnerClass))
            {
                continue;
            }
            if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
            {
                continue;
            }

            TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
            Json->SetStringField(TEXT("Name"), Prop->GetName());
            Json->SetStringField(TEXT("CppType"), Prop->GetCPPType());

            const FString Category = Prop->GetMetaData(TEXT("Category"));
            if (!Category.IsEmpty())
            {
                Json->SetStringField(TEXT("Category"), Category);
            }
            if (Prop->HasMetaData(TEXT("PCG_Overridable")))
            {
                Json->SetBoolField(TEXT("Overridable"), true);
            }
            const FString EditCondition = Prop->GetMetaData(TEXT("EditCondition"));
            if (!EditCondition.IsEmpty())
            {
                Json->SetStringField(TEXT("EditCondition"), EditCondition);
            }
            const FString Tooltip = Prop->GetMetaData(TEXT("ToolTip"));
            if (!Tooltip.IsEmpty())
            {
                Json->SetStringField(TEXT("Tooltip"), Tooltip);
            }

            const UEnum* Enum = nullptr;
            if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
            {
                Enum = EnumProp->GetEnum();
            }
            else if (const FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
            {
                Enum = ByteProp->Enum;
            }
            if (Enum)
            {
                Json->SetArrayField(TEXT("Options"), ExportEnumOptions(Enum));
            }

            const FObjectProperty* ObjectProp = CastField<FObjectProperty>(Prop);
            if (ObjectProp && Prop->HasAnyPropertyFlags(CPF_InstancedReference))
            {
                Json->SetBoolField(TEXT("Instanced"), true);
                Json->SetStringField(TEXT("Class"), ObjectProp->PropertyClass->GetPathName());
            }

            FString Default;
            Prop->ExportTextItem_InContainer(Default, Defaults, nullptr, const_cast<UPCGSettings*>(Defaults), PPF_None);
            Json->SetStringField(TEXT("Default"), Default);

            Properties.Add(MakeShared<FJsonValueObject>(Json));
        }
        return Properties;
    }

    TSharedPtr<FJsonObject> ExportNativeClass(const UClass* Class, const UPCGSettings* Defaults, const FString& Category)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("Class"), Class->GetPathName());
        Json->SetStringField(TEXT("Name"), Class->GetName());
        Json->SetStringField(TEXT("Title"), Defaults->GetDefaultNodeTitle().ToString());
        Json->SetStringField(TEXT("Category"), Category);
        Json->SetBoolField(TEXT("ExposeToLibrary"), Defaults->bExposeToLibrary);

        const FString Tooltip = Defaults->GetNodeTooltipText().ToString();
        if (!Tooltip.IsEmpty())
        {
            Json->SetStringField(TEXT("Tooltip"), Tooltip);
        }

        TArray<TSharedPtr<FJsonValue>> Aliases;
        for (const FText& Alias : Defaults->GetNodeTitleAliases())
        {
            Aliases.Add(MakeShared<FJsonValueString>(Alias.ToString()));
        }
        if (Aliases.Num() > 0)
        {
            Json->SetArrayField(TEXT("Aliases"), Aliases);
        }

        TArray<TSharedPtr<FJsonValue>> Preconfigured;
        for (const FPCGPreConfiguredSettingsInfo& Info : Defaults->GetPreconfiguredInfo())
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("Label"), Info.Label.ToString());
            Entry->SetNumberField(TEXT("Index"), Info.PreconfiguredIndex);
            Preconfigured.Add(MakeShared<FJsonValueObject>(Entry));
        }
        if (Preconfigured.Num() > 0)
        {
            Json->SetArrayField(TEXT("Preconfigured"), Preconfigured);
            Json->SetBoolField(TEXT("OnlyPreconfigured"), Defaults->OnlyExposePreconfiguredSettings());
        }

        Json->SetArrayField(TEXT("InputPins"), ExportPinList(Defaults->AllInputPinProperties()));
        Json->SetArrayField(TEXT("OutputPins"), ExportPinList(Defaults->AllOutputPinProperties()));
        Json->SetArrayField(TEXT("Properties"), ExportClassProperties(Defaults));
        return Json;
    }

    TArray<TSharedPtr<FJsonValue>> ExportNativeClasses(const FString& Filter)
    {
        TArray<TSharedPtr<FJsonValue>> Native;
        const UEnum* TypeEnum = StaticEnum<EPCGSettingsType>();

        // Mirrors PCGEditorGraphSchema: Procedural Vegetation nodes only belong to their own editor.
        const UPackage* VegetationPackage = FindPackage(nullptr, TEXT("/Script/ProceduralVegetation"));

        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            if (!Class->IsChildOf(UPCGSettings::StaticClass()))
            {
                continue;
            }
            if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_Hidden))
            {
                continue;
            }
            if (VegetationPackage && Class->GetOuterUPackage() == VegetationPackage)
            {
                continue;
            }

            const UPCGSettings* Defaults = Class->GetDefaultObject<UPCGSettings>();
            if (!Defaults)
            {
                continue;
            }

            const FString Category = TypeEnum ? TypeEnum->GetNameStringByValue(static_cast<int64>(Defaults->GetType())) : FString();
            TArray<FString> Candidates = { Class->GetName(), Defaults->GetDefaultNodeTitle().ToString(), Category };
            for (const FText& Alias : Defaults->GetNodeTitleAliases())
            {
                Candidates.Add(Alias.ToString());
            }
            if (!MatchesFilter(Filter, Candidates))
            {
                continue;
            }

            Native.Add(MakeShared<FJsonValueObject>(ExportNativeClass(Class, Defaults, Category)));
        }

        Native.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
        {
            return A->AsObject()->GetStringField(TEXT("Class")) < B->AsObject()->GetStringField(TEXT("Class"));
        });
        return Native;
    }

    TArray<FAssetData> FindAssets(IAssetRegistry& Registry, const FString& ScanDir, const TArray<FTopLevelAssetPath>& ClassPaths)
    {
        FARFilter Filter;
        Filter.PackagePaths.Add(*ScanDir);
        Filter.bRecursivePaths = true;
        Filter.ClassPaths = ClassPaths;
        Filter.bRecursiveClasses = true;

        TArray<FAssetData> Assets;
        Registry.GetAssets(Filter, Assets);
        Assets.Sort([](const FAssetData& A, const FAssetData& B)
        {
            return A.GetObjectPathString() < B.GetObjectPathString();
        });
        return Assets;
    }

    TSharedPtr<FJsonObject> ExportAssetEntry(const FAssetData& Asset)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("AssetPath"), Asset.GetObjectPathString());
        Json->SetStringField(TEXT("Class"), Asset.AssetClassPath.ToString());
        return Json;
    }

    TArray<TSharedPtr<FJsonValue>> ExportAssetList(const TArray<FAssetData>& Assets)
    {
        TArray<TSharedPtr<FJsonValue>> Array;
        for (const FAssetData& Asset : Assets)
        {
            Array.Add(MakeShared<FJsonValueObject>(ExportAssetEntry(Asset)));
        }
        return Array;
    }

    // Blueprint elements are plain Blueprint assets, only the native parent tag tells them apart.
    TArray<TSharedPtr<FJsonValue>> ExportBlueprintElements(IAssetRegistry& Registry, const FString& ScanDir)
    {
        TArray<TSharedPtr<FJsonValue>> Array;
        static const FName NativeParentTag(TEXT("NativeParentClassPath"));
        for (const FAssetData& Asset : FindAssets(Registry, ScanDir, { UBlueprint::StaticClass()->GetClassPathName() }))
        {
            FString NativeParent;
            if (!Asset.GetTagValue(NativeParentTag, NativeParent) || !NativeParent.Contains(TEXT("PCGBlueprint")))
            {
                continue;
            }
            TSharedPtr<FJsonObject> Json = ExportAssetEntry(Asset);
            Json->SetStringField(TEXT("NativeParent"), NativeParent);
            Array.Add(MakeShared<FJsonValueObject>(Json));
        }
        return Array;
    }
}

UPCGCatalogExportCommandlet::UPCGCatalogExportCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UPCGCatalogExportCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("UAssetWorkbench v%s - PCGCatalogExport"), UASSET_WORKBENCH_VERSION_STRING);

    FString Filter;
    FParse::Value(*Params, TEXT("-filter="), Filter);
    FString ScanDir = TEXT("/Game");
    FParse::Value(*Params, TEXT("-scandir="), ScanDir);

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("ExporterVersion"), UASSET_WORKBENCH_VERSION_STRING);
    Root->SetStringField(TEXT("ExportType"), TEXT("PCGCatalog"));
    Root->SetStringField(TEXT("ExportTimestamp"), FDateTime::Now().ToString());
    Root->SetStringField(TEXT("Filter"), Filter);
    Root->SetStringField(TEXT("ScanDir"), ScanDir);

    TArray<TSharedPtr<FJsonValue>> Native = ExportNativeClasses(Filter);
    Root->SetArrayField(TEXT("Native"), Native);
    Root->SetNumberField(TEXT("NativeCount"), Native.Num());

    IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    Registry.ScanPathsSynchronous({ ScanDir }, /* bForceRescan */ true);

    Root->SetArrayField(TEXT("Graphs"), ExportAssetList(FindAssets(Registry, ScanDir, { UPCGGraph::StaticClass()->GetClassPathName(), UPCGGraphInstance::StaticClass()->GetClassPathName() })));
    Root->SetArrayField(TEXT("SettingsAssets"), ExportAssetList(FindAssets(Registry, ScanDir, { UPCGSettings::StaticClass()->GetClassPathName() })));
    Root->SetArrayField(TEXT("DataAssets"), ExportAssetList(FindAssets(Registry, ScanDir, { UPCGDataAsset::StaticClass()->GetClassPathName() })));
    Root->SetArrayField(TEXT("BlueprintElements"), ExportBlueprintElements(Registry, ScanDir));

    UAssetWorkbench::FExportTarget ExportTarget(TEXT("/PCGCatalogExport"));
    if (!ExportTarget.Save(Root.ToSharedRef()))
    {
        UE_LOG(LogUAssetWorkbenchExporter, Error, TEXT("Failed to write catalog"));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Exported PCG catalog: %d native classes -> %s"), Native.Num(), *ExportTarget.GetPath());
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}
