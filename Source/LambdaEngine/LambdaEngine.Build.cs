using UnrealBuildTool;

public class LambdaEngine : ModuleRules
{
	public LambdaEngine(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"EngineSettings",
			// The view model is a procedural mesh built from a Source .mdl at runtime.
			"ProceduralMeshComponent",
			"LambdaSource"
		});

		PrivateDependencyModuleNames.AddRange(new string[] { });
	}
}
