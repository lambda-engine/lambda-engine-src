#include "LambdaEngine.h"
#include "RenderUtils.h"
#include "FileSystem/LambdaFileSystem.h"
#include "Misc/CommandLine.h"
#include "Engine/World.h"
#include "GameMapsSettings.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Core/LambdaSourceSettings.h"
#include "Core/SourceCoordinates.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "World/SourceBSPWorldActor.h"
#include "Entities/SourcePropPhysics.h"
#include "EngineUtils.h"
#include "LambdaCharacter.h"
#include "LambdaSuitVoice.h"
#include "Audio/SourceSentences.h"
#include "Gameplay/SourceDamage.h"
#include "Materials/SourceDecalScript.h"
#include "Materials/SourceSurfaceProps.h"
#include "LambdaConsole.h"
#include "Creatures/SourceNPCBase.h"
#include "NavigationSystem.h"
#include "NavigationPath.h"
#include "TimerManager.h"
#include "NavMesh/RecastNavMesh.h"
#include "LambdaLoadingScreen.h"
#include "GameFramework/GameUserSettings.h"
#include "Misc/CoreDelegates.h"
#include "Engine/GameEngine.h"
#include "Widgets/SWindow.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

DEFINE_LOG_CATEGORY(LogLambda);

namespace
{
	/** Everything a "+command" on the command line asked for, in the order it was written. */
	TArray<FString> GStartupCommands;

	/** What the command line asked the window to be, for ApplyWindowSettings to carry out. */
	struct FWindowRequest
	{
		bool bModeAsked = false;		// the line named a window mode, in either engine's spelling
		bool bModeFromUnreal = false;	// ...and it was Unreal's own spelling, which Unreal has acted on
		EWindowMode::Type Mode = EWindowMode::Fullscreen;
		int32 Width = 0;
		int32 Height = 0;
		int32 PosX = 0;
		int32 PosY = 0;
		bool bHasPosition = false;
		bool bBorderless = false;	// -noborder: no frame, whatever size it ends up
	};

	FWindowRequest GWindowRequest;

	/**
	 * Source's window switches.
	 *
	 * The two engines want the same things by different names, and a modder launching this one reaches for the
	 * ones Source taught them:
	 *
	 *     -windowed, -window, -sw     windowed
	 *     -fullscreen, -full, -fs     full screen
	 *     -noborder                   no frame: a window of the given size without one, or the whole
	 *                                 display when no size is given
	 *     -w <n>, -width <n>          width
	 *     -h <n>, -height <n>         height
	 *     -x <n>, -y <n>              where the window goes
	 *
	 * Unreal's own -Windowed/-FullScreen/-ResX=/-ResY= are recognised too and left to Unreal, which reads them
	 * itself (UGameEngine::ConditionallyOverrideSettings).
	 *
	 * Whatever the Source spellings ask for is recorded rather than rewritten into Unreal's: the window is built
	 * from the command line before this module is loaded, so a switch translated here would arrive too late to
	 * be read. ApplyWindowSettings does the work once the engine is up.
	 *
	 * The tokens are still taken off the line. Source writes a value as its own token ("-w 1280"), and a bare
	 * "1280" left behind is read by UGameInstance::GetMapOverrideName as a map to travel to - the same trap
	 * "+map" falls into.
	 *
	 * @param Tokens  The whole command line, tokenised.
	 * @param i       The token being looked at; advanced past a consumed value.
	 * @return Whether the token was one of Source's and has been taken.
	 */
	bool TakeWindowArg(const TArray<FString>& Tokens, int32& i)
	{
		const FString& Token = Tokens[i];

		if (!Token.StartsWith(TEXT("-")) || Token.Len() < 2)
		{
			return false;
		}

		const FString Switch = Token.RightChop(1);

		// Unreal's own spellings are read here as well as being left on the line for Unreal. Reading them
		// rather than deferring is what lets "-windowed -w 1280 -h 720" work: Unreal takes the mode from the
		// line but knows nothing of Source's -w, and there is no way to ask it afterwards what it decided -
		// its override never reaches the settings object. So the mode is decided here, from either spelling,
		// and Unreal's copy of it only saves a resize as the window first appears.
		if (Switch.StartsWith(TEXT("Res="), ESearchCase::IgnoreCase)
			|| Switch.StartsWith(TEXT("ResX="), ESearchCase::IgnoreCase)
			|| Switch.StartsWith(TEXT("ResY="), ESearchCase::IgnoreCase))
		{
			GWindowRequest.bModeFromUnreal = true;
			return false;
		}

		if (Switch.Equals(TEXT("windowed"), ESearchCase::IgnoreCase))
		{
			GWindowRequest.bModeAsked = true;
			GWindowRequest.bModeFromUnreal = true;
			GWindowRequest.Mode = EWindowMode::Windowed;
			return false;   // Unreal's own; left for it to read too
		}

		if (Switch.Equals(TEXT("fullscreen"), ESearchCase::IgnoreCase))
		{
			GWindowRequest.bModeAsked = true;
			GWindowRequest.bModeFromUnreal = true;
			GWindowRequest.Mode = EWindowMode::Fullscreen;
			return false;   // Unreal's own; left for it to read too
		}

		if (Switch.Equals(TEXT("window"), ESearchCase::IgnoreCase)
			|| Switch.Equals(TEXT("sw"), ESearchCase::IgnoreCase))
		{
			GWindowRequest.bModeAsked = true;
			GWindowRequest.Mode = EWindowMode::Windowed;
			return true;
		}

		if (Switch.Equals(TEXT("full"), ESearchCase::IgnoreCase)
			|| Switch.Equals(TEXT("fs"), ESearchCase::IgnoreCase))
		{
			GWindowRequest.bModeAsked = true;
			GWindowRequest.Mode = EWindowMode::Fullscreen;
			return true;
		}

		if (Switch.Equals(TEXT("noborder"), ESearchCase::IgnoreCase))
		{
			// The mode it turns into depends on whether a size comes with it, which may not have been read
			// yet - so it is only noted here and decided in ApplyWindowSettings.
			GWindowRequest.bModeAsked = true;
			GWindowRequest.bBorderless = true;
			return true;
		}

		// The ones that carry a value in the next token.
		const bool bWidth = Switch.Equals(TEXT("w"), ESearchCase::IgnoreCase)
			|| Switch.Equals(TEXT("width"), ESearchCase::IgnoreCase);
		const bool bHeight = Switch.Equals(TEXT("h"), ESearchCase::IgnoreCase)
			|| Switch.Equals(TEXT("height"), ESearchCase::IgnoreCase);
		const bool bPosX = Switch.Equals(TEXT("x"), ESearchCase::IgnoreCase);
		const bool bPosY = Switch.Equals(TEXT("y"), ESearchCase::IgnoreCase);

		if (bWidth || bHeight || bPosX || bPosY)
		{
			// Only when a value actually follows, so a switch of the same name carrying none is left alone
			// rather than swallowing whatever came after it. Quoted because a launcher writing the line
			// for us may well quote it - "-w \"1280\"" is the same request as "-w 1280".
			const FString Next = i + 1 < Tokens.Num() ? Tokens[i + 1].TrimQuotes() : FString();

			if (Next.IsEmpty() || !Next.IsNumeric())
			{
				return false;
			}

			++i;
			const int32 Value = FCString::Atoi(*Next);
			if (bWidth)
			{
				GWindowRequest.Width = Value;
			}
			else if (bHeight)
			{
				GWindowRequest.Height = Value;
			}
			else
			{
				(bPosX ? GWindowRequest.PosX : GWindowRequest.PosY) = Value;
				GWindowRequest.bHasPosition = true;
			}
			return true;
		}

		return false;
	}

