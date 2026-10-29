#include "Edit/EditBlackboardCommandlet.h"

// Editor-only by design: writes editor-only key fields and fires the editor change notifications. Trap any Runtime-type drift early.
static_assert(WITH_EDITOR, "UAssetWorkbench commandlets are editor-only, keep the uplugin Module Type=Editor.");

#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

namespace
{
    const TCHAR* kKeyTypePrefix = TEXT("BlackboardKeyType_");

    struct FBlackboardEditContext
    {
        UBlackboardData* Blackboard = nullptr;
        FString AssetPath;
        int32 Ops = 0;

        // Entries that already held the given value.
        int32 Unchanged = 0;
    };

    // Takes the short form the blackboard editor shows (Object, Bool) or the class name BehaviorTreeExport prints.
    UClass* ResolveKeyType(const FString& Name)
    {
        UClass* Class = UAssetWorkbench::ResolveClass(Name);
        if (!Class || !Class->IsChildOf(UBlackboardKeyType::StaticClass()))
        {
            Class = UAssetWorkbench::ResolveClass(kKeyTypePrefix + Name);
        }

        if (!Class || !Class->IsChildOf(UBlackboardKeyType::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s is not a concrete blackboard key type. Use the short form such as Object, Bool, Enum, or the full class name"), *Name);
            return nullptr;
        }

        return Class;
    }

    FString DescribeKeyType(const UBlackboardKeyType* KeyType)
    {
        if (!KeyType)
        {
            return TEXT("<none>");
        }

        FString Name = KeyType->GetClass()->GetName();
        Name.RemoveFromStart(kKeyTypePrefix);
        return Name;
    }

    FBlackboardEntry* FindOwnKey(UBlackboardData* Blackboard, const FName Name)
    {
        return Blackboard->Keys.FindByPredicate([Name](const FBlackboardEntry& Entry) { return Entry.EntryName == Name; });
    }

    bool IsInheritedKey(const UBlackboardData* Blackboard, const FName Name)
    {
        return Blackboard->Parent && Blackboard->Parent->GetKeyID(Name) != FBlackboard::InvalidKey;
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

    bool ApplyKeyTypeProperties(FBlackboardEditContext& Context, FBlackboardEntry& Entry, const TSharedPtr<FJsonObject>& Op)
    {
        const TSharedPtr<FJsonObject>* Properties = nullptr;
        if (!Op->TryGetObjectField(TEXT("Properties"), Properties))
        {
            return true;
        }

        UBlackboardKeyType* KeyType = Entry.KeyType;
        TSharedPtr<FJsonObject> Pending = MakeShared<FJsonObject>();
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Properties)->Values)
        {
            const FString ValueText = JsonValueText(Pair.Value);
            const FProperty* Property = FindFProperty<FProperty>(KeyType->GetClass(), *Pair.Key);
            const bool bSameText = UAssetWorkbench::ReadPropertyPathText(KeyType, Pair.Key) == ValueText;
            const bool bSame = bSameText || (Property && UAssetWorkbench::HoldsImportedValue(Property, KeyType, ValueText));
            if (bSame)
            {
                ++Context.Unchanged;
                continue;
            }

            Pending->SetField(Pair.Key, Pair.Value);
        }

        if (Pending->Values.IsEmpty())
        {
            return true;
        }

        TMap<FString, FString> Before;
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Pending->Values)
        {
            Before.Add(Pair.Key, UAssetWorkbench::ReadPropertyPathText(KeyType, Pair.Key));
        }

