#include "Migrate/RenameMaterialParameterCommandlet.h"

// Editor-only by design: renames material graph expressions and instance editor data. Trap any Runtime-type drift early.
static_assert(WITH_EDITOR, "UAssetWorkbench commandlets are editor-only, keep the uplugin Module Type=Editor.");

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"
#include "UObject/UnrealType.h"

#include "Migrate/ParameterCallRewriter.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

namespace
{
    // Material to MI to the BP or mesh that holds it, and one more hop for the BP that spawns that holder.
    constexpr int32 kBlueprintSearchDepth = 3;

    int32 RenameInValue(const FProperty* Property, void* Value, FName From, FName To, bool bApply);

    // Walks structs and arrays only. Object references lead outside the instance and stay unvisited.
    int32 RenameParameterInfos(const UStruct* Struct, void* Data, FName From, FName To, bool bApply)
    {
        int32 Count = 0;
        for (TFieldIterator<FProperty> It(Struct); It; ++It)
        {
            for (int32 Index = 0; Index < It->ArrayDim; ++Index)
            {
                Count += RenameInValue(*It, It->ContainerPtrToValuePtr<void>(Data, Index), From, To, bApply);
            }
        }

        return Count;
    }

    int32 RenameInValue(const FProperty* Property, void* Value, FName From, FName To, bool bApply)
    {
        if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
        {
            if (StructProperty->Struct == FMaterialParameterInfo::StaticStruct())
            {
                FMaterialParameterInfo* Info = static_cast<FMaterialParameterInfo*>(Value);
                if (Info->Association != GlobalParameter || Info->Name != From)
                {
                    return 0;
                }

                if (bApply)
                {
                    Info->Name = To;
                }
                return 1;
            }

            return RenameParameterInfos(StructProperty->Struct, Value, From, To, bApply);
        }

        if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
        {
            if (!ArrayProperty->Inner->IsA<FStructProperty>())
            {
                return 0;
            }

            int32 Count = 0;
            FScriptArrayHelper Helper(ArrayProperty, Value);
            for (int32 Index = 0; Index < Helper.Num(); ++Index)
            {
                Count += RenameInValue(ArrayProperty->Inner, Helper.GetRawPtr(Index), From, To, bApply);
            }
            return Count;
        }

        return 0;
    }

    int32 RenameInstanceOverrides(UMaterialInstance* Instance, FName From, FName To, bool bApply)
    {
        int32 Count = RenameParameterInfos(Instance->GetClass(), Instance, From, To, bApply);
        if (UObject* EditorOnly = Instance->GetEditorOnlyData())
        {
            Count += RenameParameterInfos(EditorOnly->GetClass(), EditorOnly, From, To, bApply);
        }

        return Count;
    }

    bool DescendsFrom(const UMaterialInstance* Instance, const UMaterial* Material)
    {
        const UMaterialInterface* Parent = Instance->Parent;
        while (const UMaterialInstance* ParentInstance = Cast<UMaterialInstance>(Parent))
        {
            Parent = ParentInstance->Parent;
        }

        return Parent == Material;
    }

    // Instances chain through each other, so the referencer walk continues only through instance packages.
    TArray<UMaterialInstanceConstant*> CollectInstances(UMaterial* Material)
    {
        IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

        TArray<UMaterialInstanceConstant*> Instances;
        TSet<FName> Visited;
        TArray<FName> Frontier = { Material->GetPackage()->GetFName() };
        while (Frontier.Num() > 0)
        {
            TArray<FName> NextFrontier;
            for (FName Package : Frontier)
            {
                TArray<FName> Referencers;
                Registry.GetReferencers(Package, Referencers, UE::AssetRegistry::EDependencyCategory::Package);

                for (FName Referencer : Referencers)
                {
                    if (Visited.Contains(Referencer))
                    {
                        continue;
                    }
                    Visited.Add(Referencer);

                    TArray<FAssetData> Assets;
                    Registry.GetAssetsByPackageName(Referencer, Assets, true);
                    for (const FAssetData& Asset : Assets)
                    {
                        const UClass* Class = Asset.GetClass();
                        if (!Class || !Class->IsChildOf(UMaterialInstanceConstant::StaticClass()))
                        {
                            continue;
                        }

                        UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Asset.GetAsset());
                        if (Instance && DescendsFrom(Instance, Material))
                        {
                            Instances.Add(Instance);
                            NextFrontier.Add(Referencer);
                        }
                    }
                }
            }

            Frontier = MoveTemp(NextFrontier);
        }

        return Instances;
    }

    TArray<UMaterialExpression*> FindParameterExpressions(UMaterial* Material, FName Name)
    {
        TArray<UMaterialExpression*> Found;
        for (UMaterialExpression* Expression : Material->GetExpressions())
        {
            if (Expression && Expression->HasAParameterName() && Expression->GetParameterName() == Name)
            {
                Found.Add(Expression);
            }
        }

        return Found;
    }
}

