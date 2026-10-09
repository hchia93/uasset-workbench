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

        // The editor delivers buffered devices from its own logging thread, after the run has already reported.
        virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

        int32 GetWarningCount() const { return m_WarningCount; }
        int32 GetErrorCount() const { return m_ErrorCount; }
        const FString& GetFirstError() const { return m_FirstError; }
        const TArray<FString>& GetFailedTargets() const { return m_FailedTargets; }

        // Closing line on the page. Severity follows what the run actually reported.
        void Finish(const FString& Summary, bool bSuccess);

        // The report of the queued run currently dispatching, null on the standalone commandlet path.
        static FRunReport* GetActive();

        void AddFailedTarget(const FString& Target);

    private:
        FString m_RunName;
        int32 m_WarningCount = 0;
        int32 m_ErrorCount = 0;
        FString m_FirstError;
        TArray<FString> m_FailedTargets;
        bool m_bEmitting = false;
    };

    // The first error this plugin logs while alive, or the text of a dialog the unattended run answered with its default.
    class FFailureReasonCapture : public FOutputDevice
    {
    public:
        FFailureReasonCapture();
        virtual ~FFailureReasonCapture();

        virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override;
        virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

        // A dialog's text wins, it is the engine's own account of why it gave up.
        FString GetReason() const;

    private:
        FString m_FirstError;
        FString m_DialogText;
    };

    // Logs "<RunName> failed for <Target>. Reason: <Reason>" and puts Target on the queued run's summary line.
    void ReportTargetFailure(const FString& RunName, const FString& Target, const FString& Reason);

    // The AssetPath a spec target names, which is what its failure line reports.
    FString DescribeSpecTarget(const TSharedPtr<FJsonObject>& Entry);

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
    // An instanced object, or an array of them, takes {"Class", "Properties"} per instance instead.
    int32 ApplyProperties(UObject* Target, const TSharedPtr<FJsonObject>& Properties, int32& OutFailures);

    // Reads back what a property path holds, in the exporter's text format. Empty when the path does not resolve.
    FString ReadPropertyPathText(UObject* Target, const FString& Path);

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
