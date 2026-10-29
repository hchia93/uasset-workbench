#include "Edit/EditBehaviorTreeCommandlet.h"

// Editor-only by design: edits the Behavior Tree editor graph and rebuilds the runtime tree from it. Trap any Runtime-type drift early.
static_assert(WITH_EDITOR, "UAssetWorkbench commandlets are editor-only, keep the uplugin Module Type=Editor.");

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeTypes.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/Composites/BTComposite_SimpleParallel.h"
#include "BehaviorTree/Tasks/BTTask_RunBehavior.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_Decorator.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Service.h"
#include "BehaviorTreeGraphNode_SubtreeTask.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

namespace
{
    constexpr int32 kChildSpacing = 300;
    constexpr int32 kChildDepth = 200;

    // Same name the Behavior Tree editor gives the graph it creates on first open.
    const TCHAR* kGraphName = TEXT("Behavior Tree");

    // The parallel graph node is not exported from BehaviorTreeEditor, so it is looked up by path.
    const TCHAR* kSimpleParallelGraphNodePath = TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_SimpleParallel");

    struct FBTEditContext
    {
        UBehaviorTree* Tree = nullptr;
        UBehaviorTreeGraph* Graph = nullptr;
        FString AssetPath;

        // Spec Ids given to nodes added earlier in this run.
        TMap<FString, UBehaviorTreeGraphNode*> Aliases;
        int32 Ops = 0;

        // Entries that already held the given value. A whole exported spec fed back counts here instead of in Ops.
        int32 Unchanged = 0;
    };

    // Mirrors what the editor does when it opens the asset, a tree built in code has no graph yet.
    UBehaviorTreeGraph* PrepareGraph(UBehaviorTree* Tree)
    {
        UBehaviorTreeGraph* Graph = Cast<UBehaviorTreeGraph>(Tree->BTGraph);
        if (!Graph)
        {
            Tree->BTGraph = FBlueprintEditorUtils::CreateNewGraph(Tree, kGraphName, UBehaviorTreeGraph::StaticClass(), UEdGraphSchema_BehaviorTree::StaticClass());
            Graph = Cast<UBehaviorTreeGraph>(Tree->BTGraph);
            Graph->GetSchema()->CreateDefaultNodesForGraph(*Graph);
            Graph->OnCreated();
        }
        else
        {
            Graph->OnLoaded();
        }

        Graph->Initialize();
        return Graph;
    }

    UBehaviorTreeGraphNode* FindRoot(const FBTEditContext& Context)
    {
        for (UEdGraphNode* Node : Context.Graph->Nodes)
        {
            if (UBehaviorTreeGraphNode_Root* Root = Cast<UBehaviorTreeGraphNode_Root>(Node))
            {
                return Root;
            }
        }

        return nullptr;
    }

    bool IsSimpleParallel(const UBehaviorTreeGraphNode* Node)
    {
        return Node && Cast<UBTComposite_SimpleParallel>(Node->NodeInstance) != nullptr;
    }

    FString DescribeNode(const UBehaviorTreeGraphNode* Node)
    {
        if (Cast<UBehaviorTreeGraphNode_Root>(Node))
        {
            return TEXT("Root");
        }

        return Node && Node->NodeInstance ? Node->NodeInstance->GetName() : TEXT("<none>");
    }

