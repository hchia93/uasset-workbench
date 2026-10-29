#include "Export/MeshExportCommandlet.h"

// Editor-only by design: reads source models and imported LOD data. Trap any Runtime-type drift early.
static_assert(WITH_EDITOR, "UAssetWorkbench commandlets are editor-only, keep the uplugin Module Type=Editor.");

#include "AssetCompilingManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "StaticMeshResources.h"

#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

namespace
{
    // Values in the exporter's text format, so EditMeshAsset takes them back unchanged.
    TSharedRef<FJsonObject> ExportStructText(const UScriptStruct* Struct, const void* Data, UObject* Owner)
    {
        TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
        for (TFieldIterator<FProperty> It(Struct); It; ++It)
        {
            if (It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
            {
                continue;
            }

            FString Value;
            It->ExportTextItem_Direct(Value, It->ContainerPtrToValuePtr<void>(Data), nullptr, Owner, PPF_None);
            Out->SetStringField(It->GetName(), Value);
        }

        return Out;
    }

    TSharedRef<FJsonObject> ExportTransform(FName Name, const FVector& Location, const FRotator& Rotation, const FVector& Scale)
    {
        TSharedRef<FJsonObject> Socket = MakeShared<FJsonObject>();
        Socket->SetStringField(TEXT("Name"), Name.ToString());
        Socket->SetStringField(TEXT("Location"), Location.ToString());
        Socket->SetStringField(TEXT("Rotation"), Rotation.ToString());
        Socket->SetStringField(TEXT("Scale"), Scale.ToString());
        return Socket;
    }

    TSharedRef<FJsonObject> ExportMaterialSlot(int32 Index, FName SlotName, const UMaterialInterface* Material)
    {
        TSharedRef<FJsonObject> Slot = MakeShared<FJsonObject>();
        Slot->SetNumberField(TEXT("Index"), Index);
        Slot->SetStringField(TEXT("SlotName"), SlotName.ToString());
        Slot->SetStringField(TEXT("Material"), Material ? Material->GetPathName() : FString());
        return Slot;
    }

    void SetIdentity(const TSharedRef<FJsonObject>& Root, UObject* Mesh, const TCHAR* ExportType)
    {
        Root->SetStringField(TEXT("ExporterVersion"), UASSET_WORKBENCH_VERSION_STRING);
        Root->SetStringField(TEXT("ExportType"), ExportType);
        Root->SetStringField(TEXT("MeshName"), Mesh->GetName());
        Root->SetStringField(TEXT("AssetPath"), Mesh->GetPathName());
        Root->SetStringField(TEXT("Class"), Mesh->GetClass()->GetName());
        Root->SetStringField(TEXT("ExportTimestamp"), FDateTime::Now().ToString());
    }

    FString CollisionTraceFlagName(ECollisionTraceFlag Flag)
    {
        return StaticEnum<ECollisionTraceFlag>()->GetNameStringByValue(static_cast<int64>(Flag));
    }
}

UMeshExportCommandlet::UMeshExportCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UMeshExportCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("UAssetWorkbench v%s - MeshExport"), UASSET_WORKBENCH_VERSION_STRING);

    TArray<FString> AssetPaths = UAssetWorkbench::ParseAssetPaths(Params);

    if (AssetPaths.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchExporter, Error, TEXT("No assets specified. Usage: -assets=\"/Game/Path/SM_A,/Game/Path/SK_B\""));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    int32 ExportedCount = 0;

    for (const FString& AssetPath : AssetPaths)
    {
        UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);

        // Render data and imported LOD models build async on load, the triangle counts read them.
        FAssetCompilingManager::Get().FinishAllCompilation();

        TSharedPtr<FJsonObject> JsonObject;
        if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset))
        {
            JsonObject = ExportStaticMesh(StaticMesh);
        }
        else if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Asset))
        {
            JsonObject = ExportSkeletalMesh(SkeletalMesh);
        }
        else
        {
            UE_LOG(LogUAssetWorkbenchExporter, Warning, TEXT("Failed to load StaticMesh or SkeletalMesh: %s"), *AssetPath);
            continue;
        }

        UAssetWorkbench::FExportTarget ExportTarget(AssetPath);
        if (ExportTarget.Save(JsonObject.ToSharedRef()))
        {
            UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Exported: %s -> %s"), *AssetPath, *ExportTarget.GetPath());
            ExportedCount++;
        }
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Export complete. %d/%d meshes exported."), ExportedCount, AssetPaths.Num());
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}

