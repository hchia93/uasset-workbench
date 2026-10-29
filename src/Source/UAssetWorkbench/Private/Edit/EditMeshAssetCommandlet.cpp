#include "Edit/EditMeshAssetCommandlet.h"

// Editor-only by design: writes source models and editor-only build settings. Trap any Runtime-type drift early.
static_assert(WITH_EDITOR, "UAssetWorkbench commandlets are editor-only, keep the uplugin Module Type=Editor.");

#include "AssetCompilingManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/BoxElem.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "StaticMeshResources.h"

#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

namespace
{
    // Settings only. Source geometry, sections and imported LOD data stay out of reach.
    const TCHAR* kStaticMeshRoots[] =
    {
        TEXT("NaniteSettings"),
        TEXT("StaticMaterials"),
        TEXT("LODGroup"),
        TEXT("MinLOD"),
        TEXT("bAutoComputeLODScreenSize")
    };

    const TCHAR* kSkeletalMeshRoots[] =
    {
        TEXT("NaniteSettings"),
        TEXT("Materials")
    };

    const TCHAR* kSourceModelRoots[] =
    {
        TEXT("ReductionSettings"),
        TEXT("ScreenSize"),
        TEXT("BuildSettings")
    };

    const TCHAR* kCollisionFields[] =
    {
        TEXT("CollisionTraceFlag"),
        TEXT("SimpleCollision")
    };

    template <int32 N>
    FString JoinNames(const TCHAR* (&Names)[N])
    {
        TArray<FString> Out;
        for (const TCHAR* Name : Names)
        {
            Out.Add(Name);
        }

        return FString::Join(Out, TEXT(" "));
    }

    template <int32 N>
    bool ContainsName(const TCHAR* (&Names)[N], const FString& Name)
    {
        for (const TCHAR* Candidate : Names)
        {
            if (Name == Candidate)
            {
                return true;
            }
        }

        return false;
    }

    // "StaticMaterials[0].MaterialInterface" is reached through StaticMaterials, so the root decides acceptance.
    FString RootSegment(const FString& Path)
    {
        int32 End = Path.Len();
        int32 Dot = INDEX_NONE;
        int32 Bracket = INDEX_NONE;
        if (Path.FindChar(TEXT('.'), Dot))
        {
            End = FMath::Min(End, Dot);
        }
        if (Path.FindChar(TEXT('['), Bracket))
        {
            End = FMath::Min(End, Bracket);
        }

        return Path.Left(End);
    }

    template <int32 N>
    bool ValidateRoots(const TSharedPtr<FJsonObject>& Properties, const TCHAR* (&Roots)[N], const FString& Owner)
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
        {
            if (!ContainsName(Roots, RootSegment(Pair.Key)))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s is not editable on %s. Accepted roots: %s"), *Pair.Key, *Owner, *JoinNames(Roots));
                return false;
            }
        }

        return true;
    }

    bool ParseTraceFlag(const FString& Text, ECollisionTraceFlag& OutFlag)
    {
        const int64 Value = StaticEnum<ECollisionTraceFlag>()->GetValueByNameString(Text);
        if (Value == INDEX_NONE)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Unknown CollisionTraceFlag %s. Accepted: CTF_UseDefault CTF_UseSimpleAndComplex CTF_UseSimpleAsComplex CTF_UseComplexAsSimple"), *Text);
            return false;
        }

        OutFlag = static_cast<ECollisionTraceFlag>(Value);
        return true;
    }

    FString TraceFlagName(ECollisionTraceFlag Flag)
    {
        return StaticEnum<ECollisionTraceFlag>()->GetNameStringByValue(static_cast<int64>(Flag));
    }

    TMap<FString, FString> ReadPaths(UObject* Target, const TSharedPtr<FJsonObject>& Properties)
    {
        TMap<FString, FString> Values;
        if (Properties.IsValid())
        {
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
            {
                Values.Add(Pair.Key, UAssetWorkbench::ReadPropertyPathText(Target, Pair.Key));
            }
        }

        return Values;
    }

    void LogPaths(const FString& AssetPath, const TMap<FString, FString>& Before, const TMap<FString, FString>& After)
    {
        for (const TPair<FString, FString>& Pair : Before)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s %s: %s -> %s"), *AssetPath, *Pair.Key, *Pair.Value, *After.FindRef(Pair.Key));
        }
    }

    int32 SimpleShapeCount(UStaticMesh* Mesh)
    {
        const UBodySetup* BodySetup = Mesh->GetBodySetup();
        return BodySetup ? BodySetup->AggGeom.GetElementCount() : 0;
    }

    FString CurrentTraceFlag(UStaticMesh* Mesh)
    {
        const UBodySetup* BodySetup = Mesh->GetBodySetup();
        return BodySetup ? TraceFlagName(BodySetup->CollisionTraceFlag) : TEXT("None");
    }
}

