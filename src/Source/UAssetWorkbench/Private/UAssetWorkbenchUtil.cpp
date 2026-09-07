#include "UAssetWorkbenchUtil.h"

#include "UAssetWorkbenchModule.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "JsonObjectConverter.h"
#include "Logging/MessageLog.h"
#include "Misc/DateTime.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"

namespace
{
    // One "Name" or "Name[3]" hop of a property path.
    struct FPropertyPathHop
    {
        FString Name;
        int32 Index = INDEX_NONE;
    };

    FPropertyPathHop ParsePropertyPathHop(const FString& Segment)
    {
        FPropertyPathHop Hop;

        int32 Bracket = INDEX_NONE;
        if (Segment.FindChar(TEXT('['), Bracket))
        {
            Hop.Name = Segment.Left(Bracket);
            Hop.Index = FCString::Atoi(*Segment.Mid(Bracket + 1));
            return Hop;
        }

        Hop.Name = Segment;
        return Hop;
    }

    // Walks "Array[2].Field" down to the property that takes the value. Owner tracks the last instanced
    // sub-object crossed, ImportText resolves object references against it. Rooting at a bare struct is
    // what lets a DataTable row take the same paths a UObject does.
    bool ResolvePropertyPath(UStruct* RootStruct, void* RootBase, UObject* RootOwner, const FString& Path, FProperty*& OutProperty, void*& OutAddress, UObject*& OutOwner)
    {
        TArray<FString> Segments;
        Path.ParseIntoArray(Segments, TEXT("."));
        if (Segments.IsEmpty())
        {
            return false;
        }

        UStruct* Struct = RootStruct;
        void* Base = RootBase;
        UObject* Owner = RootOwner;

        for (int32 SegmentIndex = 0; SegmentIndex < Segments.Num(); ++SegmentIndex)
        {
            const FPropertyPathHop Hop = ParsePropertyPathHop(Segments[SegmentIndex]);

            FProperty* Property = Struct->FindPropertyByName(FName(*Hop.Name));
            if (!Property)
            {
                return false;
            }

            void* Address = Property->ContainerPtrToValuePtr<void>(Base);

            if (Hop.Index != INDEX_NONE)
            {
                FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property);
                if (!ArrayProperty)
                {
                    return false;
                }

                FScriptArrayHelper Helper(ArrayProperty, Address);
                if (!Helper.IsValidIndex(Hop.Index))
                {
                    return false;
                }

                Property = ArrayProperty->Inner;
                Address = Helper.GetRawPtr(Hop.Index);
            }

            const bool bLastSegment = SegmentIndex == Segments.Num() - 1;
            if (bLastSegment)
            {
                OutProperty = Property;
                OutAddress = Address;
                OutOwner = Owner;
                return true;
            }

            if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
            {
                Struct = StructProperty->Struct;
                Base = Address;
                continue;
            }

            if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
            {
                UObject* Inner = ObjectProperty->GetObjectPropertyValue(Address);
                if (!Inner)
                {
                    return false;
                }

                Struct = Inner->GetClass();
                Base = Inner;
                Owner = Inner;
                continue;
            }

            return false;
        }

        return false;
    }
    // Strip an object suffix so "/Game/Maps/L_A.L_A" and "/Game/Maps/L_A" both yield the package name.
    FString ToPackageName(const FString& Path)
    {
        FString Trimmed = Path;
        Trimmed.TrimStartAndEndInline();

        int32 DotIndex;
        if (Trimmed.FindChar(TEXT('.'), DotIndex))
        {
            Trimmed = Trimmed.Left(DotIndex);
        }

        return Trimmed;
    }
}

