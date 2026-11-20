#include "Edit/BlueprintWriter.h"
#include "UAssetWorkbenchModule.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_CustomEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/Kismet2NameValidators.h"

namespace
{
    FString DescribeCustomEvents(UBlueprint* Blueprint)
    {
        TArray<UK2Node_CustomEvent*> Events;
        FBlueprintEditorUtils::GetAllNodesOfClass<UK2Node_CustomEvent>(Blueprint, Events);

        TArray<FString> Names;
        for (const UK2Node_CustomEvent* Event : Events)
        {
            Names.Add(Event->CustomFunctionName.ToString());
        }

        return Names.IsEmpty() ? FString(TEXT("<none>")) : FString::Join(Names, TEXT(", "));
    }

    // A call or bind targets this Blueprint when it is self context or names one of the Blueprint's own classes.
    bool TargetsBlueprint(const FMemberReference& Reference, const UBlueprint* Blueprint)
    {
        if (Reference.IsSelfContext())
        {
            return true;
        }

        const UClass* Parent = Reference.GetMemberParentClass();
        return Parent && (Parent == Blueprint->GeneratedClass || Parent == Blueprint->SkeletonGeneratedClass);
    }

    class FBlueprintEventWriter : public IBlueprintWriter
    {
    public:
        virtual const TCHAR* GetSpecKey() const override
        {
            return TEXT("Events");
        }

        virtual bool Apply(FBlueprintEditContext& Context, const TSharedPtr<FJsonValue>& Section) override
        {
            const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
            if (!Section->TryGetArray(Entries))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Events must be an array of operations"), *Context.AssetPath);
                return false;
            }

            for (const TSharedPtr<FJsonValue>& Value : *Entries)
            {
                const TSharedPtr<FJsonObject>& Desc = Value->AsObject();
                FString Op;
                if (!Desc.IsValid() || !Desc->TryGetStringField(TEXT("Op"), Op))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Events entry needs Op"), *Context.AssetPath);
                    return false;
                }

                if (Op != TEXT("Rename"))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: unknown Events op '%s'. Expected Rename"), *Context.AssetPath, *Op);
                    return false;
                }

                if (!Rename(Context, Desc))
                {
                    return false;
                }
            }

            return true;
        }

    private:
        bool Rename(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Desc) const
        {
            FString Name;
            FString NewName;
            if (!Desc->TryGetStringField(TEXT("Name"), Name) || !Desc->TryGetStringField(TEXT("NewName"), NewName))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Events Rename needs Name and NewName"), *Context.AssetPath);
                return false;
            }

            UBlueprint* Blueprint = Context.Blueprint;
            const FName OldName(*Name);
            const FName RequestedName(*NewName);

            TArray<UK2Node_CustomEvent*> Events;
            FBlueprintEditorUtils::GetAllNodesOfClass<UK2Node_CustomEvent>(Blueprint, Events);
            UK2Node_CustomEvent* const* Found = Events.FindByPredicate([OldName](const UK2Node_CustomEvent* Event) { return Event->CustomFunctionName == OldName; });
            if (!Found)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no custom event '%s'. Custom events: %s"), *Context.AssetPath, *Name, *DescribeCustomEvents(Blueprint));
                return false;
            }

            UK2Node_CustomEvent* Event = *Found;
            if (Event->IsOverride())
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: custom event '%s' overrides a parent event. Its name belongs to the parent"), *Context.AssetPath, *Name);
                return false;
            }

            const EValidatorResult Validity = Event->MakeNameValidator()->IsValid(NewName);
            if (Validity != EValidatorResult::Ok)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: cannot rename custom event '%s' to '%s', %s"), *Context.AssetPath, *Name, *NewName, *INameValidatorInterface::GetErrorString(NewName, Validity));
                return false;
            }

            // Calls and binds hold the name. The compiler resolves them by name, so each one is retargeted here.
            TArray<UK2Node_CallFunction*> Calls;
            FBlueprintEditorUtils::GetAllNodesOfClass<UK2Node_CallFunction>(Blueprint, Calls);
            int32 RetargetedCalls = 0;
            for (UK2Node_CallFunction* Call : Calls)
            {
                if (Call->FunctionReference.GetMemberName() != OldName || !TargetsBlueprint(Call->FunctionReference, Blueprint))
                {
                    continue;
                }

                Call->Modify();
                const FGuid Guid = Call->FunctionReference.GetMemberGuid();
                if (Call->FunctionReference.IsSelfContext())
                {
                    Call->FunctionReference.SetSelfMember(RequestedName, Guid);
                }
                else
                {
                    Call->FunctionReference.SetExternalMember(RequestedName, Call->FunctionReference.GetMemberParentClass(), Guid);
                }
                ++RetargetedCalls;
            }

            TArray<UK2Node_CreateDelegate*> Binds;
            FBlueprintEditorUtils::GetAllNodesOfClass<UK2Node_CreateDelegate>(Blueprint, Binds);
            int32 RetargetedBinds = 0;
            for (UK2Node_CreateDelegate* Bind : Binds)
            {
                if (Bind->GetFunctionName() != OldName)
                {
                    continue;
                }

                Bind->Modify();
                Bind->SetFunction(RequestedName);
                ++RetargetedBinds;
            }

            Event->Modify();
            Event->OnRenameNode(NewName);

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: custom event '%s' -> '%s', %d call(s) and %d bind(s) retargeted"), *Blueprint->GetName(), *Name, *NewName, RetargetedCalls, RetargetedBinds);

            Context.bNeedsStructuralRecompile = true;
            ++Context.Ops;
            return true;
        }
    };
}

TUniquePtr<IBlueprintWriter> MakeBlueprintEventWriter()
{
    return MakeUnique<FBlueprintEventWriter>();
}
