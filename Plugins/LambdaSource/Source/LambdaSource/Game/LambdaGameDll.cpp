#include "Game/LambdaGameDll.h"

#include "Audio/LambdaSoundLibrary.h"
#include "Core/LambdaSourceModule.h"
#include "Core/LambdaSourceSettings.h"
#include "Core/SourceCoordinates.h"
#include "FileSystem/LambdaFileSystem.h"
#include "Entities/SourceGameEntity.h"
#include "Entities/SourceGamePointEntity.h"
#include "Creatures/SourceNPCBase.h"
#include "GameFramework/Pawn.h"
#include "World/SourceEntity.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Paths.h"

typedef void* (*FCreateLambdaGameFn)(const char*);

FLambdaGameDll& FLambdaGameDll::Get()
{
	static FLambdaGameDll Instance;
	return Instance;
}

bool FLambdaGameDll::Load()
{
	if (Game)
	{
		return true;
	}

	// Two places, because there are two ways to run this. A packaged game has the DLL beside its executable;
	// running from the editor puts BaseDir() inside the Unreal installation, where the game's own binaries are
	// nowhere to be seen - so the game directory is asked as well. The mod folder is <game>\Mods\lambda, and
	// the binaries sit at <game>\LambdaEngine\Binaries\Win64.
	TArray<FString> Candidates;
	Candidates.Add(FPaths::Combine(FPlatformProcess::BaseDir(), TEXT("LambdaGame.dll")));

	const FString ModDir = FLambdaFileSystem::Get().GetGameDirectory();
	if (!ModDir.IsEmpty())
	{
		const FString GameRoot = FPaths::GetPath(FPaths::GetPath(ModDir));	// <game>\Mods\lambda -> <game>
		Candidates.Add(FPaths::Combine(GameRoot, TEXT("LambdaEngine"), TEXT("Binaries"), TEXT("Win64"), TEXT("LambdaGame.dll")));
	}

	FString DllPath;
	for (const FString& Candidate : Candidates)
	{
		if (FPaths::FileExists(Candidate))
		{
			DllPath = Candidate;
			break;
		}
	}
	if (DllPath.IsEmpty())
	{
		UE_LOG(LogLambdaSource, Log, TEXT("No LambdaGame.dll found (looked in: %s) - entities use their native implementations."),
			*FString::Join(Candidates, TEXT("; ")));
		return false;
	}

	DllHandle = FPlatformProcess::GetDllHandle(*DllPath);
	if (!DllHandle)
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("LambdaGame.dll is there but would not load: %s"), *DllPath);
		return false;
	}

	FCreateLambdaGameFn Create = (FCreateLambdaGameFn)FPlatformProcess::GetDllExport(DllHandle, TEXT("CreateLambdaGame"));
	if (!Create)
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("LambdaGame.dll exports no CreateLambdaGame - is it a game module?"));
		Unload();
		return false;
	}

	// Asked for by version. A DLL built against a different interface says no here, which is a clear message
	// rather than a crash somewhere inside a vtable that has since moved.
	Game = (lambda::IGame*)Create(LAMBDA_GAME_API_VERSION);
	if (!Game)
	{
		UE_LOG(LogLambdaSource, Warning,
			TEXT("LambdaGame.dll does not speak %hs - rebuild it against the current LambdaGameAPI.h."),
			LAMBDA_GAME_API_VERSION);
		Unload();
		return false;
	}

	if (!Game->Init(this))
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("LambdaGame.dll refused to initialise."));
		Game = nullptr;
		Unload();
		return false;
	}

	UE_LOG(LogLambdaSource, Log, TEXT("Loaded game module '%hs' (%hs)"), Game->GetName(), LAMBDA_GAME_API_VERSION);
	return true;
}

void FLambdaGameDll::Unload()
{
	if (Game)
	{
		Game->Shutdown();
		Game = nullptr;
	}
	if (DllHandle)
	{
		FPlatformProcess::FreeDllHandle(DllHandle);
		DllHandle = nullptr;
	}
	EntitiesById.Reset();
	IdsByEntity.Reset();
}

bool FLambdaGameDll::HandlesClass(const FString& ClassName) const
{
	return Game && Game->HandlesClass(TCHAR_TO_ANSI(*ClassName));
}