	/**
	 * Takes the frame off the game's window, leaving it the size it already is.
	 *
	 * Unreal has three window modes and none of them is "a window of this size with no frame" - full screen
	 * and windowed-full-screen both take the whole display. Source has it, `-noborder` is how a modder asks
	 * for it, and it is what a window has to be to sit inside another program's tab without a title bar
	 * across the top of it. So the mode stays Windowed and the frame comes off the native window.
	 */
	void RemoveWindowFrame()
	{
#if PLATFORM_WINDOWS
		UGameEngine* GameEngine = Cast<UGameEngine>(GEngine);
		TSharedPtr<SWindow> Window = GameEngine ? GameEngine->GameViewportWindow.Pin() : nullptr;
		TSharedPtr<FGenericWindow> NativeWindow = Window.IsValid() ? Window->GetNativeWindow() : nullptr;

		if (!NativeWindow.IsValid())
		{
			UE_LOG(LogLambda, Warning, TEXT("Window: -noborder had no window to take the frame off"));
			return;
		}

		const HWND Hwnd = static_cast<HWND>(NativeWindow->GetOSWindowHandle());
		if (!Hwnd)
		{
			return;
		}

		// The frame styles come off and nothing goes on in their place. Not WS_POPUP in particular: a popup
		// answers GetParent with its owner rather than its parent, so a tool that puts this window inside one
		// of its own - which is how the modding tool embeds it - can no longer tell where it ended up.
		LONG_PTR Style = GetWindowLongPtr(Hwnd, GWL_STYLE);
		Style &= ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
		SetWindowLongPtr(Hwnd, GWL_STYLE, Style);

		// SWP_FRAMECHANGED is what makes the style change take; the window is left exactly where and as big
		// as it already is, so the client area grows into what the frame was using.
		//
		// Nothing is moved or resized here on purpose. By the time this runs the window may already have been
		// made a child of somebody else's - the modding tool embeds the game in one of its tabs - and a child
		// is positioned relative to its parent, not to the screen. Passing it the screen coordinates it had a
		// moment ago would shove it that far into the corner of whatever it is now inside.
		SetWindowPos(Hwnd, nullptr, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
#endif
	}

	/**
	 * Puts the window into the mode the command line asked for, once there is an engine to ask.
	 *
	 * Source's switches cannot be answered while the window is being built - this module is not loaded yet - so
	 * they are answered immediately afterwards, through the same settings the game would change from a video
	 * options menu. A game launched without any of them goes full screen, which is where Source starts too.
	 */
	void ApplyWindowSettings()
	{
		if (GIsEditor || !GEngine)
		{
			return;
		}

		const bool bHasSize = GWindowRequest.Width > 0 && GWindowRequest.Height > 0;

		// The line spoke to Unreal in its own words and added nothing of Source's: Unreal has already built
		// the window it asked for, and setting the same mode over the top of a window that is already in it
		// is enough to bring the game down.
		if (GWindowRequest.bModeFromUnreal && !bHasSize && !GWindowRequest.bHasPosition)
		{
			return;
		}

		UGameUserSettings* Settings = GEngine->GetGameUserSettings();
		if (!Settings)
		{
			return;
		}

		// Whatever the line named, and full screen when it named nothing - which is where Source starts too.
		EWindowMode::Type Mode = GWindowRequest.bModeAsked ? GWindowRequest.Mode : EWindowMode::Fullscreen;

		// -noborder with a size is a window of that size with its frame taken off, the way Source's is - the
		// frame comes off further down, once the window exists. Without a size there is nothing to size it
		// to, so it becomes the borderless window that covers the display.
		if (GWindowRequest.bBorderless)
		{
			Mode = bHasSize ? EWindowMode::Windowed : EWindowMode::WindowedFullscreen;
		}

		Settings->SetFullscreenMode(Mode);

		FIntPoint Resolution(GWindowRequest.Width, GWindowRequest.Height);

		if (Resolution.X <= 0 || Resolution.Y <= 0)
		{
			// Nothing was asked for. Full screen means the whole display, so it is taken from the display
			// rather than from whatever size the settings happened to be left at - otherwise "full screen"
			// comes up as a 1280x720 window on a 1920x1080 monitor. A window keeps the size it had.
			if (Mode == EWindowMode::Windowed)
			{
				Resolution = Settings->GetScreenResolution();
			}
			else
			{
				Resolution = Settings->GetDesktopResolution();
			}
		}

		if (Resolution.X > 0 && Resolution.Y > 0)
		{
			Settings->SetScreenResolution(Resolution);
		}

		// Only a window can be put somewhere; a full screen one is where the display is.
		if (GWindowRequest.bHasPosition && Mode == EWindowMode::Windowed)
		{
			Settings->SetWindowPosition(GWindowRequest.PosX, GWindowRequest.PosY);
		}

		// Not saved: a switch on the command line says what this run should look like, not what every run
		// after it should.
		Settings->ApplyResolutionSettings(false);

		if (GWindowRequest.bBorderless && Mode == EWindowMode::Windowed)
		{
			RemoveWindowFrame();
		}

		UE_LOG(LogLambda, Log, TEXT("Window: %s%s %dx%d"),
			GWindowRequest.bBorderless ? TEXT("borderless ") : TEXT(""),
			Mode == EWindowMode::Windowed ? TEXT("windowed")
				: Mode == EWindowMode::WindowedFullscreen ? TEXT("windowed fullscreen") : TEXT("fullscreen"),
			Settings->GetScreenResolution().X, Settings->GetScreenResolution().Y);
	}

	/**
	 * Source-style launcher arguments. Anything written "+command arg arg" is a console command to run once the
	 * game is up, exactly as typing it would - "lambda.exe +map startup +give weapon_smg1" starts on
	 * startup.bsp with an SMG. A command runs to the end of the line or to the next token beginning with + or -.
	 *
	 * They have to be taken off the command line rather than left on it. UGameInstance::GetMapOverrideName reads
	 * the first token that does not begin with '-' as a UE map to travel to, so a leftover "+map" would be read
	 * as a map called "+map", fail to browse to it and quit. The game module loads before StartGameInstance runs,
	 * which is what makes here the place to do it.
	 *
	 * "+map" is the one that cannot simply be queued: the map has to be known while the world is being made, long
	 * before there is a console to type into, so it becomes the -sourcemap= switch the rest of the game reads.
	 */
	void TranslateSourceLauncherArgs()
	{
		const FString CmdLine = FCommandLine::Get();

		// Split into tokens, keeping quoted runs together.
		TArray<FString> Tokens;
		{
			FString Current;
			bool bInQuotes = false;
			for (int32 i = 0; i < CmdLine.Len(); ++i)
			{
				const TCHAR Ch = CmdLine[i];
				if (Ch == TEXT('"'))
				{
					bInQuotes = !bInQuotes;
					Current.AppendChar(Ch);
				}
				else if (!bInQuotes && FChar::IsWhitespace(Ch))
				{
					if (!Current.IsEmpty())
					{
						Tokens.Add(Current);
						Current.Reset();
					}
				}
				else
				{
					Current.AppendChar(Ch);
				}
			}
			if (!Current.IsEmpty())
			{
				Tokens.Add(Current);
			}
		}

		FString MapName;
		TArray<FString> Kept;
		for (int32 i = 0; i < Tokens.Num(); ++i)
		{
			if (!Tokens[i].StartsWith(TEXT("+")) || Tokens[i].Len() < 2)
			{
				if (TakeWindowArg(Tokens, i))
				{
					continue;
				}

				Kept.Add(Tokens[i]);
				continue;
			}

			// The command, and its arguments up to the next + or - token.
			FString Command = Tokens[i].RightChop(1);
			TArray<FString> Args;
			while (i + 1 < Tokens.Num() && !Tokens[i + 1].StartsWith(TEXT("+")) && !Tokens[i + 1].StartsWith(TEXT("-")))
			{
				Args.Add(Tokens[++i].TrimQuotes());
			}

			if (Command.Equals(TEXT("map"), ESearchCase::IgnoreCase))
			{
				MapName = Args.Num() > 0 ? Args[0] : FString();
				continue;
			}

			const FString Line = Args.Num() > 0
				? FString::Printf(TEXT("%s %s"), *Command, *FString::Join(Args, TEXT(" ")))
				: Command;
			GStartupCommands.Add(Line);
		}

		FString NewCmdLine = FString::Join(Kept, TEXT(" "));
		if (!MapName.IsEmpty())
		{
			NewCmdLine += FString::Printf(TEXT(" -sourcemap=%s"), *MapName);
			UE_LOG(LogLambda, Log, TEXT("+map %s -> -sourcemap=%s"), *MapName, *MapName);
		}

		if (NewCmdLine == CmdLine)
		{
			return;
		}

		FCommandLine::Set(*NewCmdLine);
		UE_LOG(LogLambda, Log, TEXT("command line: %s"), *NewCmdLine);

		for (const FString& Line : GStartupCommands)
		{
			UE_LOG(LogLambda, Log, TEXT("startup command: %s"), *Line);
		}
	}

	/** Runs what the command line asked for, once there is a world for it to act on. */
	void RunStartupCommands(UWorld* World)
	{
		if (GStartupCommands.Num() == 0 || !World)
		{
			return;
		}
		UGameInstance* Instance = World->GetGameInstance();
		ULambdaConsole* Console = Instance ? Instance->GetSubsystem<ULambdaConsole>() : nullptr;
		// Through the console, so a startup command is the same thing as a typed one - it reaches the console's
		// own commands as well as every engine one.
		for (const FString& Line : GStartupCommands)
		{
			if (Console)
			{
				Console->Execute(Line);
			}
			else if (GEngine)
			{
				GEngine->Exec(World, *Line);
			}
		}
		// Once only: they are startup commands, not something every level change repeats.
		GStartupCommands.Reset();
	}
}

namespace
{
	// ---- rtx ----------------------------------------------------------------------------------------------
	//
	// Two lighting paths, chosen per machine rather than per project:
	//
	//   rtx    - Lumen GI and reflections over hardware ray tracing. The only Lumen this project can ever
	//            have: software Lumen reads mesh distance fields, those are generated in the editor alone,
	//            and every mesh here is built at runtime from a BSP.
	//   legacy - no GI. Direct light, virtual shadow maps, screen-space reflections, and the ambient fill
	//            the world actor already places. What every card can do.
	//
	// The default is rtx wherever the card can trace; -nortx forces legacy from the first frame, and the rtx
	// console command moves between the two afterwards. All three cvars are render-thread safe, and the boot
	// config asked for ERayTracingMode::Dynamic, which is what makes r.RayTracing.Enable a live switch
	// instead of one read once at startup.

