#include "Export/BehaviorTreeExportCommandlet.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTTaskNode.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
    void AddModifyOp(const TSharedPtr<FJsonObject>& Node, TArray<TSharedPtr<FJsonValue>>& OutOps)
    {
        if (!Node.IsValid())
        {
            return;
        }

        TSharedRef<FJsonObject> Op = MakeShared<FJsonObject>();
        Op->SetStringField(TEXT("Op"), TEXT("Modify"));
        Op->SetStringField(TEXT("Node"), Node->GetStringField(TEXT("Id")));
        Op->SetStringField(TEXT("Name"), Node->GetStringField(TEXT("Name")));

        const TSharedPtr<FJsonObject>* Parameters = nullptr;
        if (Node->TryGetObjectField(TEXT("Parameters"), Parameters))
        {
            Op->SetObjectField(TEXT("Properties"), *Parameters);
        }
        OutOps.Add(MakeShared<FJsonValueObject>(Op));
    }

    void AddSubNodeOps(const TSharedPtr<FJsonObject>& Owner, const TCHAR* Field, TArray<TSharedPtr<FJsonValue>>& OutOps)
    {
        const TArray<TSharedPtr<FJsonValue>>* SubNodes = nullptr;
        if (Owner.IsValid() && Owner->TryGetArrayField(Field, SubNodes))
        {
            for (const TSharedPtr<FJsonValue>& SubNode : *SubNodes)
            {
                AddModifyOp(SubNode->AsObject(), OutOps);
            }
        }
    }

    void AddTreeOps(const TSharedPtr<FJsonObject>& Node, TArray<TSharedPtr<FJsonValue>>& OutOps)
    {
        AddModifyOp(Node, OutOps);
        AddSubNodeOps(Node, TEXT("Services"), OutOps);

        const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
        if (!Node->TryGetArrayField(TEXT("Children"), Children))
        {
            return;
        }

        for (const TSharedPtr<FJsonValue>& ChildValue : *Children)
        {
            const TSharedPtr<FJsonObject> Child = ChildValue->AsObject();
            AddSubNodeOps(Child, TEXT("Decorators"), OutOps);

            const TSharedPtr<FJsonObject>* ChildNode = nullptr;
            if (Child->TryGetObjectField(TEXT("Node"), ChildNode))
            {
                AddTreeOps(*ChildNode, OutOps);
            }
        }
    }

    // Every node as a Modify op holding its current name and parameters, so the file fed back unchanged writes nothing.
    // Root decorators are injected by a parent tree and have no graph node here, they stay out.
    TSharedRef<FJsonObject> BuildEditSpec(const TSharedRef<FJsonObject>& Export)
    {
        TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
        Target->SetStringField(TEXT("AssetPath"), Export->GetStringField(TEXT("AssetPath")));

        FString Blackboard;
        if (Export->TryGetStringField(TEXT("BlackboardAsset"), Blackboard))
        {
            Target->SetStringField(TEXT("Blackboard"), Blackboard);
        }

        TArray<TSharedPtr<FJsonValue>> Ops;
        const TSharedPtr<FJsonObject>* RootNode = nullptr;
        if (Export->TryGetObjectField(TEXT("RootNode"), RootNode))
        {
            AddTreeOps(*RootNode, Ops);
        }
        Target->SetArrayField(TEXT("Nodes"), Ops);

        TSharedRef<FJsonObject> Spec = MakeShared<FJsonObject>();
        Spec->SetArrayField(TEXT("Targets"), { MakeShared<FJsonValueObject>(Target) });
        return Spec;
    }
}

