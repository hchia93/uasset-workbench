#include "Edit/BlueprintWriter.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"

#include "Animation/AnimBlueprint.h"
#include "AnimationCustomTransitionSchema.h"
#include "AnimationStateMachineGraph.h"
#include "AnimationTransitionSchema.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimGraphNode_TransitionResult.h"
#include "AnimStateNodeBase.h"
#include "AnimStateTransitionNode.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Features/IModularFeatures.h"
#include "IPropertyAccessEditor.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_AnimGetter.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_ConstructObjectFromClass.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_GetClassDefaults.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Knot.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_MathExpression.h"
#include "K2Node_MultiGate.h"
#include "K2Node_Select.h"
#include "K2Node_Self.h"
#include "K2Node_SpawnActorFromClass.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_SwitchInteger.h"
#include "K2Node_SwitchName.h"
#include "K2Node_SwitchString.h"
#include "K2Node_Timeline.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/PackageName.h"
#include "UObject/UnrealType.h"

namespace
{
    // Gate, DoOnce, FlipFlop and the loops are macro graphs in StandardMacros, not native K2Nodes.
    const TCHAR* kStandardMacros = TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros");

    // "Package.Asset:GraphName", the same form the editor keybind config uses.
    UEdGraph* ResolveMacroGraph(const FString& MacroPath)
    {
        FString LibraryPath;
        FString GraphName;
        if (!MacroPath.Split(TEXT(":"), &LibraryPath, &GraphName))
        {
            return nullptr;
        }

        UBlueprint* Library = LoadObject<UBlueprint>(nullptr, *LibraryPath);
        if (!Library)
        {
            return nullptr;
        }

        for (UEdGraph* Graph : Library->MacroGraphs)
        {
            if (Graph && Graph->GetName() == GraphName)
            {
                return Graph;
            }
        }

        return nullptr;
    }

    // A Blueprint named by its asset path only answers as a class through its generated class.
    UClass* ResolveClassPath(const FString& ClassPath)
    {
        if (UClass* Direct = LoadClass<UObject>(nullptr, *ClassPath))
        {
            return Direct;
        }

        if (UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *ClassPath))
        {
            return Blueprint->GeneratedClass;
        }

