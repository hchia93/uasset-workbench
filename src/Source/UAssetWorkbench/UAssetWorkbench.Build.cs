using UnrealBuildTool;

public class UAssetWorkbench : ModuleRules
{
    public UAssetWorkbench(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                "InputCore",
                "UnrealEd",
                "AssetRegistry",
                "AssetTools",
                "MediaAssets",
                "Json",
                "JsonUtilities",
                "MessageLog",
                "BlueprintGraph",
                "KismetCompiler",
                "UMG",
                "UMGEditor",
                "MovieScene",
                "MovieSceneTracks",
                "Slate",
                "SlateCore",
                "MaterialEditor",
                "RenderCore",
                "RHI",
                "Niagara",
                "NiagaraCore",
                "NiagaraEditor",
                "PCG",
                "AIModule",
                "AnimGraph",
                "DirectoryWatcher",
                "Projects",
                "EditorSubsystem",
                "SubobjectDataInterface",
                "TargetPlatform",
                "TextureUtilitiesCommon"
            }
        );
    }
}
