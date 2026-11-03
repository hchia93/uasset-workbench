#include "Export/LevelExportCommandlet.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

#include "Components/ActorComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Rendering/ColorVertexBuffer.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "StaticMeshComponentLODInfo.h"
#include "UObject/Package.h"

ULevelExportCommandlet::ULevelExportCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 ULevelExportCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("UAssetWorkbench v%s - LevelExport"), UASSET_WORKBENCH_VERSION_STRING);

    TArray<FString> AssetPaths = UAssetWorkbench::ParseAssetPaths(Params);

    if (AssetPaths.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchExporter, Error, TEXT("No maps specified. Usage: -assets=\"/Game/Maps/L_A,/Game/Maps/L_B\""));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    int32 ExportedCount = 0;

    for (const FString& AssetPath : AssetPaths)
    {
        UPackage* Package = LoadPackage(nullptr, *AssetPath, LOAD_None);
        if (!Package)
        {
            UE_LOG(LogUAssetWorkbenchExporter, Warning, TEXT("Failed to load level package: %s"), *AssetPath);
            continue;
        }

        UWorld* World = UWorld::FindWorldInPackage(Package);
        if (!World)
        {
            UE_LOG(LogUAssetWorkbenchExporter, Warning, TEXT("No UWorld in package: %s"), *AssetPath);
            continue;
        }

        World->PersistentLevel->OnLevelLoaded();
        World->PersistentLevel->UpdateLevelComponents(/*bRerunConstructionScripts=*/ false);

        TSharedPtr<FJsonObject> JsonObject = ExportLevel(World, AssetPath);
        if (!JsonObject.IsValid())
        {
            UE_LOG(LogUAssetWorkbenchExporter, Warning, TEXT("Failed to export level: %s"), *AssetPath);
            continue;
        }

        UAssetWorkbench::FExportTarget ExportTarget(AssetPath);
        if (ExportTarget.Save(JsonObject.ToSharedRef()))
        {
            UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Exported: %s -> %s"), *AssetPath, *ExportTarget.GetPath());
            ExportedCount++;
        }
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Export complete. %d/%d levels exported."), ExportedCount, AssetPaths.Num());
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}

TSharedPtr<FJsonObject> ULevelExportCommandlet::ExportLevel(UWorld* World, const FString& LevelPath) const
{
    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

    Root->SetStringField(TEXT("ExporterVersion"), UASSET_WORKBENCH_VERSION_STRING);
    Root->SetStringField(TEXT("ExportType"), TEXT("Level"));
    Root->SetStringField(TEXT("LevelName"), FPaths::GetBaseFilename(LevelPath));
    Root->SetStringField(TEXT("AssetPath"), LevelPath);
    Root->SetStringField(TEXT("ExportTimestamp"), FDateTime::Now().ToString());

    // WorldSettings delta
    if (AWorldSettings* WorldSettings = World->GetWorldSettings())
    {
        TSharedPtr<FJsonObject> WsJson = ExportWorldSettings(WorldSettings);
        if (WsJson.IsValid() && WsJson->Values.Num() > 0)
        {
            Root->SetObjectField(TEXT("WorldSettings"), WsJson);
        }
    }

    // Streaming levels
    const TArray<ULevelStreaming*>& StreamingLevels = World->GetStreamingLevels();
    if (StreamingLevels.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> StreamingArray;
        for (ULevelStreaming* Streaming : StreamingLevels)
        {
            if (!Streaming) continue;
            TSharedPtr<FJsonObject> StreamObj = MakeShared<FJsonObject>();
            StreamObj->SetStringField(TEXT("PackageName"), Streaming->GetWorldAssetPackageName());
            StreamObj->SetStringField(TEXT("Class"), Streaming->GetClass()->GetName());
            StreamObj->SetBoolField(TEXT("ShouldBeLoaded"), Streaming->ShouldBeLoaded());
            StreamObj->SetBoolField(TEXT("ShouldBeVisible"), Streaming->GetShouldBeVisibleFlag());
            AddTransformField(Streaming->LevelTransform, TEXT("Transform"), StreamObj);
            StreamingArray.Add(MakeShared<FJsonValueObject>(StreamObj));
        }
        Root->SetArrayField(TEXT("StreamingLevels"), StreamingArray);
    }

    // Actors
    TArray<TSharedPtr<FJsonValue>> ActorsArray;
    for (AActor* Actor : World->PersistentLevel->Actors)
    {
        if (!Actor)
        {
            continue;
        }
        // Skip ephemeral runtime actors that shouldn't be in a fresh load
        if (Actor->IsA<AWorldSettings>())
        {
            continue;
        }

        TSharedPtr<FJsonObject> ActorJson = ExportActor(Actor);
        if (ActorJson.IsValid())
        {
            ActorsArray.Add(MakeShared<FJsonValueObject>(ActorJson));
        }
    }
    Root->SetNumberField(TEXT("ActorCount"), ActorsArray.Num());
    Root->SetArrayField(TEXT("Actors"), ActorsArray);

    return Root;
}