        int32 Failures = 0;
        Context.Ops += UAssetWorkbench::ApplyProperties(KeyType, Pending, Failures);
        if (Failures > 0)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%d propert(ies) rejected on key %s (%s), nothing saved"), Failures, *Entry.EntryName.ToString(), *DescribeKeyType(KeyType));
            return false;
        }

        for (const TPair<FString, FString>& Pair : Before)
        {
            const FString After = UAssetWorkbench::ReadPropertyPathText(KeyType, Pair.Key);
            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s %s %s: %s -> %s"), *Context.AssetPath, *Entry.EntryName.ToString(), *Pair.Key, *Pair.Value, *After);
        }

        return true;
    }

    bool ApplyKeyFields(FBlackboardEditContext& Context, FBlackboardEntry& Entry, const TSharedPtr<FJsonObject>& Op)
    {
        const FString KeyName = Entry.EntryName.ToString();

        FString Category;
        if (Op->TryGetStringField(TEXT("Category"), Category))
        {
            if (Entry.EntryCategory == FName(*Category))
            {
                ++Context.Unchanged;
            }
            else
            {
                UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s %s Category: %s -> %s"), *Context.AssetPath, *KeyName, *Entry.EntryCategory.ToString(), *Category);
                Entry.EntryCategory = FName(*Category);
                ++Context.Ops;
            }
        }

        FString Description;
        if (Op->TryGetStringField(TEXT("Description"), Description))
        {
            if (Entry.EntryDescription == Description)
            {
                ++Context.Unchanged;
            }
            else
            {
                UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s %s Description: %s -> %s"), *Context.AssetPath, *KeyName, *Entry.EntryDescription, *Description);
                Entry.EntryDescription = Description;
                ++Context.Ops;
            }
        }

        bool bInstanceSynced = false;
        if (Op->TryGetBoolField(TEXT("InstanceSynced"), bInstanceSynced))
        {
            if (Entry.bInstanceSynced == bInstanceSynced)
            {
                ++Context.Unchanged;
            }
            else
            {
                UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s %s InstanceSynced: %s -> %s"), *Context.AssetPath, *KeyName, Entry.bInstanceSynced ? TEXT("True") : TEXT("False"), bInstanceSynced ? TEXT("True") : TEXT("False"));
                Entry.bInstanceSynced = bInstanceSynced;
                ++Context.Ops;
            }
        }

        return ApplyKeyTypeProperties(Context, Entry, Op);
    }

    bool ApplyKeyOp(FBlackboardEditContext& Context, const TSharedPtr<FJsonObject>& Op)
    {
        UBlackboardData* Blackboard = Context.Blackboard;

        FString Kind = TEXT("Modify");
        Op->TryGetStringField(TEXT("Op"), Kind);

        FString KeyName;
        if (!Op->TryGetStringField(TEXT("Name"), KeyName) || KeyName.IsEmpty())
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Keys %s needs Name"), *Kind);
            return false;
        }

        const FName Name(*KeyName);
        if (Kind == TEXT("Add"))
        {
            FString TypeName;
            if (!Op->TryGetStringField(TEXT("Type"), TypeName))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Keys Add needs Type"));
                return false;
            }

            UClass* KeyClass = ResolveKeyType(TypeName);
            if (!KeyClass)
            {
                return false;
            }

            if (Blackboard->GetKeyID(Name) != FBlackboard::InvalidKey)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s is already on %s or its parent chain"), *KeyName, *Blackboard->GetName());
                return false;
            }

            // Same steps as the blackboard editor's new key action.
            FBlackboardEntry NewEntry;
            NewEntry.EntryName = Name;
            NewEntry.KeyType = NewObject<UBlackboardKeyType>(Blackboard, KeyClass);
            FBlackboardEntry& Entry = Blackboard->Keys.Add_GetRef(NewEntry);

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s Add %s (%s)"), *Context.AssetPath, *KeyName, *DescribeKeyType(Entry.KeyType));
            ++Context.Ops;
            return ApplyKeyFields(Context, Entry, Op);
        }

        FBlackboardEntry* Entry = FindOwnKey(Blackboard, Name);
        if (!Entry)
        {
            if (IsInheritedKey(Blackboard, Name))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s is inherited from %s. Edit it on the blackboard that declares it"), *KeyName, *Blackboard->Parent->GetName());
            }
            else
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("No key %s on %s"), *KeyName, *Blackboard->GetName());
            }
            return false;
        }

        if (Kind == TEXT("Delete"))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s Delete %s (%s)"), *Context.AssetPath, *KeyName, *DescribeKeyType(Entry->KeyType));
            Blackboard->Keys.RemoveAll([Name](const FBlackboardEntry& Candidate) { return Candidate.EntryName == Name; });
            ++Context.Ops;
            return true;
        }

        if (Kind == TEXT("Modify"))
        {
            FString TypeName;
            if (Op->TryGetStringField(TEXT("Type"), TypeName))
            {
                UClass* KeyClass = ResolveKeyType(TypeName);
                if (!KeyClass)
                {
                    return false;
                }

                if (Entry->KeyType && Entry->KeyType->GetClass() == KeyClass)
                {
                    ++Context.Unchanged;
                }
                else
                {
                    // A new type starts from the type's defaults, the way the details panel swaps an instanced object.
                    const FString OldType = DescribeKeyType(Entry->KeyType);
                    Entry->KeyType = NewObject<UBlackboardKeyType>(Blackboard, KeyClass);
                    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s %s Type: %s -> %s"), *Context.AssetPath, *KeyName, *OldType, *DescribeKeyType(Entry->KeyType));
                    ++Context.Ops;
                }
            }

            return ApplyKeyFields(Context, *Entry, Op);
        }

        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Unknown Keys Op %s. Accepted: Add Delete Modify"), *Kind);
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

    // Empty or None clears the parent.
    bool ApplyParent(FBlackboardEditContext& Context, const TSharedPtr<FJsonObject>& Entry)
    {
        FString ParentPath;
        if (!Entry->TryGetStringField(TEXT("Parent"), ParentPath))
        {
            return true;
        }

        UBlackboardData* Blackboard = Context.Blackboard;
        UBlackboardData* Parent = nullptr;
        const bool bClear = ParentPath.IsEmpty() || ParentPath == TEXT("None");
        if (!bClear)
        {
            Parent = LoadObject<UBlackboardData>(nullptr, *ParentPath);
            if (!Parent)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to load blackboard: %s"), *ParentPath);
                return false;
            }
        }

        if (Blackboard->Parent == Parent)
        {
            ++Context.Unchanged;
            return true;
        }

        if (Parent && (Parent == Blackboard || Parent->IsChildOf(*Blackboard)))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s already has %s in its parent chain, the parent would form a cycle"), *Parent->GetName(), *Blackboard->GetName());
            return false;
        }

        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s Parent: %s -> %s"), *Context.AssetPath, Blackboard->Parent ? *Blackboard->Parent->GetPathName() : TEXT("None"), Parent ? *Parent->GetPathName() : TEXT("None"));
        Blackboard->Parent = Parent;

        // Drops own persistent keys such as SelfActor that the parent already declares, so only real clashes remain.
        Blackboard->UpdateParentKeys();

        // The engine resolves a name clash to the parent key and leaves the own key unreachable.
        if (Parent)
        {
            for (const FBlackboardEntry& Key : Blackboard->Keys)
            {
                if (Parent->GetKeyID(Key.EntryName) != FBlackboard::InvalidKey)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s is declared on both %s and the parent chain of %s. Delete one first"), *Key.EntryName.ToString(), *Blackboard->GetName(), *Parent->GetName());
                    return false;
                }
            }
        }

        ++Context.Ops;
        return true;
    }
}