URenameMaterialParameterCommandlet::URenameMaterialParameterCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 URenameMaterialParameterCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("UAssetWorkbench v%s - RenameMaterialParameter"), UASSET_WORKBENCH_VERSION_STRING);

    FString MaterialPath;
    FString FromText;
    FString ToText;
    FParse::Value(*Params, TEXT("material="), MaterialPath);
    FParse::Value(*Params, TEXT("from="), FromText);
    FParse::Value(*Params, TEXT("to="), ToText);
    const bool bApply = FParse::Param(*Params, TEXT("apply"));

    if (MaterialPath.IsEmpty() || FromText.IsEmpty() || ToText.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("Usage: -run=RenameMaterialParameter -material=\"/Game/M_Base\" -from=\"Old\" -to=\"New\" [-blueprints=\"/Game/BP_A\"] [-apply]"));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    UMaterial* Material = LoadObject<UMaterial>(nullptr, *MaterialPath);
    if (!Material)
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("%s is not a base material. Parameters are declared on the UMaterial, pass that"), *MaterialPath);
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    const FName From(*FromText);
    const FName To(*ToText);

    const TArray<UMaterialExpression*> Expressions = FindParameterExpressions(Material, From);
    if (Expressions.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("%s has no parameter named %s"), *Material->GetPathName(), *FromText);
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    if (!FindParameterExpressions(Material, To).IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Error, TEXT("%s already has a parameter named %s"), *Material->GetPathName(), *ToText);
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("RenameMaterialParameter: %s %s -> %s %s"), *Material->GetPathName(), *FromText, *ToText, bApply ? TEXT("[APPLY]") : TEXT("[DRY RUN]"));
    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("  material: %d expression(s)"), Expressions.Num());

    const TArray<UMaterialInstanceConstant*> Instances = CollectInstances(Material);
    TArray<UMaterialInstanceConstant*> OverridingInstances;
    for (UMaterialInstanceConstant* Instance : Instances)
    {
        const int32 Overrides = RenameInstanceOverrides(Instance, From, To, false);
        if (Overrides > 0)
        {
            OverridingInstances.Add(Instance);
            UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("  instance %s: %d override(s)"), *Instance->GetPathName(), Overrides);
        }
    }
    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("  %d of %d instance(s) override it"), OverridingInstances.Num(), Instances.Num());

    TArray<FString> BlueprintPaths = UAssetWorkbench::ParsePathList(Params, TEXT("-blueprints="));
    if (BlueprintPaths.IsEmpty())
    {
        TArray<FName> Seeds = { Material->GetPackage()->GetFName() };
        for (UMaterialInstanceConstant* Instance : Instances)
        {
            Seeds.Add(Instance->GetPackage()->GetFName());
        }
        BlueprintPaths = UAssetWorkbench::CollectReferencingBlueprints(Seeds, kBlueprintSearchDepth);
    }

    // The literal can only be the bare parameter name. Collection parameters go through UKismetMaterialLibrary, left out.
    UAssetWorkbench::FParameterCallMatcher Matcher;
    Matcher.OwnerClasses = { UMaterialInterface::StaticClass(), UPrimitiveComponent::StaticClass() };
    Matcher.PinNames = { TEXT("ParameterName") };
    Matcher.Rewrite = [&FromText, &ToText](const FString& Literal, FString& OutRewritten)
    {
        if (Literal != FromText)
        {
            return false;
        }

        OutRewritten = ToText;
        return true;
    };

    TArray<UBlueprint*> TouchedBlueprints;
    TArray<UAssetWorkbench::FParameterCallSite> Sites;
    for (const FString& Path : BlueprintPaths)
    {
        UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *Path);
        if (!Blueprint)
        {
            UE_LOG(LogUAssetWorkbenchMigrator, Warning, TEXT("Failed to load Blueprint: %s"), *Path);
            continue;
        }

        if (UAssetWorkbench::RewriteParameterCalls(Blueprint, Matcher, bApply, Sites) > 0)
        {
            TouchedBlueprints.Add(Blueprint);
        }
    }
    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("  %d Blueprint call(s) in %d of %d Blueprint(s) in scope"), Sites.Num(), TouchedBlueprints.Num(), BlueprintPaths.Num());
    UAssetWorkbench::LogParameterCallSites(Sites);

    if (!bApply)
    {
        UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("Done. Dry run, nothing written. Pass -apply to rename."));
        return ToExitCode(EUAssetWorkbenchExitType::Success);
    }

    Material->Modify();
    for (UMaterialExpression* Expression : Expressions)
    {
        Expression->Modify();
        Expression->SetParameterName(To);
    }
    Material->PostEditChange();

    bool bSaveFailed = !UAssetWorkbench::CompileAndSavePackage(Material, false);

    for (UMaterialInstanceConstant* Instance : OverridingInstances)
    {
        Instance->Modify();
        RenameInstanceOverrides(Instance, From, To, true);

        // Static switch overrides are part of the permutation key, the renamed one needs a fresh permutation.
        {
            FMaterialInstanceParameterUpdateContext UpdateContext(Instance);
            UpdateContext.SetForceStaticPermutationUpdate(true);
        }
        Instance->PostEditChange();
        bSaveFailed |= !UAssetWorkbench::CompileAndSavePackage(Instance, false);
    }

    for (UBlueprint* Blueprint : TouchedBlueprints)
    {
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        bSaveFailed |= !UAssetWorkbench::CompileAndSavePackage(Blueprint);
    }

    UE_LOG(LogUAssetWorkbenchMigrator, Display, TEXT("Done. Renamed %s -> %s: material, %d instance(s), %d Blueprint(s) %s"), *FromText, *ToText, OverridingInstances.Num(), TouchedBlueprints.Num(), bSaveFailed ? TEXT("(SAVE FAILED)") : TEXT("(saved)"));
    UAssetWorkbench::WarnIfWrittenOutsideEditor();
    return ToExitCode(bSaveFailed ? EUAssetWorkbenchExitType::Failed : EUAssetWorkbenchExitType::Success);
}