UEditMeshAssetCommandlet::UEditMeshAssetCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UEditMeshAssetCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("UAssetWorkbench v%s - EditMeshAsset"), UASSET_WORKBENCH_VERSION_STRING);

    FString SpecPath;
    if (!FParse::Value(*Params, TEXT("spec="), SpecPath))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("No spec specified. Usage: -spec=\"C:/path/spec.json\" [-apply]"));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    SpecPath = SpecPath.TrimQuotes();
    const bool bApply = FParse::Param(*Params, TEXT("apply"));

    // Writes happen either way, only save reads bApply. In-editor there is no process exit to discard them.
    if (!bApply && !IsRunningCommandlet())
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Dry run needs its own process to discard the in-memory edit. Close the editor and run the commandlet, or pass -apply."));
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    FString SpecText;
    if (!FFileHelper::LoadFileToString(SpecText, *SpecPath))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to read spec: %s"), *SpecPath);
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    TSharedPtr<FJsonObject> Spec;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(SpecText);
    if (!FJsonSerializer::Deserialize(Reader, Spec) || !Spec.IsValid())
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Spec is not valid JSON: %s"), *SpecPath);
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    const TArray<TSharedPtr<FJsonValue>>* Targets = nullptr;
    if (!Spec->TryGetArrayField(TEXT("Targets"), Targets))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Spec has no Targets array"));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("EditMeshAsset: %d target(s) %s"), Targets->Num(), bApply ? TEXT("[APPLY]") : TEXT("[DRY RUN]"));

    TSet<UObject*> Touched;
    int32 Ops = 0;
    for (const TSharedPtr<FJsonValue>& Value : *Targets)
    {
        const TSharedPtr<FJsonObject>& Entry = Value->AsObject();
        UAssetWorkbench::FFailureReasonCapture FailureReason;
        if (!Entry.IsValid() || !ApplyTarget(Entry, Touched, Ops))
        {
            UAssetWorkbench::ReportTargetFailure(TEXT("EditMeshAsset"), UAssetWorkbench::DescribeSpecTarget(Entry), FailureReason.GetReason());
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }
    }

    if (!bApply)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Done. %d write(s) staged across %d asset(s) (dry run, not saved)."), Ops, Touched.Num());
        return ToExitCode(EUAssetWorkbenchExitType::Success);
    }

    for (UObject* Mesh : Touched)
    {
        UAssetWorkbench::FFailureReasonCapture FailureReason;
        if (!UAssetWorkbench::CompileAndSavePackage(Mesh, false))
        {
            UAssetWorkbench::ReportTargetFailure(TEXT("EditMeshAsset"), Mesh->GetPathName(), FailureReason.GetReason());
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }
    }

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Done. %d write(s) across %d asset(s) (saved)."), Ops, Touched.Num());
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}

bool UEditMeshAssetCommandlet::ApplyTarget(const TSharedPtr<FJsonObject>& Entry, TSet<UObject*>& OutTouched, int32& OutOps) const
{
    FString AssetPath;
    if (!Entry->TryGetStringField(TEXT("AssetPath"), AssetPath))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Target has no AssetPath field"));
        return false;
    }

    const bool bWritesSomething = Entry->HasField(TEXT("Properties")) || Entry->HasField(TEXT("NumLODs")) || Entry->HasField(TEXT("LODs")) || Entry->HasField(TEXT("Collision"));
    if (!bWritesSomething)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s writes nothing. Expected Properties, NumLODs, LODs or Collision"), *AssetPath);
        return false;
    }

    UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
    FAssetCompilingManager::Get().FinishAllCompilation();

    bool bApplied = false;
    if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset))
    {
        bApplied = ApplyStaticMesh(StaticMesh, Entry, OutOps);
    }
    else if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Asset))
    {
        bApplied = ApplySkeletalMesh(SkeletalMesh, Entry, OutOps);
    }
    else
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to load StaticMesh or SkeletalMesh: %s"), *AssetPath);
        return false;
    }

    if (bApplied)
    {
        OutTouched.Add(Asset);
    }

    return bApplied;
}