	void ApplyRtx(bool bOn)
	{
		const TCHAR* Values[][2] = {
			{ TEXT("r.RayTracing.Enable"),               bOn ? TEXT("1") : TEXT("0") },
			{ TEXT("r.DynamicGlobalIlluminationMethod"), bOn ? TEXT("1") : TEXT("0") },
			{ TEXT("r.ReflectionMethod"),                bOn ? TEXT("1") : TEXT("2") },	// Lumen / SSR
			// Where the bounce actually comes from. Lumen normally shades what a ray hits by reading its
			// surface cache, and that cache is built from mesh cards - another thing only the editor can
			// generate, which a world built at runtime therefore has none of. Rays would hit our walls and
			// come back black: occlusion, but no light. Mode 1 evaluates the real material and lighting at
			// the hit point instead, which needs no cards and is the only way this world can bounce light.
			{ TEXT("r.Lumen.HardwareRayTracing.LightingMode"), bOn ? TEXT("1") : TEXT("0") },
			{ TEXT("r.Lumen.HardwareRayTracing.HitLighting.Allowed"), TEXT("1") },
		};
		for (const auto& Pair : Values)
		{
			if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Pair[0]))
			{
				// Console priority, or the project settings the ini wrote would win the argument.
				Var->Set(Pair[1], ECVF_SetByConsole);
			}
		}
		UE_LOG(LogLambda, Log, TEXT("rtx %s: %s"), bOn ? TEXT("1") : TEXT("0"),
			bOn ? TEXT("Lumen over hardware ray tracing") : TEXT("legacy lighting (no GI, SSR)"));
	}

	/** Decides the boot value: rtx wherever the card can trace, unless -nortx said otherwise. */
	void ApplyStartupRtx()
	{
		const bool bForcedOff = FParse::Param(FCommandLine::Get(), TEXT("nortx"));
		const bool bAllowed = IsRayTracingAllowed();
		if (!bAllowed)
		{
			UE_LOG(LogLambda, Log, TEXT("rtx: this GPU cannot ray trace; legacy lighting only"));
		}
		ApplyRtx(bAllowed && !bForcedOff);
	}

	FAutoConsoleCommand GRtxCommand(
		TEXT("rtx"),
		TEXT("rtx 1|0: Lumen over hardware ray tracing, or the legacy lighting path. No restart needed."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() == 0)
			{
				UE_LOG(LogLambda, Display, TEXT("rtx is %d (%s)"), IsRayTracingEnabled() ? 1 : 0,
					IsRayTracingAllowed() ? TEXT("supported by this GPU") : TEXT("not supported by this GPU"));
				return;
			}
			const bool bOn = FCString::Atoi(*Args[0]) != 0;
			if (bOn && !IsRayTracingAllowed())
			{
				UE_LOG(LogLambda, Display, TEXT("rtx: this GPU cannot ray trace; staying on legacy lighting"));
				return;
			}
			ApplyRtx(bOn);
		}));
}

