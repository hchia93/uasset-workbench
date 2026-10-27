#pragma once

#include "CoreMinimal.h"

// File-based contract between external wrappers and the in-editor queue subsystem, under ProjectDir.
//   Saved/UAssetWorkbenchTaskQueue/.alive                  heartbeat, holds the editor pid, mtime touched periodically
//   Saved/UAssetWorkbenchTaskQueue/pending/<uuid>.json     wrapper writes task here
//   Saved/UAssetWorkbenchTaskQueue/processing/<uuid>.json  subsystem claims via rename
//   Saved/UAssetWorkbenchTaskQueue/done/<uuid>.json        subsystem writes result here

namespace UAssetWorkbenchTaskQueue
{
    inline constexpr const TCHAR* QueueRootRelative = TEXT("Saved/UAssetWorkbenchTaskQueue");
    inline constexpr const TCHAR* PendingSubdir     = TEXT("pending");
    inline constexpr const TCHAR* ProcessingSubdir  = TEXT("processing");
    inline constexpr const TCHAR* DoneSubdir        = TEXT("done");
    inline constexpr const TCHAR* AliveFileName     = TEXT(".alive");

    inline constexpr float HeartbeatIntervalSeconds  = 5.0f;
    inline constexpr float HeartbeatFreshnessSeconds = 15.0f;

    inline constexpr const TCHAR* FieldRunName   = TEXT("RunName");
    inline constexpr const TCHAR* FieldAssets    = TEXT("Assets");
    inline constexpr const TCHAR* FieldExtraArgs = TEXT("ExtraArgs");

    inline constexpr const TCHAR* FieldExitCode = TEXT("ExitCode");
    inline constexpr const TCHAR* FieldOutputs  = TEXT("Outputs");
    inline constexpr const TCHAR* FieldError    = TEXT("Error");

    // Several editors of one project share the queue. The wrapper maps this pid to the editor_session.py entry.
    inline constexpr const TCHAR* FieldEditorPid = TEXT("EditorPid");
}