bool UEditMeshAssetCommandlet::ApplyStaticMesh(UStaticMesh* Mesh, const TSharedPtr<FJsonObject>& Entry, int32& OutOps) const
{
    const FString AssetPath = Mesh->GetPathName();

    TSharedPtr<FJsonObject> Properties;
    const TSharedPtr<FJsonObject>* PropertiesField = nullptr;
    if (Entry->TryGetObjectField(TEXT("Properties"), PropertiesField))
    {
        Properties = *PropertiesField;
        if (!ValidateRoots(Properties, kStaticMeshRoots, TEXT("StaticMesh")))
        {
            return false;
        }
    }

    int32 NumLODs = Mesh->GetNumSourceModels();
    const bool bSetsNumLODs = Entry->TryGetNumberField(TEXT("NumLODs"), NumLODs);
    if (bSetsNumLODs && (NumLODs < 1 || NumLODs > MAX_STATIC_MESH_LODS))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s NumLODs %d is out of range 1-%d"), *AssetPath, NumLODs, MAX_STATIC_MESH_LODS);
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* LODs = nullptr;
    Entry->TryGetArrayField(TEXT("LODs"), LODs);
    if (LODs)
    {
        for (const TSharedPtr<FJsonValue>& Value : *LODs)
        {
            const TSharedPtr<FJsonObject> LOD = Value->AsObject();
            int32 Index = INDEX_NONE;
            const TSharedPtr<FJsonObject>* LODProperties = nullptr;
            if (!LOD.IsValid() || !LOD->TryGetNumberField(TEXT("Index"), Index) || !LOD->TryGetObjectField(TEXT("Properties"), LODProperties))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s LODs entries need Index and Properties"), *AssetPath);
                return false;
            }

            if (Index < 0 || Index >= NumLODs)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s LOD %d does not exist, the mesh has %d LOD(s) after NumLODs"), *AssetPath, Index, NumLODs);
                return false;
            }

            if (!ValidateRoots(*LODProperties, kSourceModelRoots, FString::Printf(TEXT("LOD %d"), Index)))
            {
                return false;
            }
        }
    }

    const TSharedPtr<FJsonObject>* CollisionField = nullptr;
    Entry->TryGetObjectField(TEXT("Collision"), CollisionField);
    bool bSetsTraceFlag = false;
    ECollisionTraceFlag TraceFlag = CTF_UseDefault;
    FString SimpleCollision;
    if (CollisionField)
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*CollisionField)->Values)
        {
            if (!ContainsName(kCollisionFields, Pair.Key))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s is not a Collision field. Accepted: %s"), *Pair.Key, *JoinNames(kCollisionFields));
                return false;
            }
        }

        FString TraceFlagText;
        if ((*CollisionField)->TryGetStringField(TEXT("CollisionTraceFlag"), TraceFlagText))
        {
            if (!ParseTraceFlag(TraceFlagText, TraceFlag))
            {
                return false;
            }
            bSetsTraceFlag = true;
        }

        if ((*CollisionField)->TryGetStringField(TEXT("SimpleCollision"), SimpleCollision) && SimpleCollision != TEXT("Box") && SimpleCollision != TEXT("None"))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Unknown SimpleCollision %s. Accepted: Box None"), *SimpleCollision);
            return false;
        }
    }

    // SetLODGroup reapplies the group's own LOD defaults, it cannot go through a plain property write.
    FString LODGroupText;
    FName LODGroup;
    const bool bSetsLODGroup = Properties.IsValid() && Properties->TryGetStringField(TEXT("LODGroup"), LODGroupText);
    if (bSetsLODGroup)
    {
        LODGroup = FName(*LODGroupText);

        TArray<FName> Groups;
        UStaticMesh::GetLODGroups(Groups);
        if (!Groups.Contains(LODGroup))
        {
            TArray<FString> GroupNames;
            for (FName Group : Groups)
            {
                GroupNames.Add(Group.ToString());
            }
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Unknown LODGroup %s. Accepted: %s"), *LODGroupText, *FString::Join(GroupNames, TEXT(" ")));
            return false;
        }

        Properties->RemoveField(TEXT("LODGroup"));
    }

    const FString LODGroupBefore = Mesh->GetLODGroup().ToString();
    const int32 NumLODsBefore = Mesh->GetNumSourceModels();
    const FString TraceFlagBefore = CurrentTraceFlag(Mesh);
    const int32 SimpleBefore = SimpleShapeCount(Mesh);
    const TMap<FString, FString> PropertiesBefore = ReadPaths(Mesh, Properties);

    Mesh->Modify();
    Mesh->PreEditChange(nullptr);

    if (bSetsLODGroup)
    {
        Mesh->SetLODGroup(LODGroup, false);
        ++OutOps;
    }

    if (bSetsNumLODs)
    {
        Mesh->SetNumSourceModels(NumLODs);

        // Without auto screen size, a new LOD would keep 1.0 and never switch in. Halve the previous one instead.
        if (!Mesh->GetAutoComputeLODScreenSize())
        {
            for (int32 Index = FMath::Max(NumLODsBefore, 1); Index < NumLODs; ++Index)
            {
                FStaticMeshSourceModel& Model = Mesh->GetSourceModel(Index);
                const float Previous = Mesh->GetSourceModel(Index - 1).ScreenSize.Default;
                if (Model.ScreenSize.Default >= Previous)
                {
                    Model.ScreenSize.Default = Previous * 0.5f;
                }
            }
        }
        ++OutOps;
    }

    if (Properties.IsValid() && !Properties->Values.IsEmpty())
    {
        int32 Failures = 0;
        OutOps += UAssetWorkbench::ApplyProperties(Mesh, Properties, Failures);
        if (Failures > 0)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%d propert(ies) rejected on %s, nothing saved"), Failures, *AssetPath);
            return false;
        }
    }

    if (LODs)
    {
        for (const TSharedPtr<FJsonValue>& Value : *LODs)
        {
            const TSharedPtr<FJsonObject> LOD = Value->AsObject();
            const int32 Index = static_cast<int32>(LOD->GetNumberField(TEXT("Index")));

            int32 Failures = 0;
            OutOps += UAssetWorkbench::ApplyStructProperties(FStaticMeshSourceModel::StaticStruct(), &Mesh->GetSourceModel(Index), Mesh, LOD->GetObjectField(TEXT("Properties")), Failures);
            if (Failures > 0)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%d propert(ies) rejected on %s LOD %d, nothing saved"), Failures, *AssetPath, Index);
                return false;
            }
        }
    }

    if (CollisionField)
    {
        if (!Mesh->GetBodySetup())
        {
            Mesh->CreateBodySetup();
        }

        UBodySetup* BodySetup = Mesh->GetBodySetup();
        BodySetup->Modify();

        if (bSetsTraceFlag)
        {
            BodySetup->CollisionTraceFlag = TraceFlag;
            ++OutOps;
        }

        if (!SimpleCollision.IsEmpty())
        {
            BodySetup->RemoveSimpleCollision();
            if (SimpleCollision == TEXT("Box"))
            {
                const FBox Bounds = Mesh->GetBoundingBox();
                const FVector Size = Bounds.GetSize();

                FKBoxElem Box(Size.X, Size.Y, Size.Z);
                Box.Center = Bounds.GetCenter();
                BodySetup->AggGeom.BoxElems.Add(Box);
            }
            ++OutOps;
        }

        BodySetup->InvalidatePhysicsData();
        BodySetup->CreatePhysicsMeshes();

        // Without the flag a reimport regenerates collision and drops what the spec wrote.
        Mesh->SetCustomizedCollision(true);
    }

    Mesh->PostEditChange();
    FAssetCompilingManager::Get().FinishAllCompilation();

    if (bSetsLODGroup)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s LODGroup: %s -> %s"), *AssetPath, *LODGroupBefore, *Mesh->GetLODGroup().ToString());
    }

    if (bSetsNumLODs || bSetsLODGroup)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s NumLODs: %d -> %d"), *AssetPath, NumLODsBefore, Mesh->GetNumSourceModels());
    }

    LogPaths(AssetPath, PropertiesBefore, ReadPaths(Mesh, Properties));

    if (CollisionField)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s CollisionTraceFlag: %s -> %s"), *AssetPath, *TraceFlagBefore, *CurrentTraceFlag(Mesh));
        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s simple shapes: %d -> %d"), *AssetPath, SimpleBefore, SimpleShapeCount(Mesh));
    }

    for (int32 Index = 0; Index < Mesh->GetNumLODs(); ++Index)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s LOD%d: %d triangles"), *AssetPath, Index, Mesh->GetNumTriangles(Index));
    }

    return true;
}