void FLambdaEngineModule::StartupModule()
{
	UE_LOG(LogLambda, Log, TEXT("LambdaEngine game module started"));
	TranslateSourceLauncherArgs();
	// The window is already built by the time this module is loaded, so what the command line asked of it is
	// carried out as soon as there is an engine to ask rather than from the line itself.
	FCoreDelegates::OnPostEngineInit.AddStatic(&ApplyWindowSettings);
	// After the RHI exists, which is what IsRayTracingAllowed() needs to answer for this machine.
	FCoreDelegates::OnPostEngineInit.AddStatic(&ApplyStartupRtx);
	// Armed before the engine loads its first level, so the game's startup is covered rather than showing black
	// until the menu appears.
	FLambdaLoadingScreen::Arm();
	// The command line's "+command"s run once the first world is up, which is the earliest they can act on
	// anything (CBuf: Source pushes them into the command buffer at startup for the same reason).
	FCoreUObjectDelegates::PostLoadMapWithWorld.AddStatic(&RunStartupCommands);
}

void FLambdaEngineModule::ShutdownModule()
{
}

IMPLEMENT_PRIMARY_GAME_MODULE(FLambdaEngineModule, LambdaEngine, "LambdaEngine");

// ---------------------------------------------------------------------------------------------------------------------
// Console commands
// ---------------------------------------------------------------------------------------------------------------------

static void LambdaMapCommand(const TArray<FString>& Args, UWorld* World)
{
	if (!World)
	{
		return;
	}
	if (Args.Num() < 1 || Args[0].IsEmpty())
	{
		UE_LOG(LogLambda, Display, TEXT("Usage: map <mapname>   (loads <gamedir>/maps/<mapname>.bsp)"));
		return;
	}
	const FString MapName = Args[0];
	const FString EntryMap = UGameMapsSettings::GetGameDefaultMap();
	UE_LOG(LogLambda, Log, TEXT("map: reloading '%s' with Source map '%s'"), *EntryMap, *MapName);
	FLambdaLoadingScreen::Arm();
	UGameplayStatics::OpenLevel(World, FName(*EntryMap), true, FString::Printf(TEXT("map=%s"), *MapName));
}

