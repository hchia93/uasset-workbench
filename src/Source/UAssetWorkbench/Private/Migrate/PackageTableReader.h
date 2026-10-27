#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectResource.h"

namespace UAssetWorkbench
{
    struct FPackageTables
    {
        TArray<FName> NameMap;
        TArray<FObjectImport> Imports;
    };

    // Name and import tables straight off disk, nothing loads. Whatever they still hold was saved before the redirect existed.
    bool ReadPackageTables(const FString& Filename, FPackageTables& OutTables);

    // External actor and object packages mirror the folder of their map under their own root, a scoped scan follows them there.
    TArray<FString> CollectScanFolders(const FString& ScanDir);
}