UAssetWorkbench::FLevelScanOptions UAssetWorkbench::ParseLevelScanOptions(const FString& Params, const TCHAR* ReportSubdir)
{
    FLevelScanOptions Options;

    for (const FString& Path : ParsePathList(Params, TEXT("-levels=")))
    {
        Options.LevelPaths.Add(ToPackageName(Path));
    }

    if (!FParse::Value(*Params, TEXT("-scandir="), Options.ScanDir, false) || Options.ScanDir.IsEmpty())
    {
        Options.ScanDir = TEXT("/Game");
    }
    Options.ScanDir.TrimQuotesInline();

    if (!FParse::Value(*Params, TEXT("-report="), Options.ReportPath, false) || Options.ReportPath.IsEmpty())
    {
        const FString Stamp = FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"));
        Options.ReportPath = FPaths::Combine(FPaths::ProjectDir(), TEXT("Intermediate"), ReportSubdir, FString::Printf(TEXT("%s.json"), *Stamp));
    }
    Options.ReportPath.TrimQuotesInline();

    return Options;
}

void UAssetWorkbench::CollectLevelPackages(const FLevelScanOptions& Options, TArray<FName>& OutLevelPackages)
{
    // Explicit list wins, skip the directory scan entirely.
    if (Options.LevelPaths.Num() > 0)
    {
        for (const FString& Path : Options.LevelPaths)
        {
            OutLevelPackages.AddUnique(FName(*Path));
        }
        return;
    }

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

    FARFilter Filter;
    Filter.ClassPaths.Add(UWorld::StaticClass()->GetClassPathName());
    Filter.bRecursiveClasses = true;
    Filter.PackagePaths.Add(FName(*Options.ScanDir));
    Filter.bRecursivePaths = true;

    TArray<FAssetData> LevelAssets;
    AssetRegistryModule.Get().GetAssets(Filter, LevelAssets);

    for (const FAssetData& LevelAsset : LevelAssets)
    {
        OutLevelPackages.AddUnique(LevelAsset.PackageName);
    }
}

TArray<FString> UAssetWorkbench::ParsePathList(const FString& Params, const TCHAR* ParamName)
{
    TArray<FString> Result;

    FString RawValue;
    if (FParse::Value(*Params, ParamName, RawValue, false))
    {
        RawValue.TrimQuotesInline();
        RawValue.ParseIntoArray(Result, TEXT(","), true);

        for (FString& Path : Result)
        {
            Path.TrimStartAndEndInline();
        }
    }

    return Result;
}

TArray<FString> UAssetWorkbench::ParseAssetPaths(const FString& Params)
{
    return ParsePathList(Params, TEXT("-assets="));
}

int32 UAssetWorkbench::ApplyProperties(UObject* Target, const TSharedPtr<FJsonObject>& Properties, int32& OutFailures)
{
    return ApplyStructProperties(Target->GetClass(), Target, Target, Properties, OutFailures);
}

int32 UAssetWorkbench::ApplyStructProperties(UStruct* Struct, void* Base, UObject* Owner, const TSharedPtr<FJsonObject>& Properties, int32& OutFailures)
{
    int32 Written = 0;

    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
    {
        FProperty* Property = nullptr;
        void* Address = nullptr;
        UObject* ValueOwner = nullptr;
        if (!ResolvePropertyPath(Struct, Base, Owner, Pair.Key, Property, Address, ValueOwner))
        {
            UE_LOG(LogUAssetWorkbenchCore, Error, TEXT("Cannot resolve %s on %s"), *Pair.Key, *Struct->GetName());
            ++OutFailures;
            continue;
        }

        // A string is the exporter's own format, the json converter cannot read those struct literals.
        FString StringValue;
        if (Pair.Value->TryGetString(StringValue))
        {
            FString Before;
            Property->ExportTextItem_Direct(Before, Address, nullptr, ValueOwner, PPF_None);

            if (Property->ImportText_Direct(*StringValue, Address, ValueOwner, PPF_None))
            {
                // ImportText reports success for a literal it silently ignores, an empty struct literal
                // being the usual one. Reading the value back is the only way to catch that.
                FString After;
                Property->ExportTextItem_Direct(After, Address, nullptr, ValueOwner, PPF_None);
                if (Before == After)
                {
                    UE_LOG(LogUAssetWorkbenchCore, Warning, TEXT("%s = %s left the value unchanged, it either already held it or the literal writes nothing"), *Pair.Key, *StringValue);
                }

                ++Written;
                continue;
            }

            UE_LOG(LogUAssetWorkbenchCore, Error, TEXT("ImportText failed for %s = %s"), *Pair.Key, *StringValue);
            ++OutFailures;
            continue;
        }

        if (FJsonObjectConverter::JsonValueToUProperty(Pair.Value, Property, Address))
        {
            ++Written;
            continue;
        }

        UE_LOG(LogUAssetWorkbenchCore, Error, TEXT("Json conversion failed for %s"), *Pair.Key);
        ++OutFailures;
    }

    return Written;
}

