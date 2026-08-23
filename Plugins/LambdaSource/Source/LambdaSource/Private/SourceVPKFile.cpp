#include "SourceVPKFile.h"
#include "LambdaSourceModule.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	/** Reads a null-terminated UTF-8 string from a byte buffer starting at Pos; advances Pos past the terminator. */
	bool ReadCString(const TArray<uint8>& Data, int64& Pos, FString& Out)
	{
		const int64 Start = Pos;
		while (Pos < Data.Num() && Data[Pos] != 0)
		{
			++Pos;
		}
		if (Pos >= Data.Num())
		{
			return false; // unterminated
		}
		if (Pos == Start)
		{
			Out.Reset();
		}
		else
		{
			// The region is null-terminated within Data (we stopped at the 0 byte), so a C-string conversion is safe.
			Out = FString(StringCast<TCHAR>((const UTF8CHAR*)(Data.GetData() + Start)).Get());
		}
		++Pos; // skip terminator
		return true;
	}

	template <typename T>
	bool ReadPod(const TArray<uint8>& Data, int64& Pos, T& Out)
	{
		if (Pos + (int64)sizeof(T) > Data.Num())
		{
			return false;
		}
		FMemory::Memcpy(&Out, Data.GetData() + Pos, sizeof(T));
		Pos += sizeof(T);
		return true;
	}
}

FString FSourceVPKFile::NormalizeKey(const FString& InPath)
{
	FString Key = InPath;
	Key.ReplaceInline(TEXT("\\"), TEXT("/"));
	Key.TrimStartAndEndInline();
	while (Key.StartsWith(TEXT("/")))
	{
		Key.RightChopInline(1);
	}
	return Key.ToLower();
}

bool FSourceVPKFile::Load(const FString& InDirVpkPath, FString* OutError)
{
	bLoaded = false;
	Entries.Reset();

	auto Fail = [&](const FString& Msg)
	{
		if (OutError) { *OutError = Msg; }
		return false;
	};

	DirVpkPath = InDirVpkPath;
	FPaths::NormalizeFilename(DirVpkPath);
	DirName = FPaths::GetPath(DirVpkPath) + TEXT("/");
	BaseName = FPaths::GetBaseFilename(DirVpkPath); // e.g. "hl2_textures_dir" or "pak01_dir" or "single"
	bMultiChunk = false;
	if (BaseName.EndsWith(TEXT("_dir"), ESearchCase::IgnoreCase))
	{
		BaseName.LeftChopInline(4);
		bMultiChunk = true;
	}

	// The directory tree (and header) live in the _dir file, which is small; read it whole.
	TArray<uint8> Data;
	if (!FFileHelper::LoadFileToArray(Data, *DirVpkPath))
	{
		return Fail(FString::Printf(TEXT("Could not open VPK '%s'"), *DirVpkPath));
	}

	int64 Pos = 0;
	uint32 Signature = 0;
	if (!ReadPod(Data, Pos, Signature) || Signature != VPK_SIGNATURE)
	{
		return Fail(FString::Printf(TEXT("'%s' is not a VPK (bad signature)"), *DirVpkPath));
	}
	if (!ReadPod(Data, Pos, Version) || !ReadPod(Data, Pos, TreeSize))
	{
		return Fail(TEXT("Truncated VPK header"));
	}
	if (Version == 1)
	{
		// nothing further
	}
	else if (Version == 2)
	{
		uint32 FileDataSectionSize = 0, ArchiveMD5 = 0, OtherMD5 = 0, SignatureSize = 0;
		if (!ReadPod(Data, Pos, FileDataSectionSize) || !ReadPod(Data, Pos, ArchiveMD5) ||
			!ReadPod(Data, Pos, OtherMD5) || !ReadPod(Data, Pos, SignatureSize))
		{
			return Fail(TEXT("Truncated VPK v2 header"));
		}
	}
	else
	{
		return Fail(FString::Printf(TEXT("Unsupported VPK version %u in '%s'"), Version, *DirVpkPath));
	}
	HeaderSize = (uint32)Pos;

	const int64 TreeEnd = (int64)HeaderSize + TreeSize;
	if (TreeEnd > Data.Num())
	{
		return Fail(TEXT("VPK tree extends past end of dir file"));
	}

	// Parse the nested extension / directory / filename tree.
	for (;;)
	{
		FString Extension;
		if (!ReadCString(Data, Pos, Extension) || Pos > TreeEnd)
		{
			break;
		}
		if (Extension.IsEmpty())
		{
			break; // end of tree
		}
		Extension = Extension.TrimStartAndEnd();

		for (;;)
		{
			FString Directory;
			if (!ReadCString(Data, Pos, Directory))
			{
				return Fail(TEXT("Truncated VPK tree (directory)"));
			}
			if (Directory.IsEmpty())
			{
				break; // end of this extension
			}
			Directory.ReplaceInline(TEXT("%20"), TEXT(" "));
			Directory = Directory.TrimStartAndEnd();
			if (Directory == TEXT(" "))
			{
				Directory.Reset(); // root
			}

			for (;;)
			{
				FString Filename;
				if (!ReadCString(Data, Pos, Filename))
				{
					return Fail(TEXT("Truncated VPK tree (filename)"));
				}
				if (Filename.IsEmpty())
				{
					break; // end of this directory
				}
				Filename.ReplaceInline(TEXT("%20"), TEXT(" "));
				Filename = Filename.TrimStartAndEnd();

				FEntry Entry;
				uint16 PreloadSize = 0;
				if (!ReadPod(Data, Pos, Entry.Crc32) || !ReadPod(Data, Pos, PreloadSize))
				{
					return Fail(TEXT("Truncated VPK entry header"));
				}

				// One or more (archiveIndex, offset, length) chunks, terminated by archiveIndex == 0xFFFF.
				for (;;)
				{
					uint16 ArchiveIndex = 0;
					if (!ReadPod(Data, Pos, ArchiveIndex))
					{
						return Fail(TEXT("Truncated VPK chunk list"));
					}
					if (ArchiveIndex == 0xFFFF)
					{
						break;
					}
					FChunk Chunk;
					Chunk.ArchiveIndex = ArchiveIndex;
					if (!ReadPod(Data, Pos, Chunk.Offset) || !ReadPod(Data, Pos, Chunk.Length))
					{
						return Fail(TEXT("Truncated VPK chunk"));
					}
					Entry.Chunks.Add(Chunk);
				}

				// Inline preload bytes follow the chunk list.
				if (PreloadSize > 0)
				{
					if (Pos + PreloadSize > Data.Num())
					{
						return Fail(TEXT("Truncated VPK preload data"));
					}
					Entry.Preload.Append(Data.GetData() + Pos, PreloadSize);
					Pos += PreloadSize;
				}

				FString RelPath = Directory.IsEmpty()
					? FString::Printf(TEXT("%s.%s"), *Filename, *Extension)
					: FString::Printf(TEXT("%s/%s.%s"), *Directory, *Filename, *Extension);
				Entries.Add(NormalizeKey(RelPath), MoveTemp(Entry));
			}
		}
	}

	bLoaded = true;
	UE_LOG(LogLambdaSource, Log, TEXT("Mounted VPK '%s' (v%u, %s, %d files)"), *DirVpkPath, Version,
		bMultiChunk ? TEXT("multi-chunk") : TEXT("single"), Entries.Num());
	return true;
}

