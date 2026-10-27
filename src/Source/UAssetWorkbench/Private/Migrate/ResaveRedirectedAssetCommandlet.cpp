#include "Migrate/ResaveRedirectedAssetCommandlet.h"
#include "Migrate/PackageTableReader.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

#include "Engine/Blueprint.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/PackagePath.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"

namespace
{
    using UAssetWorkbench::CollectScanFolders;
    using UAssetWorkbench::FPackageTables;
    using UAssetWorkbench::ReadPackageTables;

    // One OldName from [CoreRedirects]. A class or package is referenced through an import, so only an import counts,
    // a plain name in the table (a notify's display name, say) needs no redirect. A struct or enum never becomes an
    // import, only property tags name it, so the name table is all there is to read for those.
    struct FRedirectedName
    {
        FString Key;
        FString OldName;
        FName ObjectName;
        FName OuterName;
        bool bMatchImports = false;
    };

    struct FRedirectedPackage
    {
        FString PackageName;
        FString Filename;
        TArray<int32> MatchedNames;
    };

    // Property and function names inside Blueprint nodes are kept as plain names that a resave leaves alone, so
    // those keys are not scanned. A hit there would only ever list the same packages again.
    bool IsScannedKey(const FString& Key)
    {
        return Key == TEXT("ClassRedirects") || Key == TEXT("StructRedirects") || Key == TEXT("EnumRedirects") || Key == TEXT("PackageRedirects");
    }