static FAutoConsoleCommandWithWorldAndArgs GLambdaMapCommand(
	TEXT("map"),
	TEXT("Load a Source BSP map from the game directory: map <name>"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaMapCommand));

// nav_test - what the navmesh makes of the map: a route from the player to every NPC in it. Source has its own
// nav_* commands for the navmesh its bots use; this is the one worth having while the mesh is young.
static void LambdaNavTestCommand(const TArray<FString>& Args, UWorld* World)
{
	if (!World)
	{
		return;
	}
	// Generation runs in the background, so a report asked for the instant a map loads sees an empty mesh.
	// "nav_test 5" asks again in five seconds.
	if (Args.Num() > 0)
	{
		const float Delay = FMath::Max(0.1f, FCString::Atof(*Args[0]));
		FTimerHandle Handle;
		World->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateLambda([World]()
		{
			if (IsValid(World))
			{
				LambdaNavTestCommand(TArray<FString>(), World);
			}
		}), Delay, false);
		UE_LOG(LogLambda, Display, TEXT("nav_test: reporting in %.1fs"), Delay);
		return;
	}

	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	APlayerController* PC = World->GetFirstPlayerController();
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!NavSys || !Pawn)
	{
		UE_LOG(LogLambda, Display, TEXT("nav_test: no navigation system or no player"));
		return;
	}
	ARecastNavMesh* Recast = Cast<ARecastNavMesh>(NavSys->GetDefaultNavDataInstance(FNavigationSystem::DontCreate));
	UE_LOG(LogLambda, Display, TEXT("nav_test: navmesh %s, %d tiles with data (pool %d)"),
		Recast && Recast->HasValidNavmesh() ? TEXT("valid") : TEXT("MISSING"),
		Recast ? Recast->GetNumActiveTiles() : 0,
		Recast ? Recast->GetNavMeshTilesCount() : 0);

	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	int32 Count = 0;
	for (TActorIterator<ASourceNPCBase> It(World); It; ++It)
	{
		const FVector Goal = It->GetActorLocation();
		const float Straight = FVector::Dist(Pawn->GetActorLocation(), Goal);
		// Queried as the NPC, not as the player: the navmesh is built for the NPC's agent size, and a query
		// carries the querier's own size with it. This is the same call NavigateTo makes.
		UNavigationPath* Path = NavSys->FindPathToLocationSynchronously(World, Goal, Pawn->GetActorLocation(), *It);
		if (!Path || !Path->IsValid())
		{
			UE_LOG(LogLambda, Display, TEXT("  %s at %.0fu: NO ROUTE from it to the player"), *It->GetClassName(), Straight / Scale);
		}
		else
		{
			// A route longer than the straight line is a route around something.
			float Along = 0.0f;
			for (int32 i = 1; i < Path->PathPoints.Num(); ++i)
			{
				Along += FVector::Dist(Path->PathPoints[i - 1], Path->PathPoints[i]);
			}
			UE_LOG(LogLambda, Display, TEXT("  %s at %.0fu: %d corners, %.0fu along the route vs %.0fu straight%s%s"),
				*It->GetClassName(), Straight / Scale, Path->PathPoints.Num(), Along / Scale, Straight / Scale,
				Path->IsPartial() ? TEXT(" (partial)") : TEXT(""),
				Along > Straight * 1.1f ? TEXT("  <- goes around something") : TEXT(""));
		}
		++Count;
	}
	if (Count == 0)
	{
		UE_LOG(LogLambda, Display, TEXT("  no NPCs in the map to path to"));
	}
}