lambda::EntityId FLambdaGameDll::IdForEntity(AActor* Entity)
{
	if (!Entity)
	{
		return lambda::InvalidEntity;
	}
	if (const uint32* Existing = IdsByEntity.Find(Entity))
	{
		return *Existing;
	}
	const uint32 Id = NextId++;
	EntitiesById.Add(Id, Entity);
	IdsByEntity.Add(Entity, Id);
	return Id;
}

AActor* FLambdaGameDll::ResolveEntity(lambda::EntityId Id) const
{
	const TWeakObjectPtr<AActor>* Found = EntitiesById.Find(Id);
	return Found ? Found->Get() : nullptr;
}

ASourceEntity* FLambdaGameDll::ResolveSourceEntity(lambda::EntityId Id) const
{
	return Cast<ASourceEntity>(ResolveEntity(Id));
}

lambda::IEntity* FLambdaGameDll::CreateEntity(const FString& ClassName, ASourceEntity* Owner, lambda::EntityId& OutId)
{
	OutId = lambda::InvalidEntity;
	if (!Game || !Owner)
	{
		return nullptr;
	}
	OutId = IdForEntity(Owner);
	return Game->CreateEntity(TCHAR_TO_ANSI(*ClassName), OutId);
}

void FLambdaGameDll::DestroyEntity(lambda::IEntity* Behaviour, lambda::EntityId Id)
{
	if (Game && Behaviour)
	{
		// Freed by the side that allocated it - the two heaps are not the same one.
		Game->DestroyEntity(Behaviour);
	}
	if (Id != lambda::InvalidEntity)
	{
		if (const TWeakObjectPtr<AActor>* Found = EntitiesById.Find(Id))
		{
			IdsByEntity.Remove(*Found);
		}
		EntitiesById.Remove(Id);
	}
}

const char* FLambdaGameDll::StoreReturnString(const FString& Value) const
{
	const FTCHARToUTF8 Converted(*Value);
	ReturnedString.SetNumUninitialized(Converted.Length() + 1);
	FMemory::Memcpy(ReturnedString.GetData(), Converted.Get(), Converted.Length());
	ReturnedString[Converted.Length()] = '\0';
	return ReturnedString.GetData();
}

// ---------------------------------------------------------------------------------------------------------
// lambda::IEngine
// ---------------------------------------------------------------------------------------------------------

const char* FLambdaGameDll::GetKeyValue(lambda::EntityId Entity, const char* Key) const
{
	const ASourceEntity* Actor = ResolveSourceEntity(Entity);
	if (!Actor || !Key)
	{
		return "";
	}
	return StoreReturnString(Actor->GetEntity().Get(ANSI_TO_TCHAR(Key)));
}

const char* FLambdaGameDll::GetClassName(lambda::EntityId Entity) const
{
	const ASourceEntity* Actor = ResolveSourceEntity(Entity);
	return Actor ? StoreReturnString(Actor->GetEntity().ClassName) : "";
}

const char* FLambdaGameDll::GetTargetName(lambda::EntityId Entity) const
{
	const ASourceEntity* Actor = ResolveSourceEntity(Entity);
	return Actor ? StoreReturnString(Actor->GetTargetName()) : "";
}

void FLambdaGameDll::GetOrigin(lambda::EntityId Entity, lambda::Vec3* OutOrigin) const
{
	if (!OutOrigin)
	{
		return;
	}
	*OutOrigin = lambda::Vec3();
	if (const ASourceGameEntity* Actor = Cast<ASourceGameEntity>(ResolveEntity(Entity)))
	{
		const FVector3f Origin = Actor->GetSourceOrigin();
		OutOrigin->x = Origin.X;
		OutOrigin->y = Origin.Y;
		OutOrigin->z = Origin.Z;
	}
}

void FLambdaGameDll::SetOrigin(lambda::EntityId Entity, const lambda::Vec3& Origin)
{
	if (ASourceGameEntity* Actor = Cast<ASourceGameEntity>(ResolveEntity(Entity)))
	{
		Actor->SetSourceOriginPublic(FVector3f(Origin.x, Origin.y, Origin.z));
	}
}

void FLambdaGameDll::GetAngles(lambda::EntityId Entity, lambda::Vec3* OutAngles) const
{
	if (!OutAngles)
	{
		return;
	}
	*OutAngles = lambda::Vec3();
	if (const ASourceGameEntity* Actor = Cast<ASourceGameEntity>(ResolveEntity(Entity)))
	{
		const FVector3f Angles = Actor->GetSourceAngles();
		OutAngles->x = Angles.X;
		OutAngles->y = Angles.Y;
		OutAngles->z = Angles.Z;
	}
}

