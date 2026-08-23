#include "LambdaFileSystem.h"
#include "LambdaSourceModule.h"
#include "LambdaSourceSettings.h"
#include "SourceKeyValues.h"
#include "SourceVPKFile.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

FLambdaFileSystem& FLambdaFileSystem::Get()
{
	static FLambdaFileSystem Instance;
	if (!Instance.bInitialized)
	{
		Instance.Reinitialize();
	}
	return Instance;
}

FString FLambdaFileSystem::GetCommandLineGameDir()
{
	FString Value;
	if (FParse::Value(FCommandLine::Get(), TEXT("gamedir="), Value))
	{
		Value.TrimQuotesInline();
		return Value;
	}
	return FString();
}

FString FLambdaFileSystem::MakeAbsoluteDirectory(const FString& Dir)
{
	FString Result = Dir;
	FPaths::NormalizeDirectoryName(Result);
	if (FPaths::IsRelative(Result))
	{
		Result = FPaths::Combine(FPaths::ProjectDir(), Result);
	}
	Result = FPaths::ConvertRelativePathToFull(Result);
	FPaths::NormalizeDirectoryName(Result);
	return Result;
}

void FLambdaFileSystem::AddDirectoryMount(const FString& Dir)
{
	if (Dir.IsEmpty())
	{
		return;
	}
	const FString Abs = MakeAbsoluteDirectory(Dir);
	for (const FMount& M : Mounts)
	{
		if (M.Type == EMountType::Directory && M.AbsoluteDir == Abs)
		{
			return; // dedupe
		}
	}
	FMount Mount;
	Mount.Type = EMountType::Directory;
	Mount.AbsoluteDir = Abs;
	Mounts.Add(MoveTemp(Mount));
}

void FLambdaFileSystem::AddVPKMount(const FString& VpkPath)
{
	if (VpkPath.IsEmpty())
	{
		return;
	}
	FString FullPath = VpkPath;
	if (FPaths::IsRelative(FullPath))
	{
		FullPath = FPaths::Combine(FPaths::ProjectDir(), FullPath);
	}
	FullPath = FPaths::ConvertRelativePathToFull(FullPath);

	TSharedPtr<FSourceVPKFile> VPK = MakeShared<FSourceVPKFile>();
	FString Error;
	if (!VPK->Load(FullPath, &Error))
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("Could not mount VPK '%s': %s"), *FullPath, *Error);
		return;
	}
	FMount Mount;
	Mount.Type = EMountType::VPK;
	Mount.VPK = VPK;
	Mounts.Add(MoveTemp(Mount));
}

FString FLambdaFileSystem::ResolveGameDirectory()
{
	IFileManager& FM = IFileManager::Get();

	auto HasGameInfo = [&FM](const FString& Dir)
	{
		return !Dir.IsEmpty() && FM.FileExists(*(Dir / TEXT("gameinfo.txt")));
	};

	// 1. Explicit command-line override wins, whether or not it has a gameinfo.txt.
	const FString CmdLineDir = GetCommandLineGameDir();
	if (!CmdLineDir.IsEmpty())
	{
		return MakeAbsoluteDirectory(CmdLineDir);
	}

	// 2. Conventional locations, relative to the roots below. Packaged builds run from <root>/<Project>/Binaries/Win64,
	//    so <root> is three levels up; in the editor the game tree sits next to the project (../game).
	TArray<FString> Roots;
	Roots.Add(MakeAbsoluteDirectory(FPaths::Combine(FPlatformProcess::BaseDir(), TEXT("../../.."))));
	Roots.Add(MakeAbsoluteDirectory(FPaths::Combine(FPlatformProcess::BaseDir(), TEXT("../../../.."))));
	Roots.Add(MakeAbsoluteDirectory(TEXT("../game")));
	Roots.Add(MakeAbsoluteDirectory(TEXT(".")));

	// Named candidates, in priority order. These are also the fallback if no gameinfo.txt turns up anywhere.
	TArray<FString> Named;
	for (const FString& Root : Roots)
	{
		Named.AddUnique(Root / TEXT("Game/lambda"));	// current layout: <root>/Game/<mod>
		Named.AddUnique(Root / TEXT("lambda"));			// flat layout:    <root>/<mod>
	}
	const ULambdaSourceSettings& Settings = ULambdaSourceSettings::Get();
	for (const FString& Dir : Settings.GameDirectories)
	{
		Named.AddUnique(MakeAbsoluteDirectory(Dir));
	}

	for (const FString& Candidate : Named)
	{
		if (HasGameInfo(Candidate))
		{
			return Candidate;
		}
	}

	// The mod folder can be named anything, so as a second pass accept any subfolder that actually has a gameinfo.txt.
	for (const FString& Root : Roots)
	{
		const FString ContainerDirs[] = { Root / TEXT("Game"), Root };
		for (const FString& Container : ContainerDirs)
		{
			TArray<FString> SubDirs;
			FM.FindFiles(SubDirs, *(Container / TEXT("*")), false, true);
			for (const FString& SubDir : SubDirs)
			{
				const FString Candidate = Container / SubDir;
				if (HasGameInfo(Candidate))
				{
					return Candidate;
				}
			}
		}
	}

	// Nothing has a gameinfo.txt: fall back to the first named location that at least exists.
	for (const FString& Candidate : Named)
	{
		if (FM.DirectoryExists(*Candidate))
		{
			return Candidate;
		}
	}
	return Named.Num() > 0 ? Named[0] : FString();
}