TSharedPtr<FJsonObject> ULevelExportCommandlet::ExportWorldSettings(AWorldSettings* WorldSettings) const
{
    TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetStringField(TEXT("Class"), WorldSettings->GetClass()->GetName());

    TSharedPtr<FJsonObject> Delta = ExportDeltaProperties(WorldSettings, WorldSettings->GetArchetype());
    if (Delta.IsValid() && Delta->Values.Num() > 0)
    {
        Json->SetObjectField(TEXT("DeltaProperties"), Delta);
    }
    return Json;
}

TSharedPtr<FJsonObject> ULevelExportCommandlet::ExportActor(AActor* Actor) const
{
    TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();

    Json->SetStringField(TEXT("Name"), Actor->GetName());
    Json->SetStringField(TEXT("Label"), Actor->GetActorLabel());
    Json->SetStringField(TEXT("Class"), Actor->GetClass()->GetPathName());

    const FName Folder = Actor->GetFolderPath();
    if (!Folder.IsNone())
    {
        Json->SetStringField(TEXT("Folder"), Folder.ToString());
    }

    if (Actor->Tags.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> TagsArray;
        for (const FName& Tag : Actor->Tags)
        {
            TagsArray.Add(MakeShared<FJsonValueString>(Tag.ToString()));
        }
        Json->SetArrayField(TEXT("Tags"), TagsArray);
    }

    AddTransformField(Actor->GetActorTransform(), TEXT("Transform"), Json);

    // Attach parent (actor-level)
    if (AActor* AttachParent = Actor->GetAttachParentActor())
    {
        TSharedPtr<FJsonObject> AttachJson = MakeShared<FJsonObject>();
        AttachJson->SetStringField(TEXT("Actor"), AttachParent->GetName());
        AttachJson->SetStringField(TEXT("Socket"), Actor->GetAttachParentSocketName().ToString());
        Json->SetObjectField(TEXT("AttachedTo"), AttachJson);
    }

    // Actor-level delta properties (excludes components, they get their own section)
    TSharedPtr<FJsonObject> ActorDelta = ExportDeltaProperties(Actor, Actor->GetArchetype());
    if (ActorDelta.IsValid() && ActorDelta->Values.Num() > 0)
    {
        Json->SetObjectField(TEXT("DeltaProperties"), ActorDelta);
    }

    // Components
    TArray<UActorComponent*> Components;
    Actor->GetComponents(Components);
    if (Components.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> CompsArray;
        for (UActorComponent* Comp : Components)
        {
            if (!Comp) continue;
            TSharedPtr<FJsonObject> CompJson = ExportComponent(Comp);
            if (CompJson.IsValid())
            {
                CompsArray.Add(MakeShared<FJsonValueObject>(CompJson));
            }
        }
        Json->SetArrayField(TEXT("Components"), CompsArray);
    }

    return Json;
}