bool UAssetWorkbench::CompileAndSavePackage(UObject* Asset, bool bCompileBlueprint)
{
    if (!Asset)
    {
        return false;
    }

    if (bCompileBlueprint)
    {
        if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
        {
            FKismetEditorUtilities::CompileBlueprint(Blueprint);
            if (Blueprint->Status == BS_Error)
            {
                UE_LOG(LogUAssetWorkbenchCore, Error, TEXT("Compile failed for %s, package not saved"), *Blueprint->GetPathName());
                return false;
            }
        }
    }

    UPackage* Package = Asset->GetOutermost();
    Package->MarkPackageDirty();

    const FString Extension = Package->ContainsMap() ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension();
    const FString FileName = FPackageName::LongPackageNameToFilename(Package->GetName(), Extension);

    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    SaveArgs.SaveFlags = SAVE_None;

    return UPackage::SavePackage(Package, nullptr, *FileName, SaveArgs);
}

void UAssetWorkbench::WarnIfWrittenOutsideEditor()
{
    // The queue path runs this same code inside the editor, where IsRunningCommandlet is false.
    if (!IsRunningCommandlet())
    {
        return;
    }

    static bool bWarned = false;
    if (bWarned)
    {
        return;
    }

    bWarned = true;
    UE_LOG(LogUAssetWorkbenchCore, Warning, TEXT("Packages were written from a standalone commandlet, so the asset registry will read no dependency edges for them and Reference Viewer will show them isolated. Open the editor and run ResaveAsset on them through the in-editor queue."));
}

namespace
{
    // Stamped into every export name so a re-export of an older revision cannot overwrite a newer capture,
    // and so a reader can tell which asset revision a file describes without opening it.
    FString QueryPackageRevision(const FString& AssetPath)
    {
        FString PackageName = AssetPath;
        int32 ObjectDelimiter = INDEX_NONE;
        if (PackageName.FindChar(TEXT('.'), ObjectDelimiter))
        {
            PackageName.LeftInline(ObjectDelimiter);
        }

        FString PackageFile;
        if (!FPackageName::DoesPackageExist(PackageName, &PackageFile))
        {
            return TEXT("rNA");
        }

        int32 ReturnCode = INDEX_NONE;
        FString StdOut;
        FString StdErr;
        const FString Args = FString::Printf(TEXT("info --show-item last-changed-revision \"%s\""), *FPaths::ConvertRelativePathToFull(PackageFile));
        if (!FPlatformProcess::ExecProcess(TEXT("svn"), *Args, &ReturnCode, &StdOut, &StdErr))
        {
            return TEXT("rNA");
        }

        StdOut.TrimStartAndEndInline();
        const bool bUsable = ReturnCode == 0 && !StdOut.IsEmpty() && StdOut.IsNumeric();
        return bUsable ? FString::Printf(TEXT("r%s"), *StdOut) : TEXT("rNA");
    }

