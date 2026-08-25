#include "Materials/SourceSurfaceProps.h"
#include "FileSystem/LambdaFileSystem.h"
#include "Core/LambdaSourceModule.h"
#include "Formats/SourceKeyValues.h"

namespace
{
	const TCHAR* SurfacePropsManifest = TEXT("scripts/surfaceproperties_manifest.txt");
}

FSourceSurfaceProps& FSourceSurfaceProps::Get()
{
	static FSourceSurfaceProps Instance;
	return Instance;
}

void FSourceSurfaceProps::Reset()
{
	Props.Reset();
	bInitialized = false;
}

void FSourceSurfaceProps::Initialize()
{
	if (bInitialized)
	{
		return;
	}
	bInitialized = true;

	// physprops reads the manifest and parses each listed file in order; a property defined twice keeps the last
	// definition, which is how a mod overrides a base HL2 surface.
	TArray<uint8> Bytes;
	if (FLambdaFileSystem::Get().ReadFile(SurfacePropsManifest, Bytes))
	{
		FSourceKeyValues Root;
		FString Error;
		if (FSourceKeyValues::ParseSingle(Bytes, Root, &Error))
		{
			TArray<const FSourceKeyValues*> Files;
			Root.FindChildren(TEXT("file"), Files);
			for (const FSourceKeyValues* File : Files)
			{
				LoadFile(File->Value);
			}
		}
		else
		{
			UE_LOG(LogLambdaSource, Warning, TEXT("%s: %s"), SurfacePropsManifest, *Error);
		}
	}
	else
	{
		// No manifest (a bare mod directory): the base file is still worth trying.
		LoadFile(TEXT("scripts/surfaceproperties.txt"));
	}

	UE_LOG(LogLambdaSource, Log, TEXT("Surface properties: %d entries"), Props.Num());
}

void FSourceSurfaceProps::LoadFile(const FString& RelativePath)
{
	TArray<uint8> Bytes;
	if (!FLambdaFileSystem::Get().ReadFile(RelativePath, Bytes))
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("Surface properties file not found: %s"), *RelativePath);
		return;
	}

	TArray<FSourceKeyValues> Roots;
	FString Error;
	if (!FSourceKeyValues::ParseText(FString(FUTF8ToTCHAR(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num())), Roots, &Error))
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("%s: %s"), *RelativePath, *Error);
		return;
	}

	int32 Added = 0;
	for (const FSourceKeyValues& Root : Roots)
	{
		if (!Root.IsSection() || Root.Key.IsEmpty())
		{
			continue;
		}
		FSourceSurfaceProp Prop;
		Prop.Name = Root.Key;

		const FString GameMaterial = Root.GetString(TEXT("gamematerial"));
		if (!GameMaterial.IsEmpty())
		{
			Prop.GameMaterial = FChar::ToUpper(GameMaterial[0]);
		}

		Prop.BulletImpactSound = Root.GetString(TEXT("bulletimpact"));
		Prop.ScrapeRoughSound = Root.GetString(TEXT("scraperough"));
		Prop.ImpactHardSound = Root.GetString(TEXT("impacthard"));
		Prop.ImpactSoftSound = Root.GetString(TEXT("impactsoft"));
		Prop.BreakSound = Root.GetString(TEXT("break"));
		// -1 means the entry did not say, so the value is inherited from "base" (or left at the default).
		const float Hardness = Root.GetFloat(TEXT("audiohardnessfactor"), -1.0f);
		if (Hardness >= 0.0f)
		{
			Prop.AudioHardnessFactor = Hardness;
		}
		Prop.StepLeftSound = Root.GetString(TEXT("stepleft"));
		Prop.StepRightSound = Root.GetString(TEXT("stepright"));
		Prop.Density = Root.GetFloat(TEXT("density"), Prop.Density);
		Prop.Elasticity = Root.GetFloat(TEXT("elasticity"), Prop.Elasticity);
		Prop.Friction = Root.GetFloat(TEXT("friction"), Prop.Friction);

		// Surface properties inherit: "$base" names another entry to start from.
		const FString Base = Root.GetString(TEXT("base"));
		if (!Base.IsEmpty())
		{
			if (const FSourceSurfaceProp* Parent = Find(Base))
			{
				if (GameMaterial.IsEmpty()) { Prop.GameMaterial = Parent->GameMaterial; }
				if (Prop.BulletImpactSound.IsEmpty()) { Prop.BulletImpactSound = Parent->BulletImpactSound; }
				if (Prop.ImpactHardSound.IsEmpty()) { Prop.ImpactHardSound = Parent->ImpactHardSound; }
				if (Prop.ImpactSoftSound.IsEmpty()) { Prop.ImpactSoftSound = Parent->ImpactSoftSound; }
				if (Prop.BreakSound.IsEmpty()) { Prop.BreakSound = Parent->BreakSound; }
				if (Hardness < 0.0f) { Prop.AudioHardnessFactor = Parent->AudioHardnessFactor; }
				if (Prop.ScrapeRoughSound.IsEmpty()) { Prop.ScrapeRoughSound = Parent->ScrapeRoughSound; }
				if (Prop.StepLeftSound.IsEmpty()) { Prop.StepLeftSound = Parent->StepLeftSound; }
				if (Prop.StepRightSound.IsEmpty()) { Prop.StepRightSound = Parent->StepRightSound; }
			}
		}

		Props.Add(Root.Key.ToLower(), MoveTemp(Prop));
		++Added;
	}

	UE_LOG(LogLambdaSource, Verbose, TEXT("  %s: %d surface properties"), *RelativePath, Added);
}

const FSourceSurfaceProp* FSourceSurfaceProps::Find(const FString& Name) const
{
	return Name.IsEmpty() ? nullptr : Props.Find(Name.ToLower());
}

TCHAR FSourceSurfaceProps::GetGameMaterial(const FString& SurfacePropName) const
{
	// GetImpactDecal defaults to Impact.Concrete when it has nothing better, so concrete is the fallback here too.
	const FSourceSurfaceProp* Prop = Find(SurfacePropName);
	return Prop ? Prop->GameMaterial : TEXT('C');
}
