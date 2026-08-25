using UnrealBuildTool;

public class LambdaSource : ModuleRules
{
	public LambdaSource(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// The module's own root, because its headers live in folders named after what they do rather than under
		// Public/. Without this an include would have to be written relative to wherever the compiler started.
		PublicIncludePaths.Add(ModuleDirectory);

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