    FString StampExportPath(const FString& BasePath, const FString& AssetPath)
    {
        const FString Stamp = FString::Printf(TEXT("_%s_%s"), *QueryPackageRevision(AssetPath), *FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")));

        return FPaths::SetExtension(BasePath, TEXT("")) + Stamp + TEXT(".json");
    }
}

FString UAssetWorkbench::GetExportPath(const FString& AssetPath)
{
    FString RelativePath = AssetPath;
    RelativePath.RemoveFromStart(TEXT("/"));

    return FPaths::Combine(FPaths::ProjectDir(), TEXT("Intermediate"), TEXT("UAssetExport"), RelativePath + TEXT(".json"));
}

bool UAssetWorkbench::HasStampedExportSince(const FString& AssetPath, const FDateTime& Since)
{
    const FString BasePath = GetExportPath(AssetPath);
    const FString Directory = FPaths::GetPath(BasePath);
    const FString Pattern = FPaths::GetBaseFilename(BasePath) + TEXT("_r*.json");

    // GetTimeStamp clamps a file time to whole seconds, so a bound carrying milliseconds never lands.
    const FDateTime Bound(Since.GetTicks() - Since.GetTicks() % ETimespan::TicksPerSecond);

    TArray<FString> FileNames;
    IFileManager::Get().FindFiles(FileNames, *FPaths::Combine(Directory, Pattern), true, false);

    for (const FString& FileName : FileNames)
    {
        if (IFileManager::Get().GetTimeStamp(*FPaths::Combine(Directory, FileName)) >= Bound)
        {
            return true;
        }
    }

    return false;
}

UAssetWorkbench::FExportTarget::FExportTarget(const FString& InAssetPath)
    : m_AssetPath(InAssetPath)
    , m_ExportPath(GetExportPath(InAssetPath))
{
}

bool UAssetWorkbench::FExportTarget::Save(const TSharedRef<FJsonObject>& JsonObject)
{
    if (!SaveJsonToFile(JsonObject, m_ExportPath))
    {
        return false;
    }

    const FString Stamped = StampExportPath(m_ExportPath, m_AssetPath);
    if (IFileManager::Get().Move(*Stamped, *m_ExportPath))
    {
        m_ExportPath = Stamped;
    }

    return true;
}

bool UAssetWorkbench::SaveJsonToFile(const TSharedRef<FJsonObject>& JsonObject, const FString& FilePath)
{
    const FString OutputDir = FPaths::GetPath(FilePath);
    if (!IFileManager::Get().DirectoryExists(*OutputDir))
    {
        IFileManager::Get().MakeDirectory(*OutputDir, true);
    }

    FString OutputString;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
    if (!FJsonSerializer::Serialize(JsonObject, Writer))
    {
        UE_LOG(LogUAssetWorkbenchCore, Error, TEXT("Failed to serialize JSON for: %s"), *FilePath);
        return false;
    }

    if (!FFileHelper::SaveStringToFile(OutputString, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        UE_LOG(LogUAssetWorkbenchCore, Error, TEXT("Failed to write file: %s"), *FilePath);
        return false;
    }

    return true;
}

TSharedPtr<FJsonObject> UAssetWorkbench::ExportSubclassProperties(UObject* Object, UClass* StopAtClass)
{
    TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();

    UClass* CurrentClass = Object->GetClass();
    while (CurrentClass && CurrentClass != StopAtClass)
    {
        for (TFieldIterator<FProperty> PropIt(CurrentClass, EFieldIteratorFlags::ExcludeSuper); PropIt; ++PropIt)
        {
            FProperty* Prop = *PropIt;
            if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
            {
                continue;
            }

            // Handle array properties with element detail
            if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
            {
                FScriptArrayHelper ArrayHelper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(Object));
                int32 Num = ArrayHelper.Num();

                TArray<TSharedPtr<FJsonValue>> ElementsArray;
                FProperty* InnerProp = ArrayProp->Inner;

                // If inner is struct or object, export each element individually
                if (CastField<FStructProperty>(InnerProp) || CastField<FObjectProperty>(InnerProp))
                {
                    for (int32 i = 0; i < Num; i++)
                    {
                        FString ElemValue;
                        InnerProp->ExportTextItem_Direct(ElemValue, ArrayHelper.GetRawPtr(i), nullptr, Object, PPF_None);
                        ElementsArray.Add(MakeShared<FJsonValueString>(ElemValue));
                    }
                    // Stable key (no count suffix) so external consumers can index by property name.
                    TSharedPtr<FJsonObject> ArrayInfo = MakeShared<FJsonObject>();
                    ArrayInfo->SetNumberField(TEXT("Count"), Num);
                    ArrayInfo->SetArrayField(TEXT("Elements"), ElementsArray);
                    Props->SetObjectField(Prop->GetName(), ArrayInfo);
                }
                else
                {
                    FString Value;
                    Prop->ExportTextItem_Direct(Value, Prop->ContainerPtrToValuePtr<void>(Object), nullptr, Object, PPF_None);
                    if (!Value.IsEmpty())
                    {
                        Props->SetStringField(Prop->GetName(), Value);
                    }
                }
            }
            else
            {
                FString Value;
                Prop->ExportTextItem_Direct(Value, Prop->ContainerPtrToValuePtr<void>(Object), nullptr, Object, PPF_None);
                if (!Value.IsEmpty())
                {
                    Props->SetStringField(Prop->GetName(), Value);
                }
            }
        }
        CurrentClass = CurrentClass->GetSuperClass();
    }

    return Props;
}

namespace
{
    // Only this plugin's own categories get mirrored, the engine talks far too much otherwise.
    bool IsWorkbenchCategory(const FName& Category)
    {
        static const TSet<FName> Categories = {
            LogUAssetWorkbenchCore.GetCategoryName(),
            LogUAssetWorkbenchExporter.GetCategoryName(),
            LogUAssetWorkbenchMigrator.GetCategoryName(),
            LogUAssetWorkbenchImporter.GetCategoryName(),
            LogUAssetWorkbenchEditor.GetCategoryName(),
            LogUAssetWorkbenchAuditor.GetCategoryName(),
        };

        return Categories.Contains(Category);
    }