TSharedPtr<FJsonObject> ULevelExportCommandlet::ExportComponent(UActorComponent* Component) const
{
    TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();

    Json->SetStringField(TEXT("Name"), Component->GetName());
    Json->SetStringField(TEXT("Class"), Component->GetClass()->GetPathName());
    Json->SetBoolField(TEXT("IsEditorOnly"), Component->IsEditorOnly());

    // Scene hierarchy
    if (USceneComponent* SceneComp = Cast<USceneComponent>(Component))
    {
        if (USceneComponent* AttachParent = SceneComp->GetAttachParent())
        {
            TSharedPtr<FJsonObject> AttachJson = MakeShared<FJsonObject>();
            AttachJson->SetStringField(TEXT("Component"), AttachParent->GetName());
            const FName SocketName = SceneComp->GetAttachSocketName();
            if (!SocketName.IsNone())
            {
                AttachJson->SetStringField(TEXT("Socket"), SocketName.ToString());
            }
            Json->SetObjectField(TEXT("AttachParent"), AttachJson);
        }

        const FTransform& RelTransform = SceneComp->GetRelativeTransform();
        if (!RelTransform.Equals(FTransform::Identity))
        {
            AddTransformField(RelTransform, TEXT("RelativeTransform"), Json);
        }

        Json->SetStringField(TEXT("Mobility"),
            StaticEnum<EComponentMobility::Type>()->GetNameStringByValue(SceneComp->Mobility));
    }

    // Static mesh ref hoisted to top for easy grep
    if (UStaticMeshComponent* SmComp = Cast<UStaticMeshComponent>(Component))
    {
        if (UStaticMesh* Mesh = SmComp->GetStaticMesh())
        {
            Json->SetStringField(TEXT("StaticMesh"), Mesh->GetPathName());
        }

        // Vertex paint per-LOD: OverrideVertexColors is NOT a UProperty (raw FColorVertexBuffer*),
        // so DeltaProperties cannot see it. Surface it explicitly with a CRC so audit can compare.
        TArray<TSharedPtr<FJsonValue>> PaintLODs;
        for (int32 LODIdx = 0; LODIdx < SmComp->LODData.Num(); ++LODIdx)
        {
            const FStaticMeshComponentLODInfo& LOD = SmComp->LODData[LODIdx];
            FColorVertexBuffer* CVB = LOD.OverrideVertexColors;
            if (!CVB || CVB->GetNumVertices() == 0)
            {
                continue;
            }
            const int32 NumVerts = CVB->GetNumVertices();
            TArray<FColor> Colors;
            Colors.SetNumUninitialized(NumVerts);
            for (int32 v = 0; v < NumVerts; ++v)
            {
                Colors[v] = CVB->VertexColor(v);
            }
            const uint32 CRC = FCrc::MemCrc32(Colors.GetData(), NumVerts * sizeof(FColor));

            TSharedPtr<FJsonObject> LODJson = MakeShared<FJsonObject>();
            LODJson->SetNumberField(TEXT("LOD"), LODIdx);
            LODJson->SetNumberField(TEXT("NumVertices"), NumVerts);
            LODJson->SetStringField(TEXT("ColorCRC32"), FString::Printf(TEXT("%08x"), CRC));
            PaintLODs.Add(MakeShared<FJsonValueObject>(LODJson));
        }
        if (PaintLODs.Num() > 0)
        {
            Json->SetArrayField(TEXT("VertexPaintLODs"), PaintLODs);
        }
    }

    // Collision hoisted
    if (UPrimitiveComponent* PrimComp = Cast<UPrimitiveComponent>(Component))
    {
        Json->SetStringField(TEXT("CollisionProfile"), PrimComp->GetCollisionProfileName().ToString());
        Json->SetStringField(TEXT("CollisionEnabled"),
            StaticEnum<ECollisionEnabled::Type>()->GetNameStringByValue(PrimComp->GetCollisionEnabled()));
        Json->SetBoolField(TEXT("GenerateOverlapEvents"), PrimComp->GetGenerateOverlapEvents());

        // CustomPrimitiveData: UPROPERTY but accessed via getter to honour any per-instance default
        // initialization. Surface as a float array. Downstream audit compares element-by-element.
        const TArray<float>& CPD = PrimComp->GetCustomPrimitiveData().Data;
        if (CPD.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> CPDArr;
            CPDArr.Reserve(CPD.Num());
            for (float F : CPD)
            {
                CPDArr.Add(MakeShared<FJsonValueNumber>(F));
            }
            Json->SetArrayField(TEXT("CustomPrimitiveData"), CPDArr);
        }
    }

    // ISM / HISM / Foliage: cap per-instance data
    if (UInstancedStaticMeshComponent* ISMComp = Cast<UInstancedStaticMeshComponent>(Component))
    {
        AddInstancedComponentData(ISMComp, Json);
    }

    TSharedPtr<FJsonObject> Delta = ExportDeltaProperties(Component, Component->GetArchetype());
    if (Delta.IsValid() && Delta->Values.Num() > 0)
    {
        Json->SetObjectField(TEXT("DeltaProperties"), Delta);
    }

    return Json;
}