static FAutoConsoleCommandWithWorldAndArgs GLambdaNavTestCommand(
	TEXT("nav_test"),
	TEXT("Report the navmesh and a route from every NPC to the player: nav_test [delay_seconds]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaNavTestCommand));

static void LambdaMapsCommand(const TArray<FString>& Args, UWorld* World)
{
	TArray<FString> Maps;
	FLambdaFileSystem::Get().FindFiles(TEXT("maps"), TEXT("*.bsp"), Maps);
	Maps.Sort();
	UE_LOG(LogLambda, Display, TEXT("%d map(s) across mounts: %s"), Maps.Num(), *FString::Join(FLambdaFileSystem::Get().GetMountDescriptions(), TEXT("; ")));
	for (const FString& Map : Maps)
	{
		UE_LOG(LogLambda, Display, TEXT("  %s"), *Map);
	}
}

static FAutoConsoleCommandWithWorldAndArgs GLambdaMapsCommand(
	TEXT("maps"),
	TEXT("List the BSP maps available in the game directories"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaMapsCommand));

// Source's setpos/setang, handy for testing maps. Coordinates are Hammer units, like the originals.
static void LambdaSetPosCommand(const TArray<FString>& Args, UWorld* World)
{
	if (!World || Args.Num() < 3)
	{
		UE_LOG(LogLambda, Display, TEXT("Usage: setpos <x> <y> <z>   (Hammer units)"));
		return;
	}
	APlayerController* PC = World->GetFirstPlayerController();
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!Pawn)
	{
		return;
	}
	const FVector3f Source(FCString::Atof(*Args[0]), FCString::Atof(*Args[1]), FCString::Atof(*Args[2]));
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	const FVector Target = FSourceCoords::ToUE(Source, Scale);
	Pawn->TeleportTo(Target, Pawn->GetActorRotation());
	UE_LOG(LogLambda, Display, TEXT("setpos Source(%s) -> UE(%s)"), *Source.ToString(), *Target.ToString());
}

static FAutoConsoleCommandWithWorldAndArgs GLambdaSetPosCommand(
	TEXT("setpos"),
	TEXT("Teleport the player to a position in Hammer units: setpos <x> <y> <z>"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaSetPosCommand));

static void LambdaSetAngCommand(const TArray<FString>& Args, UWorld* World)
{
	if (!World || Args.Num() < 2)
	{
		UE_LOG(LogLambda, Display, TEXT("Usage: setang <pitch> <yaw> [roll]   (Source angles)"));
		return;
	}
	APlayerController* PC = World->GetFirstPlayerController();
	if (!PC)
	{
		return;
	}
	const FVector3f Angles(FCString::Atof(*Args[0]), FCString::Atof(*Args[1]), Args.Num() > 2 ? FCString::Atof(*Args[2]) : 0.0f);
	PC->SetControlRotation(FSourceCoords::AnglesToUE(Angles));
	UE_LOG(LogLambda, Display, TEXT("setang Source(%s)"), *Angles.ToString());
}

static FAutoConsoleCommandWithWorldAndArgs GLambdaSetAngCommand(
	TEXT("setang"),
	TEXT("Set the player's view angles in Source (pitch yaw roll) degrees"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaSetAngCommand));

// Source's ent_fire: ent_fire <targetname> <input> [parameter]
static void LambdaEntFireCommand(const TArray<FString>& Args, UWorld* World)
{
	if (!World || Args.Num() < 2)
	{
		UE_LOG(LogLambda, Display, TEXT("Usage: ent_fire <targetname> <input> [parameter]"));
		return;
	}
	for (TActorIterator<ASourceBSPWorldActor> It(World); It; ++It)
	{
		APlayerController* PC = World->GetFirstPlayerController();
		APawn* Pawn = PC ? PC->GetPawn() : nullptr;
		It->QueueEntityEvent(Args[0], Args[1], Args.Num() > 2 ? Args[2] : FString(), Pawn, Pawn, 0.0f);
		UE_LOG(LogLambda, Display, TEXT("ent_fire %s %s %s"), *Args[0], *Args[1], Args.Num() > 2 ? *Args[2] : TEXT(""));
		return;
	}
	UE_LOG(LogLambda, Display, TEXT("ent_fire: no BSP world loaded"));
}

static FAutoConsoleCommandWithWorldAndArgs GLambdaEntFireCommand(
	TEXT("ent_fire"),
	TEXT("Fire an input on an entity by targetname: ent_fire <targetname> <input> [parameter]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaEntFireCommand));

// Source's give: give weapon_pistol
static void LambdaGiveCommand(const TArray<FString>& Args, UWorld* World)
{
	if (!World || Args.Num() < 1)
	{
		UE_LOG(LogLambda, Display, TEXT("Usage: give <weapon_classname>"));
		return;
	}
	APlayerController* PC = World->GetFirstPlayerController();
	ALambdaCharacter* Player = PC ? Cast<ALambdaCharacter>(PC->GetPawn()) : nullptr;
	if (!Player)
	{
		return;
	}
	if (Player->GiveWeapon(Args[0]))
	{
		UE_LOG(LogLambda, Display, TEXT("gave %s"), *Args[0]);
	}
}

static FAutoConsoleCommandWithWorldAndArgs GLambdaGiveCommand(
	TEXT("give"),
	TEXT("Give the player a weapon by classname: give weapon_pistol"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaGiveCommand));

// giveammo <type> <count>
static void LambdaGiveAmmoCommand(const TArray<FString>& Args, UWorld* World)
{
	if (!World || Args.Num() < 2)
	{
		UE_LOG(LogLambda, Display, TEXT("Usage: giveammo <ammotype> <count>   e.g. giveammo Pistol 50"));
		return;
	}
	APlayerController* PC = World->GetFirstPlayerController();
	ALambdaCharacter* Player = PC ? Cast<ALambdaCharacter>(PC->GetPawn()) : nullptr;
	if (!Player)
	{
		return;
	}
	const int32 Given = Player->GiveAmmo(Args[0], FCString::Atoi(*Args[1]));
	UE_LOG(LogLambda, Display, TEXT("gave %d %s ammo (now %d)"), Given, *Args[0], Player->GetAmmoCount(Args[0]));
}

static FAutoConsoleCommandWithWorldAndArgs GLambdaGiveAmmoCommand(
	TEXT("giveammo"),
	TEXT("Give the player ammo: giveammo <ammotype> <count>"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaGiveAmmoCommand));

// hurt [amount] [damagetype] - Source's "hurtme", plus the damage type so the suit's reaction can be tried.
static void LambdaHurtCommand(const TArray<FString>& Args, UWorld* World)
{
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	ALambdaCharacter* Player = PC ? Cast<ALambdaCharacter>(PC->GetPawn()) : nullptr;
	if (!Player)
	{
		return;
	}
	const float Amount = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 10.0f;

	// The names are shareddefs.h's, less the DMG_ - "hurt 30 slash" is a claw, "hurt 30 fall" is a drop.
	const int32 Type = Args.Num() > 1 ? SourceDamage::TypeFromName(Args[1]) : SourceDamageType::DMG_GENERIC;

	FHitResult Hit;
	Hit.ImpactPoint = Player->GetActorLocation();
	Hit.Location = Hit.ImpactPoint;
	FSourceDamageEvent Info(Amount, Hit, -Player->GetActorForwardVector(), UDamageType::StaticClass(),
		FVector::ZeroVector, Type);
	Player->TakeDamage(Amount, Info, nullptr, nullptr);
	UE_LOG(LogLambda, Display, TEXT("hurt %.0f (%s): health %.0f, armour %.0f"),
		Amount, Args.Num() > 1 ? *Args[1] : TEXT("generic"), Player->GetHealth(), Player->GetArmor());
}

static FAutoConsoleCommandWithWorldAndArgs GLambdaHurtCommand(
	TEXT("hurt"),
	TEXT("Damage the player: hurt <amount> [bullet|slash|fall|burn|poison|radiation|...]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaHurtCommand));

// givearmor [amount] - a suit battery's worth by default
static void LambdaGiveArmorCommand(const TArray<FString>& Args, UWorld* World)
{
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (ALambdaCharacter* Player = PC ? Cast<ALambdaCharacter>(PC->GetPawn()) : nullptr)
	{
		Player->GiveArmor(Args.Num() > 0 ? FCString::Atof(*Args[0]) : 15.0f);
		UE_LOG(LogLambda, Display, TEXT("armour %.0f"), Player->GetArmor());
	}
}

static FAutoConsoleCommandWithWorldAndArgs GLambdaGiveArmorCommand(
	TEXT("givearmor"),
	TEXT("Give the player suit armour: givearmor [amount]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaGiveArmorCommand));

// speak <sentence> - make the suit say one line, for checking the sentence system
static void LambdaSpeakCommand(const TArray<FString>& Args, UWorld* World)
{
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	ALambdaCharacter* Player = PC ? Cast<ALambdaCharacter>(PC->GetPawn()) : nullptr;
	if (!Player)
	{
		return;
	}
	if (Args.Num() < 1)
	{
		UE_LOG(LogLambda, Display, TEXT("Usage: speak <sentence>   e.g. speak HEV_DMG4  (%d sentences loaded)"),
			FSourceSentences::Get().Num());
		return;
	}
	const FSourceSentence* Sentence = FSourceSentences::Get().Find(Args[0]);
	if (!Sentence)
	{
		UE_LOG(LogLambda, Display, TEXT("speak: no sentence '%s'"), *Args[0]);
		return;
	}
	FString Words;
	for (const FSourceVoxWord& Word : Sentence->Words)
	{
		Words += FString::Printf(TEXT("%s(p%d) "), *Word.Wave, Word.Pitch);
	}
	UE_LOG(LogLambda, Display, TEXT("speak %s: %s"), *Sentence->Name, *Words);
	Player->GetSuitVoice().SetSuitUpdate(Player, Args[0], FLambdaSuitVoice::RepeatOK);
}

static FAutoConsoleCommandWithWorldAndArgs GLambdaSpeakCommand(
	TEXT("speak"),
	TEXT("Make the HEV suit say a sentence: speak HEV_DMG4"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaSpeakCommand));

// weaponmenu [bucket] - open the weapon selection and leave it open, so the row can be looked at
static void LambdaWeaponMenuCommand(const TArray<FString>& Args, UWorld* World)
{
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (ALambdaCharacter* Player = PC ? Cast<ALambdaCharacter>(PC->GetPawn()) : nullptr)
	{
		Player->OpenWeaponSelection(Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 0);
	}
}

static FAutoConsoleCommandWithWorldAndArgs GLambdaWeaponMenuCommand(
	TEXT("weaponmenu"),
	TEXT("Open the weapon selection on a bucket and leave it open: weaponmenu [bucket]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaWeaponMenuCommand));

// cl_playermodel <models/player/x.mdl> - the model the player wears, as in HL2 Deathmatch. With no argument it
// says which one that currently is.
static FAutoConsoleCommandWithWorldAndArgs GLambdaPlayerModelCommand(
	TEXT("cl_playermodel"),
	TEXT("Set the player's model, body and legs together."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
		{
			APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
			ALambdaCharacter* Player = PC ? Cast<ALambdaCharacter>(PC->GetPawn()) : nullptr;
			if (!Player)
			{
				return;
			}
			if (Args.Num() == 0)
			{
				UE_LOG(LogLambda, Display, TEXT("cl_playermodel is \"%s\""),
					*ULambdaSourceSettings::Get().PlayerBodyModel);
				return;
			}
			Player->SetPlayerModel(Args[0]);
		}));

// thirdperson / firstperson - Source's own pair, and they mean the same thing here.
static void LambdaViewModeCommand(UWorld* World, bool bThird)
{
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (ALambdaCharacter* Player = PC ? Cast<ALambdaCharacter>(PC->GetPawn()) : nullptr)
	{
		Player->SetThirdPerson(bThird);
	}
}

static FAutoConsoleCommandWithWorldAndArgs GLambdaThirdPersonCommand(
	TEXT("thirdperson"),
	TEXT("Watch the player from behind (cam_idealdist sets how far)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World) { LambdaViewModeCommand(World, true); }));

static FAutoConsoleCommandWithWorldAndArgs GLambdaFirstPersonCommand(
	TEXT("firstperson"),
	TEXT("Return to the player's own eyes."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World) { LambdaViewModeCommand(World, false); }));

// viewmodel <models/path.mdl> - load any Source model as the view model, for checking MDL support
static void LambdaViewModelCommand(const TArray<FString>& Args, UWorld* World)
{
	if (!World || Args.Num() < 1)
	{
		UE_LOG(LogLambda, Display, TEXT("Usage: viewmodel <models/weapons/v_pistol.mdl>"));
		return;
	}
	APlayerController* PC = World->GetFirstPlayerController();
	ALambdaCharacter* Player = PC ? Cast<ALambdaCharacter>(PC->GetPawn()) : nullptr;
	if (Player)
	{
		UE_LOG(LogLambda, Display, TEXT("viewmodel %s: %s"), *Args[0], Player->SetViewModel(Args[0]) ? TEXT("ok") : TEXT("failed"));
	}
}

static FAutoConsoleCommandWithWorldAndArgs GLambdaViewModelCommand(
	TEXT("viewmodel"),
	TEXT("Load a Source .mdl as the first-person view model"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaViewModelCommand));

// surfaceinfo [surfaceprop ...] - show how each surface resolves to a game material and impact decal,
// the chain GetImpactDecal walks (VMT $surfaceprop -> surfaceproperties gamematerial -> decals_subrect group).
static void LambdaSurfaceInfoCommand(const TArray<FString>& Args, UWorld* World)
{
	FSourceSurfaceProps& Props = FSourceSurfaceProps::Get();
	FSourceDecalScript& Decals = FSourceDecalScript::Get();
	Props.Initialize();
	Decals.Initialize();

	TArray<FString> Names = Args;
	if (Names.Num() == 0)
	{
		Names = { TEXT("concrete"), TEXT("metal"), TEXT("wood"), TEXT("glass"), TEXT("dirt"), TEXT("sand"),
			TEXT("flesh"), TEXT("tile"), TEXT("plastic"), TEXT("water") };
	}

	UE_LOG(LogLambda, Display, TEXT("%-14s %-4s %-22s %-34s %s"), TEXT("surfaceprop"), TEXT("mat"), TEXT("decal group"), TEXT("decal material"), TEXT("bullet impact sound"));
	for (const FString& Name : Names)
	{
		const FSourceSurfaceProp* Prop = Props.Find(Name);
		const TCHAR GameMaterial = Props.GetGameMaterial(Name);
		const FString Group = Decals.TranslateDecalForGameMaterial(TEXT("Impact.Concrete"), GameMaterial);
		const FString Picked = Decals.GetImpactDecalMaterial(GameMaterial);
		UE_LOG(LogLambda, Display, TEXT("%-14s %-4c %-22s %-34s %s"),
			*Name, GameMaterial,
			Group.IsEmpty() ? TEXT("(none)") : *Group,
			Picked.IsEmpty() ? TEXT("(none)") : *Picked,
			Prop ? *Prop->BulletImpactSound : TEXT("(unknown surfaceprop)"));
	}
}

// npc_create <classname> - Source's npc_create: spawn an NPC where the player is looking
static void LambdaNPCCreateCommand(const TArray<FString>& Args, UWorld* World)
{
	if (!World || Args.Num() < 1)
	{
		UE_LOG(LogLambda, Display, TEXT("Usage: npc_create <npc_headcrab>"));
		return;
	}
	APlayerController* PC = World->GetFirstPlayerController();
	if (ALambdaCharacter* Player = PC ? Cast<ALambdaCharacter>(PC->GetPawn()) : nullptr)
	{
		Player->NPCCreate(Args[0], Args.Num() > 1 ? FCString::Atof(*Args[1]) : 5000.0f);
	}
}

// prop_list - what physics props exist and where they ended up (Source's ent_dump, cut down)
static void LambdaPropListCommand(const TArray<FString>& Args, UWorld* World)
{
	if (!World)
	{
		return;
	}
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	int32 Count = 0;
	for (TActorIterator<ASourcePropPhysics> It(World); It; ++It)
	{
		const FVector3f Source = FSourceCoords::ToSource(It->GetActorLocation(), Scale);
		UE_LOG(LogLambda, Display, TEXT("prop %d: %s at %g %g %g, %.1f kg, %.0f units, '%s'%s"),
			Count++, *It->GetClassName_Lambda(), Source.X, Source.Y, Source.Z,
			It->GetMass(), It->GetSizeUnits(), *It->GetSurfaceProp(), It->IsHeld() ? TEXT(" (held)") : TEXT(""));
	}
	UE_LOG(LogLambda, Display, TEXT("%d physics props"), Count);
}

static FAutoConsoleCommandWithWorldAndArgs GLambdaPropListCommand(
	TEXT("prop_list"),
	TEXT("List the physics props in the world and where they are"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaPropListCommand));

// prop_create <model> - Source's prop_physics_create
static void LambdaPropCreateCommand(const TArray<FString>& Args, UWorld* World)
{
	if (!World || Args.Num() < 1)
	{
		UE_LOG(LogLambda, Display, TEXT("Usage: prop_create <models/props_junk/wood_crate001a.mdl>"));
		return;
	}
	APlayerController* PC = World->GetFirstPlayerController();
	if (ALambdaCharacter* Player = PC ? Cast<ALambdaCharacter>(PC->GetPawn()) : nullptr)
	{
		Player->PropCreate(Args[0], Args.Num() > 1 ? FCString::Atof(*Args[1]) : 300.0f);
	}
}

static FAutoConsoleCommandWithWorldAndArgs GLambdaPropCreateCommand(
	TEXT("prop_create"),
	TEXT("Drop a physics prop of that model where the player is looking (Source's prop_physics_create)"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaPropCreateCommand));

static FAutoConsoleCommandWithWorldAndArgs GLambdaNPCCreateCommand(
	TEXT("npc_create"),
	TEXT("Spawn an NPC by classname where the player is looking (Source's npc_create)"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaNPCCreateCommand));

// decaltest [distance_cm] [angle_deg] - stamp test impact decals ahead and jump to a fixed viewpoint
static void LambdaDecalTestCommand(const TArray<FString>& Args, UWorld* World)
{
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	ALambdaCharacter* Player = PC ? Cast<ALambdaCharacter>(PC->GetPawn()) : nullptr;
	if (Player)
	{
		Player->RunDecalTest(Args.Num() > 0 ? FCString::Atof(*Args[0]) : 90.0f, Args.Num() > 1 ? FCString::Atof(*Args[1]) : 45.0f,
			3, Args.Num() > 2 ? Args[2] : FString());
	}
}

static FAutoConsoleCommandWithWorldAndArgs GLambdaDecalTestCommand(
	TEXT("decaltest"),
	TEXT("Stamp a row of impact decals on the wall ahead and view them from <distance_cm> at <angle_deg>"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaDecalTestCommand));

static FAutoConsoleCommandWithWorldAndArgs GLambdaSurfaceInfoCommand(
	TEXT("surfaceinfo"),
	TEXT("Show the surfaceprop -> game material -> impact decal chain for one or more surfaces"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaSurfaceInfoCommand));