bool UEditMeshAssetCommandlet::ApplySkeletalMesh(USkeletalMesh* Mesh, const TSharedPtr<FJsonObject>& Entry, int32& OutOps) const
{
    const FString AssetPath = Mesh->GetPathName();

    for (const TCHAR* StaticOnly : { TEXT("NumLODs"), TEXT("LODs"), TEXT("Collision") })
    {
        if (Entry->HasField(StaticOnly))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s applies to StaticMesh only, %s is a SkeletalMesh"), StaticOnly, *AssetPath);
            return false;
        }
    }

    const TSharedPtr<FJsonObject> Properties = Entry->GetObjectField(TEXT("Properties"));
    if (!ValidateRoots(Properties, kSkeletalMeshRoots, TEXT("SkeletalMesh")))
    {
        return false;
    }

    const TMap<FString, FString> Before = ReadPaths(Mesh, Properties);

    Mesh->Modify();
    Mesh->PreEditChange(nullptr);

    int32 Failures = 0;
    OutOps += UAssetWorkbench::ApplyProperties(Mesh, Properties, Failures);
    if (Failures > 0)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%d propert(ies) rejected on %s, nothing saved"), Failures, *AssetPath);
        return false;
    }

    Mesh->PostEditChange();
    FAssetCompilingManager::Get().FinishAllCompilation();

    LogPaths(AssetPath, Before, ReadPaths(Mesh, Properties));
    return true;
}