    // Read off the project file, not the merged config, which also carries every redirect the engine ships with.
    TArray<FRedirectedName> ReadProjectRedirects(const TArray<FString>& OnlyOldNames)
    {
        TArray<FRedirectedName> Result;

        const FString IniPath = FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("DefaultEngine.ini"));
        TArray<FString> Lines;
        if (!FFileHelper::LoadFileToStringArray(Lines, *IniPath))
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Cannot read %s"), *IniPath);
            return Result;
        }

        bool bInSection = false;
        for (const FString& RawLine : Lines)
        {
            const FString Line = RawLine.TrimStartAndEnd();
            if (Line.StartsWith(TEXT("[")))
            {
                bInSection = Line == TEXT("[CoreRedirects]");
                continue;
            }

            if (!bInSection || Line.StartsWith(TEXT(";")))
            {
                continue;
            }

            FString Key;
            FString Value;
            if (!Line.Split(TEXT("="), &Key, &Value))
            {
                continue;
            }
            Key.RemoveFromStart(TEXT("+"));

            FString OldName;
            if (!IsScannedKey(Key) || !FParse::Value(*Value, TEXT("OldName="), OldName))
            {
                continue;
            }

            const bool bWanted = OnlyOldNames.IsEmpty() || OnlyOldNames.ContainsByPredicate([&OldName](const FString& Wanted) { return OldName == Wanted || OldName.EndsWith(TEXT(".") + Wanted); });
            if (!bWanted)
            {
                continue;
            }

            FRedirectedName& Entry = Result.AddDefaulted_GetRef();
            Entry.Key = Key;
            Entry.OldName = OldName;

            Entry.bMatchImports = Key == TEXT("ClassRedirects") || Key == TEXT("PackageRedirects");

            FString Outer;
            FString Object;
            if (Key == TEXT("PackageRedirects"))
            {
                Entry.ObjectName = FName(*OldName);
            }
            else if (OldName.Split(TEXT("."), &Outer, &Object, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
            {
                Entry.ObjectName = FName(*Object);
                Entry.OuterName = FName(*Outer);
            }
            else
            {
                Entry.ObjectName = FName(*OldName);
            }
        }

        return Result;
    }

    FName GetImportPackageName(const TArray<FObjectImport>& Imports, const FObjectImport& Import)
    {
        const FObjectImport* Walker = &Import;
        while (Walker->OuterIndex.IsImport() && Imports.IsValidIndex(Walker->OuterIndex.ToImport()))
        {
            Walker = &Imports[Walker->OuterIndex.ToImport()];
        }
        return Walker->ObjectName;
    }

    bool MatchesImport(const FPackageTables& Tables, const FRedirectedName& Redirect)
    {
        for (const FObjectImport& Import : Tables.Imports)
        {
            if (Import.ObjectName != Redirect.ObjectName)
            {
                continue;
            }

            // A package redirect names the package import itself, which has no outer to compare.
            const bool bOuterMatches = Redirect.OuterName.IsNone() || GetImportPackageName(Tables.Imports, Import) == Redirect.OuterName;
            if (bOuterMatches)
            {
                return true;
            }
        }
        return false;
    }

    TArray<int32> MatchRedirects(const FPackageTables& Tables, const TArray<FRedirectedName>& Redirects)
    {
        const TSet<FName> Names(Tables.NameMap);

        TArray<int32> Matched;
        for (int32 Index = 0; Index < Redirects.Num(); ++Index)
        {
            const FRedirectedName& Redirect = Redirects[Index];
            if (Redirect.bMatchImports)
            {
                if (MatchesImport(Tables, Redirect))
                {
                    Matched.Add(Index);
                }
                continue;
            }

            const bool bOuterPresent = Redirect.OuterName.IsNone() || Names.Contains(Redirect.OuterName);
            if (bOuterPresent && Names.Contains(Redirect.ObjectName))
            {
                Matched.Add(Index);
            }
        }
        return Matched;
    }

    TArray<FRedirectedPackage> ScanPackages(const FString& ScanDir, const TArray<FRedirectedName>& Redirects)
    {
        TArray<FRedirectedPackage> Result;

        for (const FString& Folder : CollectScanFolders(ScanDir))
        {
            FString Directory;
            if (!FPackageName::TryConvertLongPackageNameToFilename(Folder + TEXT("/"), Directory) || !IFileManager::Get().DirectoryExists(*Directory))
            {
                continue;
            }

            TArray<FString> Files;
            IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("*.uasset"), true, false);
            IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("*.umap"), true, false, /* bClearFileNames */ false);

            for (const FString& File : Files)
            {
                FPackageTables Tables;
                if (!ReadPackageTables(File, Tables))
                {
                    UE_LOG(LogUAssetWorkbenchMigrator, Warning, TEXT("Cannot read the package tables of %s"), *File);
                    continue;
                }

                TArray<int32> Matched = MatchRedirects(Tables, Redirects);
                FString PackageName;
                if (Matched.IsEmpty() || !FPackageName::TryConvertFilenameToLongPackageName(File, PackageName))
                {
                    continue;
                }

                FRedirectedPackage& Entry = Result.AddDefaulted_GetRef();
                Entry.PackageName = PackageName;
                Entry.Filename = File;
                Entry.MatchedNames = MoveTemp(Matched);
            }
        }

        Result.Sort([](const FRedirectedPackage& A, const FRedirectedPackage& B) { return A.PackageName < B.PackageName; });
        return Result;
    }

    FString DescribeMatches(const FRedirectedPackage& Package, const TArray<FRedirectedName>& Redirects)
    {
        return FString::JoinBy(Package.MatchedNames, TEXT(", "), [&Redirects](const int32 Index) { return Redirects[Index].OldName; });
    }
}