UEditBlackboardCommandlet::UEditBlackboardCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UEditBlackboardCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("UAssetWorkbench v%s - EditBlackboard"), UASSET_WORKBENCH_VERSION_STRING);

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

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("EditBlackboard: %d target(s) %s"), Targets->Num(), bApply ? TEXT("[APPLY]") : TEXT("[DRY RUN]"));

    TSet<UObject*> Touched;
    int32 Ops = 0;
    for (const TSharedPtr<FJsonValue>& Value : *Targets)
    {
        const TSharedPtr<FJsonObject>& Entry = Value->AsObject();
        UAssetWorkbench::FFailureReasonCapture FailureReason;
        if (!Entry.IsValid() || !ApplyTarget(Entry, Touched, Ops))
        {
            UAssetWorkbench::ReportTargetFailure(TEXT("EditBlackboard"), UAssetWorkbench::DescribeSpecTarget(Entry), FailureReason.GetReason());
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }
    }

    if (!bApply)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Done. %d change(s) staged across %d asset(s) (dry run, not saved)."), Ops, Touched.Num());
        return ToExitCode(EUAssetWorkbenchExitType::Success);
    }

    for (UObject* Blackboard : Touched)
    {
        UAssetWorkbench::FFailureReasonCapture FailureReason;
        if (!UAssetWorkbench::CompileAndSavePackage(Blackboard, false))
        {
            UAssetWorkbench::ReportTargetFailure(TEXT("EditBlackboard"), Blackboard->GetPathName(), FailureReason.GetReason());
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }
    }

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("Done. %d change(s) across %d asset(s) (saved)."), Ops, Touched.Num());
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}

