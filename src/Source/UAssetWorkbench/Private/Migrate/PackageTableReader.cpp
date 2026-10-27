#include "Migrate/PackageTableReader.h"

#include "HAL/FileManager.h"
#include "Misc/PackagePath.h"
#include "Serialization/ArchiveProxy.h"
#include "UObject/NameTypes.h"
#include "UObject/ObjectVersion.h"
#include "UObject/PackageFileSummary.h"

namespace
{
    // Index pairs resolved against the name table, the way the linker reads the import table.
    class FNameTableReader : public FArchiveProxy
    {
    public:
        FNameTableReader(FArchive& Inner, const TArray<FName>& NameMap)
            : FArchiveProxy(Inner)
            , m_NameMap(NameMap)
        {
        }

        virtual FArchive& operator<<(FName& Name) override
        {
            int32 NameIndex = 0;
            int32 Number = 0;
            InnerArchive << NameIndex << Number;
            Name = m_NameMap.IsValidIndex(NameIndex) ? FName(m_NameMap[NameIndex], Number) : FName();
            return *this;
        }

    private:
        const TArray<FName>& m_NameMap;
    };
}

bool UAssetWorkbench::ReadPackageTables(const FString& Filename, FPackageTables& OutTables)
{
    TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Filename));
    if (!Reader)
    {
        return false;
    }

    FPackageFileSummary Summary;
    *Reader << Summary;
    if (Reader->IsError() || Summary.Tag != PACKAGE_FILE_TAG)
    {
        return false;
    }

    // Tables are read at the version the package was saved with, a fresh reader assumes the current one.
    Reader->SetUEVer(Summary.GetFileVersionUE());
    Reader->SetLicenseeUEVer(Summary.GetFileVersionLicenseeUE());
    Reader->SetEngineVer(Summary.SavedByEngineVersion);
    Reader->SetCustomVersions(Summary.GetCustomVersionContainer());
    Reader->SetFilterEditorOnly((Summary.GetPackageFlags() & PKG_FilterEditorOnly) != 0);

    Reader->Seek(Summary.NameOffset);
    OutTables.NameMap.Reserve(Summary.NameCount);
    for (int32 NameIndex = 0; NameIndex < Summary.NameCount; ++NameIndex)
    {
        FNameEntrySerialized Entry(ENAME_LinkerConstructor);
        *Reader << Entry;
        if (Reader->IsError())
        {
            return false;
        }
        OutTables.NameMap.Add(FName(Entry));
    }

    Reader->Seek(Summary.ImportOffset);
    FNameTableReader TableReader(*Reader, OutTables.NameMap);
    OutTables.Imports.SetNum(Summary.ImportCount);
    for (FObjectImport& Import : OutTables.Imports)
    {
        TableReader << Import;
    }

    return !Reader->IsError();
}

TArray<FString> UAssetWorkbench::CollectScanFolders(const FString& ScanDir)
{
    TArray<FString> Folders = { ScanDir };

    const int32 MountEnd = ScanDir.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 1);
    if (MountEnd != INDEX_NONE)
    {
        const FString Mount = ScanDir.Left(MountEnd);
        const FString Rest = ScanDir.Mid(MountEnd);
        Folders.Add(FString::Printf(TEXT("%s/%s%s"), *Mount, FPackagePath::GetExternalActorsFolderName(), *Rest));
        Folders.Add(FString::Printf(TEXT("%s/%s%s"), *Mount, FPackagePath::GetExternalObjectsFolderName(), *Rest));
    }

    return Folders;
}