TSharedPtr<FJsonObject> UMeshExportCommandlet::ExportStaticMesh(UStaticMesh* Mesh) const
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    SetIdentity(Root, Mesh, TEXT("StaticMesh"));

    Root->SetStringField(TEXT("LODGroup"), Mesh->GetLODGroup().ToString());
    Root->SetNumberField(TEXT("MinLOD"), Mesh->GetMinLOD().Default);
    Root->SetBoolField(TEXT("AutoComputeLODScreenSize"), Mesh->GetAutoComputeLODScreenSize());

    const int32 BuiltLODs = Mesh->GetNumLODs();
    TArray<TSharedPtr<FJsonValue>> LODsJson;
    for (int32 Index = 0; Index < Mesh->GetNumSourceModels(); ++Index)
    {
        FStaticMeshSourceModel& SourceModel = Mesh->GetSourceModel(Index);

        TSharedRef<FJsonObject> LOD = MakeShared<FJsonObject>();
        LOD->SetNumberField(TEXT("Index"), Index);
        LOD->SetNumberField(TEXT("Triangles"), Index < BuiltLODs ? Mesh->GetNumTriangles(Index) : 0);
        LOD->SetNumberField(TEXT("Vertices"), Index < BuiltLODs ? Mesh->GetNumVertices(Index) : 0);

        // An imported LOD carries its own source, a generated one is reduced from the LOD it names.
        const bool bImported = Mesh->IsMeshDescriptionValid(Index);
        LOD->SetBoolField(TEXT("Generated"), !bImported);
        if (bImported)
        {
            const FMeshDescription* Description = Mesh->GetMeshDescription(Index);
            LOD->SetNumberField(TEXT("SourceTriangles"), Description ? Description->Triangles().Num() : 0);
        }

        // Authored value. Auto screen size resolves only when render resources init, which a -nullrhi commandlet never does.
        LOD->SetNumberField(TEXT("ScreenSize"), SourceModel.ScreenSize.Default);
        LOD->SetObjectField(TEXT("ReductionSettings"), ExportStructText(FMeshReductionSettings::StaticStruct(), &SourceModel.ReductionSettings, Mesh));
        LODsJson.Add(MakeShared<FJsonValueObject>(LOD));
    }
    Root->SetArrayField(TEXT("LODs"), LODsJson);

    TSharedRef<FJsonObject> Nanite = ExportStructText(FMeshNaniteSettings::StaticStruct(), &Mesh->GetNaniteSettings(), Mesh);
    if (Mesh->IsNaniteEnabled())
    {
        Nanite->SetNumberField(TEXT("NaniteTriangles"), Mesh->GetNumNaniteTriangles());
    }
    Root->SetObjectField(TEXT("Nanite"), Nanite);

    TSharedRef<FJsonObject> Collision = MakeShared<FJsonObject>();
    Collision->SetBoolField(TEXT("CustomizedCollision"), Mesh->GetCustomizedCollision());
    Collision->SetStringField(TEXT("ComplexCollisionMesh"), Mesh->ComplexCollisionMesh ? Mesh->ComplexCollisionMesh->GetPathName() : FString());

    if (UBodySetup* BodySetup = Mesh->GetBodySetup())
    {
        const FKAggregateGeom& Geom = BodySetup->AggGeom;
        Collision->SetStringField(TEXT("CollisionTraceFlag"), CollisionTraceFlagName(BodySetup->CollisionTraceFlag));
        Collision->SetStringField(TEXT("CollisionProfile"), BodySetup->DefaultInstance.GetCollisionProfileName().ToString());

        TSharedRef<FJsonObject> Simple = MakeShared<FJsonObject>();
        Simple->SetNumberField(TEXT("Boxes"), Geom.BoxElems.Num());
        Simple->SetNumberField(TEXT("Spheres"), Geom.SphereElems.Num());
        Simple->SetNumberField(TEXT("Capsules"), Geom.SphylElems.Num());
        Simple->SetNumberField(TEXT("Convexes"), Geom.ConvexElems.Num());
        Simple->SetNumberField(TEXT("TaperedCapsules"), Geom.TaperedCapsuleElems.Num());
        Collision->SetObjectField(TEXT("Simple"), Simple);
    }
    Root->SetObjectField(TEXT("Collision"), Collision);

    TArray<TSharedPtr<FJsonValue>> SlotsJson;
    const TArray<FStaticMaterial>& Materials = Mesh->GetStaticMaterials();
    for (int32 Index = 0; Index < Materials.Num(); ++Index)
    {
        SlotsJson.Add(MakeShared<FJsonValueObject>(ExportMaterialSlot(Index, Materials[Index].MaterialSlotName, Materials[Index].MaterialInterface)));
    }
    Root->SetArrayField(TEXT("MaterialSlots"), SlotsJson);

    TArray<TSharedPtr<FJsonValue>> SocketsJson;
    for (const UStaticMeshSocket* Socket : Mesh->Sockets)
    {
        if (Socket)
        {
            SocketsJson.Add(MakeShared<FJsonValueObject>(ExportTransform(Socket->SocketName, Socket->RelativeLocation, Socket->RelativeRotation, Socket->RelativeScale)));
        }
    }
    Root->SetArrayField(TEXT("Sockets"), SocketsJson);

    return Root;
}