    // Root, a spec Id from this run, or the node object name BehaviorTreeExport prints as Id.
    UBehaviorTreeGraphNode* ResolveNode(const FBTEditContext& Context, const FString& Key)
    {
        if (Key == TEXT("Root"))
        {
            return FindRoot(Context);
        }

        if (UBehaviorTreeGraphNode* const* Alias = Context.Aliases.Find(Key))
        {
            return *Alias;
        }

        for (UEdGraphNode* EdNode : Context.Graph->Nodes)
        {
            UBehaviorTreeGraphNode* Node = Cast<UBehaviorTreeGraphNode>(EdNode);
            if (!Node)
            {
                continue;
            }

            if (Node->NodeInstance && Node->NodeInstance->GetName() == Key)
            {
                return Node;
            }

            for (UAIGraphNode* SubNode : Node->SubNodes)
            {
                UBehaviorTreeGraphNode* Typed = Cast<UBehaviorTreeGraphNode>(SubNode);
                if (Typed && Typed->NodeInstance && Typed->NodeInstance->GetName() == Key)
                {
                    return Typed;
                }
            }
        }

        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("No node %s in %s. Use Root, an Id from BehaviorTreeExport, or an Id given earlier in the spec"), *Key, *Context.AssetPath);
        return nullptr;
    }

    UBehaviorTreeGraphNode* FindSubNodeOwner(const FBTEditContext& Context, const UBehaviorTreeGraphNode* SubNode)
    {
        for (UEdGraphNode* EdNode : Context.Graph->Nodes)
        {
            UBehaviorTreeGraphNode* Node = Cast<UBehaviorTreeGraphNode>(EdNode);
            if (Node && Node->SubNodes.Contains(SubNode))
            {
                return Node;
            }
        }

        return nullptr;
    }

    // The runtime tree orders children by the graph X position, not by link order.
    TArray<UBehaviorTreeGraphNode*> GetChildren(UBehaviorTreeGraphNode* Parent)
    {
        TArray<UBehaviorTreeGraphNode*> Children;
        UEdGraphPin* Out = Parent->GetOutputPin();
        if (!Out)
        {
            return Children;
        }

        for (UEdGraphPin* Linked : Out->LinkedTo)
        {
            if (UBehaviorTreeGraphNode* Child = Linked ? Cast<UBehaviorTreeGraphNode>(Linked->GetOwningNode()) : nullptr)
            {
                Children.Add(Child);
            }
        }

        Children.Sort([](const UBehaviorTreeGraphNode& A, const UBehaviorTreeGraphNode& B) { return A.NodePosX < B.NodePosX; });
        return Children;
    }

    UBehaviorTreeGraphNode* GetParent(UBehaviorTreeGraphNode* Node)
    {
        UEdGraphPin* In = Node->GetInputPin();
        if (!In || In->LinkedTo.IsEmpty() || !In->LinkedTo[0])
        {
            return nullptr;
        }

        return Cast<UBehaviorTreeGraphNode>(In->LinkedTo[0]->GetOwningNode());
    }

    void ShiftSubtree(UBehaviorTreeGraphNode* Node, int32 DeltaX, int32 DeltaY)
    {
        Node->Modify();
        Node->NodePosX += DeltaX;
        Node->NodePosY += DeltaY;
        for (UBehaviorTreeGraphNode* Child : GetChildren(Node))
        {
            ShiftSubtree(Child, DeltaX, DeltaY);
        }
    }

    // Spreads the children left to right in the given order. Each subtree moves with its node.
    void LayoutChildren(UBehaviorTreeGraphNode* Parent, const TArray<UBehaviorTreeGraphNode*>& Ordered)
    {
        for (int32 Index = 0; Index < Ordered.Num(); ++Index)
        {
            const int32 TargetX = Parent->NodePosX + Index * kChildSpacing;
            const int32 TargetY = Parent->NodePosY + kChildDepth;
            ShiftSubtree(Ordered[Index], TargetX - Ordered[Index]->NodePosX, TargetY - Ordered[Index]->NodePosY);
        }
    }

    void PlaceChild(UBehaviorTreeGraphNode* Parent, UBehaviorTreeGraphNode* Child, int32 Index)
    {
        TArray<UBehaviorTreeGraphNode*> Ordered = GetChildren(Parent);
        Ordered.Remove(Child);
        Ordered.Insert(Child, Index < 0 ? Ordered.Num() : FMath::Min(Index, Ordered.Num()));
        LayoutChildren(Parent, Ordered);
    }

    bool IsInSubtree(UBehaviorTreeGraphNode* Root, const UBehaviorTreeGraphNode* Candidate)
    {
        if (Root == Candidate)
        {
            return true;
        }

        for (UBehaviorTreeGraphNode* Child : GetChildren(Root))
        {
            if (IsInSubtree(Child, Candidate))
            {
                return true;
            }
        }

        return false;
    }

    UClass* ResolveNodeClass(const FString& Name, UClass* Base, const TCHAR* Label)
    {
        UClass* Class = UAssetWorkbench::ResolveClass(Name);
        if (!Class || !Class->IsChildOf(Base) || Class->HasAnyClassFlags(CLASS_Abstract))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s is not a concrete %s class"), *Name, Label);
            return nullptr;
        }

        return Class;
    }

    bool CheckParentTakesChild(const FBTEditContext& Context, UBehaviorTreeGraphNode* Parent, UClass* ChildClass, const UBehaviorTreeGraphNode* Moving)
    {
        if (Cast<UBehaviorTreeGraphNode_Root>(Parent))
        {
            if (!ChildClass->IsChildOf(UBTCompositeNode::StaticClass()))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Root takes a composite, %s is not one"), *ChildClass->GetName());
                return false;
            }

            const TArray<UBehaviorTreeGraphNode*> Children = GetChildren(Parent);
            const bool bOccupied = Children.Num() > 0 && !(Children.Num() == 1 && Children[0] == Moving);
            if (bOccupied)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Root already has a child in %s"), *Context.AssetPath);
                return false;
            }
            return true;
        }

        if (!Cast<UBTCompositeNode>(Parent->NodeInstance))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s is not a composite and takes no children"), *DescribeNode(Parent));
            return false;
        }

        if (IsSimpleParallel(Parent))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s is a Simple Parallel, its main task and background pins are not editable here"), *DescribeNode(Parent));
            return false;
        }

        return true;
    }

    bool RegisterAlias(FBTEditContext& Context, const TSharedPtr<FJsonObject>& Op, UBehaviorTreeGraphNode* Node)
    {
        FString Id;
        if (!Op->TryGetStringField(TEXT("Id"), Id))
        {
            return true;
        }

        if (Context.Aliases.Contains(Id))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Id %s is given twice in the spec"), *Id);
            return false;
        }

        Context.Aliases.Add(Id, Node);
        return true;
    }

    // A key selector takes a bare key name and resolves it against the tree's blackboard, any other value is plain text.
    bool ApplyKeySelector(FBTEditContext& Context, UBTNode* Instance, FBlackboardKeySelector& Selector, const FString& Key, const FString& KeyName)
    {
        if (Selector.SelectedKeyName == FName(*KeyName))
        {
            ++Context.Unchanged;
            return true;
        }

        UBlackboardData* Blackboard = Context.Tree->BlackboardAsset;
        if (!Blackboard)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s %s: the tree has no blackboard asset to resolve %s against"), *Instance->GetName(), *Key, *KeyName);
            return false;
        }

        Instance->Modify();
        Selector.SelectedKeyName = FName(*KeyName);
        Selector.ResolveSelectedKey(*Blackboard);
        if (!Selector.IsSet())
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s %s: %s is not on %s, or its type is not allowed here"), *Instance->GetName(), *Key, *KeyName, *Blackboard->GetName());
            return false;
        }

        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s %s %s: -> %s"), *Context.AssetPath, *Instance->GetName(), *Key, *KeyName);
        ++Context.Ops;
        return true;
    }

    // FValueOrBBKey_Float and its siblings take a bare value for their DefaultValue, the way the details panel shows them.
    bool IsValueOrKeyStruct(const FStructProperty* Property)
    {
        return Property && Property->Struct->GetName().StartsWith(TEXT("ValueOrBBKey"));
    }

    // Checked by type, TryGetBool also succeeds on numbers and strings.
    FString JsonValueText(const TSharedPtr<FJsonValue>& Value)
    {
        if (Value->Type == EJson::Boolean)
        {
            return Value->AsBool() ? TEXT("True") : TEXT("False");
        }

        return Value->AsString();
    }

    // Compares by value, so 1.5 and 1.500000 count as the same and a fed-back export stays a no-op.
    bool HoldsImportedValue(const FProperty* Property, UObject* Owner, const FString& Text)
    {
        const void* Current = Property->ContainerPtrToValuePtr<void>(Owner);
        void* Temp = FMemory::Malloc(Property->GetSize(), Property->GetMinAlignment());
        Property->InitializeValue(Temp);
        const bool bImported = Property->ImportText_Direct(*Text, Temp, Owner, PPF_None) != nullptr;
        const bool bSame = bImported && Property->Identical(Current, Temp);
        Property->DestroyValue(Temp);
        FMemory::Free(Temp);
        return bSame;
    }

    bool ApplyNodeFields(FBTEditContext& Context, UBehaviorTreeGraphNode* Node, const TSharedPtr<FJsonObject>& Op)
    {
        UBTNode* Instance = Cast<UBTNode>(Node->NodeInstance);
        if (!Instance)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no node instance to write"), *DescribeNode(Node));
            return false;
        }

        FString Name;
        if (Op->TryGetStringField(TEXT("Name"), Name))
        {
            if (Name == Instance->GetNodeName())
            {
                ++Context.Unchanged;
            }
            else
            {
                Instance->Modify();
                UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s %s Name: %s -> %s"), *Context.AssetPath, *Instance->GetName(), *Instance->GetNodeName(), *Name);
                Instance->NodeName = Name;
                ++Context.Ops;
            }
        }

        const TSharedPtr<FJsonObject>* Properties = nullptr;
        if (!Op->TryGetObjectField(TEXT("Properties"), Properties))
        {
            return true;
        }

        TSharedPtr<FJsonObject> Pending = MakeShared<FJsonObject>();
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Properties)->Values)
        {
            FString Text;
            const bool bIsText = Pair.Value->TryGetString(Text);

            const FStructProperty* StructProperty = FindFProperty<FStructProperty>(Instance->GetClass(), *Pair.Key);
            const bool bKeySelector = StructProperty && StructProperty->Struct == FBlackboardKeySelector::StaticStruct();
            if (bKeySelector && bIsText && !Text.StartsWith(TEXT("(")))
            {
                FBlackboardKeySelector* Selector = StructProperty->ContainerPtrToValuePtr<FBlackboardKeySelector>(Instance);
                if (!ApplyKeySelector(Context, Instance, *Selector, Pair.Key, Text))
                {
                    return false;
                }
                continue;
            }

            TSharedPtr<FJsonValue> Value = Pair.Value;
            const bool bBareValue = !bIsText || !Text.StartsWith(TEXT("("));
            if (IsValueOrKeyStruct(StructProperty) && bBareValue)
            {
                Value = MakeShared<FJsonValueString>(FString::Printf(TEXT("(DefaultValue=%s)"), *JsonValueText(Pair.Value)));
            }

            // Export text catches key selectors, whose resolved fields never reach the text. Value compare catches 1.5 against 1.500000.
            const FProperty* Property = FindFProperty<FProperty>(Instance->GetClass(), *Pair.Key);
            FString ValueText;
            if (Value->TryGetString(ValueText))
            {
                const bool bSameText = UAssetWorkbench::ReadPropertyPathText(Instance, Pair.Key) == ValueText;
                const bool bSame = bSameText || (Property && HoldsImportedValue(Property, Instance, ValueText));
                if (bSame)
                {
                    ++Context.Unchanged;
                    continue;
                }
            }

            Pending->SetField(Pair.Key, Value);
        }

        if (Pending->Values.IsEmpty())
        {
            return true;
        }

        TMap<FString, FString> Before;
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Pending->Values)
        {
            Before.Add(Pair.Key, UAssetWorkbench::ReadPropertyPathText(Instance, Pair.Key));
        }

        Instance->Modify();
        int32 Failures = 0;
        Context.Ops += UAssetWorkbench::ApplyProperties(Instance, Pending, Failures);
        if (Failures > 0)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%d propert(ies) rejected on %s, nothing saved"), Failures, *Instance->GetName());
            return false;
        }

        for (const TPair<FString, FString>& Pair : Before)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s %s %s: %s -> %s"), *Context.AssetPath, *Instance->GetName(), *Pair.Key, *Pair.Value, *UAssetWorkbench::ReadPropertyPathText(Instance, Pair.Key));
        }

        return true;
    }

    UBehaviorTreeGraphNode* SpawnMainNode(FBTEditContext& Context, UClass* RuntimeClass)
    {
        UClass* GraphNodeClass = UBehaviorTreeGraphNode_Task::StaticClass();
        if (RuntimeClass->IsChildOf(UBTComposite_SimpleParallel::StaticClass()))
        {
            GraphNodeClass = FindObject<UClass>(nullptr, kSimpleParallelGraphNodePath);
        }
        else if (RuntimeClass->IsChildOf(UBTCompositeNode::StaticClass()))
        {
            GraphNodeClass = UBehaviorTreeGraphNode_Composite::StaticClass();
        }
        else if (RuntimeClass->IsChildOf(UBTTask_RunBehavior::StaticClass()))
        {
            GraphNodeClass = UBehaviorTreeGraphNode_SubtreeTask::StaticClass();
        }

        if (!GraphNodeClass)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("No graph node class for %s"), *RuntimeClass->GetName());
            return nullptr;
        }

        // Same steps as the editor's new node action, minus the transaction.
        UBehaviorTreeGraphNode* Node = NewObject<UBehaviorTreeGraphNode>(Context.Graph, GraphNodeClass);
        Node->ClassData = FGraphNodeClassData(RuntimeClass, FString());
        Node->SetFlags(RF_Transactional);
        Context.Graph->AddNode(Node, /* bFromUI */ false, /* bSelectNewNode */ false);
        Node->CreateNewGuid();
        Node->PostPlacedNewNode();
        Node->AllocateDefaultPins();
        return Node;
    }

    void DeleteSubtree(UBehaviorTreeGraphNode* Node)
    {
        for (UBehaviorTreeGraphNode* Child : GetChildren(Node))
        {
            DeleteSubtree(Child);
        }

        for (UAIGraphNode* SubNode : TArray<UAIGraphNode*>(Node->SubNodes))
        {
            Node->RemoveSubNode(SubNode);
            SubNode->DestroyNode();
        }

        Node->Modify();
        Node->BreakAllNodeLinks();
        Node->DestroyNode();
    }

    bool ApplyNodeOp(FBTEditContext& Context, const TSharedPtr<FJsonObject>& Op)
    {
        FString Kind = TEXT("Modify");
        Op->TryGetStringField(TEXT("Op"), Kind);

        if (Kind == TEXT("Add"))
        {
            FString ParentKey;
            FString ClassName;
            if (!Op->TryGetStringField(TEXT("Parent"), ParentKey) || !Op->TryGetStringField(TEXT("Class"), ClassName))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Nodes Add needs Parent and Class"));
                return false;
            }

            UBehaviorTreeGraphNode* Parent = ResolveNode(Context, ParentKey);
            UClass* RuntimeClass = ResolveNodeClass(ClassName, UBTNode::StaticClass(), TEXT("composite or task"));
            if (!Parent || !RuntimeClass)
            {
                return false;
            }

            const bool bTreeNode = RuntimeClass->IsChildOf(UBTCompositeNode::StaticClass()) || RuntimeClass->IsChildOf(UBTTaskNode::StaticClass());
            if (!bTreeNode)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s is not a composite or task. Decorators and services go in their own sections"), *ClassName);
                return false;
            }

            if (!CheckParentTakesChild(Context, Parent, RuntimeClass, nullptr))
            {
                return false;
            }

            UBehaviorTreeGraphNode* Node = SpawnMainNode(Context, RuntimeClass);
            if (!Node)
            {
                return false;
            }

            Parent->GetOutputPin()->MakeLinkTo(Node->GetInputPin());

            int32 Index = INDEX_NONE;
            Op->TryGetNumberField(TEXT("Index"), Index);
            PlaceChild(Parent, Node, Index);

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s Add %s under %s -> %s"), *Context.AssetPath, *RuntimeClass->GetName(), *DescribeNode(Parent), *DescribeNode(Node));
            ++Context.Ops;
            return RegisterAlias(Context, Op, Node) && ApplyNodeFields(Context, Node, Op);
        }

        FString NodeKey;
        if (!Op->TryGetStringField(TEXT("Node"), NodeKey))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Nodes %s needs Node"), *Kind);
            return false;
        }

        UBehaviorTreeGraphNode* Node = ResolveNode(Context, NodeKey);
        if (!Node)
        {
            return false;
        }

        if (Kind == TEXT("Modify"))
        {
            return ApplyNodeFields(Context, Node, Op);
        }

        const bool bMainNode = Node->NodeInstance && !Cast<UBehaviorTreeGraphNode_Root>(Node) && Context.Graph->Nodes.Contains(Node);
        if (!bMainNode)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Nodes %s takes a composite or task, %s is not one. Decorators and services go in their own sections"), *Kind, *NodeKey);
            return false;
        }

        if (Kind == TEXT("Delete"))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s Delete %s and its subtree"), *Context.AssetPath, *DescribeNode(Node));
            DeleteSubtree(Node);
            ++Context.Ops;
            return true;
        }

        if (Kind == TEXT("Move"))
        {
            int32 Index = INDEX_NONE;
            if (!Op->TryGetNumberField(TEXT("Index"), Index))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Nodes Move needs Index"));
                return false;
            }

            UBehaviorTreeGraphNode* OldParent = GetParent(Node);
            UBehaviorTreeGraphNode* NewParent = OldParent;
            FString ParentKey;
            if (Op->TryGetStringField(TEXT("Parent"), ParentKey))
            {
                NewParent = ResolveNode(Context, ParentKey);
            }

            if (!NewParent)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no parent to move under"), *NodeKey);
                return false;
            }

            if (IsInSubtree(Node, NewParent))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Cannot move %s under its own subtree"), *NodeKey);
                return false;
            }

            if (!CheckParentTakesChild(Context, NewParent, Node->NodeInstance->GetClass(), Node))
            {
                return false;
            }

            if (NewParent != OldParent)
            {
                Node->GetInputPin()->BreakAllPinLinks();
                NewParent->GetOutputPin()->MakeLinkTo(Node->GetInputPin());
                if (OldParent)
                {
                    LayoutChildren(OldParent, GetChildren(OldParent));
                }
            }

            PlaceChild(NewParent, Node, Index);
            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s Move %s under %s at %d"), *Context.AssetPath, *DescribeNode(Node), *DescribeNode(NewParent), Index);
            ++Context.Ops;
            return true;
        }

        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Unknown Nodes Op %s. Accepted: Add Delete Move Modify"), *Kind);
        return false;
    }

    // Injected decorators come from a subtree task and always stay last.
    void ReorderSubNode(UBehaviorTreeGraphNode* Owner, UBehaviorTreeGraphNode* SubNode, bool bDecorator, int32 Index)
    {
        TArray<TObjectPtr<UBehaviorTreeGraphNode>>& Typed = bDecorator ? Owner->Decorators : Owner->Services;
        Typed.Remove(SubNode);

        int32 Authored = 0;
        while (Authored < Typed.Num() && !Typed[Authored]->bInjectedNode)
        {
            ++Authored;
        }

        Owner->Modify();
        Typed.Insert(SubNode, Index < 0 ? Authored : FMath::Min(Index, Authored));
    }

    bool ApplySubNodeOp(FBTEditContext& Context, const TSharedPtr<FJsonObject>& Op, bool bDecorator)
    {
        const TCHAR* Section = bDecorator ? TEXT("Decorators") : TEXT("Services");
        UClass* Base = bDecorator ? UBTDecorator::StaticClass() : UBTService::StaticClass();

        FString Kind;
        FString NodeKey;
        if (!Op->TryGetStringField(TEXT("Op"), Kind) || !Op->TryGetStringField(TEXT("Node"), NodeKey))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s ops need Op and Node"), Section);
            return false;
        }

        UBehaviorTreeGraphNode* Node = ResolveNode(Context, NodeKey);
        if (!Node)
        {
            return false;
        }

        if (Kind == TEXT("Add"))
        {
            FString ClassName;
            if (!Op->TryGetStringField(TEXT("Class"), ClassName))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s Add needs Class"), Section);
                return false;
            }

            UClass* RuntimeClass = ResolveNodeClass(ClassName, Base, bDecorator ? TEXT("decorator") : TEXT("service"));
            if (!RuntimeClass)
            {
                return false;
            }

            const bool bHost = Cast<UBTCompositeNode>(Node->NodeInstance) || Cast<UBTTaskNode>(Node->NodeInstance);
            if (!bHost)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s goes on a composite or task, %s is not one"), Section, *NodeKey);
                return false;
            }

            UBehaviorTreeGraphNode* SubNode = bDecorator ? static_cast<UBehaviorTreeGraphNode*>(NewObject<UBehaviorTreeGraphNode_Decorator>(Context.Graph)) : static_cast<UBehaviorTreeGraphNode*>(NewObject<UBehaviorTreeGraphNode_Service>(Context.Graph));
            SubNode->ClassData = FGraphNodeClassData(RuntimeClass, FString());
            Node->AddSubNode(SubNode, Context.Graph);

            int32 Index = INDEX_NONE;
            if (Op->TryGetNumberField(TEXT("Index"), Index))
            {
                ReorderSubNode(Node, SubNode, bDecorator, Index);
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s Add %s on %s -> %s"), *Context.AssetPath, *RuntimeClass->GetName(), *DescribeNode(Node), *DescribeNode(SubNode));
            ++Context.Ops;
            return RegisterAlias(Context, Op, SubNode) && ApplyNodeFields(Context, SubNode, Op);
        }

        UBehaviorTreeGraphNode* Owner = FindSubNodeOwner(Context, Node);
        const bool bRightKind = Node->NodeInstance && Node->NodeInstance->IsA(Base);
        if (!Owner || !bRightKind)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s %s takes one of its own entries, %s is not one"), Section, *Kind, *NodeKey);
            return false;
        }

        if (Kind == TEXT("Modify"))
        {
            return ApplyNodeFields(Context, Node, Op);
        }

        if (Kind == TEXT("Delete"))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s Delete %s from %s"), *Context.AssetPath, *DescribeNode(Node), *DescribeNode(Owner));
            Owner->RemoveSubNode(Node);
            Node->ParentNode = nullptr;
            Node->DestroyNode();
            ++Context.Ops;
            return true;
        }

        if (Kind == TEXT("Move"))
        {
            int32 Index = INDEX_NONE;
            if (!Op->TryGetNumberField(TEXT("Index"), Index))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s Move needs Index"), Section);
                return false;
            }

            ReorderSubNode(Owner, Node, bDecorator, Index);
            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s Move %s on %s to %d"), *Context.AssetPath, *DescribeNode(Node), *DescribeNode(Owner), Index);
            ++Context.Ops;
            return true;
        }

        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Unknown %s Op %s. Accepted: Add Delete Move Modify"), Section, *Kind);
        return false;
    }

    bool ForEachOp(const TSharedPtr<FJsonObject>& Entry, const TCHAR* Section, TFunctionRef<bool(const TSharedPtr<FJsonObject>&)> Apply)
    {
        const TArray<TSharedPtr<FJsonValue>>* Ops = nullptr;
        if (!Entry->HasField(Section))
        {
            return true;
        }

        if (!Entry->TryGetArrayField(Section, Ops))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s must be an array of ops"), Section);
            return false;
        }

        for (const TSharedPtr<FJsonValue>& Value : *Ops)
        {
            const TSharedPtr<FJsonObject> Op = Value->AsObject();
            if (!Op.IsValid() || !Apply(Op))
            {
                return false;
            }
        }

        return true;
    }

    bool ApplyBlackboard(FBTEditContext& Context, const TSharedPtr<FJsonObject>& Entry)
    {
        FString BlackboardPath;
        if (!Entry->TryGetStringField(TEXT("Blackboard"), BlackboardPath))
        {
            return true;
        }

        UBlackboardData* Blackboard = LoadObject<UBlackboardData>(nullptr, *BlackboardPath);
        if (!Blackboard)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to load blackboard: %s"), *BlackboardPath);
            return false;
        }

        if (Context.Tree->BlackboardAsset == Blackboard)
        {
            ++Context.Unchanged;
            return true;
        }

        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s Blackboard: %s -> %s"), *Context.AssetPath, Context.Tree->BlackboardAsset ? *Context.Tree->BlackboardAsset->GetPathName() : TEXT("None"), *Blackboard->GetPathName());
        Context.Tree->Modify();
        Context.Tree->BlackboardAsset = Blackboard;
        Context.Graph->UpdateBlackboardChange();
        ++Context.Ops;
        return true;
    }
}

