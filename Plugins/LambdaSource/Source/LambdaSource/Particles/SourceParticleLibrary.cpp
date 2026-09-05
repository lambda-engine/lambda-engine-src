#include "Particles/SourceParticleLibrary.h"
#include "Core/LambdaSourceModule.h"
#include "FileSystem/LambdaFileSystem.h"
#include "Formats/SourceKeyValues.h"
#include "Materials/LambdaMaterialLibrary.h"

// ---- FSourceParticleDefinition -----------------------------------------------------------------------------------

FString FSourceParticleDefinition::CanonicalOperatorName(const FString& FunctionName)
{
	// RemapOperatorName, particles/particles.cpp. Written once so the operator registry only ever sees the
	// current spellings.
	static const TMap<FString, FString> Remap = []()
	{
		TMap<FString, FString> M;
		M.Add(TEXT("alpha_fade"), TEXT("Alpha Fade and Decay"));
		M.Add(TEXT("alpha_fade_in_random"), TEXT("Alpha Fade In Random"));
		M.Add(TEXT("alpha_fade_out_random"), TEXT("Alpha Fade Out Random"));
		M.Add(TEXT("basic_movement"), TEXT("Movement Basic"));
		M.Add(TEXT("color_fade"), TEXT("Color Fade"));
		M.Add(TEXT("controlpoint_light"), TEXT("Color Light From Control Point"));
		M.Add(TEXT("Dampen Movement Relative to Control Point"), TEXT("Movement Dampen Relative to Control Point"));
		M.Add(TEXT("Distance Between Control Points Scale"), TEXT("Remap Distance Between Two Control Points to Scalar"));
		M.Add(TEXT("Distance to Control Points Scale"), TEXT("Remap Distance to Control Point to Scalar"));
		M.Add(TEXT("lifespan_decay"), TEXT("Lifespan Decay"));
		M.Add(TEXT("lock to bone"), TEXT("Movement Lock to Bone"));
		M.Add(TEXT("postion_lock_to_controlpoint"), TEXT("Movement Lock to Control Point"));
		M.Add(TEXT("maintain position along path"), TEXT("Movement Maintain Position Along Path"));
		M.Add(TEXT("Match Particle Velocities"), TEXT("Movement Match Particle Velocities"));
		M.Add(TEXT("Max Velocity"), TEXT("Movement Max Velocity"));
		M.Add(TEXT("noise"), TEXT("Noise Scalar"));
		M.Add(TEXT("vector noise"), TEXT("Noise Vector"));
		M.Add(TEXT("oscillate_scalar"), TEXT("Oscillate Scalar"));
		M.Add(TEXT("oscillate_vector"), TEXT("Oscillate Vector"));
		M.Add(TEXT("Orient Rotation to 2D Direction"), TEXT("Rotation Orient to 2D Direction"));
		M.Add(TEXT("radius_scale"), TEXT("Radius Scale"));
		M.Add(TEXT("Random Cull"), TEXT("Cull Random"));
		M.Add(TEXT("remap_scalar"), TEXT("Remap Scalar"));
		M.Add(TEXT("rotation_movement"), TEXT("Rotation Basic"));
		M.Add(TEXT("rotation_spin"), TEXT("Rotation Spin Roll"));
		M.Add(TEXT("rotation_spin yaw"), TEXT("Rotation Spin Yaw"));
		M.Add(TEXT("alpha_random"), TEXT("Alpha Random"));
		M.Add(TEXT("color_random"), TEXT("Color Random"));
		M.Add(TEXT("create from parent particles"), TEXT("Position From Parent Particles"));
		M.Add(TEXT("Create In Hierarchy"), TEXT("Position In CP Hierarchy"));
		M.Add(TEXT("random position along path"), TEXT("Position Along Path Random"));
		M.Add(TEXT("random position on model"), TEXT("Position on Model Random"));
		M.Add(TEXT("sequential position along path"), TEXT("Position Along Path Sequential"));
		M.Add(TEXT("position_offset_random"), TEXT("Position Modify Offset Random"));
		M.Add(TEXT("position_warp_random"), TEXT("Position Modify Warp Random"));
		M.Add(TEXT("position_within_box"), TEXT("Position Within Box Random"));
		M.Add(TEXT("position_within_sphere"), TEXT("Position Within Sphere Random"));
		M.Add(TEXT("Inherit Velocity"), TEXT("Velocity Inherit from Control Point"));
		M.Add(TEXT("Initial Repulsion Velocity"), TEXT("Velocity Repulse from World"));
		M.Add(TEXT("Initial Velocity Noise"), TEXT("Velocity Noise"));
		M.Add(TEXT("Initial Scalar Noise"), TEXT("Remap Noise to Scalar"));
		M.Add(TEXT("Lifespan from distance to world"), TEXT("Lifetime from Time to Impact"));
		M.Add(TEXT("Pre-Age Noise"), TEXT("Lifetime Pre-Age Noise"));
		M.Add(TEXT("lifetime_random"), TEXT("Lifetime Random"));
		M.Add(TEXT("radius_random"), TEXT("Radius Random"));
		M.Add(TEXT("random yaw"), TEXT("Rotation Yaw Random"));
		M.Add(TEXT("Randomly Flip Yaw"), TEXT("Rotation Yaw Flip Random"));
		M.Add(TEXT("rotation_random"), TEXT("Rotation Random"));
		M.Add(TEXT("rotation_speed_random"), TEXT("Rotation Speed Random"));
		M.Add(TEXT("sequence_random"), TEXT("Sequence Random"));
		M.Add(TEXT("second_sequence_random"), TEXT("Sequence Two Random"));
		M.Add(TEXT("trail_length_random"), TEXT("Trail Length Random"));
		M.Add(TEXT("velocity_random"), TEXT("Velocity Random"));
		// Not in Source's table, but written by earlier versions of the particle editor.
		M.Add(TEXT("emit_noise"), TEXT("emit noise"));
		return M;
	}();

	for (const TPair<FString, FString>& Pair : Remap)
	{
		if (Pair.Key.Equals(FunctionName, ESearchCase::IgnoreCase))
		{
			return Pair.Value;
		}
	}
	return FunctionName;
}

