#include "LambdaEngine.h"
#include "LambdaFileSystem.h"
#include "Misc/CommandLine.h"
#include "Engine/World.h"
#include "GameMapsSettings.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "LambdaSourceSettings.h"
#include "SourceCoordinates.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "SourceBSPWorldActor.h"
#include "SourcePropPhysics.h"
#include "EngineUtils.h"
#include "LambdaCharacter.h"
#include "SourceDecalScript.h"
#include "SourceSurfaceProps.h"
#include "LambdaLoadingScreen.h"

DEFINE_LOG_CATEGORY(LogLambda);

namespace
{
	/**
	 * Source-style launcher arguments. "LambdaEngine.exe +map mymap" runs mymap.bsp - but the bare "+map" token
	 * must never reach the engine's own parsing: UGameInstance::GetMapOverrideName takes the first token that does
	 * not start with '-' as a UE map override, would read the override as a map called "+map", fail to browse to
	 * it and exit. The game module loads before StartGameInstance runs, so the tokens are rewritten here into the
	 * -sourcemap= switch the rest of the game already understands.
	 */
	void TranslateSourceLauncherArgs()
	{
		const FString CmdLine = FCommandLine::Get();

		// Find "+map" standing alone as a token.
		int32 TokenStart = INDEX_NONE;
		for (int32 i = 0; i + 4 <= CmdLine.Len(); ++i)
		{
			if ((i == 0 || FChar::IsWhitespace(CmdLine[i - 1]))
				&& FCString::Strnicmp(*CmdLine + i, TEXT("+map"), 4) == 0
				&& (i + 4 == CmdLine.Len() || FChar::IsWhitespace(CmdLine[i + 4])))
			{
				TokenStart = i;
				break;
			}
		}
		if (TokenStart == INDEX_NONE)
		{
			return;
		}

		// The map name is the next token; it is taken out of the command line along with "+map" itself, or the
		// engine would seize the now-leading bare name as its own map override.
		int32 End = TokenStart + 4;
		while (End < CmdLine.Len() && FChar::IsWhitespace(CmdLine[End])) { ++End; }
		const int32 ValueStart = End;
		while (End < CmdLine.Len() && !FChar::IsWhitespace(CmdLine[End])) { ++End; }
		FString MapName = CmdLine.Mid(ValueStart, End - ValueStart);
		MapName = MapName.TrimQuotes();

		FString NewCmdLine = CmdLine.Left(TokenStart) + CmdLine.Mid(End);
		if (!MapName.IsEmpty())
		{
			NewCmdLine += FString::Printf(TEXT(" -sourcemap=%s"), *MapName);
		}
		FCommandLine::Set(*NewCmdLine);
		UE_LOG(LogLambda, Log, TEXT("+map %s -> -sourcemap=%s"), *MapName, *MapName);
	}
}

void FLambdaEngineModule::StartupModule()
{
	UE_LOG(LogLambda, Log, TEXT("LambdaEngine game module started"));
	TranslateSourceLauncherArgs();
	// Armed before the engine loads its first level, so the game's startup is covered rather than showing black
	// until the menu appears.
	FLambdaLoadingScreen::Arm();
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
		UE_LOG(LogLambda, Display, TEXT("Usage: lambda.map <mapname>   (loads <gamedir>/maps/<mapname>.bsp)"));
		return;
	}
	const FString MapName = Args[0];
	const FString EntryMap = UGameMapsSettings::GetGameDefaultMap();
	UE_LOG(LogLambda, Log, TEXT("lambda.map: reloading '%s' with Source map '%s'"), *EntryMap, *MapName);
	FLambdaLoadingScreen::Arm();
	UGameplayStatics::OpenLevel(World, FName(*EntryMap), true, FString::Printf(TEXT("map=%s"), *MapName));
}

static FAutoConsoleCommandWithWorldAndArgs GLambdaMapCommand(
	TEXT("lambda.map"),
	TEXT("Load a Source BSP map from the game directory: lambda.map <name>"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaMapCommand));

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
	TEXT("lambda.maps"),
	TEXT("List the BSP maps available in the game directories"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaMapsCommand));