bool UEditBlackboardCommandlet::ApplyTarget(const TSharedPtr<FJsonObject>& Entry, TSet<UObject*>& OutTouched, int32& OutOps) const
{
    FString AssetPath;
    if (!Entry->TryGetStringField(TEXT("AssetPath"), AssetPath))
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Target has no AssetPath field"));
        return false;
    }

    const bool bWritesSomething = Entry->HasField(TEXT("Parent")) || Entry->HasField(TEXT("Keys"));
    if (!bWritesSomething)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s writes nothing. Expected Parent or Keys"), *AssetPath);
        return false;
    }

    UBlackboardData* Blackboard = LoadObject<UBlackboardData>(nullptr, *AssetPath);
    if (!Blackboard)
    {
        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Failed to load blackboard: %s"), *AssetPath);
        return false;
    }

    FBlackboardEditContext Context;
    Context.Blackboard = Blackboard;
    Context.AssetPath = AssetPath;
    Blackboard->Modify();

    // Parent first, key name clashes are checked against the new parent chain.
    if (!ApplyParent(Context, Entry))
    {
        return false;
    }

    if (!ForEachOp(Entry, TEXT("Keys"), [&Context](const TSharedPtr<FJsonObject>& Op) { return ApplyKeyOp(Context, Op); }))
    {
        return false;
    }

    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s: %d change(s), %d entr%s already held the value"), *AssetPath, Context.Ops, Context.Unchanged, Context.Unchanged == 1 ? TEXT("y") : TEXT("ies"));

    // Nothing differed, so there is nothing to save.
    if (Context.Ops == 0)
    {
        return true;
    }

    // Same notification the blackboard editor sends, derived blackboards loaded in this process refresh their inherited keys.
    Blackboard->UpdateIfHasSynchronizedKeys();
    FPropertyChangedEvent KeysChanged(FindFProperty<FProperty>(UBlackboardData::StaticClass(), GET_MEMBER_NAME_CHECKED(UBlackboardData, Keys)));
    Blackboard->PostEditChangeProperty(KeysChanged);

    TArray<FString> KeyList;
    for (const FBlackboardEntry& Key : Blackboard->Keys)
    {
        KeyList.Add(FString::Printf(TEXT("%s (%s)"), *Key.EntryName.ToString(), *DescribeKeyType(Key.KeyType)));
    }
    UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("%s own keys: %s"), *AssetPath, *FString::Join(KeyList, TEXT(", ")));

    Blackboard->MarkPackageDirty();
    OutOps += Context.Ops;
    OutTouched.Add(Blackboard);
    return true;
}