UBehaviorTreeExportCommandlet::UBehaviorTreeExportCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UBehaviorTreeExportCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("UAssetWorkbench v%s - BehaviorTreeExport"), UASSET_WORKBENCH_VERSION_STRING);

    TArray<FString> AssetPaths = UAssetWorkbench::ParseAssetPaths(Params);

    if (AssetPaths.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchExporter, Error, TEXT("No assets specified. Usage: -assets=\"/Game/Path/BT_A,/Game/Path/BT_B\""));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    int32 ExportedCount = 0;

    for (const FString& AssetPath : AssetPaths)
    {
        UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *AssetPath);
        if (!BT)
        {
            UE_LOG(LogUAssetWorkbenchExporter, Warning, TEXT("Failed to load BehaviorTree: %s"), *AssetPath);
            continue;
        }

        TSharedPtr<FJsonObject> JsonObject = ExportBehaviorTree(BT);
        if (!JsonObject.IsValid())
        {
            UE_LOG(LogUAssetWorkbenchExporter, Warning, TEXT("Failed to export BehaviorTree: %s"), *AssetPath);
            continue;
        }

        UAssetWorkbench::FExportTarget ExportTarget(AssetPath);
        if (ExportTarget.Save(JsonObject.ToSharedRef()))
        {
            UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Exported: %s -> %s"), *AssetPath, *ExportTarget.GetPath());
            ExportedCount++;

            // Saved after the stamp so it shares the export's revision and time.
            const FString SpecPath = FPaths::ChangeExtension(ExportTarget.GetPath(), TEXT("spec.json"));
            if (UAssetWorkbench::SaveJsonToFile(BuildEditSpec(JsonObject.ToSharedRef()), SpecPath))
            {
                UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Edit spec: %s"), *SpecPath);
            }
        }
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Export complete. %d/%d behavior trees exported."), ExportedCount, AssetPaths.Num());
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}

TSharedPtr<FJsonObject> UBehaviorTreeExportCommandlet::ExportBehaviorTree(UBehaviorTree* BT) const
{
    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

    Root->SetStringField(TEXT("ExporterVersion"), UASSET_WORKBENCH_VERSION_STRING);
    Root->SetStringField(TEXT("ExportType"), TEXT("BehaviorTree"));
    Root->SetStringField(TEXT("BehaviorTreeName"), BT->GetName());
    Root->SetStringField(TEXT("AssetPath"), BT->GetPathName());
    Root->SetStringField(TEXT("ExportTimestamp"), FDateTime::Now().ToString());

    // Blackboard asset
    if (UBlackboardData* BB = BT->BlackboardAsset)
    {
        Root->SetStringField(TEXT("BlackboardAsset"), BB->GetPathName());

        TArray<TSharedPtr<FJsonValue>> KeysArray;
        TArray<FBlackboardEntry> AllKeys;

        // Include parent keys
        UBlackboardData* CurrentBB = BB;
        while (CurrentBB)
        {
            for (const FBlackboardEntry& Key : CurrentBB->Keys)
            {
                AllKeys.Add(Key);
            }
            CurrentBB = CurrentBB->Parent;
        }

        for (const FBlackboardEntry& Key : AllKeys)
        {
            TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
            KeyObj->SetStringField(TEXT("Name"), Key.EntryName.ToString());
            if (Key.KeyType)
            {
                KeyObj->SetStringField(TEXT("Type"), Key.KeyType->GetClass()->GetName());
            }
            KeysArray.Add(MakeShared<FJsonValueObject>(KeyObj));
        }
        Root->SetArrayField(TEXT("BlackboardKeys"), KeysArray);
    }

    // Root decorators
    if (BT->RootDecorators.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> RootDecoratorsArray;
        for (UBTDecorator* Decorator : BT->RootDecorators)
        {
            if (Decorator)
            {
                RootDecoratorsArray.Add(MakeShared<FJsonValueObject>(ExportNode(Decorator)));
            }
        }
        Root->SetArrayField(TEXT("RootDecorators"), RootDecoratorsArray);
    }

    // Tree structure
    if (BT->RootNode)
    {
        Root->SetObjectField(TEXT("RootNode"), ExportCompositeNode(BT->RootNode));
    }

    return Root;
}

TSharedPtr<FJsonObject> UBehaviorTreeExportCommandlet::ExportCompositeNode(UBTCompositeNode* Node) const
{
    TSharedPtr<FJsonObject> Obj = ExportNode(Node);
    if (!Obj.IsValid())
    {
        return nullptr;
    }

    // Services on this composite
    if (Node->Services.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> ServicesArray;
        for (UBTService* Service : Node->Services)
        {
            if (Service)
            {
                ServicesArray.Add(MakeShared<FJsonValueObject>(ExportNode(Service)));
            }
        }
        Obj->SetArrayField(TEXT("Services"), ServicesArray);
    }

    // Children
    TArray<TSharedPtr<FJsonValue>> ChildrenArray;
    for (int32 i = 0; i < Node->Children.Num(); i++)
    {
        TSharedPtr<FJsonObject> ChildObj = ExportChildEntry(Node->Children[i]);
        if (ChildObj.IsValid())
        {
            ChildObj->SetNumberField(TEXT("ChildIndex"), i);
            ChildrenArray.Add(MakeShared<FJsonValueObject>(ChildObj));
        }
    }
    Obj->SetArrayField(TEXT("Children"), ChildrenArray);

    return Obj;
}