TSharedPtr<FJsonObject> UMeshExportCommandlet::ExportSkeletalMesh(USkeletalMesh* Mesh) const
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    SetIdentity(Root, Mesh, TEXT("SkeletalMesh"));

    const USkeleton* Skeleton = Mesh->GetSkeleton();
    Root->SetStringField(TEXT("Skeleton"), Skeleton ? Skeleton->GetPathName() : FString());
    Root->SetNumberField(TEXT("Bones"), Mesh->GetRefSkeleton().GetNum());

    const FSkeletalMeshModel* ImportedModel = Mesh->GetImportedModel();
    TArray<TSharedPtr<FJsonValue>> LODsJson;
    for (int32 Index = 0; Index < Mesh->GetLODNum(); ++Index)
    {
        TSharedRef<FJsonObject> LOD = MakeShared<FJsonObject>();
        LOD->SetNumberField(TEXT("Index"), Index);

        int32 Triangles = 0;
        int32 Vertices = 0;
        if (ImportedModel && ImportedModel->LODModels.IsValidIndex(Index))
        {
            for (const FSkelMeshSection& Section : ImportedModel->LODModels[Index].Sections)
            {
                Triangles += Section.NumTriangles;
                Vertices += Section.NumVertices;
            }
        }
        LOD->SetNumberField(TEXT("Triangles"), Triangles);
        LOD->SetNumberField(TEXT("Vertices"), Vertices);

        if (const FSkeletalMeshLODInfo* Info = Mesh->GetLODInfo(Index))
        {
            LOD->SetBoolField(TEXT("Generated"), Info->bHasBeenSimplified);
            LOD->SetNumberField(TEXT("ScreenSize"), Info->ScreenSize.Default);
            LOD->SetObjectField(TEXT("ReductionSettings"), ExportStructText(FSkeletalMeshOptimizationSettings::StaticStruct(), &Info->ReductionSettings, Mesh));
        }
        LODsJson.Add(MakeShared<FJsonValueObject>(LOD));
    }
    Root->SetArrayField(TEXT("LODs"), LODsJson);

    Root->SetObjectField(TEXT("Nanite"), ExportStructText(FMeshNaniteSettings::StaticStruct(), &Mesh->GetNaniteSettings(), Mesh));

    TSharedRef<FJsonObject> Collision = MakeShared<FJsonObject>();
    const UPhysicsAsset* PhysicsAsset = Mesh->GetPhysicsAsset();
    Collision->SetStringField(TEXT("PhysicsAsset"), PhysicsAsset ? PhysicsAsset->GetPathName() : FString());
    Collision->SetNumberField(TEXT("Bodies"), PhysicsAsset ? PhysicsAsset->SkeletalBodySetups.Num() : 0);
    Root->SetObjectField(TEXT("Collision"), Collision);

    TArray<TSharedPtr<FJsonValue>> SlotsJson;
    const TArray<FSkeletalMaterial>& Materials = Mesh->GetMaterials();
    for (int32 Index = 0; Index < Materials.Num(); ++Index)
    {
        SlotsJson.Add(MakeShared<FJsonValueObject>(ExportMaterialSlot(Index, Materials[Index].MaterialSlotName, Materials[Index].MaterialInterface)));
    }
    Root->SetArrayField(TEXT("MaterialSlots"), SlotsJson);

    // NumSockets walks the mesh's own sockets first, then the skeleton's.
    TArray<TSharedPtr<FJsonValue>> SocketsJson;
    for (int32 Index = 0; Index < Mesh->NumSockets(); ++Index)
    {
        const USkeletalMeshSocket* Socket = Mesh->GetSocketByIndex(Index);
        if (Socket)
        {
            TSharedRef<FJsonObject> SocketJson = ExportTransform(Socket->SocketName, Socket->RelativeLocation, Socket->RelativeRotation, Socket->RelativeScale);
            SocketJson->SetStringField(TEXT("Bone"), Socket->BoneName.ToString());
            SocketsJson.Add(MakeShared<FJsonValueObject>(SocketJson));
        }
    }
    Root->SetArrayField(TEXT("Sockets"), SocketsJson);

    return Root;
}