UEditBehaviorTreeCommandlet::UEditBehaviorTreeCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UEditBehaviorTreeCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("UAssetWorkbench v%s - EditBehaviorTree"), UASSET_WORKBENCH_VERSION_STRING);

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

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("EditBehaviorTree: %d target(s) %s"), Targets->Num(), bApply ? TEXT("[APPLY]") : TEXT("[DRY RUN]"));

    TSet<UObject*> Touched;
    int32 Ops = 0;
    for (const TSharedPtr<FJsonValue>& Value : *Targets)
    {
        const TSharedPtr<FJsonObject>& Entry = Value->AsObject();
        UAssetWorkbench::FFailureReasonCapture FailureReason;
        if (!Entry.IsValid() || !ApplyTarget(Entry, Touched, Ops))
        {
            UAssetWorkbench::ReportTargetFailure(TEXT("EditBehaviorTree"), UAssetWorkbench::DescribeSpecTarget(Entry), FailureReason.GetReason());
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }
    }

    if (!bApply)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Done. %d change(s) staged across %d asset(s) (dry run, not saved)."), Ops, Touched.Num());
        return ToExitCode(EUAssetWorkbenchExitType::Success);
    }

    for (UObject* Tree : Touched)
    {
        UAssetWorkbench::FFailureReasonCapture FailureReason;
        if (!UAssetWorkbench::CompileAndSavePackage(Tree, false))
        {
            UAssetWorkbench::ReportTargetFailure(TEXT("EditBehaviorTree"), Tree->GetPathName(), FailureReason.GetReason());
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }
    }

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Done. %d change(s) across %d asset(s) (saved)."), Ops, Touched.Num());
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}

