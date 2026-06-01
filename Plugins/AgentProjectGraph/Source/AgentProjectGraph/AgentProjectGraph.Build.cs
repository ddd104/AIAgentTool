using UnrealBuildTool;

public class AgentProjectGraph : ModuleRules
{
    public AgentProjectGraph(ReadOnlyTargetRules Target) : base(Target)
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
            "AssetRegistry",
            "BlueprintGraph",
            "HTTPServer",
            "Json",
            "UnrealEd"
        });
    }
}
