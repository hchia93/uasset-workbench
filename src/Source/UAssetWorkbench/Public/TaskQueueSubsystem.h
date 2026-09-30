#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "EditorSubsystem.h"
#include "TaskQueueSubsystem.generated.h"

class SNotificationItem;
struct FFileChangeData;

UCLASS()
class UAssetWorkbenchTaskQueueSubsystem : public UEditorSubsystem
{
    GENERATED_BODY()

public:
    virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    static FString GetQueueRoot();
    static FString GetPendingDir();
    static FString GetProcessingDir();
    static FString GetDoneDir();
    static FString GetAliveFile();
    static FString GetExportOutputRoot();

private:
    void InitializeQueueDirectories() const;
    void TouchHeartbeat() const;
    bool HeartbeatTick(float DeltaTime);

    void HandleEditorInitialized(double StartupSeconds);
    void StartQueueIntake();
    void SweepStaleProcessing() const;

    void OnPendingDirectoryChanged(const TArray<FFileChangeData>& FileChanges);
    void ScanPendingDirectory();
    void ProcessTaskFile(const FString& PendingPath);

    // Returns the commandlet's own exit code, so an audit reporting IssuesFound is not read as failure.
    int32 DispatchRun(const FString& RunName, const FString& AssetsCsv, const FString& ExtraArgs) const;

    static UClass* FindCommandletClass(const FString& RunName);
    static void WriteDoneFile(const FString& DoneFilePath, int32 ExitCode, const TArray<FString>& Outputs, const FString& ErrorMessage);

    TSharedPtr<SNotificationItem> ShowStartToast(const FString& RunName, const TArray<FString>& Assets) const;
    static void FinishToast(TSharedPtr<SNotificationItem> Item, bool bSuccess, const FString& Summary);

    FTSTicker::FDelegateHandle m_HeartbeatHandle;
    FDelegateHandle m_EditorInitializedHandle;
    FDelegateHandle m_DirectoryWatcherHandle;
    FString m_WatchedDirectory;
    bool m_bProcessing = false;
};