bool UEditBehaviorTreeCommandlet::ApplyTarget(const TSharedPtr<FJsonObject>& Entry, TSet<UObject*>& OutTouched, int32& OutOps) const
{
    FString AssetPath;
    if (!Entry->TryGetStringField(TEXT("AssetPath"), AssetPath))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Target has no AssetPath field"));
        return false;
    }

    const bool bWritesSomething = Entry->HasField(TEXT("Blackboard")) || Entry->HasField(TEXT("Nodes")) || Entry->HasField(TEXT("Decorators")) || Entry->HasField(TEXT("Services"));
    if (!bWritesSomething)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s writes nothing. Expected Blackboard, Nodes, Decorators or Services"), *AssetPath);
        return false;
    }

    UBehaviorTree* Tree = LoadObject<UBehaviorTree>(nullptr, *AssetPath);
    if (!Tree)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to load Behavior Tree: %s"), *AssetPath);
        return false;
    }

    FBTEditContext Context;
    Context.Tree = Tree;
    Context.AssetPath = AssetPath;
    Context.Graph = PrepareGraph(Tree);

    // Blackboard first, key selectors in the node sections resolve against it.
    if (!ApplyBlackboard(Context, Entry))
    {
        return false;
    }

    if (!ForEachOp(Entry, TEXT("Nodes"), [&Context](const TSharedPtr<FJsonObject>& Op) { return ApplyNodeOp(Context, Op); }))
    {
        return false;
    }

    if (!ForEachOp(Entry, TEXT("Decorators"), [&Context](const TSharedPtr<FJsonObject>& Op) { return ApplySubNodeOp(Context, Op, true); }))
    {
        return false;
    }

    if (!ForEachOp(Entry, TEXT("Services"), [&Context](const TSharedPtr<FJsonObject>& Op) { return ApplySubNodeOp(Context, Op, false); }))
    {
        return false;
    }

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s: %d change(s), %d entr%s already held the value"), *AssetPath, Context.Ops, Context.Unchanged, Context.Unchanged == 1 ? TEXT("y") : TEXT("ies"));

    // Nothing differed, so there is nothing to rebuild or save.
    if (Context.Ops == 0)
    {
        return true;
    }

    Context.Graph->OnSave();
    Context.Graph->UpdateErrorMessages();
    for (UEdGraphNode* EdNode : Context.Graph->Nodes)
    {
        const UAIGraphNode* Node = Cast<UAIGraphNode>(EdNode);
        if (Node && !Node->ErrorMessage.IsEmpty())
        {
            UE_LOG(LogUAssetWorkbenchEditor, Warning, TEXT("%s %s: %s"), *AssetPath, Node->NodeInstance ? *Node->NodeInstance->GetName() : *Node->GetName(), *Node->ErrorMessage);
        }
    }

    Tree->MarkPackageDirty();
    OutOps += Context.Ops;
    OutTouched.Add(Tree);
    return true;
}
