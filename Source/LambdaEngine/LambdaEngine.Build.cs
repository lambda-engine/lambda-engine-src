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
			"LambdaSource"
		});

		PrivateDependencyModuleNames.AddRange(new string[] { });
	}
}
