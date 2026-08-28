#include "LambdaEngine.h"
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

DEFINE_LOG_CATEGORY(LogLambda);

namespace
{
	/** Everything a "+command" on the command line asked for, in the order it was written. */
	TArray<FString> GStartupCommands;

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

		if (MapName.IsEmpty() && GStartupCommands.Num() == 0)
		{
			return;
		}

		FString NewCmdLine = FString::Join(Kept, TEXT(" "));
		if (!MapName.IsEmpty())
		{
			NewCmdLine += FString::Printf(TEXT(" -sourcemap=%s"), *MapName);
			UE_LOG(LogLambda, Log, TEXT("+map %s -> -sourcemap=%s"), *MapName, *MapName);
		}
		FCommandLine::Set(*NewCmdLine);

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

void FLambdaEngineModule::StartupModule()
{
	UE_LOG(LogLambda, Log, TEXT("LambdaEngine game module started"));
	TranslateSourceLauncherArgs();
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

