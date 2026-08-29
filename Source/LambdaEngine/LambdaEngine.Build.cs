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
			"LambdaSource",
			// nav_test reports on the navmesh the NPCs walk.
			"NavigationSystem"
		});

		// SlateCore: the runtime font face the HUD builds from the scheme's own .ttf (FFontFaceData/FFontData).
		// MoviePlayer: the loading screen, which has to be drawn on its own thread because the map load blocks
		// the game thread from start to finish.
		PrivateDependencyModuleNames.AddRange(new string[] { "SlateCore", "Slate", "MoviePlayer", "RenderCore" });
	}
}
