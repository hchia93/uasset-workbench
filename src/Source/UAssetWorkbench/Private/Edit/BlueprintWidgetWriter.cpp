#include "Edit/BlueprintWriter.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"

#include "Animation/WidgetAnimation.h"
#include "Blueprint/WidgetNavigation.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "K2Node_Variable.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "MovieScene.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintEditorUtils.h"

namespace
{
    FString DescribeWidgets(const UWidgetBlueprint* WidgetBP)
    {
        TArray<FString> Names;
        WidgetBP->WidgetTree->ForEachWidget([&Names](UWidget* Widget)
        {
            if (Widget)
            {
                Names.Add(Widget->GetName());
            }
        });

        return FString::Join(Names, TEXT(", "));
    }

    // Same resolution order the importer uses, so a spec can name "SizeBox", "/Script/UMG.SizeBox" or a
    // WidgetBlueprint generated class path and mean the same thing.
    UClass* ResolveWidgetClass(const FString& ClassName)
    {
        if (ClassName.Contains(TEXT("/")))
        {
            return LoadObject<UClass>(nullptr, *ClassName);
        }

        if (UClass* UMGClass = UClass::TryFindTypeSlow<UClass>(FString::Printf(TEXT("/Script/UMG.%s"), *ClassName)))
        {
            return UMGClass;
        }

        return UClass::TryFindTypeSlow<UClass>(ClassName);
    }

    // Reparenting a widget under its own descendant would orphan the subtree from the root without the
    // tree ever reporting an error, so the walk runs before anything is detached.
    bool IsAncestorOf(const UWidget* Candidate, const UWidget* Widget)
    {
        for (const UWidget* Walker = Widget; Walker; Walker = Walker->GetParent())
        {
            if (Walker == Candidate)
            {
                return true;
            }
        }
        return false;
    }

    // Resolves either destination: a named panel, or the tree root when the spec says AsRoot.
    bool AttachWidget(const FBlueprintEditContext& Context, UWidgetBlueprint* WidgetBP, UWidget* Widget, const TSharedPtr<FJsonObject>& Desc, const TCHAR* OpLabel)
    {
        bool bAsRoot = false;
        Desc->TryGetBoolField(TEXT("AsRoot"), bAsRoot);

        FString ParentName;
        const bool bHasParent = Desc->TryGetStringField(TEXT("Parent"), ParentName);
        if (bAsRoot == bHasParent)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: widget %s needs exactly one of Parent or AsRoot"), *Context.AssetPath, OpLabel);
            return false;
        }

        if (bAsRoot)
        {
            WidgetBP->WidgetTree->Modify();
            WidgetBP->WidgetTree->RootWidget = Widget;
            return true;
        }