UResaveRedirectedAssetCommandlet::UResaveRedirectedAssetCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UResaveRedirectedAssetCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("UAssetWorkbench v%s - ResaveRedirectedAsset"), UASSET_WORKBENCH_VERSION_STRING);

    const bool bApply = FParse::Param(*Params, TEXT("apply"));
    const TArray<FString> OnlyOldNames = UAssetWorkbench::ParsePathList(Params, TEXT("-oldnames="));

    FString ScanDir;
    if (!FParse::Value(*Params, TEXT("-scandir="), ScanDir, false) || ScanDir.IsEmpty())
    {
        ScanDir = TEXT("/Game");
    }
    ScanDir.TrimQuotesInline();
    ScanDir.RemoveFromEnd(TEXT("/"));

    const TArray<FRedirectedName> Redirects = ReadProjectRedirects(OnlyOldNames);
    if (Redirects.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("No Class, Struct, Enum or Package redirect in [CoreRedirects] of DefaultEngine.ini matches, nothing to scan for"));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("ResaveRedirectedAsset: %d old name(s) under %s %s"), Redirects.Num(), *ScanDir, bApply ? TEXT("[APPLY]") : TEXT("[DRY RUN]"));

    const TArray<FRedirectedPackage> Packages = ScanPackages(ScanDir, Redirects);

    TArray<int32> HitsPerName;
    HitsPerName.SetNumZeroed(Redirects.Num());
    for (const FRedirectedPackage& Package : Packages)
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("  %s  (%s)"), *Package.PackageName, *DescribeMatches(Package, Redirects));
        for (const int32 Index : Package.MatchedNames)
        {
            ++HitsPerName[Index];
        }
    }

    for (int32 Index = 0; Index < Redirects.Num(); ++Index)
    {
        if (HitsPerName[Index] == 0)
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("  %s %s: no package under %s names it"), *Redirects[Index].Key, *Redirects[Index].OldName, *ScanDir);
        }
    }

    if (!bApply || Packages.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("Done. %d package(s) still name an old name%s"), Packages.Num(), bApply ? TEXT("") : TEXT(" (dry run, nothing written)"));
        return ToExitCode(EUAssetWorkbenchExitType::Success);
    }

    int32 Failed = 0;
    TArray<UPackage*> PackagesToSave;
    for (const FRedirectedPackage& Entry : Packages)
    {
        // Saving would also write whatever edit is still pending in this editor, which is not this run's to commit.
        UPackage* Loaded = FindPackage(nullptr, *Entry.PackageName);
        if (Loaded && Loaded->IsDirty())
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("%s has unsaved edits in this editor, save or discard them first"), *Entry.PackageName);
            ++Failed;
            continue;
        }

        UPackage* Package = Loaded ? Loaded : LoadPackage(nullptr, *Entry.PackageName, LOAD_None);
        if (!Package)
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Failed to load %s"), *Entry.PackageName);
            ++Failed;
            continue;
        }

        Package->FullyLoad();

        // Same as ResaveAsset, a Blueprint compiles against the redirected types before it saves.
        if (UBlueprint* Blueprint = Cast<UBlueprint>(Package->FindAssetInPackage()))
        {
            FKismetEditorUtilities::CompileBlueprint(Blueprint);
            if (Blueprint->Status == BS_Error)
            {
                UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Compile failed for %s, package not saved"), *Entry.PackageName);
                ++Failed;
                continue;
            }
        }

        Package->MarkPackageDirty();
        PackagesToSave.Add(Package);
    }

    // The editor's own save path, it knows how to write a map and an external actor package, a plain SavePackage does not.
    TArray<UPackage*> FailedPackages;
    if (PackagesToSave.Num() > 0)
    {
        FEditorFileUtils::PromptForCheckoutAndSave(PackagesToSave, /* bCheckDirty */ false, /* bPromptToSave */ false, &FailedPackages);
    }
    for (const UPackage* FailedPackage : FailedPackages)
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Failed to save %s"), *GetNameSafe(FailedPackage));
        ++Failed;
    }

    int32 StillNaming = 0;
    for (const FRedirectedPackage& Entry : Packages)
    {
        FPackageTables Tables;
        if (!ReadPackageTables(Entry.Filename, Tables))
        {
            continue;
        }

        const TArray<int32> Matched = MatchRedirects(Tables, Redirects);
        if (Matched.IsEmpty())
        {
            continue;
        }

        // Struct and enum names can survive a resave as plain names, a Blueprint pin type for one. The redirect stays needed.
        FRedirectedPackage Remaining = Entry;
        Remaining.MatchedNames = Matched;
        UE_LOG(LogUAssetWorkbenchMigrator, Warning, TEXT("  %s still names %s after the resave"), *Entry.PackageName, *DescribeMatches(Remaining, Redirects));
        ++StillNaming;
    }

    if (Failed > 0)
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Done with %d package(s) not resaved, the errors above say why"), Failed);
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("Done. %d package(s) resaved, %d still name an old name"), PackagesToSave.Num(), StillNaming);
    UAssetWorkbench::WarnIfWrittenOutsideEditor();
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}
