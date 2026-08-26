#include "Entities/SourceInfoLadder.h"

#include "Core/LambdaSourceModule.h"
#include "Core/LambdaSourceSettings.h"
#include "World/SourceEntity.h"

ASourceInfoLadder::ASourceInfoLadder()
{
	PrimaryActorTick.bCanEverTick = false;
	SetActorEnableCollision(false);
}

void ASourceInfoLadder::InitializeFromEntity(const FSourceEntity& Entity)
{
	// vbsp writes the volume as six scalars rather than two vectors.
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	auto Num = [&Entity](const TCHAR* Key) { return FCString::Atof(*Entity.Get(Key)); };

	const FVector3f Mins(Num(TEXT("mins.x")), Num(TEXT("mins.y")), Num(TEXT("mins.z")));
	const FVector3f Maxs(Num(TEXT("maxs.x")), Num(TEXT("maxs.y")), Num(TEXT("maxs.z")));

	// Source (x, y, z) -> UE (x, -y, z): the y mirror swaps which of mins/maxs bounds y.
	const FVector A(Mins.X * Scale, -Maxs.Y * Scale, Mins.Z * Scale);
	const FVector B(Maxs.X * Scale, -Mins.Y * Scale, Maxs.Z * Scale);
	Volume = FBox(A, B);
	SetActorLocation(Volume.GetCenter());

	const FVector Size = Volume.GetSize();
	ThinAxis = Size.X <= Size.Y ? FVector(1.0f, 0.0f, 0.0f) : FVector(0.0f, 1.0f, 0.0f);

	UE_LOG(LogLambdaSource, Log, TEXT("info_ladder: %.0fx%.0fx%.0f units at %s"),
		Size.X / Scale, Size.Y / Scale, Size.Z / Scale, *Volume.GetCenter().ToCompactString());
}

FVector ASourceInfoLadder::GetNormalToward(const FVector& WorldPoint) const
{
	const float Side = FVector::DotProduct(WorldPoint - Volume.GetCenter(), ThinAxis);
	return Side >= 0.0f ? ThinAxis : -ThinAxis;
}
