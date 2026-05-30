using UnrealBuildTool;

public class AgentBlueprintTools : ModuleRules
{
    public AgentBlueprintTools(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        CppStandard = CppStandardVersion.Cpp17;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine"
        });

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "ApplicationCore",
            "AssetRegistry",
            "AssetTools",
            "BlueprintGraph",
            "EditorScriptingUtilities",
            "HTTP",
            "HTTPServer",
            "InputCore",
            "Json",
            "JsonUtilities",
            "Kismet",
            "KismetCompiler",
            "MaterialEditor",
            "MovieScene",
            "MovieSceneTracks",
            "Projects",
            "Slate",
            "SlateCore",
            "UMG",
            "UMGEditor",
            "UnrealEd"
        });
    }
}