void FLambdaGameDll::SetAngles(lambda::EntityId Entity, const lambda::Vec3& Angles)
{
	if (ASourceGameEntity* Actor = Cast<ASourceGameEntity>(ResolveEntity(Entity)))
	{
		Actor->SetSourceAngles(FVector3f(Angles.x, Angles.y, Angles.z));
	}
}

void FLambdaGameDll::AngularMove(lambda::EntityId Entity, const lambda::Vec3& DestinationAngles, float Speed)
{
	if (ASourceGameEntity* Actor = Cast<ASourceGameEntity>(ResolveEntity(Entity)))
	{
		Actor->BeginAngularMove(FVector3f(DestinationAngles.x, DestinationAngles.y, DestinationAngles.z), Speed);
	}
}

void FLambdaGameDll::AngularMoveAxis(lambda::EntityId Entity, const lambda::Vec3& Axis, const lambda::Vec3& DestinationAngles, float Speed)
{
	if (ASourceGameEntity* Actor = Cast<ASourceGameEntity>(ResolveEntity(Entity)))
	{
		Actor->BeginAxisMove(FVector3f(Axis.x, Axis.y, Axis.z),
			FVector3f(DestinationAngles.x, DestinationAngles.y, DestinationAngles.z), Speed);
	}
}

void FLambdaGameDll::RotateAngles(const lambda::Vec3& Angles, const lambda::Vec3& Axis, float Degrees,
	lambda::Vec3* OutAngles) const
{
	if (!OutAngles)
	{
		return;
	}

	const FQuat Start = FSourceCoords::AnglesToUE(FVector3f(Angles.x, Angles.y, Angles.z)).Quaternion();
	const FVector Unit = FSourceCoords::ToUEDirection(FVector3f(Axis.x, Axis.y, Axis.z));

	const FVector3f Result = Unit.IsNearlyZero()
		? FVector3f(Angles.x, Angles.y, Angles.z)
		: FSourceCoords::AnglesFromUE((FQuat(Unit, FMath::DegreesToRadians(Degrees)) * Start).Rotator());

	*OutAngles = lambda::Vec3{ Result.X, Result.Y, Result.Z };
}

void FLambdaGameDll::SetSolid(lambda::EntityId Entity, bool bSolid)
{
	if (ASourceGameEntity* Actor = Cast<ASourceGameEntity>(ResolveEntity(Entity)))
	{
		Actor->SetSolidity(bSolid);
	}
}

void FLambdaGameDll::EmitSoundLooping(lambda::EntityId Entity, const char* SoundNameOrPath)
{
	if (ASourceGameEntity* Actor = Cast<ASourceGameEntity>(ResolveEntity(Entity)))
	{
		Actor->StartLoopingSound(ANSI_TO_TCHAR(SoundNameOrPath ? SoundNameOrPath : ""));
	}
}

void FLambdaGameDll::StopLoopingSound(lambda::EntityId Entity)
{
	if (ASourceGameEntity* Actor = Cast<ASourceGameEntity>(ResolveEntity(Entity)))
	{
		Actor->StopLoopingSoundNow();
	}
}

void FLambdaGameDll::GetBoundsSize(lambda::EntityId Entity, lambda::Vec3* OutSize) const
{
	if (!OutSize)
	{
		return;
	}
	*OutSize = lambda::Vec3();
	const ASourceGameEntity* Actor = Cast<ASourceGameEntity>(ResolveEntity(Entity));
	if (!Actor)
	{
		return;
	}
	// Handed over in Source units, because that is the only unit the game side knows.
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	const FVector Size = Actor->GetLocalBoundsSize();
	OutSize->x = (float)FMath::Abs(Size.X) / Scale;
	OutSize->y = (float)FMath::Abs(Size.Y) / Scale;
	OutSize->z = (float)FMath::Abs(Size.Z) / Scale;
}

void FLambdaGameDll::LinearMove(lambda::EntityId Entity, const lambda::Vec3& Destination, float Speed)
{
	if (ASourceGameEntity* Actor = Cast<ASourceGameEntity>(ResolveEntity(Entity)))
	{
		Actor->BeginLinearMove(FVector3f(Destination.x, Destination.y, Destination.z), Speed);
	}
}