        UWidget* ParentWidget = WidgetBP->WidgetTree->FindWidget(FName(*ParentName));
        if (!ParentWidget)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: no widget named '%s'. Tree has: %s"), *Context.AssetPath, *ParentName, *DescribeWidgets(WidgetBP));
            return false;
        }

        UPanelWidget* Panel = Cast<UPanelWidget>(ParentWidget);
        if (!Panel)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' is a %s, not a panel, it cannot take children"), *Context.AssetPath, *ParentName, *ParentWidget->GetClass()->GetName());
            return false;
        }

        if (IsAncestorOf(Widget, Panel))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' sits inside '%s', reparenting there would detach the subtree"), *Context.AssetPath, *ParentName, *Widget->GetName());
            return false;
        }

        Panel->Modify();

        int32 Index = INDEX_NONE;
        const bool bHasIndex = Desc->TryGetNumberField(TEXT("Index"), Index);
        UPanelSlot* Slot = bHasIndex ? Panel->InsertChildAt(Index, Widget) : Panel->AddChild(Widget);
        if (!Slot)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' rejected child '%s', it is likely at its child limit"), *Context.AssetPath, *ParentName, *Widget->GetName());
            return false;
        }

        const TSharedPtr<FJsonObject>* SlotProperties = nullptr;
        if (!Desc->TryGetObjectField(TEXT("Slot"), SlotProperties))
        {
            return true;
        }

        int32 Failures = 0;
        Slot->Modify();
        UAssetWorkbench::ApplyProperties(Slot, *SlotProperties, Failures);
        if (Failures > 0)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: %d slot property write(s) failed on '%s'"), *Context.AssetPath, Failures, *Widget->GetName());
            return false;
        }

        return true;
    }

    // The old parent has to let go before the new one takes it, and a root widget has no parent to ask.
    void DetachWidget(UWidgetBlueprint* WidgetBP, UWidget* Widget)
    {
        if (UPanelWidget* OldParent = Widget->GetParent())
        {
            OldParent->Modify();
            OldParent->RemoveChild(Widget);
            return;
        }

        if (WidgetBP->WidgetTree->RootWidget == Widget)
        {
            WidgetBP->WidgetTree->Modify();
            WidgetBP->WidgetTree->RootWidget = nullptr;
        }
    }

    // The engine's RenameWidget lives on FWidgetBlueprintEditor, which a commandlet has no way to build.
    // Everything it does past the preview widget is plain data on the asset, so this mirrors that half.
    bool RenameWidgetInTree(const FBlueprintEditContext& Context, UWidgetBlueprint* WidgetBP, const FName& OldName, const FString& NewDisplayName)
    {
        UWidget* Widget = WidgetBP->WidgetTree->FindWidget(OldName);
        if (!Widget)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: no widget named '%s'. Tree has: %s"), *Context.AssetPath, *OldName.ToString(), *DescribeWidgets(WidgetBP));
            return false;
        }

        const FName NewFName(*NewDisplayName);
        if (OldName == NewFName)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Warning, TEXT("%s: widget '%s' already carries that name"), *Context.AssetPath, *OldName.ToString());
            return true;
        }

        // A BindWidget property on the parent class owns the new name on purpose, the validator would reject it.
        FObjectPropertyBase* ExistingProperty = CastField<FObjectPropertyBase>(WidgetBP->ParentClass ? WidgetBP->ParentClass->FindPropertyByName(NewFName) : nullptr);
        const bool bBindWidget = ExistingProperty && FWidgetBlueprintEditorUtils::IsBindWidgetProperty(ExistingProperty) && Widget->IsA(ExistingProperty->PropertyClass);

        FKismetNameValidator Validator(WidgetBP, OldName);
        const bool bNameFree = Validator.IsValid(NewFName) == EValidatorResult::Ok;
        if (!bNameFree && !bBindWidget)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' is already taken in this Blueprint"), *Context.AssetPath, *NewDisplayName);
            return false;
        }

        WidgetBP->Modify();
        Widget->Modify();

        // Carries the existing GUID onto the new name. Animation and binding lookups key off that GUID,
        // so skipping this is exactly how a rebuilt tree silently drops its animation bindings.
        WidgetBP->OnVariableRenamed(OldName, NewFName);

        Widget->SetDisplayLabel(NewDisplayName);
        Widget->Rename(*NewDisplayName);

#if UE_HAS_WIDGET_GENERATED_BY_CLASS
        // Getter nodes already sitting on the new name hold a stale generated class on their value pin.
        if (Widget->bIsVariable)
        {
            TArray<UEdGraph*> AllGraphs;
            WidgetBP->GetAllGraphs(AllGraphs);

            for (const UEdGraph* CurrentGraph : AllGraphs)
            {
                TArray<UK2Node_Variable*> GraphNodes;
                CurrentGraph->GetNodesOfClass(GraphNodes);

                for (UK2Node_Variable* CurrentNode : GraphNodes)
                {
                    UClass* SelfClass = WidgetBP->GeneratedClass;
                    UClass* VariableParent = CurrentNode->VariableReference.GetMemberParentClass(SelfClass);
                    const bool bOwnedHere = SelfClass == VariableParent;
                    const bool bMatchesNewName = NewFName == CurrentNode->GetVarName();
                    if (!bOwnedHere || !bMatchesNewName)
                    {
                        continue;
                    }

                    UEdGraphPin* ValuePin = CurrentNode->GetValuePin();
                    ValuePin->Modify();
                    CurrentNode->Modify();

                    CurrentNode->CreatePin(ValuePin->Direction, ValuePin->PinType.PinCategory, ValuePin->PinType.PinSubCategory, Widget->WidgetGeneratedByClass.Get(), NewFName);
                    ValuePin->bOrphanedPin = true;
                }
            }
        }
