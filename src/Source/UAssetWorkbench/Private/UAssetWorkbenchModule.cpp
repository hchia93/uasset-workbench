#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchVersion.h"
#include "TaskQueueProtocol.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "MessageLogModule.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Timespan.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogUAssetWorkbenchCore);
DEFINE_LOG_CATEGORY(LogUAssetWorkbenchExporter);
DEFINE_LOG_CATEGORY(LogUAssetWorkbenchMigrator);
DEFINE_LOG_CATEGORY(LogUAssetWorkbenchImporter);
DEFINE_LOG_CATEGORY(LogUAssetWorkbenchAuditor);
DEFINE_LOG_CATEGORY(LogUAssetWorkbenchEditor);

void FUAssetWorkbenchModule::StartupModule()
{
    UE_LOG(LogUAssetWorkbenchCore, Log, TEXT("UAssetWorkbench v%s loaded."), UASSET_WORKBENCH_VERSION_STRING);

    // Commandlet runs have no message log UI, registering a listing there is dead weight.
    if (IsRunningCommandlet())
    {
        return;
    }

    FMessageLogModule* MessageLogModule = FModuleManager::LoadModulePtr<FMessageLogModule>(TEXT("MessageLog"));
    if (!MessageLogModule)
    {
        return;
    }

    FMessageLogInitializationOptions Options;
    Options.bShowFilters = true;
    Options.bShowPages = true;
    MessageLogModule->RegisterLogListing(FName(UAssetWorkbench::MessageLogName), NSLOCTEXT("UAssetWorkbench", "MessageLogLabel", "UAsset Workbench"), Options);
}

void FUAssetWorkbenchModule::ShutdownModule()
{
    FMessageLogModule* MessageLogModule = FModuleManager::GetModulePtr<FMessageLogModule>(TEXT("MessageLog"));
    if (MessageLogModule)
    {
        MessageLogModule->UnregisterLogListing(FName(UAssetWorkbench::MessageLogName));
    }
}

UAssetWorkbench::EGroup UAssetWorkbench::ResolveGroup(const FString& RunName)
{
    if (RunName.EndsWith(TEXT("Export")))
    {
        return EGroup::Export;
    }

    if (RunName.EndsWith(TEXT("Import")) || RunName.StartsWith(TEXT("Create")))
    {
        return EGroup::Import;
    }

    if (RunName.StartsWith(TEXT("Edit")))
    {
        return EGroup::Edit;
    }

    if (RunName.StartsWith(TEXT("Audit")))
    {
        return EGroup::Audit;
    }

    return EGroup::Migrate;
}

bool UAssetWorkbench::IsLiveEditorPresent()
{
    const FString AlivePath = FPaths::Combine(
        FPaths::ProjectDir(),
        UAssetWorkbenchTaskQueue::QueueRootRelative,
        UAssetWorkbenchTaskQueue::AliveFileName);

    if (!IFileManager::Get().FileExists(*AlivePath))
    {
        return false;
    }

    const FDateTime ModTime = IFileManager::Get().GetTimeStamp(*AlivePath);
    if (ModTime == FDateTime::MinValue())
    {
        return false;
    }

    const FTimespan Age = FDateTime::UtcNow() - ModTime;
    if (Age.GetTotalSeconds() <= UAssetWorkbenchTaskQueue::HeartbeatFreshnessSeconds)
    {
        return true;
    }

    // The ticker that stamps the file stops during a synchronous editor stall, registry scan or shader
    // compile, so a stale mtime is not proof of death. The pid the editor wrote there is.
    FString PidText;
    if (!FFileHelper::LoadFileToString(PidText, *AlivePath))
    {
        return false;
    }

    const int32 EditorPid = FCString::Atoi(*PidText.TrimStartAndEnd());
    if (EditorPid <= 0)
    {
        return false;
    }

    // A killed editor leaves the file behind and its pid goes to the next process, so the pid alone is not the editor.
    const uint32 ProcessId = static_cast<uint32>(EditorPid);
    const bool bEditorProcess = FPaths::GetBaseFilename(FPlatformProcess::GetApplicationName(ProcessId)).StartsWith(TEXT("UnrealEditor"));
    return FPlatformProcess::IsApplicationRunning(ProcessId) && bEditorProcess;
}

static int32 GInternalDispatchDepth = 0;

UAssetWorkbench::FInternalDispatchScope::FInternalDispatchScope()
{
    ++GInternalDispatchDepth;
}

UAssetWorkbench::FInternalDispatchScope::~FInternalDispatchScope()
{
    --GInternalDispatchDepth;
}

bool UAssetWorkbench::AbortIfLiveEditor()
{
    if (GInternalDispatchDepth > 0)
    {
        return false;
    }
    if (!IsLiveEditorPresent())
    {
        return false;
    }
    UE_LOG(LogUAssetWorkbenchCore, Error,
        TEXT("Live editor session detected. Refusing commandlet run to avoid project lock conflict. ")
        TEXT("Close the editor, or let the in-editor queue subsystem handle the export."));
    return true;
}

IMPLEMENT_MODULE(FUAssetWorkbenchModule, UAssetWorkbench)