// ---- FSourceParticleLibrary --------------------------------------------------------------------------------------

FSourceParticleLibrary& FSourceParticleLibrary::Get()
{
	static FSourceParticleLibrary Instance;
	return Instance;
}

void FSourceParticleLibrary::Reset()
{
	Definitions.Reset();
	LoadedFiles.Reset();
	bInitialized = false;
}

void FSourceParticleLibrary::Initialize()
{
	if (bInitialized)
	{
		return;
	}
	bInitialized = true;
	const double StartTime = FPlatformTime::Seconds();

	// The manifest first, in its order: later files replace earlier definitions of the same name, as in
	// CParticleSystemMgr::ReadParticleDefinitions. A "!" in front of a path only marks it for precaching in
	// Source; it is the same file here.
	int32 NumManifest = 0;
	TArray<uint8> ManifestBytes;
	if (FLambdaFileSystem::Get().ReadFile(TEXT("particles/particles_manifest.txt"), ManifestBytes))
	{
		FSourceKeyValues Root;
		FString Error;
		if (FSourceKeyValues::ParseSingle(ManifestBytes, Root, &Error))
		{
			for (const FSourceKeyValues& Child : Root.Children)
			{
				if (Child.Key.Equals(TEXT("file"), ESearchCase::IgnoreCase) && !Child.Value.IsEmpty())
				{
					FString Path = Child.Value;
					if (Path.StartsWith(TEXT("!")))
					{
						Path.RightChopInline(1);
					}
					if (LoadFile(Path, /*bReplace=*/ true) != INDEX_NONE)
					{
						++NumManifest;
					}
				}
			}
		}
		else
		{
			UE_LOG(LogLambdaSource, Warning, TEXT("particles_manifest.txt: %s"), *Error);
		}
	}

	// Then whatever else is lying in particles/ across the mounts, adding only names nobody has defined.
	TArray<FString> Found;
	FLambdaFileSystem::Get().FindFiles(TEXT("particles"), TEXT("*.pcf"), Found);
	Found.Sort();
	int32 NumScanned = 0;
	for (FString Path : Found)
	{
		// A loose file comes back with its folder, a VPK entry as the bare name.
		if (!Path.Contains(TEXT("/")))
		{
			Path = TEXT("particles/") + Path;
		}
		if (LoadFile(Path, /*bReplace=*/ false) != INDEX_NONE)
		{
			++NumScanned;
		}
	}

	UE_LOG(LogLambdaSource, Log, TEXT("Particles: %d definitions from %d manifest + %d found files (%.0f ms)"),
		Definitions.Num(), NumManifest, NumScanned, (FPlatformTime::Seconds() - StartTime) * 1000.0);
}