    // Verbose and below is tracing, not a report, and does not belong on the page.
    bool ToMessageSeverity(ELogVerbosity::Type Verbosity, EMessageSeverity::Type& OutSeverity)
    {
        switch (Verbosity)
        {
        case ELogVerbosity::Fatal:
        case ELogVerbosity::Error:
            OutSeverity = EMessageSeverity::Error;
            return true;
        case ELogVerbosity::Warning:
            OutSeverity = EMessageSeverity::Warning;
            return true;
        case ELogVerbosity::Display:
        case ELogVerbosity::Log:
            OutSeverity = EMessageSeverity::Info;
            return true;
        default:
            return false;
        }
    }
}

UAssetWorkbench::FRunReport::FRunReport(const FString& RunName)
    : m_RunName(RunName)
{
    FMessageLog(FName(MessageLogName)).NewPage(FText::FromString(RunName));
    GLog->AddOutputDevice(this);
}

UAssetWorkbench::FRunReport::~FRunReport()
{
    GLog->RemoveOutputDevice(this);
}

void UAssetWorkbench::FRunReport::Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category)
{
    // FMessageLog logs on its own account, and a run can log off the game thread.
    if (m_bEmitting || !IsInGameThread() || !IsWorkbenchCategory(Category))
    {
        return;
    }

    EMessageSeverity::Type Severity = EMessageSeverity::Info;
    if (!ToMessageSeverity(Verbosity, Severity))
    {
        return;
    }

    if (Severity == EMessageSeverity::Error)
    {
        ++m_ErrorCount;
    }
    else if (Severity == EMessageSeverity::Warning)
    {
        ++m_WarningCount;
    }

    TGuardValue<bool> Emitting(m_bEmitting, true);
    FMessageLog(FName(MessageLogName)).SuppressLoggingToOutputLog().Message(Severity, FText::FromString(Message));
}

void UAssetWorkbench::FRunReport::Finish(const FString& Summary, bool bSuccess)
{
    const EMessageSeverity::Type Severity = bSuccess ? EMessageSeverity::Info : EMessageSeverity::Error;

    TGuardValue<bool> Emitting(m_bEmitting, true);
    const FName ListingName(MessageLogName);
    FMessageLog Log(ListingName);
    Log.SuppressLoggingToOutputLog().Message(Severity, FText::FromString(Summary));

    // A clean run should not steal focus, anything else should be noticed without opening the log.
    if (!bSuccess || m_ErrorCount > 0 || m_WarningCount > 0)
    {
        Log.Notify(FText::FromString(Summary), EMessageSeverity::Info, true);
    }
}