// Source's setpos/setang, handy for testing maps. Coordinates are Hammer units, like the originals.
static void LambdaSetPosCommand(const TArray<FString>& Args, UWorld* World)
{
	if (!World || Args.Num() < 3)
	{
		UE_LOG(LogLambda, Display, TEXT("Usage: lambda.setpos <x> <y> <z>   (Hammer units)"));
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
	TEXT("lambda.setpos"),
	TEXT("Teleport the player to a position in Hammer units: lambda.setpos <x> <y> <z>"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaSetPosCommand));

static void LambdaSetAngCommand(const TArray<FString>& Args, UWorld* World)
{
	if (!World || Args.Num() < 2)
	{
		UE_LOG(LogLambda, Display, TEXT("Usage: lambda.setang <pitch> <yaw> [roll]   (Source angles)"));
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
	TEXT("lambda.setang"),
	TEXT("Set the player's view angles in Source (pitch yaw roll) degrees"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaSetAngCommand));

// Source's ent_fire: lambda.ent_fire <targetname> <input> [parameter]
static void LambdaEntFireCommand(const TArray<FString>& Args, UWorld* World)
{
	if (!World || Args.Num() < 2)
	{
		UE_LOG(LogLambda, Display, TEXT("Usage: lambda.ent_fire <targetname> <input> [parameter]"));
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
	TEXT("lambda.ent_fire"),
	TEXT("Fire an input on an entity by targetname: lambda.ent_fire <targetname> <input> [parameter]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaEntFireCommand));

// Source's give: lambda.give weapon_pistol
static void LambdaGiveCommand(const TArray<FString>& Args, UWorld* World)
{
	if (!World || Args.Num() < 1)
	{
		UE_LOG(LogLambda, Display, TEXT("Usage: lambda.give <weapon_classname>"));
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
	TEXT("lambda.give"),
	TEXT("Give the player a weapon by classname: lambda.give weapon_pistol"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaGiveCommand));

// lambda.giveammo <type> <count>
static void LambdaGiveAmmoCommand(const TArray<FString>& Args, UWorld* World)
{
	if (!World || Args.Num() < 2)
	{
		UE_LOG(LogLambda, Display, TEXT("Usage: lambda.giveammo <ammotype> <count>   e.g. lambda.giveammo Pistol 50"));
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
	TEXT("lambda.giveammo"),
	TEXT("Give the player ammo: lambda.giveammo <ammotype> <count>"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaGiveAmmoCommand));

// lambda.viewmodel <models/path.mdl> - load any Source model as the view model, for checking MDL support
static void LambdaViewModelCommand(const TArray<FString>& Args, UWorld* World)
{
	if (!World || Args.Num() < 1)
	{
		UE_LOG(LogLambda, Display, TEXT("Usage: lambda.viewmodel <models/weapons/v_pistol.mdl>"));
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
	TEXT("lambda.viewmodel"),
	TEXT("Load a Source .mdl as the first-person view model"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaViewModelCommand));

// lambda.surfaceinfo [surfaceprop ...] - show how each surface resolves to a game material and impact decal,
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

// lambda.npc_create <classname> - Source's npc_create: spawn an NPC where the player is looking
static void LambdaNPCCreateCommand(const TArray<FString>& Args, UWorld* World)
{
	if (!World || Args.Num() < 1)
	{
		UE_LOG(LogLambda, Display, TEXT("Usage: lambda.npc_create <npc_headcrab>"));
		return;
	}
	APlayerController* PC = World->GetFirstPlayerController();
	if (ALambdaCharacter* Player = PC ? Cast<ALambdaCharacter>(PC->GetPawn()) : nullptr)
	{
		Player->NPCCreate(Args[0], Args.Num() > 1 ? FCString::Atof(*Args[1]) : 5000.0f);
	}
}

// lambda.prop_list - what physics props exist and where they ended up (Source's ent_dump, cut down)
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
	TEXT("lambda.prop_list"),
	TEXT("List the physics props in the world and where they are"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaPropListCommand));

// lambda.prop_create <model> - Source's prop_physics_create
static void LambdaPropCreateCommand(const TArray<FString>& Args, UWorld* World)
{
	if (!World || Args.Num() < 1)
	{
		UE_LOG(LogLambda, Display, TEXT("Usage: lambda.prop_create <models/props_junk/wood_crate001a.mdl>"));
		return;
	}
	APlayerController* PC = World->GetFirstPlayerController();
	if (ALambdaCharacter* Player = PC ? Cast<ALambdaCharacter>(PC->GetPawn()) : nullptr)
	{
		Player->PropCreate(Args[0], Args.Num() > 1 ? FCString::Atof(*Args[1]) : 300.0f);
	}
}

static FAutoConsoleCommandWithWorldAndArgs GLambdaPropCreateCommand(
	TEXT("lambda.prop_create"),
	TEXT("Drop a physics prop of that model where the player is looking (Source's prop_physics_create)"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaPropCreateCommand));

static FAutoConsoleCommandWithWorldAndArgs GLambdaNPCCreateCommand(
	TEXT("lambda.npc_create"),
	TEXT("Spawn an NPC by classname where the player is looking (Source's npc_create)"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaNPCCreateCommand));

// lambda.decaltest [distance_cm] [angle_deg] - stamp test impact decals ahead and jump to a fixed viewpoint
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
	TEXT("lambda.decaltest"),
	TEXT("Stamp a row of impact decals on the wall ahead and view them from <distance_cm> at <angle_deg>"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaDecalTestCommand));

static FAutoConsoleCommandWithWorldAndArgs GLambdaSurfaceInfoCommand(
	TEXT("lambda.surfaceinfo"),
	TEXT("Show the surfaceprop -> game material -> impact decal chain for one or more surfaces"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaSurfaceInfoCommand));

