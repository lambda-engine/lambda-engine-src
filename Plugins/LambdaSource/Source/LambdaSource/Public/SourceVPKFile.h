#pragma once

#include "CoreMinimal.h"

class FArchive;

/**
 * Reader for Valve Pak (VPK) archives, versions 1 and 2, single- and multi-chunk (the "_dir.vpk" + "_NNN.vpk" layout
 * used by HL2 / Source SDK 2013). Read-only: parses the directory tree up front, then streams file data on demand from
 * the appropriate chunk archive (only the requested byte range is read, so the multi-hundred-MB chunk files are never
 * loaded whole).
 *
 * Paths are stored and matched lower-case with forward slashes, e.g. "materials/dev/dev_measuregeneric01.vtf".
 */
class LAMBDASOURCE_API FSourceVPKFile
{
public:
	static constexpr uint32 VPK_SIGNATURE = 0x55AA1234u;

	/** One chunk of a file's data inside an archive (files are almost always a single chunk). */
	struct FChunk
	{
		uint16 ArchiveIndex = 0;	// 0x7FFF => stored in the _dir.vpk itself (after header+tree)
		uint32 Offset = 0;
		uint32 Length = 0;
	};

	struct FEntry
	{
		uint32 Crc32 = 0;
		TArray<uint8> Preload;		// "small data" stored inline in the directory tree
		TArray<FChunk> Chunks;

		int64 TotalSize() const
		{
			int64 Size = Preload.Num();
			for (const FChunk& C : Chunks) { Size += C.Length; }
			return Size;
		}
	};

	/** Opens and parses <name>_dir.vpk (or <name>.vpk for single-file archives). */
	bool Load(const FString& DirVpkPath, FString* OutError = nullptr);

	bool IsLoaded() const { return bLoaded; }
	const FString& GetPath() const { return DirVpkPath; }
	uint32 GetVersion() const { return Version; }
	bool IsMultiChunk() const { return bMultiChunk; }
	int32 GetNumFiles() const { return Entries.Num(); }

	/** relPath is normalised (lower-case, forward slashes). */
	bool Contains(const FString& RelPath) const { return Entries.Contains(NormalizeKey(RelPath)); }
	const FEntry* Find(const FString& RelPath) const { return Entries.Find(NormalizeKey(RelPath)); }

	/** Reads and assembles a file's bytes. Returns false if the entry is missing or a chunk read fails. */
	bool ReadFile(const FString& RelPath, TArray<uint8>& OutData) const;

	/** Appends every stored relative path (optionally only those under DirPrefix and matching a "*.ext" wildcard). */
	void EnumerateFiles(TArray<FString>& OutRelPaths, const FString& DirPrefix = FString(), const FString& Wildcard = FString()) const;

	static FString NormalizeKey(const FString& InPath);

private:
	/** Absolute path of the chunk archive holding a given chunk (0x7FFF -> the dir/single file). */
	FString GetChunkArchivePath(uint16 ArchiveIndex) const;

	TMap<FString, FEntry> Entries;
	FString DirVpkPath;		// full path to the _dir.vpk (or .vpk)
	FString DirName;		// containing folder, with trailing slash
	FString BaseName;		// archive name without "_dir" and without extension
	uint32 Version = 0;
	uint32 TreeSize = 0;
	uint32 HeaderSize = 0;
	bool bMultiChunk = false;
	bool bLoaded = false;
};