int32 FSourceParticleLibrary::LoadFile(const FString& InRelativePath, bool bReplace)
{
	const FString RelativePath = FLambdaFileSystem::NormalizeRelativePath(InRelativePath);
	const FString Key = RelativePath.ToLower();
	if (LoadedFiles.Contains(Key))
	{
		return 0;
	}
	LoadedFiles.Add(Key);

	TArray<uint8> Bytes;
	if (!FLambdaFileSystem::Get().ReadFile(RelativePath, Bytes))
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("Particle file '%s' not found"), *RelativePath);
		return INDEX_NONE;
	}
	TSharedPtr<FSourceDMXFile> File = MakeShared<FSourceDMXFile>();
	FString Error;
	if (!File->Load(Bytes, &Error))
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("Particle file '%s': %s"), *RelativePath, *Error);
		return INDEX_NONE;
	}
	const FSourceDMXElement* Root = File->GetRoot();
	if (!Root)
	{
		return INDEX_NONE;
	}

	// Either a library (root holds particleSystemDefinitions) or a single definition as the root.
	TArray<int32> RootDefinitions;
	if (Root->Type.Equals(TEXT("DmeParticleSystemDefinition"), ESearchCase::IgnoreCase))
	{
		RootDefinitions.Add(0);
	}
	else
	{
		Root->GetElementIndices(TEXT("particleSystemDefinitions"), RootDefinitions);
		if (RootDefinitions.Num() == 0)
		{
			// The attribute may be named differently by a tool; take the first element array on the root.
			for (const TPair<FString, FSourceDMXValue>& Pair : Root->Attributes)
			{
				if (Pair.Value.Type == ESourceDMXType::ElementArray)
				{
					Root->GetElementIndices(Pair.Key, RootDefinitions);
					break;
				}
			}
		}
	}

	TMap<int32, TSharedPtr<FSourceParticleDefinition>> ByElement;
	TArray<TSharedPtr<FSourceParticleDefinition>> Parsed;
	for (int32 Index : RootDefinitions)
	{
		if (TSharedPtr<FSourceParticleDefinition> Def = ParseDefinition(File, Index, ByElement, RelativePath))
		{
			Parsed.Add(Def);
		}
	}

	int32 NumAdded = 0;
	for (const TPair<int32, TSharedPtr<FSourceParticleDefinition>>& Pair : ByElement)
	{
		const FString NameKey = Pair.Value->Name.ToLower();
		if (NameKey.IsEmpty())
		{
			continue;
		}
		if (const TSharedPtr<FSourceParticleDefinition>* Existing = Definitions.Find(NameKey))
		{
			if (!bReplace)
			{
				continue;
			}
			UE_LOG(LogLambdaSource, Verbose, TEXT("Particle '%s' from %s replaces the one from %s"), *Pair.Value->Name,
				*RelativePath, *(*Existing)->SourceFile);
		}
		Definitions.Add(NameKey, Pair.Value);
		++NumAdded;
	}
	UE_LOG(LogLambdaSource, Verbose, TEXT("Particle file '%s': %d definitions (%d new)"), *RelativePath, ByElement.Num(), NumAdded);
	return ByElement.Num();
}

void FSourceParticleLibrary::ParseOperators(const FSourceDMXFile& File, const FSourceDMXElement& Element, const TCHAR* ArrayName,
	TArray<FSourceParticleOperatorDef>& Out)
{
	TArray<int32> Indices;
	Element.GetElementIndices(ArrayName, Indices);
	for (int32 Index : Indices)
	{
		const FSourceDMXElement* Op = File.GetElement(Index);
		if (!Op)
		{
			continue;
		}
		FSourceParticleOperatorDef Def;
		// functionName names the operator; the element's own name is only a fallback for hand-made files.
		Def.FunctionName = FSourceParticleDefinition::CanonicalOperatorName(Op->GetString(TEXT("functionName"), Op->Name));
		Def.Element = Op;
		Out.Add(MoveTemp(Def));
	}
}