void FLambdaGameDll::StopMove(lambda::EntityId Entity)
{
	if (ASourceGameEntity* Actor = Cast<ASourceGameEntity>(ResolveEntity(Entity)))
	{
		Actor->CancelLinearMove();
	}
}

void FLambdaGameDll::SetLightScale(lambda::EntityId Entity, float Scale)
{
	if (ASourceGamePointEntity* Actor = Cast<ASourceGamePointEntity>(ResolveEntity(Entity)))
	{
		Actor->SetLightScale(Scale);
	}
}

void FLambdaGameDll::SetTriggerVolume(lambda::EntityId Entity, bool bTrigger)
{
	if (ASourceGameEntity* Actor = Cast<ASourceGameEntity>(ResolveEntity(Entity)))
	{
		Actor->SetTriggerVolume(bTrigger);
	}
}

bool FLambdaGameDll::IsPlayer(lambda::EntityId Entity) const
{
	const APawn* Pawn = Cast<APawn>(ResolveEntity(Entity));
	return Pawn && Pawn->IsPlayerControlled();
}

bool FLambdaGameDll::IsNPC(lambda::EntityId Entity) const
{
	return Cast<ASourceNPCBase>(ResolveEntity(Entity)) != nullptr;
}

void FLambdaGameDll::EmitSound(lambda::EntityId Entity, const char* SoundNameOrPath)
{
	ASourceEntity* Actor = ResolveSourceEntity(Entity);
	if (!Actor || !SoundNameOrPath || !SoundNameOrPath[0])
	{
		return;
	}
	const FString Name = ANSI_TO_TCHAR(SoundNameOrPath);
	float Volume = 1.0f, Pitch = 1.0f;
	if (ULambdaSoundWave* Wave = FLambdaSoundCache::Get().CreateWaveResolved(Actor, Name, false, Volume, Pitch))
	{
		UGameplayStatics::SpawnSoundAtLocation(Actor, Wave, Actor->GetActorLocation(), FRotator::ZeroRotator, Volume, Pitch);
	}
}

void FLambdaGameDll::FireOutput(lambda::EntityId Entity, const char* OutputName, lambda::EntityId Activator)
{
	ASourceEntity* Actor = ResolveSourceEntity(Entity);
	if (!Actor || !OutputName)
	{
		return;
	}
	Actor->FireOutput(ANSI_TO_TCHAR(OutputName), ResolveEntity(Activator));
}

float FLambdaGameDll::GetOutputMaxDelay(lambda::EntityId Entity, const char* OutputName) const
{
	const ASourceEntity* Actor = const_cast<FLambdaGameDll*>(this)->ResolveSourceEntity(Entity);
	if (!Actor || !OutputName)
	{
		return -1.0f;
	}
	return Actor->GetOutputMaxDelay(ANSI_TO_TCHAR(OutputName));
}

void FLambdaGameDll::CancelPendingOutputs(lambda::EntityId Entity)
{
	ASourceEntity* Actor = ResolveSourceEntity(Entity);
	if (Actor)
	{
		Actor->CancelPendingOutputs();
	}
}

void FLambdaGameDll::Remove(lambda::EntityId Entity)
{
	AActor* Actor = ResolveEntity(Entity);
	if (!Actor)
	{
		return;
	}
	// Not Destroy(): the entity is almost always asking for this from inside an input it is still handling, and
	// destroying it here would pull the ground out from under the call that asked. A lifespan takes it away on
	// the world's own time instead, which is what UTIL_Remove does.
	Actor->SetLifeSpan(KINDA_SMALL_NUMBER);
}

float FLambdaGameDll::GetTime() const
{
	// Any live entity can answer this; they all share one world.
	for (const TPair<uint32, TWeakObjectPtr<AActor>>& Pair : EntitiesById)
	{
		if (const AActor* Actor = Pair.Value.Get())
		{
			if (const UWorld* World = Actor->GetWorld())
			{
				return World->GetTimeSeconds();
			}
		}
	}
	return 0.0f;
}

void FLambdaGameDll::Log(const char* Message) const
{
	if (Message)
	{
		UE_LOG(LogLambdaSource, Log, TEXT("%hs"), Message);
	}
}
