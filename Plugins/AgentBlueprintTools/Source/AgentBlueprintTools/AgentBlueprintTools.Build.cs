using UnrealBuildTool;

public class AgentBlueprintTools : ModuleRules
{
    public AgentBlueprintTools(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine"
        });

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "ApplicationCore",
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
            "Projects",
            "Slate",
            "SlateCore",
            "UnrealEd"
        });
    }
}
