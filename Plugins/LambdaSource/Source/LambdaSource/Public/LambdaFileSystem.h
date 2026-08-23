#pragma once

#include "CoreMinimal.h"

class FSourceVPKFile;

/**
 * Source-style virtual file system: resolves relative content paths ("maps/test.bsp", "materials/dev/x.vmt") against an
 * ordered list of mount points. A mount point is either a loose directory on disk or a mounted VPK archive.
 *
 * The mount list comes from the game directory's gameinfo.txt (FileSystem/SearchPaths), exactly like Source. The game
 * directory is located in this order:
 *   1. -gamedir=<path> on the command line
 *   2. <root>/Game/<mod> (or <root>/<mod>) where <root> is <ExecutableDir>/../../.. when packaged and
 *      <ProjectDir>/../game in the editor; any subfolder holding a gameinfo.txt is accepted
 *   3. ULambdaSourceSettings::GameDirectories
 *
 * Each SearchPath value ending in ".vpk" mounts an archive, anything else mounts a loose directory. "|gameinfo_path|"
 * expands to the game directory. If gameinfo.txt is missing, the game directory itself is mounted plus any VPKs listed
 * in ULambdaSourceSettings::MountedVPKs (legacy fallback).
 */
class LAMBDASOURCE_API FLambdaFileSystem
{
public:
	static FLambdaFileSystem& Get();

	/** Rebuilds the mount list from settings + command line. Called lazily on first use; call again to remount. */
	void Reinitialize();

	/** Human-readable description of each mount, for logging. */
	TArray<FString> GetMountDescriptions() const;

	/** True if the path exists in any mount. */
	bool FileExists(const FString& RelativePath) const;
	/** Reads a file's bytes from the first mount that has it (loose file or VPK entry). */
	bool ReadFile(const FString& RelativePath, TArray<uint8>& OutData) const;
	bool ReadFileToString(const FString& RelativePath, FString& OutText) const;

	/** For loose files only: the absolute path of the first match. VPK-backed files return false (no disk path). */
	bool ResolveLoose(const FString& RelativePath, FString& OutAbsolutePath) const;

	/** Lists files matching a wildcard inside a relative directory across all mounts (e.g. "maps", "*.bsp"). */
	void FindFiles(const FString& RelativeDirectory, const FString& Wildcard, TArray<FString>& OutRelativePaths) const;

	/** Normalises separators, strips leading "./" and "/", collapses duplicate slashes. Case is preserved. */
	static FString NormalizeRelativePath(const FString& InPath);
	/** Value of -gamedir=<path> on the command line, or empty. */
	static FString GetCommandLineGameDir();

	/** Absolute path of the resolved game directory (the folder holding gameinfo.txt), or empty before init. */
	const FString& GetGameDirectory() const { return GameDirectory; }
	/** Display name from gameinfo.txt ("game" key), or empty. */
	const FString& GetGameName() const { return GameName; }

private:
	FLambdaFileSystem() = default;

	enum class EMountType : uint8 { Directory, VPK };
	struct FMount
	{
		EMountType Type = EMountType::Directory;
		FString AbsoluteDir;					// for Directory mounts
		TSharedPtr<FSourceVPKFile> VPK;			// for VPK mounts
	};

	static FString MakeAbsoluteDirectory(const FString& Dir);
	void AddDirectoryMount(const FString& Dir);
	void AddVPKMount(const FString& VpkPath);

	/** Finds the folder that should hold gameinfo.txt. */
	static FString ResolveGameDirectory();
	/** Reads gameinfo.txt in GameDirectory and mounts its SearchPaths. Returns false if it is missing/unparsable. */
	bool MountFromGameInfo();

	TArray<FMount> Mounts;
	FString GameDirectory;
	FString GameName;
	bool bInitialized = false;
};