void ULevelExportCommandlet::AddInstancedComponentData(UInstancedStaticMeshComponent* ISMComp, TSharedPtr<FJsonObject>& OutJson) const
{
    const int32 InstanceCount = ISMComp->GetInstanceCount();
    OutJson->SetNumberField(TEXT("InstanceCount"), InstanceCount);

    FBox Bounds(ForceInit);
    const int32 Sample = FMath::Min(InstanceCount, kInstanceSampleCount);
    TArray<TSharedPtr<FJsonValue>> Samples;

    for (int32 i = 0; i < InstanceCount; ++i)
    {
        FTransform InstXform;
        if (!ISMComp->GetInstanceTransform(i, InstXform, /*bWorldSpace=*/ false)) continue;
        Bounds += InstXform.GetLocation();

        if (i < Sample)
        {
            TSharedPtr<FJsonObject> XformJson = MakeShared<FJsonObject>();
            AddTransformField(InstXform, TEXT("Transform"), XformJson);
            XformJson->SetNumberField(TEXT("Index"), i);
            Samples.Add(MakeShared<FJsonValueObject>(XformJson));
        }
    }

    if (InstanceCount > 0)
    {
        TSharedPtr<FJsonObject> BoundsJson = MakeShared<FJsonObject>();
        const FVector Min = Bounds.Min;
        const FVector Max = Bounds.Max;
        BoundsJson->SetStringField(TEXT("Min"), FString::Printf(TEXT("(%.2f,%.2f,%.2f)"), Min.X, Min.Y, Min.Z));
        BoundsJson->SetStringField(TEXT("Max"), FString::Printf(TEXT("(%.2f,%.2f,%.2f)"), Max.X, Max.Y, Max.Z));
        OutJson->SetObjectField(TEXT("InstanceBounds"), BoundsJson);
    }

    if (InstanceCount <= kInstanceDumpThreshold)
    {
        // Dump all
        TArray<TSharedPtr<FJsonValue>> AllInstances;
        for (int32 i = 0; i < InstanceCount; ++i)
        {
            FTransform InstXform;
            if (!ISMComp->GetInstanceTransform(i, InstXform, false)) continue;
            TSharedPtr<FJsonObject> XformJson = MakeShared<FJsonObject>();
            AddTransformField(InstXform, TEXT("Transform"), XformJson);
            XformJson->SetNumberField(TEXT("Index"), i);
            AllInstances.Add(MakeShared<FJsonValueObject>(XformJson));
        }
        OutJson->SetArrayField(TEXT("Instances"), AllInstances);
    }
    else
    {
        // Just samples
        OutJson->SetArrayField(TEXT("InstanceSamples"), Samples);
    }
}

