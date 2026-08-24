#include "SourcePointTemplate.h"

#include "LambdaSourceModule.h"
#include "SourceBSPWorldActor.h"
#include "SourceCoordinates.h"

int32 ASourcePointTemplate::UniqueInstanceNumber = 0;

namespace
{
	/** A 3x3 rotation in Source space, indexed [row][column] as matrix3x4_t is. */
	struct FSourceRotation
	{
		float M[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
	};

	/** AngleMatrix (mathlib_base.cpp): Source's pitch/yaw/roll to a rotation matrix. */
	FSourceRotation AngleMatrix(const FVector3f& Angles)
	{
		float sp, cp, sy, cy, sr, cr;
		FMath::SinCos(&sp, &cp, FMath::DegreesToRadians(Angles.X));
		FMath::SinCos(&sy, &cy, FMath::DegreesToRadians(Angles.Y));
		FMath::SinCos(&sr, &cr, FMath::DegreesToRadians(Angles.Z));

		FSourceRotation R;
		R.M[0][0] = cp * cy;
		R.M[1][0] = cp * sy;
		R.M[2][0] = -sp;

		const float crcy = cr * cy;
		const float crsy = cr * sy;
		const float srcy = sr * cy;
		const float srsy = sr * sy;
		R.M[0][1] = sp * srcy - crsy;
		R.M[1][1] = sp * srsy + crcy;
		R.M[2][1] = sr * cp;

		R.M[0][2] = sp * crcy + srsy;
		R.M[1][2] = sp * crsy - srcy;
		R.M[2][2] = cr * cp;
		return R;
	}

	/** MatrixAngles (mathlib_base.cpp): back from a rotation matrix to pitch/yaw/roll. */
	FVector3f MatrixAngles(const FSourceRotation& R)
	{
		const float Forward0 = R.M[0][0];
		const float Forward1 = R.M[1][0];
		const float Forward2 = R.M[2][0];
		const float Left0 = R.M[0][1];
		const float Left1 = R.M[1][1];
		const float Left2 = R.M[2][1];
		const float Up2 = R.M[2][2];

		const float XYDist = FMath::Sqrt(Forward0 * Forward0 + Forward1 * Forward1);
		FVector3f Angles;
		if (XYDist > 0.001f)
		{
			Angles.Y = FMath::RadiansToDegrees(FMath::Atan2(Forward1, Forward0));
			Angles.X = FMath::RadiansToDegrees(FMath::Atan2(-Forward2, XYDist));
			Angles.Z = FMath::RadiansToDegrees(FMath::Atan2(Left2, Up2));
		}
		else
		{
			// Forward is mostly Z - gimbal lock.
			Angles.Y = FMath::RadiansToDegrees(FMath::Atan2(-Left0, Left1));
			Angles.X = FMath::RadiansToDegrees(FMath::Atan2(-Forward2, XYDist));
			Angles.Z = 0.0f;
		}
		return Angles;
	}

	/** VectorRotate. */
	FVector3f Rotate(const FSourceRotation& R, const FVector3f& V)
	{
		return FVector3f(
			V.X * R.M[0][0] + V.Y * R.M[0][1] + V.Z * R.M[0][2],
			V.X * R.M[1][0] + V.Y * R.M[1][1] + V.Z * R.M[1][2],
			V.X * R.M[2][0] + V.Y * R.M[2][1] + V.Z * R.M[2][2]);
	}

	/** VectorIRotate: the transpose, which for a rotation is the inverse. */
	FVector3f RotateInverse(const FSourceRotation& R, const FVector3f& V)
	{
		return FVector3f(
			V.X * R.M[0][0] + V.Y * R.M[1][0] + V.Z * R.M[2][0],
			V.X * R.M[0][1] + V.Y * R.M[1][1] + V.Z * R.M[2][1],
			V.X * R.M[0][2] + V.Y * R.M[1][2] + V.Z * R.M[2][2]);
	}

	/** ConcatRotations: Out = A * B. */
	FSourceRotation Concat(const FSourceRotation& A, const FSourceRotation& B)
	{
		FSourceRotation Out;
		for (int32 Row = 0; Row < 3; ++Row)
		{
			for (int32 Col = 0; Col < 3; ++Col)
			{
				Out.M[Row][Col] = A.M[Row][0] * B.M[0][Col] + A.M[Row][1] * B.M[1][Col] + A.M[Row][2] * B.M[2][Col];
			}
		}
		return Out;
	}

	FSourceRotation Transpose(const FSourceRotation& A)
	{
		FSourceRotation Out;
		for (int32 Row = 0; Row < 3; ++Row)
		{
			for (int32 Col = 0; Col < 3; ++Col)
			{
				Out.M[Row][Col] = A.M[Col][Row];
			}
		}
		return Out;
	}

	void SetKey(FSourceEntity& Entity, const FString& Key, const FString& Value)
	{
		for (TPair<FString, FString>& Pair : Entity.Pairs)
		{
			if (Pair.Key.Equals(Key, ESearchCase::IgnoreCase))
			{
				Pair.Value = Value;
				return;
			}
		}
		Entity.Pairs.Emplace(Key, Value);
	}

