#pragma once

#include "CoreMinimal.h"

class UObjectRedirector;

namespace UAssetWorkbench
{
    struct FRedirectorFixupResult
    {
        // Files on disk the fixup deleted. A working copy still lists them, they need a VCS delete.
        TArray<FString> DeletedFiles;

        // Redirectors left in place because a referencer could not be loaded or saved, or code still names them.
        int32 KeptCount = 0;
    };

    // AssetTools' Fix Up Redirectors without its closing report window, a Slate modal that asserts in an unattended
    // in-editor run. Resaves every referencer so it points at the destination, then deletes each redirector nothing reaches.
    FRedirectorFixupResult FixupRedirectors(const TArray<UObjectRedirector*>& Redirectors);

    // The redirector a package holds, loading the package if needed. Null when the package is not a redirector.
    UObjectRedirector* FindRedirectorInPackage(const FString& PackageName);
}