FString FSourceVPKFile::GetChunkArchivePath(uint16 ArchiveIndex) const
{
	if (ArchiveIndex == 0x7FFF)
	{
		return DirVpkPath; // data embedded in the dir/single file
	}
	return FString::Printf(TEXT("%s%s_%03d.vpk"), *DirName, *BaseName, (int32)ArchiveIndex);
}

bool FSourceVPKFile::ReadFile(const FString& RelPath, TArray<uint8>& OutData) const
{
	const FEntry* Entry = Find(RelPath);
	if (!Entry)
	{
		return false;
	}

	OutData.Reset();
	OutData.Reserve((int32)Entry->TotalSize());
	OutData.Append(Entry->Preload);

	for (const FChunk& Chunk : Entry->Chunks)
	{
		if (Chunk.Length == 0)
		{
			continue;
		}
		const FString ArchivePath = GetChunkArchivePath(Chunk.ArchiveIndex);
		TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*ArchivePath));
		if (!Reader)
		{
			UE_LOG(LogLambdaSource, Warning, TEXT("VPK '%s': could not open chunk archive '%s'"), *DirVpkPath, *ArchivePath);
			return false;
		}
		int64 ChunkOffset = Chunk.Offset;
		if (Chunk.ArchiveIndex == 0x7FFF)
		{
			ChunkOffset += (int64)HeaderSize + TreeSize; // relative to the data section of the dir/single file
		}
		if (ChunkOffset + Chunk.Length > Reader->TotalSize())
		{
			UE_LOG(LogLambdaSource, Warning, TEXT("VPK '%s': chunk for '%s' is out of range in '%s'"), *DirVpkPath, *RelPath, *ArchivePath);
			return false;
		}
		const int32 Base = OutData.Num();
		OutData.AddUninitialized((int32)Chunk.Length);
		Reader->Seek(ChunkOffset);
		Reader->Serialize(OutData.GetData() + Base, Chunk.Length);
		if (Reader->IsError())
		{
			UE_LOG(LogLambdaSource, Warning, TEXT("VPK '%s': read error for '%s' in '%s'"), *DirVpkPath, *RelPath, *ArchivePath);
			return false;
		}
	}
	return true;
}

void FSourceVPKFile::EnumerateFiles(TArray<FString>& OutRelPaths, const FString& DirPrefix, const FString& Wildcard) const
{
	const FString Prefix = DirPrefix.IsEmpty() ? FString() : (NormalizeKey(DirPrefix) + TEXT("/"));
	for (const TPair<FString, FEntry>& Pair : Entries)
	{
		if (!Prefix.IsEmpty() && !Pair.Key.StartsWith(Prefix))
		{
			continue;
		}
		if (!Wildcard.IsEmpty())
		{
			const FString Leaf = FPaths::GetCleanFilename(Pair.Key);
			if (!Leaf.MatchesWildcard(Wildcard))
			{
				continue;
			}
		}
		OutRelPaths.Add(Pair.Key);
	}
}
