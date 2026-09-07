#pragma once

#include "CoreMinimal.h"
#include "Misc/OutputDevice.h"

class FJsonObject;

namespace UAssetWorkbench
{
    // Taps GLog for the length of a run and mirrors every LogUAssetWorkbench* line onto one Message Log
    // page, so a finished run reads back and a buried warning is one filter click away. Mirroring is
    // one-way, the listing suppresses its own write back or every captured line would return.
    class FRunReport : public FOutputDevice
    {
    public:
        explicit FRunReport(const FString& RunName);
        virtual ~FRunReport();

        virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override;

        int32 GetWarningCount() const { return m_WarningCount; }
        int32 GetErrorCount() const { return m_ErrorCount; }

        // Closing line on the page. Severity follows what the run actually reported.
        void Finish(const FString& Summary, bool bSuccess);

    private:
        FString m_RunName;
        int32 m_WarningCount = 0;
        int32 m_ErrorCount = 0;
        bool m_bEmitting = false;
    };

    // Parse "-<ParamName>A,B,C" out of a commandlet param string. Trims quotes and whitespace.
    // ParamName carries its own leading dash and trailing equals, e.g. TEXT("-blueprints=").
    TArray<FString> ParsePathList(const FString& Params, const TCHAR* ParamName);

    // Parse "-assets=A,B,C" out of a commandlet param string. Trims quotes and whitespace.
    TArray<FString> ParseAssetPaths(const FString& Params);

    // Compile Blueprints, mark dirty, write the package to disk. Extension follows level vs asset.
    // A Blueprint that comes out of the compile in error is not saved, the call returns false instead.
    bool CompileAndSavePackage(UObject* Asset, bool bCompileBlueprint = true);

    // A package written from a standalone commandlet process lands without the imports the asset registry
    // builds its dependency graph from. The asset itself is fine, it opens and runs, but Reference Viewer
    // shows it isolated until something saves it again from inside the editor. Warns once per run.
    void WarnIfWrittenOutsideEditor();

    // Property path takes "Array[2].Field" to reach inside arrays, structs and instanced sub-objects. A
    // string value goes through ImportText, the exporter's own format, anything else through the converter.
    int32 ApplyProperties(UObject* Target, const TSharedPtr<FJsonObject>& Properties, int32& OutFailures);

    // Same path syntax rooted at a bare struct instance, which is how a DataTable row is reached.
    // Owner only resolves object references inside a literal, it is not written to.
    int32 ApplyStructProperties(UStruct* Struct, void* Base, UObject* Owner, const TSharedPtr<FJsonObject>& Properties, int32& OutFailures);

    // Map a /Game/... asset path to <ProjectDir>/Intermediate/UAssetExport/Game/.../<asset>.json
    FString GetExportPath(const FString& AssetPath);

    // True when a stamped export for AssetPath was written at or after Since. The stamp is not known
    // up front, so presence is judged by "this run wrote one", never by the plain path.
    bool HasStampedExportSince(const FString& AssetPath, const FDateTime& Since);

    // The file name carries the asset's revision and the moment of capture, so two exports never collide
    // and a reader knows which revision a file describes without opening it. Stamping happens after the
    // write succeeds, so a failed export burns no timestamp. GetPath is the plain path until then.
    class FExportTarget
    {
    public:
        explicit FExportTarget(const FString& InAssetPath);

        bool Save(const TSharedRef<FJsonObject>& JsonObject);

        const FString& GetPath() const { return m_ExportPath; }

    private:
        FString m_AssetPath;
        FString m_ExportPath;
    };

    // Serialize JsonObject to FilePath as UTF-8 (no BOM). Creates output dir if missing.
    bool SaveJsonToFile(const TSharedRef<FJsonObject>& JsonObject, const FString& FilePath);

    // Reflection walk from Object's class down to (not including) StopAtClass. Skips Transient/Deprecated.
    TSharedPtr<FJsonObject> ExportSubclassProperties(UObject* Object, UClass* StopAtClass);

    // Level selection shared by the audit runs. An explicit -levels= list wins over -scandir=.
    struct FLevelScanOptions
    {
        TArray<FString> LevelPaths;
        FString ScanDir;
        FString ReportPath;
    };

    // Parse -levels= / -scandir= / -report=. ReportSubdir names the folder under Intermediate that
    // the default report path lands in.
    FLevelScanOptions ParseLevelScanOptions(const FString& Params, const TCHAR* ReportSubdir);

    // Resolve scan options into level package names through the asset registry.
    void CollectLevelPackages(const FLevelScanOptions& Options, TArray<FName>& OutLevelPackages);
}
