using UnrealBuildTool;

public class LambdaSource : ModuleRules
{
	public LambdaSource(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"DeveloperSettings",
			"ProceduralMeshComponent"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"RenderCore",
			"RHI",
			"Projects",
			// USoundWaveProcedural derives from IAudioProxyDataFactory, which lives here.
			"AudioExtensions"
		});
	}
}