TSharedPtr<FJsonObject> ULevelExportCommandlet::ExportDeltaProperties(UObject* Object, UObject* Archetype) const
{
    TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
    if (!Object || !Archetype) return Props;

    UClass* Class = Object->GetClass();
    for (TFieldIterator<FProperty> PropIt(Class); PropIt; ++PropIt)
    {
        FProperty* Prop = *PropIt;

        if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_EditorOnly)) continue;
        if (Prop->HasAnyPropertyFlags(CPF_InstancedReference))
        {
            // Components have their own section. Other instanced subobjects (nav system config, etc)
            // carry real level data and would vanish entirely under a blanket skip.
            FObjectProperty* ObjectProp = CastField<FObjectProperty>(Prop);
            if (!ObjectProp || ObjectProp->PropertyClass->IsChildOf(UActorComponent::StaticClass()))
            {
                continue;
            }

            if (UObject* SubObject = ObjectProp->GetObjectPropertyValue(Prop->ContainerPtrToValuePtr<void>(Object)))
            {
                Props->SetObjectField(Prop->GetName(), ExportInstancedSubobject(SubObject));
            }
            continue;
        }

        void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Object);
        void* ArchetypePtr = (Archetype->GetClass() == Class || Archetype->IsA(Class))
            ? Prop->ContainerPtrToValuePtr<void>(Archetype)
            : nullptr;

        if (ArchetypePtr && Prop->Identical(ValuePtr, ArchetypePtr, PPF_DeepComparison))
        {
            continue;
        }

        FString Value;
        Prop->ExportTextItem_Direct(Value, ValuePtr, ArchetypePtr, Object, PPF_None);
        if (!Value.IsEmpty())
        {
            Props->SetStringField(Prop->GetName(), Value);
        }
    }
    return Props;
}

TSharedPtr<FJsonObject> ULevelExportCommandlet::ExportInstancedSubobject(UObject* SubObject) const
{
    TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetStringField(TEXT("Class"), SubObject->GetClass()->GetPathName());

    // Absolute values, not a delta. These defaults come from project settings, so a delta hides
    // exactly the case worth checking, a map that merely agrees with the current ini.
    TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
    for (TFieldIterator<FProperty> PropIt(SubObject->GetClass()); PropIt; ++PropIt)
    {
        FProperty* Prop = *PropIt;
        if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
        {
            continue;
        }

        FString Value;
        Value += UAssetWorkbench::ExportPropertyText(Prop, Prop->ContainerPtrToValuePtr<void>(SubObject), SubObject);
        if (!Value.IsEmpty())
        {
            Props->SetStringField(Prop->GetName(), Value);
        }
    }

    if (Props->Values.Num() > 0)
    {
        Json->SetObjectField(TEXT("Properties"), Props);
    }

    return Json;
}

void ULevelExportCommandlet::AddTransformField(const FTransform& Transform, const FString& FieldName, TSharedPtr<FJsonObject>& OutJson) const
{
    TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
    const FVector Loc = Transform.GetLocation();
    const FRotator Rot = Transform.Rotator();
    const FVector Scale = Transform.GetScale3D();
    Json->SetStringField(TEXT("Loc"), FString::Printf(TEXT("(%.3f,%.3f,%.3f)"), Loc.X, Loc.Y, Loc.Z));
    Json->SetStringField(TEXT("Rot"), FString::Printf(TEXT("(P=%.3f,Y=%.3f,R=%.3f)"), Rot.Pitch, Rot.Yaw, Rot.Roll));
    Json->SetStringField(TEXT("Scale"), FString::Printf(TEXT("(%.3f,%.3f,%.3f)"), Scale.X, Scale.Y, Scale.Z));
    OutJson->SetObjectField(FieldName, Json);
}

