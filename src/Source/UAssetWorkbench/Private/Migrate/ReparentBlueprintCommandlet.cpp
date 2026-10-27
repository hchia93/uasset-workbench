#include "Migrate/ReparentBlueprintCommandlet.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

UReparentBlueprintCommandlet::UReparentBlueprintCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UReparentBlueprintCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    FString OldClassName, NewClassName;
    TArray<FString> BlueprintPaths = UAssetWorkbench::ParsePathList(Params, TEXT("-blueprints="));

    if (BlueprintPaths.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Usage: -run=ReparentBlueprint -blueprints=\"/Game/Path/BP_X\" -oldclass=\"AOldClass\" -newclass=\"ANewClass\""));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    if (!FParse::Value(*Params, TEXT("-oldclass="), OldClassName, true))
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Missing -oldclass parameter"));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    if (!FParse::Value(*Params, TEXT("-newclass="), NewClassName, true))
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Missing -newclass parameter"));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("ReparentBlueprint: %d blueprint(s), %s -> %s"),
           BlueprintPaths.Num(), *OldClassName, *NewClassName);

    int32 Reparented = 0;
    for (const FString& BlueprintPath : BlueprintPaths)
    {
        if (ReparentBlueprint(BlueprintPath, OldClassName, NewClassName))
        {
            ++Reparented;
        }
    }

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("Done. Reparented %d/%d blueprint(s)."), Reparented, BlueprintPaths.Num());
    return ToExitCode(Reparented == BlueprintPaths.Num() ? EUAssetWorkbenchExitType::Success : EUAssetWorkbenchExitType::Failed);
}

bool UReparentBlueprintCommandlet::ReparentBlueprint(
    const FString& BlueprintPath,
    const FString& OldClassName,
    const FString& NewClassName) const
{
    // Load the Blueprint
    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
    if (!Blueprint)
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Warning, TEXT("Failed to load blueprint: %s"), *BlueprintPath);
        return false;
    }

    UPackage* Package = Blueprint->GetOutermost();
    Package->FullyLoad();

    // Get the generated class
    UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);
    if (!GeneratedClass)
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Warning, TEXT("Blueprint has no generated class: %s"), *BlueprintPath);
        return false;
    }

    // Check if the current parent matches the old class name (with or without A prefix)
    UClass* CurrentParent = GeneratedClass->GetSuperClass();
    if (!CurrentParent)
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Warning, TEXT("Blueprint has no parent class: %s"), *BlueprintPath);
        return false;
    }

    if (!UAssetWorkbench::NamesClass(CurrentParent, OldClassName))
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Warning, TEXT("Blueprint parent mismatch (expected %s, got %s): %s"), *OldClassName, *CurrentParent->GetName(), *BlueprintPath);
        return false;
    }

    UClass* NewParentClass = UAssetWorkbench::ResolveClass(NewClassName);
    if (!NewParentClass)
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Warning, TEXT("New class not found: %s"), *NewClassName);
        return false;
    }

    // Same sequence as the editor's reparent. The compiler moves the generated class onto the new super itself,
    // setting it by hand first leaves the reinstancer reading old instances with the new layout.
    Blueprint->Modify();
    Blueprint->ParentClass = NewParentClass;

    if (Blueprint->SimpleConstructionScript)
    {
        Blueprint->SimpleConstructionScript->ValidateSceneRootNodes();
    }

    FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    GeneratedClass->PrepareToConformSparseClassData(NewParentClass->GetSparseClassDataStruct());

    const EBlueprintCompileOptions CompileOptions = EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::UseDeltaSerializationDuringReinstancing | EBlueprintCompileOptions::SkipNewVariableDefaultsDetection;
    FKismetEditorUtilities::CompileBlueprint(Blueprint, CompileOptions);

    if (Blueprint->Status == BS_Error)
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Compile failed after reparenting, package not saved: %s"), *BlueprintPath);
        return false;
    }

    if (UAssetWorkbench::CompileAndSavePackage(Blueprint, /* bCompileBlueprint */ false))
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("Successfully reparented: %s"), *BlueprintPath);
        return true;
    }
    else
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Failed to save blueprint: %s"), *BlueprintPath);
        return false;
    }
}