TSharedPtr<FSourceParticleDefinition> FSourceParticleLibrary::ParseDefinition(const TSharedPtr<FSourceDMXFile>& File, int32 ElementIndex,
	TMap<int32, TSharedPtr<FSourceParticleDefinition>>& ByElement, const FString& SourceFile)
{
	if (const TSharedPtr<FSourceParticleDefinition>* Known = ByElement.Find(ElementIndex))
	{
		return *Known;
	}
	const FSourceDMXElement* E = File->GetElement(ElementIndex);
	if (!E || !E->Type.Equals(TEXT("DmeParticleSystemDefinition"), ESearchCase::IgnoreCase))
	{
		return nullptr;
	}

	TSharedPtr<FSourceParticleDefinition> Def = MakeShared<FSourceParticleDefinition>();
	// Registered before the children are walked, so a child link back to an ancestor ends here.
	ByElement.Add(ElementIndex, Def);

	Def->File = File;
	Def->SourceFile = SourceFile;
	Def->Name = E->Name;
	Def->Material = ULambdaMaterialLibrary::NormalizeMaterialName(E->GetString(TEXT("material"), TEXT("vgui/white")));
	Def->MaxParticles = FMath::Clamp(E->GetInt(TEXT("max_particles"), 1000), 1, 5000);	// MAX_PARTICLES_IN_A_SYSTEM
	Def->InitialParticles = FMath::Max(0, E->GetInt(TEXT("initial_particles"), 0));
	Def->BoundingBoxMin = E->GetVector3(TEXT("bounding_box_min"), FVector3f(-10, -10, -10));
	Def->BoundingBoxMax = E->GetVector3(TEXT("bounding_box_max"), FVector3f(10, 10, 10));
	Def->Color = E->GetColor(TEXT("color"), FColor::White);
	Def->Radius = E->GetFloat(TEXT("radius"), 5.0f);
	Def->Rotation = E->GetFloat(TEXT("rotation"), 0.0f);
	Def->RotationSpeed = E->GetFloat(TEXT("rotation_speed"), 0.0f);
	Def->Normal = E->GetVector3(TEXT("normal"), FVector3f(0, 0, 1));
	Def->SequenceNumber = E->GetInt(TEXT("sequence_number"), 0);
	Def->SequenceNumber1 = E->GetInt(TEXT("sequence_number 1"), 0);
	Def->CullRadius = E->GetFloat(TEXT("cull_radius"), 0.0f);
	Def->MaximumDrawDistance = E->GetFloat(TEXT("maximum draw distance"), 100000.0f);
	Def->TimeToSleepWhenNotDrawn = E->GetFloat(TEXT("time to sleep when not drawn"), 8.0f);
	Def->bSortParticles = E->GetBool(TEXT("Sort particles"), true);
	Def->MaximumTimeStep = E->GetFloat(TEXT("maximum time step"), 0.1f);
	Def->MinimumSimulationTimeStep = E->GetFloat(TEXT("minimum simulation time step"), 0.0f);
	Def->FreezeSimulationAfterTime = E->GetFloat(TEXT("freeze simulation after time"), 1000000000.0f);
	Def->bViewModelEffect = E->GetBool(TEXT("view model effect"), false);
	Def->bScreenSpaceEffect = E->GetBool(TEXT("screen space effect"), false);
	Def->GroupId = E->GetInt(TEXT("group id"), 0);
	Def->ControlPointToDisableRenderingIfCamera = E->GetInt(TEXT("control point to disable rendering if it is the camera"), -1);
	Def->ControlPointToOnlyEnableRenderingIfCamera = E->GetInt(TEXT("control point to only enable rendering if it is the camera"), -1);

	ParseOperators(*File, *E, TEXT("renderers"), Def->Renderers);
	ParseOperators(*File, *E, TEXT("operators"), Def->Operators);
	ParseOperators(*File, *E, TEXT("initializers"), Def->Initializers);
	ParseOperators(*File, *E, TEXT("emitters"), Def->Emitters);
	ParseOperators(*File, *E, TEXT("forces"), Def->Forces);
	ParseOperators(*File, *E, TEXT("constraints"), Def->Constraints);

	TArray<int32> ChildLinks;
	E->GetElementIndices(TEXT("children"), ChildLinks);
	for (int32 LinkIndex : ChildLinks)
	{
		const FSourceDMXElement* Link = File->GetElement(LinkIndex);
		if (!Link)
		{
			continue;
		}
		const int32 ChildElement = Link->GetElementIndex(TEXT("child"));
		TSharedPtr<FSourceParticleDefinition> ChildDef = ParseDefinition(File, ChildElement, ByElement, SourceFile);
		if (!ChildDef)
		{
			continue;
		}
		FSourceParticleChildDef Child;
		Child.Name = ChildDef->Name;
		Child.Definition = ChildDef;
		Child.Delay = Link->GetFloat(TEXT("delay"), 0.0f);
		Child.bEndCapEffect = Link->GetBool(TEXT("end cap effect"), false);
		Def->Children.Add(MoveTemp(Child));
	}
	return Def;
}

TSharedPtr<const FSourceParticleDefinition> FSourceParticleLibrary::Find(const FString& Name)
{
	Initialize();
	if (const TSharedPtr<FSourceParticleDefinition>* Found = Definitions.Find(Name.ToLower()))
	{
		return *Found;
	}
	return nullptr;
}

void FSourceParticleLibrary::GetDefinitionNames(TArray<FString>& OutNames) const
{
	for (const TPair<FString, TSharedPtr<FSourceParticleDefinition>>& Pair : Definitions)
	{
		OutNames.Add(Pair.Value->Name);
	}
	OutNames.Sort();
}
