#pragma once

#include "CoreMinimal.h"

class UBlueprint;
class UClass;

namespace UAssetWorkbench
{
    // One Blueprint call that names a parameter. A name fed by a pin link has no literal to rewrite.
    struct FParameterCallSite
    {
        FString Blueprint;
        FString Graph;
        FString Function;
        FString Literal;
        FString Rewritten;
        bool bFromLink = false;
    };

    struct FParameterCallMatcher
    {
        // A call qualifies when its function lives on one of these classes and carries one of these name pins.
        TArray<UClass*> OwnerClasses;
        TArray<FName> PinNames;

        // True when Literal names the renamed parameter, OutRewritten is the literal to write back.
        TFunction<bool(const FString& Literal, FString& OutRewritten)> Rewrite;
    };

    // Blueprints that reach a seed package within Depth referencer hops, through any asset in between.
    TArray<FString> CollectReferencingBlueprints(const TArray<FName>& SeedPackages, int32 Depth);

    // Lists every qualifying call. With bApply the literal ones are rewritten, the caller compiles and saves.
    // Calls named through a link are listed with bFromLink and never touched.
    int32 RewriteParameterCalls(UBlueprint* Blueprint, const FParameterCallMatcher& Matcher, bool bApply, TArray<FParameterCallSite>& OutSites);

    void LogParameterCallSites(const TArray<FParameterCallSite>& Sites);
}