bool FLambdaFileSystem::MountFromGameInfo()
{
	const FString GameInfoPath = GameDirectory / TEXT("gameinfo.txt");
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *GameInfoPath))
	{
		return false;
	}

	FSourceKeyValues Root;
	FString Error;
	if (!FSourceKeyValues::ParseSingle(Bytes, Root, &Error))
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("Could not parse '%s': %s"), *GameInfoPath, *Error);
		return false;
	}

	GameName = Root.GetString(TEXT("game"));

	const FSourceKeyValues* FileSystem = Root.FindChild(TEXT("FileSystem"));
	const FSourceKeyValues* SearchPaths = FileSystem ? FileSystem->FindChild(TEXT("SearchPaths")) : nullptr;
	if (!SearchPaths)
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("'%s' has no FileSystem/SearchPaths block"), *GameInfoPath);
		return false;
	}

	// Each child is <pathType> <value>; we mount every entry in declared order regardless of the type token.
	for (const FSourceKeyValues& Entry : SearchPaths->Children)
	{
		if (Entry.IsSection() || Entry.Value.IsEmpty())
		{
			continue;
		}

		FString Value = Entry.Value;
		Value.ReplaceInline(TEXT("|gameinfo_path|"), *(GameDirectory + TEXT("/")));
		Value.ReplaceInline(TEXT("|all_source_engine_paths|"), TEXT(""));
		Value.ReplaceInline(TEXT("\\"), TEXT("/"));
		Value.TrimStartAndEndInline();

		// A trailing "/." (Source's way of writing "this folder") would otherwise become a literal path component.
		if (Value.EndsWith(TEXT("/.")))
		{
			Value.LeftChopInline(2);
		}
		if (Value.IsEmpty())
		{
			continue;
		}
		// Resolve relative entries against the game directory, matching Source's behaviour.
		if (FPaths::IsRelative(Value))
		{
			Value = GameDirectory / Value;
		}

		if (Value.EndsWith(TEXT(".vpk"), ESearchCase::IgnoreCase))
		{
			AddVPKMount(Value);
		}
		else
		{
			AddDirectoryMount(Value);
		}
	}
	return Mounts.Num() > 0;
}

void FLambdaFileSystem::Reinitialize()
{
	Mounts.Reset();
	GameName.Reset();

	GameDirectory = ResolveGameDirectory();
	UE_LOG(LogLambdaSource, Log, TEXT("Game directory: %s"), *GameDirectory);

	if (!MountFromGameInfo())
	{
		// No usable gameinfo.txt: mount the game directory itself, then any VPKs configured in project settings.
		UE_LOG(LogLambdaSource, Warning, TEXT("No usable gameinfo.txt in '%s' - falling back to project settings"), *GameDirectory);
		AddDirectoryMount(GameDirectory);

		const ULambdaSourceSettings& Settings = ULambdaSourceSettings::Get();
		for (const FString& Dir : Settings.GameDirectories)
		{
			AddDirectoryMount(Dir);
		}
		for (const FString& Vpk : Settings.MountedVPKs)
		{
			AddVPKMount(Vpk);
		}
	}

	bInitialized = true;

	if (!GameName.IsEmpty())
	{
		UE_LOG(LogLambdaSource, Log, TEXT("Game: %s"), *GameName);
	}
	for (const FString& Desc : GetMountDescriptions())
	{
		UE_LOG(LogLambdaSource, Log, TEXT("Mount: %s"), *Desc);
	}
}

TArray<FString> FLambdaFileSystem::GetMountDescriptions() const
{
	TArray<FString> Out;
	for (const FMount& M : Mounts)
	{
		if (M.Type == EMountType::Directory)
		{
			const bool bExists = IFileManager::Get().DirectoryExists(*M.AbsoluteDir);
			Out.Add(FString::Printf(TEXT("[dir] %s%s"), *M.AbsoluteDir, bExists ? TEXT("") : TEXT("  (missing)")));
		}
		else if (M.VPK.IsValid())
		{
			Out.Add(FString::Printf(TEXT("[vpk] %s (%d files)"), *M.VPK->GetPath(), M.VPK->GetNumFiles()));
		}
	}
	return Out;
}