	FString FormatVector(const FVector3f& V)
	{
		return FString::Printf(TEXT("%g %g %g"), V.X, V.Y, V.Z);
	}
}

void ASourcePointTemplate::InitializeEntity(const FSourceEntity& InEntity, ASourceBSPWorldActor* InWorldActor)
{
	Super::InitializeEntity(InEntity, InWorldActor);

	InEntity.GetVector(TEXT("origin"), TemplateOrigin);
	InEntity.GetVector(TEXT("angles"), TemplateAngles);
	SetActorLocation(FSourceCoords::ToUE(TemplateOrigin));

	// Template01..Template16 name the entities this one owns.
	for (int32 i = 1; i <= 16; ++i)
	{
		const FString Key = FString::Printf(TEXT("Template%02d"), i);
		const FString Name = InEntity.Get(Key);
		if (!Name.IsEmpty())
		{
			TemplateNames.Add(Name);
		}
	}
}

bool ASourcePointTemplate::OwnsEntityNamed(const FString& Name) const
{
	for (const FString& Pattern : TemplateNames)
	{
		// FindEntityByName matches a trailing '*' as a prefix wildcard.
		if (Pattern.EndsWith(TEXT("*")))
		{
			if (Name.StartsWith(Pattern.LeftChop(1), ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		else if (Name.Equals(Pattern, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

bool ASourcePointTemplate::ShouldRemoveTemplateEntities() const
{
	return !HasSpawnFlags(SourcePointTemplateFlags::DontRemoveTemplateEntities);
}

bool ASourcePointTemplate::AllowNameFixup() const
{
	return !HasSpawnFlags(SourcePointTemplateFlags::PreserveNames);
}

void ASourcePointTemplate::AddTemplate(const FSourceEntity& TemplateEntity)
{
	// matEntityToTemplate = matWorldToTemplate * matEntityToWorld: where the entity sits in the template's space.
	FVector3f EntityOrigin = FVector3f::ZeroVector;
	FVector3f EntityAngles = FVector3f::ZeroVector;
	TemplateEntity.GetVector(TEXT("origin"), EntityOrigin);
	TemplateEntity.GetVector(TEXT("angles"), EntityAngles);

	const FSourceRotation TemplateToWorld = AngleMatrix(TemplateAngles);

	FSourceTemplateEntry Entry;
	Entry.Entity = TemplateEntity;
	Entry.OriginToTemplate = RotateInverse(TemplateToWorld, EntityOrigin - TemplateOrigin);
	Entry.AnglesToTemplate = MatrixAngles(Concat(Transpose(TemplateToWorld), AngleMatrix(EntityAngles)));
	Templates.Add(MoveTemp(Entry));
}

bool ASourcePointTemplate::CreateInstance(const FVector3f& Origin, const FVector3f& Angles, TArray<AActor*>& OutSpawned)
{
	if (Templates.Num() == 0)
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("point_template '%s' was asked to spawn but owns no templates"), *GetTargetName());
		return false;
	}
	ASourceBSPWorldActor* World = WorldActor.Get();
	if (!World)
	{
		return false;
	}

	// Templates_StartUniqueInstance: every instance gets its own number, so two copies of a group do not answer
	// to each other's names.
	++UniqueInstanceNumber;

	// The names inside this group and what they become in this instance, so the group's own entity I/O keeps
	// pointing within the copy (Templates_ReconnectIOForGroup).
	TMap<FString, FString> NameFixups;
	if (AllowNameFixup())
	{
		for (const FSourceTemplateEntry& Entry : Templates)
		{
			const FString Name = Entry.Entity.Get(TEXT("targetname"));
			if (!Name.IsEmpty())
			{
				NameFixups.Add(Name, FString::Printf(TEXT("%s&%04d"), *Name, UniqueInstanceNumber));
			}
		}
	}

	const FSourceRotation NewTemplateToWorld = AngleMatrix(Angles);

	for (const FSourceTemplateEntry& Entry : Templates)
	{
		FSourceEntity Spawn = Entry.Entity;

		// matStoredLocalToWorld = matNewTemplateToWorld * matEntityToTemplate.
		const FVector3f NewOrigin = Origin + Rotate(NewTemplateToWorld, Entry.OriginToTemplate);
		const FVector3f NewAngles = MatrixAngles(Concat(NewTemplateToWorld, AngleMatrix(Entry.AnglesToTemplate)));
		SetKey(Spawn, TEXT("origin"), FormatVector(NewOrigin));
		SetKey(Spawn, TEXT("angles"), FormatVector(NewAngles));

		if (NameFixups.Num() > 0)
		{
			for (TPair<FString, FString>& Pair : Spawn.Pairs)
			{
				if (Pair.Key.Equals(TEXT("targetname"), ESearchCase::IgnoreCase))
				{
					if (const FString* Fixed = NameFixups.Find(Pair.Value))
					{
						Pair.Value = *Fixed;
					}
					continue;
				}
				// An output reads "target,input,parameter,delay,timestofire"; if it names one of our own, point
				// it at this instance's copy instead of at the original group.
				int32 Comma = INDEX_NONE;
				if (Pair.Value.FindChar(TCHAR(','), Comma))
				{
					const FString Target = Pair.Value.Left(Comma);
					if (const FString* Fixed = NameFixups.Find(Target))
					{
						Pair.Value = *Fixed + Pair.Value.RightChop(Comma);
					}
				}
			}
		}

		if (AActor* Spawned = World->SpawnEntityFromKeyValues(Spawn))
		{
			OutSpawned.Add(Spawned);
		}
	}

	UE_LOG(LogLambdaSource, Log, TEXT("point_template '%s' spawned %d template entities at %s"),
		*GetTargetName(), Templates.Num(), *FormatVector(Origin));
	return true;
}

bool ASourcePointTemplate::AcceptInput(const FString& InputName, AActor* Activator, AActor* Caller, const FString& Parameter)
{
	if (InputName.Equals(TEXT("ForceSpawn"), ESearchCase::IgnoreCase))
	{
		// InputForceSpawn: the group is stamped down at the template's own position and angles.
		TArray<AActor*> Spawned;
		if (CreateInstance(TemplateOrigin, TemplateAngles, Spawned))
		{
			FireOutput(TEXT("OnEntitySpawned"), Activator);
		}
		return true;
	}
	return Super::AcceptInput(InputName, Activator, Caller, Parameter);
}