TSharedPtr<FJsonObject> UBehaviorTreeExportCommandlet::ExportChildEntry(const FBTCompositeChild& Child) const
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

    // Decorators on this child branch
    if (Child.Decorators.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> DecoratorsArray;
        for (UBTDecorator* Decorator : Child.Decorators)
        {
            if (Decorator)
            {
                DecoratorsArray.Add(MakeShared<FJsonValueObject>(ExportNode(Decorator)));
            }
        }
        Obj->SetArrayField(TEXT("Decorators"), DecoratorsArray);
    }

    // Child node (either composite or task)
    if (Child.ChildComposite)
    {
        Obj->SetObjectField(TEXT("Node"), ExportCompositeNode(Child.ChildComposite));
    }
    else if (Child.ChildTask)
    {
        TSharedPtr<FJsonObject> TaskObj = ExportNode(Child.ChildTask);

        // Services on tasks
        if (TaskObj.IsValid() && Child.ChildTask->Services.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> ServicesArray;
            for (UBTService* Service : Child.ChildTask->Services)
            {
                if (Service)
                {
                    ServicesArray.Add(MakeShared<FJsonValueObject>(ExportNode(Service)));
                }
            }
            TaskObj->SetArrayField(TEXT("Services"), ServicesArray);
        }

        Obj->SetObjectField(TEXT("Node"), TaskObj);
    }

    return Obj;
}

TSharedPtr<FJsonObject> UBehaviorTreeExportCommandlet::ExportNode(UBTNode* Node) const
{
    if (!Node)
    {
        return nullptr;
    }

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

    // The node object name is what EditBehaviorTree addresses, display names repeat across a tree.
    Obj->SetStringField(TEXT("Id"), Node->GetName());
    Obj->SetStringField(TEXT("Name"), Node->GetNodeName());
    Obj->SetStringField(TEXT("Class"), Node->GetClass()->GetName());
    Obj->SetStringField(TEXT("Description"), Node->GetStaticDescription());

    // Subclass properties (project-specific parameters)
    UClass* StopAtClass = UBTNode::StaticClass();

    // Use more specific stop class if applicable
    if (Node->IsA<UBTCompositeNode>())
    {
        StopAtClass = UBTCompositeNode::StaticClass();
    }
    else if (Node->IsA<UBTTaskNode>())
    {
        StopAtClass = UBTTaskNode::StaticClass();
    }
    else if (Node->IsA<UBTDecorator>())
    {
        StopAtClass = UBTDecorator::StaticClass();
    }
    else if (Node->IsA<UBTService>())
    {
        StopAtClass = UBTService::StaticClass();
    }

    TSharedPtr<FJsonObject> Props = ExportSubclassProperties(Node, StopAtClass);
    if (Props.IsValid() && Props->Values.Num() > 0)
    {
        Obj->SetObjectField(TEXT("Parameters"), Props);
    }

    return Obj;
}

TSharedPtr<FJsonObject> UBehaviorTreeExportCommandlet::ExportSubclassProperties(UObject* Object, UClass* StopAtClass) const
{
    TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();

    UClass* CurrentClass = Object->GetClass();
    while (CurrentClass && CurrentClass != StopAtClass)
    {
        for (TFieldIterator<FProperty> PropIt(CurrentClass, EFieldIteratorFlags::ExcludeSuper); PropIt; ++PropIt)
        {
            FProperty* Prop = *PropIt;
            if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
            {
                continue;
            }

            FString Value;
            Prop->ExportTextItem_Direct(Value, Prop->ContainerPtrToValuePtr<void>(Object), nullptr, Object, PPF_None);
            if (!Value.IsEmpty())
            {
                Props->SetStringField(Prop->GetName(), Value);
            }
        }
        CurrentClass = CurrentClass->GetSuperClass();
    }

    return Props;
}