FString FLambdaFileSystem::NormalizeRelativePath(const FString& InPath)
{
	FString Path = InPath;
	Path.ReplaceInline(TEXT("\\"), TEXT("/"));
	Path.TrimStartAndEndInline();
	while (Path.StartsWith(TEXT("./")))
	{
		Path.RightChopInline(2);
	}
	while (Path.StartsWith(TEXT("/")))
	{
		Path.RightChopInline(1);
	}
	while (Path.Contains(TEXT("//")))
	{
		Path.ReplaceInline(TEXT("//"), TEXT("/"));
	}
	return Path;
}

bool FLambdaFileSystem::ResolveLoose(const FString& RelativePath, FString& OutAbsolutePath) const
{
	const FString Rel = NormalizeRelativePath(RelativePath);
	if (Rel.IsEmpty())
	{
		return false;
	}
	IFileManager& FM = IFileManager::Get();
	for (const FMount& M : Mounts)
	{
		if (M.Type != EMountType::Directory)
		{
			continue;
		}
		const FString Candidate = M.AbsoluteDir / Rel;
		if (FM.FileExists(*Candidate))
		{
			OutAbsolutePath = Candidate;
			return true;
		}
		const FString Lower = M.AbsoluteDir / Rel.ToLower();
		if (Lower != Candidate && FM.FileExists(*Lower))
		{
			OutAbsolutePath = Lower;
			return true;
		}
	}
	return false;
}

bool FLambdaFileSystem::FileExists(const FString& RelativePath) const
{
	const FString Rel = NormalizeRelativePath(RelativePath);
	if (Rel.IsEmpty())
	{
		return false;
	}
	FString Dummy;
	if (ResolveLoose(Rel, Dummy))
	{
		return true;
	}
	for (const FMount& M : Mounts)
	{
		if (M.Type == EMountType::VPK && M.VPK.IsValid() && M.VPK->Contains(Rel))
		{
			return true;
		}
	}
	return false;
}

bool FLambdaFileSystem::ReadFile(const FString& RelativePath, TArray<uint8>& OutData) const
{
	const FString Rel = NormalizeRelativePath(RelativePath);
	if (Rel.IsEmpty())
	{
		return false;
	}
	IFileManager& FM = IFileManager::Get();
	for (const FMount& M : Mounts)
	{
		if (M.Type == EMountType::Directory)
		{
			FString Candidate = M.AbsoluteDir / Rel;
			if (!FM.FileExists(*Candidate))
			{
				const FString Lower = M.AbsoluteDir / Rel.ToLower();
				Candidate = (Lower != Candidate && FM.FileExists(*Lower)) ? Lower : FString();
			}
			if (!Candidate.IsEmpty() && FFileHelper::LoadFileToArray(OutData, *Candidate))
			{
				return true;
			}
		}
		else if (M.VPK.IsValid())
		{
			if (M.VPK->ReadFile(Rel, OutData))
			{
				return true;
			}
		}
	}
	return false;
}

bool FLambdaFileSystem::ReadFileToString(const FString& RelativePath, FString& OutText) const
{
	TArray<uint8> Bytes;
	if (!ReadFile(RelativePath, Bytes))
	{
		return false;
	}
	FFileHelper::BufferToString(OutText, Bytes.GetData(), Bytes.Num());
	return true;
}

void FLambdaFileSystem::FindFiles(const FString& RelativeDirectory, const FString& Wildcard, TArray<FString>& OutRelativePaths) const
{
	const FString RelDir = NormalizeRelativePath(RelativeDirectory);
	TSet<FString> Seen;
	auto AddUnique = [&](const FString& Rel)
	{
		if (!Seen.Contains(Rel.ToLower()))
		{
			Seen.Add(Rel.ToLower());
			OutRelativePaths.Add(Rel);
		}
	};

	for (const FMount& M : Mounts)
	{
		if (M.Type == EMountType::Directory)
		{
			const FString Dir = RelDir.IsEmpty() ? M.AbsoluteDir : M.AbsoluteDir / RelDir;
			TArray<FString> Found;
			IFileManager::Get().FindFiles(Found, *(Dir / Wildcard), true, false);
			for (const FString& File : Found)
			{
				AddUnique(RelDir.IsEmpty() ? File : RelDir / File);
			}
		}
		else if (M.VPK.IsValid())
		{
			TArray<FString> Found;
			M.VPK->EnumerateFiles(Found, RelDir, Wildcard);
			for (const FString& Rel : Found)
			{
				// EnumerateFiles returns the full relative path; keep just the leaf-relative form for consistency.
				AddUnique(RelDir.IsEmpty() ? Rel : Rel.RightChop(RelDir.Len() + 1));
			}
		}
	}
}