#endif // UE_HAS_WIDGET_GENERATED_BY_CLASS

        FWidgetBlueprintEditorUtils::ReplaceDesiredFocus(WidgetBP, OldName, NewFName);

        const FString OldNameStr = OldName.ToString();
        const FString NewNameStr = NewFName.ToString();
        for (FDelegateEditorBinding& Binding : WidgetBP->Bindings)
        {
            if (Binding.ObjectName == OldNameStr)
            {
                Binding.ObjectName = NewNameStr;
            }
        }

        for (UWidgetAnimation* Animation : WidgetBP->Animations)
        {
            if (!Animation)
            {
                continue;
            }

            for (FWidgetAnimationBinding& AnimBinding : Animation->AnimationBindings)
            {
                if (AnimBinding.WidgetName != OldName)
                {
                    continue;
                }

                AnimBinding.WidgetName = NewFName;
                Animation->MovieScene->Modify();

                // A slot binding names the slot, not the widget, so the possessable keeps its own label.
                if (AnimBinding.SlotWidgetName != NAME_None)
                {
                    break;
                }

                if (FMovieScenePossessable* Possessable = Animation->MovieScene->FindPossessable(AnimBinding.AnimationGuid))
                {
                    Possessable->SetName(NewNameStr);
                }
            }
        }

        WidgetBP->WidgetTree->ForEachWidget([OldName, NewFName](UWidget* Other)
        {
            if (Other && Other->Navigation)
            {
                Other->Navigation->SetFlags(RF_Transactional);
                Other->Navigation->Modify();
                Other->Navigation->TryToRenameBinding(OldName, NewFName);
            }
        });

        FBlueprintEditorUtils::ValidateBlueprintChildVariables(WidgetBP, NewFName);
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
        FBlueprintEditorUtils::ReplaceVariableReferences(WidgetBP, OldName, NewFName);
        return true;
    }

    class FBlueprintWidgetWriter : public IBlueprintWriter
    {
    public:
        virtual const TCHAR* GetSpecKey() const override
        {
            return TEXT("Widgets");
        }

        virtual bool Apply(FBlueprintEditContext& Context, const TSharedPtr<FJsonValue>& Section) override
        {
            const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
            if (!Section->TryGetArray(Operations))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Widgets must be an array of operations"), *Context.AssetPath);
                return false;
            }

            UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(Context.Blueprint);
            if (!WidgetBP || !WidgetBP->WidgetTree)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s is not a WidgetBlueprint, Widgets does not apply"), *Context.AssetPath);
                return false;
            }

            for (const TSharedPtr<FJsonValue>& Value : *Operations)
            {
                const TSharedPtr<FJsonObject>& Desc = Value->AsObject();
                FString Op;
                FString Name;
                if (!Desc.IsValid() || !Desc->TryGetStringField(TEXT("Op"), Op) || !Desc->TryGetStringField(TEXT("Name"), Name))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: widget operation needs Op and Name"), *Context.AssetPath);
                    return false;
                }

                UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: widget %s %s"), *WidgetBP->GetName(), *Op, *Name);
                ++Context.Ops;

                if (!ApplyOne(Context, WidgetBP, Op, Name, Desc))
                {
                    return false;
                }
            }

            Context.bNeedsStructuralRecompile = true;
            return true;
        }

    private:
        bool ApplyOne(const FBlueprintEditContext& Context, UWidgetBlueprint* WidgetBP, const FString& Op, const FString& Name, const TSharedPtr<FJsonObject>& Desc) const
        {
            if (Op == TEXT("Rename"))
            {
                FString NewName;
                if (!Desc->TryGetStringField(TEXT("NewName"), NewName))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: widget Rename needs a NewName"), *Context.AssetPath);
                    return false;
                }

                return RenameWidgetInTree(Context, WidgetBP, FName(*Name), NewName);
            }

            if (Op == TEXT("Modify"))
            {
                return ApplyModify(Context, WidgetBP, Name, Desc);
            }

            if (Op == TEXT("Add"))
            {
                return ApplyAdd(Context, WidgetBP, Name, Desc);
            }

            if (Op == TEXT("Delete"))
            {
                return ApplyDelete(Context, WidgetBP, Name, Desc);
            }

            if (Op == TEXT("Reparent"))
            {
                return ApplyReparent(Context, WidgetBP, Name, Desc);
            }

            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: unknown widget Op '%s'. Accepted: Add, Delete, Reparent, Rename, Modify"), *Context.AssetPath, *Op);
            return false;
        }

        bool ApplyAdd(const FBlueprintEditContext& Context, UWidgetBlueprint* WidgetBP, const FString& Name, const TSharedPtr<FJsonObject>& Desc) const
        {
            FString ClassName;
            if (!Desc->TryGetStringField(TEXT("Class"), ClassName))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: widget Add needs a Class"), *Context.AssetPath);
                return false;
            }

            UClass* WidgetClass = ResolveWidgetClass(ClassName);
            if (!WidgetClass || !WidgetClass->IsChildOf(UWidget::StaticClass()))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' does not resolve to a UWidget class"), *Context.AssetPath, *ClassName);
                return false;
            }

            const FName NewName(*Name);
            if (WidgetBP->WidgetTree->FindWidget(NewName))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' is already taken in this Blueprint"), *Context.AssetPath, *Name);
                return false;
            }

            WidgetBP->Modify();
            WidgetBP->WidgetTree->Modify();
            UWidget* Widget = WidgetBP->WidgetTree->ConstructWidget<UWidget>(WidgetClass, NewName);
            if (!Widget)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: failed to construct '%s' of class %s"), *Context.AssetPath, *Name, *ClassName);
                return false;
            }

            Widget->SetDisplayLabel(Name);

            const TSharedPtr<FJsonObject>* Properties = nullptr;
            if (Desc->TryGetObjectField(TEXT("Properties"), Properties))
            {
                int32 Failures = 0;
                UAssetWorkbench::ApplyProperties(Widget, *Properties, Failures);
                if (Failures > 0)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: %d property write(s) failed on '%s'"), *Context.AssetPath, Failures, *Name);
                    return false;
                }
            }

            return AttachWidget(Context, WidgetBP, Widget, Desc, TEXT("Add"));
        }

        bool ApplyDelete(const FBlueprintEditContext& Context, UWidgetBlueprint* WidgetBP, const FString& Name, const TSharedPtr<FJsonObject>& Desc) const
        {
            UWidget* Widget = WidgetBP->WidgetTree->FindWidget(FName(*Name));
            if (!Widget)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: no widget named '%s'. Tree has: %s"), *Context.AssetPath, *Name, *DescribeWidgets(WidgetBP));
                return false;
            }

            // Deleting a panel takes its whole subtree with it, which is rarely what a spec author typed.
            bool bRecursive = false;
            Desc->TryGetBoolField(TEXT("Recursive"), bRecursive);

            const UPanelWidget* Panel = Cast<UPanelWidget>(Widget);
            if (Panel && Panel->HasAnyChildren() && !bRecursive)
            {
                TArray<FString> ChildNames;
                for (int32 ChildIndex = 0; ChildIndex < Panel->GetChildrenCount(); ChildIndex++)
                {
                    if (const UWidget* Child = Panel->GetChildAt(ChildIndex))
                    {
                        ChildNames.Add(Child->GetName());
                    }
                }
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' still holds %s. Reparent them first, or pass Recursive to delete the subtree"), *Context.AssetPath, *Name, *FString::Join(ChildNames, TEXT(", ")));
                return false;
            }

            WidgetBP->Modify();
            WidgetBP->WidgetTree->Modify();
            FWidgetBlueprintEditorUtils::DeleteWidgets(WidgetBP, { Widget }, FWidgetBlueprintEditorUtils::EDeleteWidgetWarningType::DeleteSilently);
            return true;
        }

        bool ApplyReparent(const FBlueprintEditContext& Context, UWidgetBlueprint* WidgetBP, const FString& Name, const TSharedPtr<FJsonObject>& Desc) const
        {
            UWidget* Widget = WidgetBP->WidgetTree->FindWidget(FName(*Name));
            if (!Widget)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: no widget named '%s'. Tree has: %s"), *Context.AssetPath, *Name, *DescribeWidgets(WidgetBP));
                return false;
            }

            WidgetBP->Modify();
            Widget->Modify();
            DetachWidget(WidgetBP, Widget);
            return AttachWidget(Context, WidgetBP, Widget, Desc, TEXT("Reparent"));
        }

        bool ApplyModify(const FBlueprintEditContext& Context, UWidgetBlueprint* WidgetBP, const FString& Name, const TSharedPtr<FJsonObject>& Desc) const
        {
            UWidget* Widget = WidgetBP->WidgetTree->FindWidget(FName(*Name));
            if (!Widget)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: no widget named '%s'. Tree has: %s"), *Context.AssetPath, *Name, *DescribeWidgets(WidgetBP));
                return false;
            }

            const TSharedPtr<FJsonObject>* Properties = nullptr;
            const TSharedPtr<FJsonObject>* SlotProperties = nullptr;
            const bool bHasProperties = Desc->TryGetObjectField(TEXT("Properties"), Properties);
            const bool bHasSlot = Desc->TryGetObjectField(TEXT("Slot"), SlotProperties);
            if (!bHasProperties && !bHasSlot)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: widget Modify needs Properties or Slot"), *Context.AssetPath);
                return false;
            }

            int32 Failures = 0;

            if (bHasProperties)
            {
                Widget->Modify();
                UAssetWorkbench::ApplyProperties(Widget, *Properties, Failures);
            }

            if (bHasSlot)
            {
                if (!Widget->Slot)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: widget '%s' has no slot, it is the root or not in a panel"), *Context.AssetPath, *Name);
                    return false;
                }

                Widget->Slot->Modify();
                UAssetWorkbench::ApplyProperties(Widget->Slot, *SlotProperties, Failures);
            }

            if (Failures > 0)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: %d property write(s) failed on '%s'"), *Context.AssetPath, Failures, *Name);
                return false;
            }

            return true;
        }
    };
}

TUniquePtr<IBlueprintWriter> MakeBlueprintWidgetWriter()
{
    return MakeUnique<FBlueprintWidgetWriter>();
}