        return nullptr;
    }

    // The overridable set is what the editor's override list shows, walked up the native chain.
    UFunction* FindOverridableEvent(UClass* SearchClass, const FName EventName)
    {
        for (UClass* Current = SearchClass; Current; Current = Current->GetSuperClass())
        {
            UFunction* Function = Current->FindFunctionByName(EventName, EIncludeSuperFlag::ExcludeSuper);
            if (Function && UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(Function))
            {
                return Function;
            }
        }

        return nullptr;
    }

    FString DescribeOverridableEvents(UClass* SearchClass)
    {
        TArray<FString> Names;
        for (TFieldIterator<UFunction> It(SearchClass); It; ++It)
        {
            if (UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(*It))
            {
                Names.AddUnique(It->GetName());
            }
        }

        Names.Sort();
        return FString::Join(Names, TEXT(", "));
    }

    FString DescribeAssignableDelegates(UClass* OwnerClass)
    {
        TArray<FString> Names;
        for (TFieldIterator<FMulticastDelegateProperty> It(OwnerClass); It; ++It)
        {
            if (It->HasAnyPropertyFlags(CPF_BlueprintAssignable))
            {
                Names.AddUnique(It->GetName());
            }
        }

        Names.Sort();
        return FString::Join(Names, TEXT(", "));
    }

    FString DescribeEnumerators(const UEnum* Enum)
    {
        TArray<FString> Names;
        for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
        {
            Names.Add(Enum->GetNameStringByIndex(Index));
        }

        return FString::Join(Names, TEXT(", "));
    }

    // String and name switches derive their case pins from PinNames, so the spec fills that member.
    bool ReadSwitchCases(const TSharedPtr<FJsonObject>& Desc, const TCHAR* Label, TArray<FName>& OutNames)
    {
        const TArray<TSharedPtr<FJsonValue>>* Cases = nullptr;
        if (!Desc->TryGetArrayField(TEXT("Cases"), Cases))
        {
            return true;
        }

        for (const TSharedPtr<FJsonValue>& Value : *Cases)
        {
            FString CaseName;
            if (!Value->TryGetString(CaseName))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Cases entries must be strings"), Label);
                return false;
            }

            OutNames.Add(FName(*CaseName));
        }

        return true;
    }

    // Exposed-on-spawn pins are read off the class default, so nothing about this node is addressable
    // until the class pin carries the class and the node has rebuilt around it.
    bool ConfigureSpawnActor(UK2Node_SpawnActorFromClass* Node, const TSharedPtr<FJsonObject>& Desc)
    {
        FString ClassPath;
        if (!Desc->TryGetStringField(TEXT("Class"), ClassPath))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("SpawnActor needs a Class"));
            return false;
        }

        UClass* SpawnClass = ResolveClassPath(ClassPath);
        if (!SpawnClass)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("SpawnActor: cannot resolve Class '%s'"), *ClassPath);
            return false;
        }

        UEdGraphPin* ClassPin = Node->GetClassPin();
        if (!ClassPin)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("SpawnActor: no class pin"));
            return false;
        }

        Node->GetSchema()->TrySetDefaultObject(*ClassPin, SpawnClass);
        Node->ReconstructNode();
        return true;
    }

    // The schema checks only that a soft pin's path is well formed, so one naming no asset is accepted.
    // The registry answers without loading, which is the whole point of a soft reference.
    bool SoftPathExists(const FSoftObjectPath& Path)
    {
        IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

        // A commandlet's registry scan runs in the background and may not have reached this package.
        FString PackageFilename;
        if (FPackageName::DoesPackageExist(Path.GetLongPackageName(), &PackageFilename))
        {
            AssetRegistry.ScanFilesSynchronous({ PackageFilename });
        }

        if (AssetRegistry.GetAssetByObjectPath(Path).IsValid())
        {
            return true;
        }

        // A soft class path names the generated class, which the registry knows only as the Blueprint beside it.
        FString AssetName = Path.GetAssetName();
        if (!AssetName.RemoveFromEnd(TEXT("_C")))
        {
            return false;
        }

        const FSoftObjectPath BlueprintPath(FTopLevelAssetPath(FName(*Path.GetLongPackageName()), FName(*AssetName)));
        return AssetRegistry.GetAssetByObjectPath(BlueprintPath).IsValid();
    }

    // TrySetDefault* keeps the old value when validation fails and says nothing, so the reason comes
    // out of the schema first and a rejected default is an error rather than a silent no-op.
    bool SetPinDefault(const FString& AssetPath, UEdGraphPin* Pin, const FString& NodeId, const FString& PinName, const FString& Value)
    {
        const UEdGraphSchema* Schema = Pin->GetOwningNode()->GetSchema();
        const FName PinCategory = Pin->PinType.PinCategory;
        const bool bIsClassPin = PinCategory == UEdGraphSchema_K2::PC_Class;
        const bool bIsObjectPin = bIsClassPin || PinCategory == UEdGraphSchema_K2::PC_Object || PinCategory == UEdGraphSchema_K2::PC_Interface;
        const bool bIsSoftPin = PinCategory == UEdGraphSchema_K2::PC_SoftObject || PinCategory == UEdGraphSchema_K2::PC_SoftClass;
        const bool bIsTextPin = PinCategory == UEdGraphSchema_K2::PC_Text;

        UObject* AsObject = nullptr;
        if (bIsObjectPin)
        {
            AsObject = bIsClassPin ? ResolveClassPath(Value) : LoadObject<UObject>(nullptr, *Value);
            if (!AsObject)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: %s.%s cannot load '%s'"), *AssetPath, *NodeId, *PinName, *Value);
                return false;
            }
        }

        if (bIsSoftPin)
        {
            const FSoftObjectPath SoftPath(*Value);
            if (!SoftPath.IsValid() || !SoftPathExists(SoftPath))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: %s.%s cannot resolve '%s'"), *AssetPath, *NodeId, *PinName, *Value);
                return false;
            }
        }

        if (const UEnum* PinEnum = Cast<UEnum>(Pin->PinType.PinSubCategoryObject.Get()))
        {
            if (PinEnum->GetIndexByNameString(Value) == INDEX_NONE)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: %s.%s has no enumerator '%s'. Enumerators: %s"), *AssetPath, *NodeId, *PinName, *Value, *DescribeEnumerators(PinEnum));
                return false;
            }
        }

        FString UseValue = Value;
        TObjectPtr<UObject> UseObject = AsObject;
        FText UseText;

        if (bIsObjectPin)
        {
            // Object and class literals live in DefaultObject, a non-empty string half is itself a rejection.
            UseValue.Reset();
        }
        else if (bIsTextPin)
        {
            UseValue.Reset();
            UseText = FText::FromString(Value);
        }
        else if (const UEdGraphSchema_K2* K2Schema = Cast<UEdGraphSchema_K2>(Schema))
        {
            // Same split TrySetDefaultValue makes before it validates, so the reason matches what it would do.
            K2Schema->GetPinDefaultValuesFromString(Pin->PinType, Pin->GetOwningNodeUnchecked(), Value, UseValue, UseObject, UseText, /* bPreserveTextIdentity */ false);
        }

        const FString Reason = Schema->IsPinDefaultValid(Pin, UseValue, UseObject, UseText);
        if (!Reason.IsEmpty())
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: %s.%s rejected '%s': %s"), *AssetPath, *NodeId, *PinName, *Value, *Reason);
            return false;
        }

        if (bIsObjectPin)
        {
            Schema->TrySetDefaultObject(*Pin, AsObject);
        }
        else if (bIsTextPin)
        {
            Schema->TrySetDefaultText(*Pin, UseText);
        }
        else
        {
            Schema->TrySetDefaultValue(*Pin, Value);
        }

        return true;
    }

    // Exposed-on-spawn pins and class default pins are derived from the class literal, so a write to
    // the class pin leaves the rest of the node stale until it rebuilds.
    bool IsClassDrivenPin(const UEdGraphNode* Node, const UEdGraphPin* Pin)
    {
        const bool bIsClassDriven = Node->IsA<UK2Node_ConstructObjectFromClass>() || Node->IsA<UK2Node_GetClassDefaults>();
        return bIsClassDriven && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class;
    }

    // Nothing drags a wire here to resolve the wildcard index, so its type is set outright and the node
    // rebuilds off it. Enum mode derives option pins from the enum entries and ignores OptionCount.
    bool ConfigureSelect(UK2Node_Select* Node, const TSharedPtr<FJsonObject>& Desc)
    {
        UEdGraphPin* IndexPin = Node->GetIndexPin();
        if (!IndexPin)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Select: no index pin"));
            return false;
        }

        FString EnumPath;
        if (Desc->TryGetStringField(TEXT("Enum"), EnumPath))
        {
            UEnum* Enum = LoadObject<UEnum>(nullptr, *EnumPath);
            if (!Enum)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Select: cannot load enum '%s'"), *EnumPath);
                return false;
            }

            // PSC_Index survives from the wildcard pin and would keep enum mode from being recognised.
            IndexPin->PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
            IndexPin->PinType.PinSubCategory = NAME_None;
            IndexPin->PinType.PinSubCategoryObject = Enum;
            Node->ChangePinType(IndexPin);
            return true;
        }

        int32 OptionCount = 0;
        if (!Desc->TryGetNumberField(TEXT("OptionCount"), OptionCount))
        {
            return true;
        }

        if (OptionCount < 2)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Select: OptionCount %d is below the two the node always has"), OptionCount);
            return false;
        }

        // Two options is a ternary on a bool, more than two indexes by integer.
        IndexPin->PinType.PinCategory = OptionCount == 2 ? UEdGraphSchema_K2::PC_Boolean : UEdGraphSchema_K2::PC_Int;
        IndexPin->PinType.PinSubCategory = NAME_None;
        IndexPin->PinType.PinSubCategoryObject = nullptr;
        Node->ChangePinType(IndexPin);

        TArray<UEdGraphPin*> OptionPins;
        Node->GetOptionPins(OptionPins);
        while (OptionPins.Num() < OptionCount && Node->CanAddPin())
        {
            Node->AddInputPin();
            Node->GetOptionPins(OptionPins);
        }

        if (OptionPins.Num() != OptionCount)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Select: wanted %d option pins, node settled at %d"), OptionCount, OptionPins.Num());
            return false;
        }

        return true;
    }

    // Component name on the Blueprint, else a class path. Static and library calls use the latter.
    UClass* ResolveCallTargetClass(UBlueprint* Blueprint, const FString& Target)
    {
        if (Target.IsEmpty())
        {
            return Blueprint->GeneratedClass;
        }

        if (Blueprint->SimpleConstructionScript)
        {
            for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
            {
                if (Node && Node->ComponentTemplate && Node->GetVariableName().ToString() == Target)
                {
                    return Node->ComponentTemplate->GetClass();
                }
            }
        }

        // Native subobject on the CDO, which is where C++ components live.
        if (UClass* GeneratedClass = Blueprint->GeneratedClass)
        {
            if (UObject* CDO = GeneratedClass->GetDefaultObject())
            {
                if (FObjectProperty* ObjProp = FindFProperty<FObjectProperty>(GeneratedClass, FName(*Target)))
                {
                    if (UObject* SubObj = ObjProp->GetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(CDO)))
                    {
                        return SubObj->GetClass();
                    }
                }
            }
        }

        return LoadClass<UObject>(nullptr, *Target);
    }

    // A transition carries its rule on the result node inside its bound graph, which is where the Details
    // panel binds as well, so naming the transition is enough to reach bCanEnterTransition.
    UAnimGraphNode_Base* ResolveBindableNode(UEdGraphNode* Node)
    {
        if (UAnimStateTransitionNode* TransitionNode = Cast<UAnimStateTransitionNode>(Node))
        {
            if (!TransitionNode->BoundGraph)
            {
                return nullptr;
            }

            for (UEdGraphNode* Inner : TransitionNode->BoundGraph->Nodes)
            {
                if (UAnimGraphNode_TransitionResult* ResultNode = Cast<UAnimGraphNode_TransitionResult>(Inner))
                {
                    return ResultNode;
                }
            }

            return nullptr;
        }

        return Cast<UAnimGraphNode_Base>(Node);
    }

    // ShowPinForProperties is the exposable set, a property outside it has neither a pin nor a binding slot.
    // The friendly name is what the Details panel shows and what a spec author copies.
    int32 FindOptionalPinIndex(const UAnimGraphNode_Base* AnimNode, const FString& PropertyName)
    {
        return AnimNode->ShowPinForProperties.IndexOfByPredicate([&PropertyName](const FOptionalPinFromProperty& OptionalPin)
        {
            return OptionalPin.PropertyName.ToString() == PropertyName || OptionalPin.PropertyFriendlyName == PropertyName;
        });
    }

    FString DescribeBindableProperties(const UAnimGraphNode_Base* AnimNode)
    {
        TArray<FString> Names;
        for (const FOptionalPinFromProperty& OptionalPin : AnimNode->ShowPinForProperties)
        {
            if (!OptionalPin.PropertyName.IsNone())
            {
                Names.Add(OptionalPin.PropertyName.ToString());
            }
        }

        return FString::Join(Names, TEXT(", "));
    }

    // The concrete binding class keeps PropertyBindings in a Private header, so the map is reached by
    // reflection, the same route AnimBlueprintExport reads it back through.
    bool OpenBindingMap(UAnimGraphNode_Base* AnimNode, UObject*& OutBindingObject, const FMapProperty*& OutMapProperty)
    {
        const FObjectProperty* BindingProperty = FindFProperty<FObjectProperty>(AnimNode->GetClass(), TEXT("Binding"));
        if (!BindingProperty)
        {
            return false;
        }

        OutBindingObject = BindingProperty->GetObjectPropertyValue(BindingProperty->ContainerPtrToValuePtr<void>(AnimNode));
        if (!OutBindingObject)
        {
            return false;
        }

        OutMapProperty = FindFProperty<FMapProperty>(OutBindingObject->GetClass(), TEXT("PropertyBindings"));
        if (!OutMapProperty || !CastField<FNameProperty>(OutMapProperty->KeyProp))
        {
            return false;
        }

        const FStructProperty* ValueProperty = CastField<FStructProperty>(OutMapProperty->ValueProp);
        return ValueProperty && ValueProperty->Struct == FAnimGraphNodePropertyBinding::StaticStruct();
    }

    // Anim getters are the native AnimInstance functions tagged with the AnimGetter meta, which is also the
    // class the node records so the menu can filter it back out.
    UClass* FindNativeAnimInstanceClass(const UBlueprint* Blueprint)
    {
        UClass* NativeClass = Blueprint->ParentClass;
        while (NativeClass && !NativeClass->HasAnyClassFlags(CLASS_Native))
        {
            NativeClass = NativeClass->GetSuperClass();
        }

        return NativeClass;
    }

    bool IsAnimGetter(const UFunction* Function)
    {
        return Function->HasMetaData(TEXT("AnimGetter")) && Function->HasAnyFunctionFlags(FUNC_Native);
    }

    UFunction* FindAnimGetter(UClass* GetterClass, const FName GetterName)
    {
        for (TFieldIterator<UFunction> It(GetterClass); It; ++It)
        {
            if (It->GetFName() == GetterName && IsAnimGetter(*It))
            {
                return *It;
            }
        }

        return nullptr;
    }

    FString DescribeAnimGetters(UClass* GetterClass)
    {
        TArray<FString> Entries;
        for (TFieldIterator<UFunction> It(GetterClass); It; ++It)
        {
            if (!IsAnimGetter(*It))
            {
                continue;
            }

            const FString GetterContext = It->GetMetaData(TEXT("GetterContext"));
            Entries.AddUnique(GetterContext.IsEmpty() ? It->GetName() : FString::Printf(TEXT("%s [%s]"), *It->GetName(), *GetterContext));
        }

        Entries.Sort();
        return FString::Join(Entries, TEXT(", "));
    }

    // The compiler bakes the index pins from SourceNode and SourceStateNode, so the parameters a getter
    // declares are exactly what the spec has to name.
    bool AnimGetterTakes(const UFunction* Getter, const TCHAR* ParameterName)
    {
        for (TFieldIterator<FProperty> It(Getter); It && (It->PropertyFlags & CPF_Parm); ++It)
        {
            if (It->GetName() == ParameterName)
            {
                return true;
            }
        }

        return false;
    }

    // UK2Node_AnimGetter::GetNodeTitle hands back CachedTitle verbatim, an unset one leaves the node nameless.
    FText MakeAnimGetterTitle(UFunction* Getter, const UAnimStateNodeBase* SourceStateNode, const UAnimGraphNode_Base* SourceNode)
    {
        const bool bNamesState = AnimGetterTakes(Getter, TEXT("StateIndex")) || AnimGetterTakes(Getter, TEXT("TransitionIndex"));
        const UEdGraphNode* Subject = bNamesState ? StaticCast<const UEdGraphNode*>(SourceStateNode) : StaticCast<const UEdGraphNode*>(SourceNode);
        if (!Subject)
        {
            return Getter->GetDisplayNameText();
        }

        return FText::FromString(FString::Printf(TEXT("%s (%s)"), *Getter->GetDisplayNameText().ToString(), *Subject->GetNodeTitle(ENodeTitleType::ListView).ToString()));
    }

    // A getter reads indices the surrounding graph decides, so it belongs in a transition rule graph or a
    // custom blend graph. GetterContext narrows that per getter, and the engine reads only its first entry.
    bool ValidateAnimGetterGraph(const FBlueprintEditContext& Context, const UFunction* Getter, const FString& GetterName)
    {
        const UEdGraphSchema* Schema = Context.Graph->GetSchema();
        const bool bIsRuleGraph = Schema && Schema->GetClass() == UAnimationTransitionSchema::StaticClass();
        const bool bIsCustomBlendGraph = Schema && Schema->GetClass() == UAnimationCustomTransitionSchema::StaticClass();
        if (!bIsRuleGraph && !bIsCustomBlendGraph)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: AnimGetter only goes in a transition rule or custom blend graph, '%s' is a %s"), *Context.AssetPath, *Context.Graph->GetName(), *Context.Graph->GetClass()->GetName());
            return false;
        }

        TArray<FString> GetterContexts;
        Getter->GetMetaData(TEXT("GetterContext")).ParseIntoArray(GetterContexts, TEXT("|"), /* bCullEmpty */ true);
        if (GetterContexts.Num() == 0)
        {
            return true;
        }

        const bool bWantsRuleGraph = GetterContexts[0] == TEXT("Transition") && bIsRuleGraph;
        const bool bWantsCustomBlendGraph = GetterContexts[0] == TEXT("CustomBlend") && bIsCustomBlendGraph;
        if (!bWantsRuleGraph && !bWantsCustomBlendGraph)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' is a %s getter and '%s' is a %s"), *Context.AssetPath, *GetterName, *GetterContexts[0], *Context.Graph->GetName(), *Context.Graph->GetClass()->GetName());
            return false;
        }

        return true;
    }

    bool ResolveAnimGetterSource(const FBlueprintEditContext& Context, const UFunction* Getter, const FString& GetterName, const TSharedPtr<FJsonObject>& Desc, UAnimGraphNode_StateMachineBase*& OutMachineNode, UAnimStateNodeBase*& OutStateNode)
    {
        if (AnimGetterTakes(Getter, TEXT("AssetPlayerIndex")))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' reads an asset player node, which Machine / State / Transition cannot name"), *Context.AssetPath, *GetterName);
            return false;
        }

        if (!AnimGetterTakes(Getter, TEXT("MachineIndex")))
        {
            return true;
        }

        FString MachineName;
        if (!Desc->TryGetStringField(TEXT("Machine"), MachineName))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' needs a Machine"), *Context.AssetPath, *GetterName);
            return false;
        }

        UAnimationStateMachineGraph* MachineGraph = BlueprintEdit::FindMachine(Context, MachineName);
        if (!MachineGraph)
        {
            return false;
        }

        OutMachineNode = BlueprintEdit::FindMachineNode(Context.Blueprint, MachineGraph);
        if (!OutMachineNode)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: state machine '%s' hangs off no anim graph node"), *Context.AssetPath, *MachineName);
            return false;
        }

        if (AnimGetterTakes(Getter, TEXT("StateIndex")))
        {
            FString StateName;
            if (!Desc->TryGetStringField(TEXT("State"), StateName))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' needs a State. States: %s"), *Context.AssetPath, *GetterName, *BlueprintEdit::DescribeStates(MachineGraph));
                return false;
            }

            OutStateNode = BlueprintEdit::FindState(MachineGraph, StateName);
            if (!OutStateNode)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: %s has no state named '%s'. Present: %s"), *Context.AssetPath, *MachineName, *StateName, *BlueprintEdit::DescribeStates(MachineGraph));
                return false;
            }

            return true;
        }

        if (AnimGetterTakes(Getter, TEXT("TransitionIndex")))
        {
            const TSharedPtr<FJsonObject>* TransitionDesc = nullptr;
            FString FromName;
            FString ToName;
            if (!Desc->TryGetObjectField(TEXT("Transition"), TransitionDesc) || !(*TransitionDesc)->TryGetStringField(TEXT("From"), FromName) || !(*TransitionDesc)->TryGetStringField(TEXT("To"), ToName))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' needs a Transition with From and To. Present: %s"), *Context.AssetPath, *GetterName, *BlueprintEdit::DescribeTransitions(MachineGraph));
                return false;
            }

            OutStateNode = BlueprintEdit::ResolveTransition(Context, MachineGraph, FromName, ToName, *TransitionDesc);
            return OutStateNode != nullptr;
        }

        return true;
    }

    // Repeats UK2Node_AnimGetter::PostSpawnNodeSetup, which is protected and only the editor's node spawner
    // reaches. Index pins stay empty, the compiler rewrites them from SourceNode on every build.
    bool ConfigureAnimGetter(const FBlueprintEditContext& Context, UK2Node_AnimGetter* Node, const TSharedPtr<FJsonObject>& Desc)
    {
        FString GetterName;
        if (!Desc->TryGetStringField(TEXT("Getter"), GetterName))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("AnimGetter needs a Getter"));
            return false;
        }

        UAnimBlueprint* AnimBlueprint = Cast<UAnimBlueprint>(Context.Blueprint);
        UClass* GetterClass = AnimBlueprint ? FindNativeAnimInstanceClass(AnimBlueprint) : nullptr;
        if (!GetterClass)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: AnimGetter needs an Animation Blueprint with a native parent class"), *Context.AssetPath);
            return false;
        }

        UFunction* Getter = FindAnimGetter(GetterClass, FName(*GetterName));
        if (!Getter)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: %s has no anim getter '%s'. Getters: %s"), *Context.AssetPath, *GetterClass->GetName(), *GetterName, *DescribeAnimGetters(GetterClass));
            return false;
        }

        if (!ValidateAnimGetterGraph(Context, Getter, GetterName))
        {
            return false;
        }

        UAnimGraphNode_StateMachineBase* MachineNode = nullptr;
        UAnimStateNodeBase* StateNode = nullptr;
        if (!ResolveAnimGetterSource(Context, Getter, GetterName, Desc, MachineNode, StateNode))
        {
            return false;
        }

        Node->SourceNode = MachineNode;
        Node->SourceStateNode = StateNode;
        Node->GetterClass = GetterClass;
        Node->SourceAnimBlueprint = AnimBlueprint;
        Node->SetFromFunction(Getter);
        Node->CachedTitle = MakeAnimGetterTitle(Getter, StateNode, MachineNode);
        Getter->GetMetaData(TEXT("GetterContext")).ParseIntoArray(Node->Contexts, TEXT("|"), /* bCullEmpty */ true);
        return true;
    }

    void PlaceNode(UEdGraphNode* Node, const TSharedPtr<FJsonObject>& Desc)
    {
        int32 PosX = 0;
        int32 PosY = 0;
        Desc->TryGetNumberField(TEXT("PosX"), PosX);
        Desc->TryGetNumberField(TEXT("PosY"), PosY);
        Node->NodePosX = PosX;
        Node->NodePosY = PosY;
    }

    class FBlueprintGraphWriter : public IBlueprintWriter
    {
    public:
        virtual const TCHAR* GetSpecKey() const override
        {
            return TEXT("Graph");
        }

        virtual bool Apply(FBlueprintEditContext& Context, const TSharedPtr<FJsonValue>& Section) override
        {
            const TSharedPtr<FJsonObject>* SectionObject = nullptr;
            if (!Section->TryGetObject(SectionObject))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Graph must be an object"), *Context.AssetPath);
                return false;
            }

            const TSharedPtr<FJsonObject>& Entry = *SectionObject;

            Context.Graph = ResolveTargetGraph(Context, Entry);
            if (!Context.Graph)
            {
                return false;
            }

            BlueprintEdit::RegisterExistingNodes(Context.Graph, Context.NodesById);

            // Fixed order: create, node properties, bind, expose, defaults, unlink, link, delete.
            // Bind shows the pin it binds so ExposePins gets the last word, Unlink runs before Links so one
            // pass can re-route an exec chain, Delete runs last so that pass can splice around a node first.
            if (!ApplyNodes(Context, Entry) || !ApplyNodeProperties(Context, Entry) || !ApplyBind(Context, Entry) || !ApplyExposePins(Context, Entry) || !ApplyPinDefaults(Context, Entry) || !ApplyUnlink(Context, Entry) || !ApplyLinks(Context, Entry) || !ApplyDelete(Context, Entry))
            {
                return false;
            }

            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Context.Blueprint);

            Context.bNeedsStructuralRecompile = true;
            return true;
        }

    private:
        // Every rule graph the engine makes is called Transition and only has to be unique inside its own
        // transition node, so a name reaches an arbitrary one. The state pair is what identifies it.
        UEdGraph* ResolveTargetGraph(const FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Entry) const
        {
            const TSharedPtr<FJsonObject>* TransitionDesc = nullptr;
            if (Entry->TryGetObjectField(TEXT("Transition"), TransitionDesc))
            {
                FString MachineName;
                FString FromName;
                FString ToName;
                if (!Entry->TryGetStringField(TEXT("Machine"), MachineName) || !(*TransitionDesc)->TryGetStringField(TEXT("From"), FromName) || !(*TransitionDesc)->TryGetStringField(TEXT("To"), ToName))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Graph Transition needs a Machine beside it, and From and To inside it"), *Context.AssetPath);
                    return nullptr;
                }

                UAnimationStateMachineGraph* MachineGraph = BlueprintEdit::FindMachine(Context, MachineName);
                if (!MachineGraph)
                {
                    return nullptr;
                }

                UAnimStateTransitionNode* TransitionNode = BlueprintEdit::ResolveTransition(Context, MachineGraph, FromName, ToName, *TransitionDesc);
                if (!TransitionNode)
                {
                    return nullptr;
                }

                if (!TransitionNode->BoundGraph)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: transition '%s' to '%s' carries no rule graph"), *Context.AssetPath, *FromName, *ToName);
                }

                return TransitionNode->BoundGraph;
            }

            FString GraphName = TEXT("EventGraph");
            Entry->TryGetStringField(TEXT("Name"), GraphName);

            UEdGraph* Graph = BlueprintEdit::FindGraph(Context.Blueprint, GraphName);
            if (!Graph)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no graph named '%s'"), *Context.AssetPath, *GraphName);
            }

            return Graph;
        }

        bool ApplyNodes(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Entry) const
        {
            const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
            if (!Entry->TryGetArrayField(TEXT("Nodes"), Nodes))
            {
                return true;
            }

            for (const TSharedPtr<FJsonValue>& Value : *Nodes)
            {
                const TSharedPtr<FJsonObject>& Desc = Value->AsObject();
                FString Id;
                if (!Desc.IsValid() || !Desc->TryGetStringField(TEXT("Id"), Id))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: node entry needs an Id"), *Context.AssetPath);
                    return false;
                }

                FString Type;
                Desc->TryGetStringField(TEXT("Type"), Type);
                UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: + node %s (%s)"), *Context.Blueprint->GetName(), *Id, *Type);
                ++Context.Ops;

                UEdGraphNode* Node = SpawnNode(Context, Desc);
                if (!Node)
                {
                    return false;
                }

                Context.NodesById.Add(Id, Node);
            }

            return true;
        }

        // Writes properties onto the node object itself, the way Notifies writes onto a notify. Reaches anim
        // graph nodes, whose settings are node properties rather than pins.
        bool ApplyNodeProperties(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Entry) const
        {
            const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
            if (!Entry->TryGetArrayField(TEXT("NodeProperties"), Items))
            {
                return true;
            }

            TArray<UEdGraphNode*> Written;
            for (const TSharedPtr<FJsonValue>& Value : *Items)
            {
                const TSharedPtr<FJsonObject>& Desc = Value->AsObject();
                FString NodeId;
                const TSharedPtr<FJsonObject>* Properties = nullptr;
                if (!Desc.IsValid() || !Desc->TryGetStringField(TEXT("Node"), NodeId) || !Desc->TryGetObjectField(TEXT("Properties"), Properties))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: NodeProperties entry needs Node and Properties"), *Context.AssetPath);
                    return false;
                }

                UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: set %d property(s) on %s"), *Context.Blueprint->GetName(), (*Properties)->Values.Num(), *NodeId);
                ++Context.Ops;
                Context.bNeedsStructuralRecompile = true;

                UEdGraphNode** Found = Context.NodesById.Find(NodeId);
                if (!Found || !*Found)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: no node with id '%s'"), *Context.AssetPath, *NodeId);
                    return false;
                }

                UEdGraphNode* Node = *Found;
                Node->Modify();

                int32 Failures = 0;
                UAssetWorkbench::ApplyProperties(Node, *Properties, Failures);
                if (Failures > 0)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%d property(s) failed on node '%s' in %s"), Failures, *NodeId, *Context.AssetPath);
                    return false;
                }

                Written.AddUnique(Node);
            }

            // Topology-shaping properties (a cast TargetType, a struct StructType, a switch Enum) only reach
            // the pins on a rebuild, and PinDefaults and Links right after here address those pins by name.
            for (UEdGraphNode* Node : Written)
            {
                Node->ReconstructNode();
            }

            return true;
        }

        // Property access bindings, the Details panel's Bind dropdown. A bound property is driven by the
        // anim instance every frame instead of by the pin's literal or by whatever the pin is wired to.
        bool ApplyBind(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Entry) const
        {
            const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
            if (!Entry->TryGetArrayField(TEXT("Bind"), Items))
            {
                return true;
            }

            for (const TSharedPtr<FJsonValue>& Value : *Items)
            {
                const TSharedPtr<FJsonObject>& Desc = Value->AsObject();
                FString NodeId;
                FString PropertyName;
                if (!Desc.IsValid() || !Desc->TryGetStringField(TEXT("Node"), NodeId) || !Desc->TryGetStringField(TEXT("Property"), PropertyName))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Bind entry needs Node and Property"), *Context.AssetPath);
                    return false;
                }

                FString Path;
                FString Function;
                const bool bHasPath = Desc->TryGetStringField(TEXT("Path"), Path);
                const bool bHasFunction = Desc->TryGetStringField(TEXT("Function"), Function);
                if (bHasPath == bHasFunction)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Bind %s.%s takes exactly one of Path or Function"), *Context.AssetPath, *NodeId, *PropertyName);
                    return false;
                }

                const FString Target = bHasPath ? Path : Function;
                UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: bind %s.%s to %s"), *Context.Blueprint->GetName(), *NodeId, *PropertyName, *Target);
                ++Context.Ops;
                Context.bNeedsStructuralRecompile = true;

                UEdGraphNode** Found = Context.NodesById.Find(NodeId);
                if (!Found || !*Found)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: no node with id '%s'"), *Context.AssetPath, *NodeId);
                    return false;
                }

                UAnimGraphNode_Base* AnimNode = ResolveBindableNode(*Found);
                if (!AnimNode)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Bind needs an anim graph node, '%s' is a %s"), *Context.AssetPath, *NodeId, *(*Found)->GetClass()->GetName());
                    return false;
                }

                if (!ApplyOneBinding(Context, AnimNode, NodeId, PropertyName, Target, bHasFunction))
                {
                    return false;
                }
            }

            return true;
        }

        bool ApplyOneBinding(const FBlueprintEditContext& Context, UAnimGraphNode_Base* AnimNode, const FString& NodeId, const FString& PropertyName, const FString& Target, bool bFunctionKey) const
        {
            const int32 OptionalPinIndex = FindOptionalPinIndex(AnimNode, PropertyName);
            if (OptionalPinIndex == INDEX_NONE)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: node '%s' has no bindable property '%s'. Properties: %s"), *Context.AssetPath, *NodeId, *PropertyName, *DescribeBindableProperties(AnimNode));
                return false;
            }

            const FName BindingName = AnimNode->ShowPinForProperties[OptionalPinIndex].PropertyName;

            UObject* BindingObject = nullptr;
            const FMapProperty* MapProperty = nullptr;
            if (!OpenBindingMap(AnimNode, BindingObject, MapProperty))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: node '%s' carries no property binding container"), *Context.AssetPath, *NodeId);
                return false;
            }

            AnimNode->Modify();
            BindingObject->Modify();

            if (Target == TEXT("None"))
            {
                FScriptMapHelper MapHelper(MapProperty, MapProperty->ContainerPtrToValuePtr<void>(BindingObject));
                MapHelper.RemovePair(&BindingName);
                AnimNode->ReconstructNode();
                return true;
            }

            FAnimGraphNodePropertyBinding NewBinding;
            if (!BuildBinding(Context, AnimNode, BindingName, Target, bFunctionKey, NewBinding))
            {
                return false;
            }

            // A binding replaces whatever drove the property, and since 5.0 it is displayed as a pin, so the
            // editor breaks the links and shows the pin rather than hiding it. ExposePins can hide it again.
            if (UEdGraphPin* Pin = AnimNode->FindPin(BindingName))
            {
                Pin->BreakAllPinLinks();
            }

            AnimNode->SetPinVisibility(true, OptionalPinIndex);

            FScriptMapHelper MapHelper(MapProperty, MapProperty->ContainerPtrToValuePtr<void>(BindingObject));
            MapHelper.RemovePair(&BindingName);
            MapHelper.AddPair(&BindingName, &NewBinding);

            // PinType, PromotedPinType and bIsPromotion are filled by the binding object's own reconstruct pass.
            AnimNode->ReconstructNode();
            return true;
        }

        bool BuildBinding(const FBlueprintEditContext& Context, UAnimGraphNode_Base* AnimNode, const FName BindingName, const FString& Target, bool bFunctionKey, FAnimGraphNodePropertyBinding& OutBinding) const
        {
            if (!IModularFeatures::Get().IsModularFeatureAvailable("PropertyAccessEditor"))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: the PropertyAccessEditor feature is not registered, bindings cannot be resolved"), *Context.AssetPath);
                return false;
            }

            UClass* SkeletonClass = Context.Blueprint->SkeletonGeneratedClass;
            if (!SkeletonClass)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no skeleton class to resolve '%s' against"), *Context.AssetPath, *Target);
                return false;
            }

            TArray<FString> Segments;
            Target.ParseIntoArray(Segments, TEXT("."), /* bCullEmpty */ true);
            if (Segments.Num() == 0)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Bind path '%s' is empty"), *Context.AssetPath, *Target);
                return false;
            }

            IPropertyAccessEditor& PropertyAccessEditor = IModularFeatures::Get().GetModularFeature<IPropertyAccessEditor>("PropertyAccessEditor");

            const int32 LeafIndex = Segments.Num() - 1;
            FProperty* LeafProperty = nullptr;
            bool bLeafIsFunction = false;

            IPropertyAccessEditor::FResolvePropertyAccessArgs ResolveArgs;
            ResolveArgs.PropertyFunction = [&LeafProperty, &bLeafIsFunction, LeafIndex](int32 SegmentIndex, FProperty* Property, int32)
            {
                if (SegmentIndex == LeafIndex)
                {
                    LeafProperty = Property;
                    bLeafIsFunction = false;
                }
            };
            ResolveArgs.ArrayFunction = [&LeafProperty, &bLeafIsFunction, LeafIndex](int32 SegmentIndex, FArrayProperty* Property, int32)
            {
                if (SegmentIndex == LeafIndex)
                {
                    LeafProperty = Property;
                    bLeafIsFunction = false;
                }
            };
            ResolveArgs.FunctionFunction = [&LeafProperty, &bLeafIsFunction, LeafIndex](int32 SegmentIndex, UFunction*, FProperty* ReturnProperty)
            {
                if (SegmentIndex == LeafIndex)
                {
                    LeafProperty = ReturnProperty;
                    bLeafIsFunction = true;
                }
            };

            const FPropertyAccessResolveResult Result = PropertyAccessEditor.ResolvePropertyAccess(SkeletonClass, Segments, ResolveArgs);
            if (Result.Result == EPropertyAccessResolveResult::Failed || !LeafProperty)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' does not resolve against %s"), *Context.AssetPath, *Target, *SkeletonClass->GetName());
                return false;
            }

            if (bFunctionKey && !bLeafIsFunction)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' resolves to a property, use Path for it"), *Context.AssetPath, *Target);
                return false;
            }

            FProperty* BoundProperty = AnimNode->GetPinProperty(BindingName);
            if (!BoundProperty)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' is not a property of %s"), *Context.AssetPath, *BindingName.ToString(), *AnimNode->GetClass()->GetName());
                return false;
            }

            if (PropertyAccessEditor.GetPropertyCompatibility(LeafProperty, BoundProperty) == EPropertyAccessCompatibility::Incompatible)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' is %s and cannot drive %s, which is %s"), *Context.AssetPath, *Target, *LeafProperty->GetCPPType(),
                    *BindingName.ToString(), *BoundProperty->GetCPPType());
                return false;
            }

            OutBinding.PropertyName = BindingName;
            OutBinding.PropertyPath = Segments;
            OutBinding.PathAsText = PropertyAccessEditor.MakeTextPath(Segments, SkeletonClass);
            OutBinding.Type = bLeafIsFunction ? EAnimGraphNodePropertyBindingType::Function : EAnimGraphNodePropertyBindingType::Property;
            OutBinding.bIsBound = true;
            return true;
        }

        // ShowPinForProperties[i].bShowPin, the Details panel's pin checkbox. NodeProperties reaches the same
        // flag but only by index, which shifts whenever the anim node struct gains a property.
        bool ApplyExposePins(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Entry) const
        {
            const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
            if (!Entry->TryGetArrayField(TEXT("ExposePins"), Items))
            {
                return true;
            }

            for (const TSharedPtr<FJsonValue>& Value : *Items)
            {
                const TSharedPtr<FJsonObject>& Desc = Value->AsObject();
                FString NodeId;
                if (!Desc.IsValid() || !Desc->TryGetStringField(TEXT("Node"), NodeId))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: ExposePins entry needs a Node"), *Context.AssetPath);
                    return false;
                }

                ++Context.Ops;
                Context.bNeedsStructuralRecompile = true;

                UEdGraphNode** Found = Context.NodesById.Find(NodeId);
                if (!Found || !*Found)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: no node with id '%s'"), *Context.AssetPath, *NodeId);
                    return false;
                }

                UAnimGraphNode_Base* AnimNode = ResolveBindableNode(*Found);
                if (!AnimNode)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: ExposePins needs an anim graph node, '%s' is a %s"), *Context.AssetPath, *NodeId, *(*Found)->GetClass()->GetName());
                    return false;
                }

                AnimNode->Modify();

                if (!SetPinExposure(Context, AnimNode, NodeId, Desc, TEXT("Show"), true) || !SetPinExposure(Context, AnimNode, NodeId, Desc, TEXT("Hide"), false))
                {
                    return false;
                }
            }

            return true;
        }

        bool SetPinExposure(const FBlueprintEditContext& Context, UAnimGraphNode_Base* AnimNode, const FString& NodeId, const TSharedPtr<FJsonObject>& Desc, const TCHAR* Key, bool bVisible) const
        {
            const TArray<TSharedPtr<FJsonValue>>* Names = nullptr;
            if (!Desc->TryGetArrayField(Key, Names))
            {
                return true;
            }

            for (const TSharedPtr<FJsonValue>& Value : *Names)
            {
                FString PropertyName;
                if (!Value.IsValid() || !Value->TryGetString(PropertyName))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: ExposePins %s takes an array of property names"), *Context.AssetPath, Key);
                    return false;
                }

                const int32 OptionalPinIndex = FindOptionalPinIndex(AnimNode, PropertyName);
                if (OptionalPinIndex == INDEX_NONE)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: node '%s' has no optional pin '%s'. Properties: %s"), *Context.AssetPath, *NodeId, *PropertyName, *DescribeBindableProperties(AnimNode));
                    return false;
                }

                UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: %s %s.%s"), *Context.Blueprint->GetName(), Key, *NodeId, *PropertyName);
                AnimNode->SetPinVisibility(bVisible, OptionalPinIndex);
            }

            return true;
        }

        bool ApplyPinDefaults(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Entry) const
        {
            const TArray<TSharedPtr<FJsonValue>>* PinDefaults = nullptr;
            if (!Entry->TryGetArrayField(TEXT("PinDefaults"), PinDefaults))
            {
                return true;
            }

            TArray<UEdGraphNode*> ClassPinWrites;
            for (const TSharedPtr<FJsonValue>& Value : *PinDefaults)
            {
                const TSharedPtr<FJsonObject>& Desc = Value->AsObject();
                FString NodeId;
                FString PinName;
                FString PinValue;
                if (!Desc.IsValid() || !Desc->TryGetStringField(TEXT("Node"), NodeId) || !Desc->TryGetStringField(TEXT("Pin"), PinName))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: PinDefaults entry needs Node and Pin"), *Context.AssetPath);
                    return false;
                }

                if (!Desc->TryGetStringField(TEXT("Value"), PinValue))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: PinDefaults entry for %s.%s needs a Value"), *Context.AssetPath, *NodeId, *PinName);
                    return false;
                }

                UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: %s.%s = %s"), *Context.Blueprint->GetName(), *NodeId, *PinName, *PinValue);
                ++Context.Ops;

                UEdGraphNode** Found = Context.NodesById.Find(NodeId);
                UEdGraphPin* Pin = Found ? BlueprintEdit::FindPin(*Found, PinName) : nullptr;
                if (!Pin)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: node '%s' has no pin '%s'. Pins: %s"), *Context.AssetPath, *NodeId, *PinName,
                        *BlueprintEdit::DescribePins(Found ? *Found : nullptr));
                    return false;
                }

                if (!SetPinDefault(Context.AssetPath, Pin, NodeId, PinName, PinValue))
                {
                    return false;
                }

                if (IsClassDrivenPin(*Found, Pin))
                {
                    ClassPinWrites.AddUnique(*Found);
                }
            }

            for (UEdGraphNode* Node : ClassPinWrites)
            {
                Node->ReconstructNode();
            }

            return true;
        }

        bool ApplyUnlink(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Entry) const
        {
            const TArray<TSharedPtr<FJsonValue>>* Unlink = nullptr;
            if (!Entry->TryGetArrayField(TEXT("Unlink"), Unlink))
            {
                return true;
            }

            for (const TSharedPtr<FJsonValue>& Value : *Unlink)
            {
                const TSharedPtr<FJsonObject>& Desc = Value->AsObject();
                FString NodeId;
                FString PinName;
                if (!Desc.IsValid() || !Desc->TryGetStringField(TEXT("Node"), NodeId) || !Desc->TryGetStringField(TEXT("Pin"), PinName))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Unlink entry needs Node and Pin"), *Context.AssetPath);
                    return false;
                }

                UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: unlink %s.%s"), *Context.Blueprint->GetName(), *NodeId, *PinName);
                ++Context.Ops;

                UEdGraphNode** Found = Context.NodesById.Find(NodeId);
                UEdGraphPin* Pin = Found ? BlueprintEdit::FindPin(*Found, PinName) : nullptr;
                if (!Pin)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: node '%s' has no pin '%s'. Pins: %s"), *Context.AssetPath, *NodeId, *PinName,
                        *BlueprintEdit::DescribePins(Found ? *Found : nullptr));
                    return false;
                }

                Pin->BreakAllPinLinks();
            }

            return true;
        }

        bool ApplyLinks(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Entry) const
        {
            const TArray<TSharedPtr<FJsonValue>>* Links = nullptr;
            if (!Entry->TryGetArrayField(TEXT("Links"), Links))
            {
                return true;
            }

            for (const TSharedPtr<FJsonValue>& Value : *Links)
            {
                const TSharedPtr<FJsonObject>& Desc = Value->AsObject();
                FString FromNode;
                FString FromPin;
                FString ToNode;
                FString ToPin;
                const bool bComplete = Desc.IsValid()
                    && Desc->TryGetStringField(TEXT("FromNode"), FromNode) && Desc->TryGetStringField(TEXT("FromPin"), FromPin)
                    && Desc->TryGetStringField(TEXT("ToNode"), ToNode) && Desc->TryGetStringField(TEXT("ToPin"), ToPin);
                if (!bComplete)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Links entry needs FromNode/FromPin/ToNode/ToPin"), *Context.AssetPath);
                    return false;
                }

                UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: link %s.%s -> %s.%s"), *Context.Blueprint->GetName(), *FromNode, *FromPin, *ToNode, *ToPin);
                ++Context.Ops;

                UEdGraphNode** FoundFrom = Context.NodesById.Find(FromNode);
                UEdGraphNode** FoundTo = Context.NodesById.Find(ToNode);
                UEdGraphPin* A = FoundFrom ? BlueprintEdit::FindPin(*FoundFrom, FromPin) : nullptr;
                UEdGraphPin* B = FoundTo ? BlueprintEdit::FindPin(*FoundTo, ToPin) : nullptr;
                if (!A || !B)
                {
                    UEdGraphNode* BadNode = !A ? (FoundFrom ? *FoundFrom : nullptr) : (FoundTo ? *FoundTo : nullptr);
                    const FString& BadId = !A ? FromNode : ToNode;
                    const FString& BadPin = !A ? FromPin : ToPin;
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: node '%s' has no pin '%s'. Pins: %s"), *Context.AssetPath, *BadId, *BadPin,
                        *BlueprintEdit::DescribePins(BadNode));
                    return false;
                }

                // Schema validates and inserts conversion nodes, same as dragging a wire in the editor.
                if (!Context.Graph->GetSchema()->TryCreateConnection(A, B))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: schema rejected %s.%s -> %s.%s"), *Context.AssetPath, *FromNode, *FromPin, *ToNode, *ToPin);
                    return false;
                }
            }

            return true;
        }

        bool ApplyDelete(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Entry) const
        {
            const TArray<TSharedPtr<FJsonValue>>* Delete = nullptr;
            if (!Entry->TryGetArrayField(TEXT("Delete"), Delete))
            {
                return true;
            }

            for (const TSharedPtr<FJsonValue>& Value : *Delete)
            {
                const TSharedPtr<FJsonObject>& Desc = Value->AsObject();
                FString NodeId;
                if (!Desc.IsValid() || !Desc->TryGetStringField(TEXT("Node"), NodeId))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Delete entry needs Node"), *Context.AssetPath);
                    return false;
                }

                UEdGraphNode** Found = Context.NodesById.Find(NodeId);
                if (!Found || !*Found)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: no node '%s' to delete"), *Context.AssetPath, *NodeId);
                    return false;
                }

                UEdGraphNode* Node = *Found;

                // Entry and result nodes refuse deletion in the editor, a spec gets the same answer.
                if (!Node->CanUserDeleteNode())
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: node '%s' (%s) cannot be deleted"), *Context.AssetPath, *NodeId, *Node->GetClass()->GetName());
                    return false;
                }

                UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: delete %s (%s)"), *Context.Blueprint->GetName(), *NodeId, *Node->GetClass()->GetName());
                ++Context.Ops;

                // Layout addresses nodes by Id after this writer, a dropped node must not stay addressable.
                Context.NodesById.Remove(NodeId);
                FBlueprintEditorUtils::RemoveNode(Context.Blueprint, Node, /*bDontRecompile=*/true);
            }

            return true;
        }

        UEdGraphNode* SpawnNode(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Desc) const
        {
            FString Type;
            if (!Desc->TryGetStringField(TEXT("Type"), Type))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Node entry has no Type"));
                return nullptr;
            }

            UBlueprint* Blueprint = Context.Blueprint;
            UEdGraph* Graph = Context.Graph;
            UEdGraphNode* Result = nullptr;

            if (Type == TEXT("CallFunction"))
            {
                FString FunctionName;
                if (!Desc->TryGetStringField(TEXT("Function"), FunctionName))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("CallFunction needs a Function"));
                    return nullptr;
                }

                FString Target;
                Desc->TryGetStringField(TEXT("Target"), Target);
                UClass* TargetClass = ResolveCallTargetClass(Blueprint, Target);
                if (!TargetClass)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("CallFunction cannot resolve Target '%s'"), *Target);
                    return nullptr;
                }

                UFunction* Function = TargetClass->FindFunctionByName(FName(*FunctionName));
                if (!Function)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("CallFunction: '%s' has no function '%s'"), *TargetClass->GetName(), *FunctionName);
                    return nullptr;
                }

                UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(Graph);
                Node->SetFromFunction(Function);
                Result = Node;
            }
            else if (Type == TEXT("VariableGet") || Type == TEXT("VariableSet"))
            {
                FString VariableName;
                if (!Desc->TryGetStringField(TEXT("Variable"), VariableName))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s needs a Variable"), *Type);
                    return nullptr;
                }

                if (Type == TEXT("VariableGet"))
                {
                    UK2Node_VariableGet* Node = NewObject<UK2Node_VariableGet>(Graph);
                    Node->VariableReference.SetSelfMember(FName(*VariableName));
                    Result = Node;
                }
                else
                {
                    UK2Node_VariableSet* Node = NewObject<UK2Node_VariableSet>(Graph);
                    Node->VariableReference.SetSelfMember(FName(*VariableName));
                    Result = Node;
                }
            }
            else if (Type == TEXT("Branch"))
            {
                Result = NewObject<UK2Node_IfThenElse>(Graph);
            }
            else if (Type == TEXT("Sequence"))
            {
                Result = NewObject<UK2Node_ExecutionSequence>(Graph);
            }
            else if (Type == TEXT("MacroInstance"))
            {
                FString MacroPath;
                if (!Desc->TryGetStringField(TEXT("Macro"), MacroPath))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("MacroInstance needs a Macro"));
                    return nullptr;
                }

                UEdGraph* MacroGraph = ResolveMacroGraph(MacroPath);
                if (!MacroGraph)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Cannot resolve macro graph '%s'"), *MacroPath);
                    return nullptr;
                }

                UK2Node_MacroInstance* Node = NewObject<UK2Node_MacroInstance>(Graph);
                Node->SetMacroGraph(MacroGraph);
                Result = Node;
            }
            else if (Type == TEXT("MultiGate"))
            {
                Result = NewObject<UK2Node_MultiGate>(Graph);
            }
            else if (Type == TEXT("Select"))
            {
                Result = NewObject<UK2Node_Select>(Graph);
            }
            else if (Type == TEXT("MathExpression"))
            {
                FString Expression;
                Desc->TryGetStringField(TEXT("Expression"), Expression);
                UK2Node_MathExpression* Node = NewObject<UK2Node_MathExpression>(Graph);
                Node->Expression = Expression;
                Result = Node;
            }
            else if (Type == TEXT("CustomEvent"))
            {
                FString EventName;
                if (!Desc->TryGetStringField(TEXT("EventName"), EventName))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("CustomEvent needs an EventName"));
                    return nullptr;
                }

                UK2Node_CustomEvent* Node = NewObject<UK2Node_CustomEvent>(Graph);
                Node->CustomFunctionName = FName(*EventName);
                Result = Node;
            }
            else if (Type == TEXT("Self"))
            {
                Result = NewObject<UK2Node_Self>(Graph);
            }
            else if (Type == TEXT("Event"))
            {
                FString EventName;
                if (!Desc->TryGetStringField(TEXT("EventName"), EventName))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Event needs an EventName"));
                    return nullptr;
                }

                UClass* SearchClass = Blueprint->ParentClass;
                FString ClassPath;
                if (Desc->TryGetStringField(TEXT("Class"), ClassPath))
                {
                    SearchClass = ResolveClassPath(ClassPath);
                    if (!SearchClass)
                    {
                        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Event: cannot resolve Class '%s'"), *ClassPath);
                        return nullptr;
                    }
                }

                UFunction* Signature = FindOverridableEvent(SearchClass, FName(*EventName));
                if (!Signature)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Event: '%s' has no overridable event '%s'. Events: %s"), *GetNameSafe(SearchClass), *EventName, *DescribeOverridableEvents(SearchClass));
                    return nullptr;
                }

                UClass* SignatureClass = Signature->GetOwnerClass();
                if (FBlueprintEditorUtils::FindOverrideForFunction(Blueprint, SignatureClass, Signature->GetFName()))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Event: %s already implements '%s'"), *Blueprint->GetName(), *EventName);
                    return nullptr;
                }

                UK2Node_Event* Node = NewObject<UK2Node_Event>(Graph);
                Node->EventReference.SetExternalMember(Signature->GetFName(), SignatureClass);
                Node->bOverrideFunction = true;
                Result = Node;
            }
            else if (Type == TEXT("AddDelegate"))
            {
                FString DelegateName;
                if (!Desc->TryGetStringField(TEXT("Delegate"), DelegateName))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("AddDelegate needs a Delegate"));
                    return nullptr;
                }

                UClass* OwnerClass = Blueprint->SkeletonGeneratedClass;
                FString ClassPath;
                if (Desc->TryGetStringField(TEXT("Class"), ClassPath))
                {
                    OwnerClass = ResolveClassPath(ClassPath);
                    if (!OwnerClass)
                    {
                        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("AddDelegate: cannot resolve Class '%s'"), *ClassPath);
                        return nullptr;
                    }
                }

                const FMulticastDelegateProperty* Property = FindFProperty<FMulticastDelegateProperty>(OwnerClass, FName(*DelegateName));
                if (!Property || !Property->HasAnyPropertyFlags(CPF_BlueprintAssignable))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("AddDelegate: '%s' has no BlueprintAssignable delegate '%s'. Delegates: %s"), *GetNameSafe(OwnerClass), *DelegateName, *DescribeAssignableDelegates(OwnerClass));
                    return nullptr;
                }

                // A delegate outside the Blueprint's own hierarchy binds through the self pin, which takes the owning object.
                const bool bSelfContext = Blueprint->SkeletonGeneratedClass && Blueprint->SkeletonGeneratedClass->IsChildOf(Property->GetOwnerClass());

                UK2Node_AddDelegate* Node = NewObject<UK2Node_AddDelegate>(Graph);
                Node->SetFromProperty(Property, bSelfContext, Property->GetOwnerClass());
                Result = Node;
            }
            else if (Type == TEXT("CreateDelegate"))
            {
                FString FunctionName;
                if (!Desc->TryGetStringField(TEXT("Function"), FunctionName))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("CreateDelegate needs a Function"));
                    return nullptr;
                }

                UClass* SkeletonClass = Blueprint->SkeletonGeneratedClass;
                if (!SkeletonClass || !SkeletonClass->FindFunctionByName(FName(*FunctionName)))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("CreateDelegate: %s has no function '%s'"), *Blueprint->GetName(), *FunctionName);
                    return nullptr;
                }

                // Any reconstruct before the delegate pin is linked fails validation and clears the function name.
                UK2Node_CreateDelegate* Node = NewObject<UK2Node_CreateDelegate>(Graph);
                Node->SetFunction(FName(*FunctionName));
                Result = Node;
            }
            else if (Type == TEXT("DynamicCast"))
            {
                FString ClassPath;
                if (!Desc->TryGetStringField(TEXT("Class"), ClassPath))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("DynamicCast needs a Class"));
                    return nullptr;
                }

                UClass* TargetClass = ResolveClassPath(ClassPath);
                if (!TargetClass)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("DynamicCast: cannot resolve Class '%s'"), *ClassPath);
                    return nullptr;
                }

                UK2Node_DynamicCast* Node = NewObject<UK2Node_DynamicCast>(Graph);
                Node->TargetType = TargetClass;
                Result = Node;
            }
            else if (Type == TEXT("MakeStruct") || Type == TEXT("BreakStruct"))
            {
                FString StructPath;
                if (!Desc->TryGetStringField(TEXT("Struct"), StructPath))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s needs a Struct"), *Type);
                    return nullptr;
                }

                UScriptStruct* Struct = LoadObject<UScriptStruct>(nullptr, *StructPath);
                if (!Struct)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: cannot resolve Struct '%s'"), *Type, *StructPath);
                    return nullptr;
                }

                if (Type == TEXT("MakeStruct"))
                {
                    UK2Node_MakeStruct* Node = NewObject<UK2Node_MakeStruct>(Graph);
                    Node->StructType = Struct;
                    Result = Node;
                }
                else
                {
                    UK2Node_BreakStruct* Node = NewObject<UK2Node_BreakStruct>(Graph);
                    Node->StructType = Struct;
                    Result = Node;
                }
            }
            else if (Type == TEXT("SwitchEnum"))
            {
                FString EnumPath;
                if (!Desc->TryGetStringField(TEXT("Enum"), EnumPath))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("SwitchEnum needs an Enum"));
                    return nullptr;
                }

                UEnum* Enum = LoadObject<UEnum>(nullptr, *EnumPath);
                if (!Enum)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("SwitchEnum: cannot resolve Enum '%s'"), *EnumPath);
                    return nullptr;
                }

                UK2Node_SwitchEnum* Node = NewObject<UK2Node_SwitchEnum>(Graph);
                Node->SetEnum(Enum);
                Result = Node;
            }
            else if (Type == TEXT("SwitchString"))
            {
                UK2Node_SwitchString* Node = NewObject<UK2Node_SwitchString>(Graph);
                if (!ReadSwitchCases(Desc, TEXT("SwitchString"), Node->PinNames))
                {
                    return nullptr;
                }

                Result = Node;
            }
            else if (Type == TEXT("SwitchName"))
            {
                UK2Node_SwitchName* Node = NewObject<UK2Node_SwitchName>(Graph);
                if (!ReadSwitchCases(Desc, TEXT("SwitchName"), Node->PinNames))
                {
                    return nullptr;
                }

                Result = Node;
            }
            else if (Type == TEXT("SwitchInteger"))
            {
                UK2Node_SwitchInteger* Node = NewObject<UK2Node_SwitchInteger>(Graph);
                Desc->TryGetNumberField(TEXT("StartIndex"), Node->StartIndex);
                Result = Node;
            }
            else if (Type == TEXT("CallParentFunction"))
            {
                FString FunctionName;
                if (!Desc->TryGetStringField(TEXT("Function"), FunctionName))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("CallParentFunction needs a Function"));
                    return nullptr;
                }

                UClass* ParentClass = Blueprint->ParentClass;
                UFunction* Function = ParentClass ? ParentClass->FindFunctionByName(FName(*FunctionName)) : nullptr;
                if (!Function)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("CallParentFunction: '%s' has no function '%s'"), *GetNameSafe(ParentClass), *FunctionName);
                    return nullptr;
                }

                UK2Node_CallParentFunction* Node = NewObject<UK2Node_CallParentFunction>(Graph);
                Node->SetFromFunction(Function);
                Result = Node;
            }
            else if (Type == TEXT("SpawnActor"))
            {
                Result = NewObject<UK2Node_SpawnActorFromClass>(Graph);
            }
            else if (Type == TEXT("Timeline"))
            {
                FString TimelineName;
                if (!Desc->TryGetStringField(TEXT("Name"), TimelineName))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Timeline needs a Name"));
                    return nullptr;
                }

                // The node reads its tracks off the template, which has to exist before pins are allocated.
                if (!FBlueprintEditorUtils::AddNewTimeline(Blueprint, FName(*TimelineName)))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Timeline: %s would not take a timeline named '%s'"), *Blueprint->GetName(), *TimelineName);
                    return nullptr;
                }

                UK2Node_Timeline* Node = NewObject<UK2Node_Timeline>(Graph);
                Node->TimelineName = FName(*TimelineName);
                Result = Node;
            }
            else if (Type == TEXT("AnimGetter"))
            {
                // SetFromFunction resolves against the owning Blueprint, which the node reaches through its
                // outer, so the whole setup runs before the shared placement below allocates the pins.
                UK2Node_AnimGetter* Node = NewObject<UK2Node_AnimGetter>(Graph);
                if (!ConfigureAnimGetter(Context, Node, Desc))
                {
                    return nullptr;
                }

                Result = Node;
            }
            else if (Type == TEXT("Knot"))
            {
                Result = NewObject<UK2Node_Knot>(Graph);
            }
            else if (Type == TEXT("Comment"))
            {
                Result = NewObject<UEdGraphNode_Comment>(Graph);
            }
            else if (Type.StartsWith(TEXT("/")))
            {
                // A class path builds the node straight from its class, which is how anim graph nodes and
                // anything else outside the K2 set gets in without a case of its own here.
                UClass* NodeClass = LoadClass<UEdGraphNode>(nullptr, *Type);
                if (!NodeClass)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Cannot resolve node class '%s'"), *Type);
                    return nullptr;
                }

                Result = NewObject<UEdGraphNode>(Graph, NodeClass);
            }
            else if (UEdGraph* StandardMacro = ResolveMacroGraph(FString::Printf(TEXT("%s:%s"), kStandardMacros, *Type)))
            {
                // Anything left that names a StandardMacros graph: Gate, DoOnce, ForEachLoop, "Do N".
                UK2Node_MacroInstance* Node = NewObject<UK2Node_MacroInstance>(Graph);
                Node->SetMacroGraph(StandardMacro);
                Result = Node;
            }
            else
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Unknown node Type '%s'"), *Type);
                return nullptr;
            }

            PlaceNode(Result, Desc);
            // Editor heals missing RF_Transactional on open and marks the Blueprint dirty, which raises the resave toast
            Result->SetFlags(RF_Transactional);
            Graph->AddNode(Result, /* bFromUI */ false, /* bSelectNewNode */ false);
            Result->CreateNewGuid();

            // The construct-object family reads its own pins inside PostPlacedNewNode and needs the spawner's
            // order. Everything else wants the reverse, MathExpression builds its bound graph in that call.
            if (Result->IsA<UK2Node_ConstructObjectFromClass>())
            {
                Result->AllocateDefaultPins();
                Result->PostPlacedNewNode();
            }
            else
            {
                Result->PostPlacedNewNode();
                Result->AllocateDefaultPins();
            }

            // Select derives its option pins from the index type, which only exists once defaults are in.
            if (UK2Node_Select* SelectNode = Cast<UK2Node_Select>(Result))
            {
                if (!ConfigureSelect(SelectNode, Desc))
                {
                    return nullptr;
                }
            }

            // Expression parses into pins on reconstruct only, PostPlacedNewNode just makes the bound graph.
            if (UK2Node_MathExpression* MathNode = Cast<UK2Node_MathExpression>(Result))
            {
                MathNode->ReconstructNode();
            }

            // Purity swaps the exec pins for a success bool, which needs the default set to swap out of.
            if (UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Result))
            {
                bool bPureCast = false;
                if (Desc->TryGetBoolField(TEXT("PureCast"), bPureCast))
                {
                    CastNode->SetPurity(bPureCast);
                }
            }

            if (UK2Node_SpawnActorFromClass* SpawnActorNode = Cast<UK2Node_SpawnActorFromClass>(Result))
            {
                if (!ConfigureSpawnActor(SpawnActorNode, Desc))
                {
                    return nullptr;
                }
            }

            // Integer cases sit on no member, the node hands out the next consecutive name on demand.
            if (UK2Node_SwitchInteger* SwitchIntegerNode = Cast<UK2Node_SwitchInteger>(Result))
            {
                const TArray<TSharedPtr<FJsonValue>>* Cases = nullptr;
                if (Desc->TryGetArrayField(TEXT("Cases"), Cases))
                {
                    for (int32 Index = 0; Index < Cases->Num(); ++Index)
                    {
                        SwitchIntegerNode->AddPinToSwitchNode();
                    }
                }
            }

            // PostPlacedNewNode stamps the placeholder text, so the spec's own text has to land after it.
            if (UEdGraphNode_Comment* CommentNode = Cast<UEdGraphNode_Comment>(Result))
            {
                FString CommentText;
                if (!Desc->TryGetStringField(TEXT("Text"), CommentText))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Comment needs a Text"));
                    return nullptr;
                }

                CommentNode->NodeComment = CommentText;
                Desc->TryGetNumberField(TEXT("Width"), CommentNode->NodeWidth);
                Desc->TryGetNumberField(TEXT("Height"), CommentNode->NodeHeight);
            }

            // A Sequence allocates two outputs, further ones are the editor's Add pin button.
            if (UK2Node_ExecutionSequence* SequenceNode = Cast<UK2Node_ExecutionSequence>(Result))
            {
                int32 OutputCount = 0;
                if (Desc->TryGetNumberField(TEXT("OutputCount"), OutputCount))
                {
                    if (OutputCount < 2)
                    {
                        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Sequence: OutputCount %d is below the two the node always has"), OutputCount);
                        return nullptr;
                    }

                    while (SequenceNode->GetThenPinGivenIndex(OutputCount - 1) == nullptr)
                    {
                        SequenceNode->AddInputPin();
                    }
                }
            }

            return Result;
        }
    };
}

TUniquePtr<IBlueprintWriter> MakeBlueprintGraphWriter()
{
    return MakeUnique<FBlueprintGraphWriter>();
}
