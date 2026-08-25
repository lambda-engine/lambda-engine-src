#pragma once

#include "CoreMinimal.h"

/**
 * On-disk structures of the Source Engine BSP format (VBSP versions 19-21: HL2, Episodes, Source 2013, L4D...).
 * Mirrors public/bspfile.h and public/bspflags.h from the Source SDK.
 *
 * All structs use natural alignment exactly like the originals (Valve designed them without implicit padding except
 * dnode_t / dleaf_t which pad to 32 bytes). Sizes are static_asserted against the on-disk layout below.
 */
namespace SourceBSP
{
	constexpr int32 IDBSPHEADER = (('P' << 24) + ('S' << 16) + ('B' << 8) + 'V'); // little-endian "VBSP"
	constexpr int32 HEADER_LUMPS = 64;
	constexpr int32 MIN_SUPPORTED_VERSION = 19;
	constexpr int32 MAX_SUPPORTED_VERSION = 21;
	constexpr int32 MAXLIGHTMAPS = 4;
	constexpr int32 MAX_DISP_POWER = 4;

	enum ELump : int32
	{
		LUMP_ENTITIES = 0,
		LUMP_PLANES = 1,
		LUMP_TEXDATA = 2,
		LUMP_VERTEXES = 3,
		LUMP_VISIBILITY = 4,
		LUMP_NODES = 5,
		LUMP_TEXINFO = 6,
		LUMP_FACES = 7,
		LUMP_LIGHTING = 8,
		LUMP_OCCLUSION = 9,
		LUMP_LEAFS = 10,
		LUMP_FACEIDS = 11,
		LUMP_EDGES = 12,
		LUMP_SURFEDGES = 13,
		LUMP_MODELS = 14,
		LUMP_WORLDLIGHTS = 15,
		LUMP_LEAFFACES = 16,
		LUMP_LEAFBRUSHES = 17,
		LUMP_BRUSHES = 18,
		LUMP_BRUSHSIDES = 19,
		LUMP_AREAS = 20,
		LUMP_AREAPORTALS = 21,
		LUMP_PORTALS = 22,			// FACEBRUSHES in newer branches
		LUMP_CLUSTERS = 23,			// FACEBRUSHLIST in newer branches
		LUMP_PORTALVERTS = 24,
		LUMP_CLUSTERPORTALS = 25,
		LUMP_DISPINFO = 26,
		LUMP_ORIGINALFACES = 27,
		LUMP_PHYSDISP = 28,
		LUMP_PHYSCOLLIDE = 29,
		LUMP_VERTNORMALS = 30,
		LUMP_VERTNORMALINDICES = 31,
		LUMP_DISP_LIGHTMAP_ALPHAS = 32,
		LUMP_DISP_VERTS = 33,
		LUMP_DISP_LIGHTMAP_SAMPLE_POSITIONS = 34,
		LUMP_GAME_LUMP = 35,
		LUMP_LEAFWATERDATA = 36,
		LUMP_PRIMITIVES = 37,
		LUMP_PRIMVERTS = 38,
		LUMP_PRIMINDICES = 39,
		LUMP_PAKFILE = 40,
		LUMP_CLIPPORTALVERTS = 41,
		LUMP_CUBEMAPS = 42,
		LUMP_TEXDATA_STRING_DATA = 43,
		LUMP_TEXDATA_STRING_TABLE = 44,
		LUMP_OVERLAYS = 45,
		LUMP_LEAFMINDISTTOWATER = 46,
		LUMP_FACE_MACRO_TEXTURE_INFO = 47,
		LUMP_DISP_TRIS = 48,
		LUMP_PHYSCOLLIDESURFACE = 49,	// PROP_BLOB in newer branches
		LUMP_WATEROVERLAYS = 50,
		LUMP_LEAF_AMBIENT_INDEX_HDR = 51,
		LUMP_LEAF_AMBIENT_INDEX = 52,
		LUMP_LIGHTING_HDR = 53,
		LUMP_WORLDLIGHTS_HDR = 54,
		LUMP_LEAF_AMBIENT_LIGHTING_HDR = 55,
		LUMP_LEAF_AMBIENT_LIGHTING = 56,
		LUMP_XZIPPAKFILE = 57,
		LUMP_FACES_HDR = 58,
		LUMP_MAP_FLAGS = 59,
		LUMP_OVERLAY_FADES = 60,
		LUMP_OVERLAY_SYSTEM_LEVELS = 61,
		LUMP_PHYSLEVEL = 62,
		LUMP_DISP_MULTIBLEND = 63,
	};

	/** texinfo_t::flags (bspflags.h SURF_*) */
	enum ESurfFlags : int32
	{
		SURF_LIGHT = 0x0001,		// value will hold the light strength
		SURF_SKY2D = 0x0002,		// don't draw, indicates we should skylight + draw 2d sky but not draw the 3D skybox
		SURF_SKY = 0x0004,			// don't draw, but add to skybox
		SURF_WARP = 0x0008,			// turbulent water warp
		SURF_TRANS = 0x0010,
		SURF_NOPORTAL = 0x0020,		// the surface can not have a portal placed on it
		SURF_TRIGGER = 0x0040,		// xbox hack to work around elimination of trigger surfaces
		SURF_NODRAW = 0x0080,		// don't bother referencing the texture
		SURF_HINT = 0x0100,			// make a primary bsp splitter
		SURF_SKIP = 0x0200,			// completely ignore, allowing non-closed brushes
		SURF_NOLIGHT = 0x0400,		// don't calculate light
		SURF_BUMPLIGHT = 0x0800,	// calculate three lightmaps for the surface for bumpmapping
		SURF_NOSHADOWS = 0x1000,	// don't receive shadows
		SURF_NODECALS = 0x2000,		// don't receive decals
		SURF_NOCHOP = 0x4000,		// don't subdivide patches on this surface
		SURF_HITBOX = 0x8000,		// surface is part of a hitbox
	};

	/** dbrush_t::contents / dleaf_t::contents (bspflags.h CONTENTS_*) */
	enum EContents : int32
	{
		CONTENTS_EMPTY = 0,
		CONTENTS_SOLID = 0x1,
		CONTENTS_WINDOW = 0x2,
		CONTENTS_AUX = 0x4,
		CONTENTS_GRATE = 0x8,
		CONTENTS_SLIME = 0x10,
		CONTENTS_WATER = 0x20,
		CONTENTS_BLOCKLOS = 0x40,
		CONTENTS_OPAQUE = 0x80,
		CONTENTS_TESTFOGVOLUME = 0x100,
		CONTENTS_UNUSED = 0x200,
		CONTENTS_BLOCKLIGHT = 0x400,
		CONTENTS_TEAM1 = 0x800,
		CONTENTS_TEAM2 = 0x1000,
		CONTENTS_IGNORE_NODRAW_OPAQUE = 0x2000,
		CONTENTS_MOVEABLE = 0x4000,
		CONTENTS_AREAPORTAL = 0x8000,
		CONTENTS_PLAYERCLIP = 0x10000,
		CONTENTS_MONSTERCLIP = 0x20000,
		CONTENTS_CURRENT_0 = 0x40000,
		CONTENTS_CURRENT_90 = 0x80000,
		CONTENTS_CURRENT_180 = 0x100000,
		CONTENTS_CURRENT_270 = 0x200000,
		CONTENTS_CURRENT_UP = 0x400000,
		CONTENTS_CURRENT_DOWN = 0x800000,
		CONTENTS_ORIGIN = 0x1000000,
		CONTENTS_MONSTER = 0x2000000,
		CONTENTS_DEBRIS = 0x4000000,
		CONTENTS_DETAIL = 0x8000000,
		CONTENTS_TRANSLUCENT = 0x10000000,
		CONTENTS_LADDER = 0x20000000,
		CONTENTS_HITBOX = 0x40000000,
	};

	/** Plain 3-float vector as stored on disk (Source "Vector"). */
	struct FVec3
	{
		float x, y, z;
		FVector3f ToVector3f() const { return FVector3f(x, y, z); }
	};
	static_assert(sizeof(FVec3) == 12, "FVec3 size");

	struct lump_t
	{
		int32 fileofs;
		int32 filelen;
		int32 version;		// default to zero
		char fourCC[4];		// default to 0,0,0,0
	};
	static_assert(sizeof(lump_t) == 16, "lump_t size");

	struct dheader_t
	{
		int32 ident;
		int32 version;
		lump_t lumps[HEADER_LUMPS];
		int32 mapRevision;
	};
	static_assert(sizeof(dheader_t) == 1036, "dheader_t size");

	struct dgamelumpheader_t
	{
		int32 lumpCount;
		// dgamelump_t follow this
	};
	static_assert(sizeof(dgamelumpheader_t) == 4, "dgamelumpheader_t size");

	struct dgamelump_t
	{
		int32 id;			// four-CC
		uint16 flags;
		uint16 version;
		int32 fileofs;
		int32 filelen;
	};
	static_assert(sizeof(dgamelump_t) == 16, "dgamelump_t size");

	struct dmodel_t
	{
		FVec3 mins, maxs;
		FVec3 origin;					// for sounds or lights
		int32 headnode;
		int32 firstface, numfaces;		// submodels just draw faces without walking the bsp tree
	};
	static_assert(sizeof(dmodel_t) == 48, "dmodel_t size");

	struct dvertex_t
	{
		FVec3 point;
	};
	static_assert(sizeof(dvertex_t) == 12, "dvertex_t size");

	// planes (x&~1) and (x&~1)+1 are always opposites
	struct dplane_t
	{
		FVec3 normal;
		float dist;
		int32 type;			// PLANE_X - PLANE_ANYZ
	};
	static_assert(sizeof(dplane_t) == 20, "dplane_t size");

	struct dnode_t
	{
		int32 planenum;
		int32 children[2];	// negative numbers are -(leafs+1), not nodes
		int16 mins[3];		// for frustum culling
		int16 maxs[3];
		uint16 firstface;
		uint16 numfaces;	// counting both sides
		int16 area;			// if all leaves below this node are in the same area, then this is the area index; else -1
		// 2 bytes of implicit padding
	};
	static_assert(sizeof(dnode_t) == 32, "dnode_t size");

	struct texinfo_t
	{
		float textureVecsTexelsPerWorldUnits[2][4];		// [s/t][xyz offset]
		float lightmapVecsLuxelsPerWorldUnits[2][4];	// [s/t][xyz offset] - length is in units of texels/area
		int32 flags;			// SURF_* flags
		int32 texdata;			// index into LUMP_TEXDATA
	};
	static_assert(sizeof(texinfo_t) == 72, "texinfo_t size");

	struct dtexdata_t
	{
		FVec3 reflectivity;
		int32 nameStringTableID;		// index into LUMP_TEXDATA_STRING_TABLE
		int32 width, height;			// source image
		int32 view_width, view_height;
	};
	static_assert(sizeof(dtexdata_t) == 32, "dtexdata_t size");

	// note that edge 0 is never used, because negative edge nums are used for counterclockwise use of the edge in a face
	struct dedge_t
	{
		uint16 v[2];		// vertex numbers
	};
	static_assert(sizeof(dedge_t) == 4, "dedge_t size");

	struct dface_t
	{
		uint16 planenum;
		uint8 side;				// faces opposite to the node's plane direction
		uint8 onNode;			// 1 if on node, 0 if in leaf
		int32 firstedge;		// index into LUMP_SURFEDGES
		int16 numedges;
		int16 texinfo;
		int16 dispinfo;			// -1 if not a displacement
		int16 surfaceFogVolumeID;
		uint8 styles[MAXLIGHTMAPS];
		int32 lightofs;			// start of [numstyles*surfsize] samples
		float area;
		int32 m_LightmapTextureMinsInLuxels[2];
		int32 m_LightmapTextureSizeInLuxels[2];
		int32 origFace;			// reference the original face this face was derived from
		uint16 m_NumPrims;		// top bit set = dynamic shadows disabled
		uint16 firstPrimID;
		uint32 smoothingGroups;

		uint16 GetNumPrims() const { return m_NumPrims & 0x7FFF; }
	};
	static_assert(sizeof(dface_t) == 56, "dface_t size");

	// LUMP_LEAFS version 1 (Episode One and newer)
	struct dleaf_t
	{
		int32 contents;			// OR of all brushes
		int16 cluster;
		int16 areaFlags;		// area:9, flags:7
		int16 mins[3];			// for frustum culling
		int16 maxs[3];
		uint16 firstleafface;
		uint16 numleaffaces;
		uint16 firstleafbrush;
		uint16 numleafbrushes;
		int16 leafWaterDataID;	// -1 for not in water
		// 2 bytes of implicit padding

		int16 GetArea() const { return (int16)(areaFlags & 0x1FF); }
		int16 GetFlags() const { return (int16)((areaFlags >> 9) & 0x7F); }
	};
	static_assert(sizeof(dleaf_t) == 32, "dleaf_t size");

	// LUMP_LEAFS version 0 (original HL2 release) - has the ambient light cube embedded
	struct dleaf_v0_t
	{
		int32 contents;
		int16 cluster;
		int16 areaFlags;
		int16 mins[3];
		int16 maxs[3];
		uint16 firstleafface;
		uint16 numleaffaces;
		uint16 firstleafbrush;
		uint16 numleafbrushes;
		int16 leafWaterDataID;
		uint8 ambientLighting[24];	// CompressedLightCube: 6 x ColorRGBExp32
	};
	static_assert(sizeof(dleaf_v0_t) == 56, "dleaf_v0_t size");

	struct dbrushside_t
	{
		uint16 planenum;		// facing out of the leaf
		int16 texinfo;
		int16 dispinfo;			// displacement info
		uint8 bevel;			// is the side a bevel plane?
		uint8 thin;				// is a thin side? (0 in HL2-era BSPs; whole field was 'short bevel')
	};
	static_assert(sizeof(dbrushside_t) == 8, "dbrushside_t size");

	struct dbrush_t
	{
		int32 firstside;
		int32 numsides;
		int32 contents;
	};
	static_assert(sizeof(dbrush_t) == 12, "dbrush_t size");

	struct darea_t
	{
		int32 numareaportals;
		int32 firstareaportal;
	};
	static_assert(sizeof(darea_t) == 8, "darea_t size");

	struct dcubemapsample_t
	{
		int32 origin[3];	// position of light snapped to the nearest integer
		int32 size;			// resolution of cubemap, 0 - default
	};
	static_assert(sizeof(dcubemapsample_t) == 16, "dcubemapsample_t size");

	// ---- Displacements -------------------------------------------------------------------------------------------------

	struct CDispSubNeighbor
	{
		uint16 m_iNeighbor;				// 0xFFFF if there is no neighbor here
		uint8 m_NeighborOrientation;	// (CCW) rotation of the neighbor wrt this displacement
		uint8 m_Span;					// where the neighbor fits onto this side of our displacement
		uint8 m_NeighborSpan;			// where we fit onto our neighbor
		// 1 byte of implicit padding
	};
	static_assert(sizeof(CDispSubNeighbor) == 6, "CDispSubNeighbor size");

	struct CDispNeighbor
	{
		CDispSubNeighbor m_SubNeighbors[2];
	};
	static_assert(sizeof(CDispNeighbor) == 12, "CDispNeighbor size");

	struct CDispCornerNeighbors
	{
		uint16 m_Neighbors[4];	// indices of neighbors
		uint8 m_nNeighbors;
		// 1 byte of implicit padding
	};
	static_assert(sizeof(CDispCornerNeighbors) == 10, "CDispCornerNeighbors size");

	struct ddispinfo_t
	{
		FVec3 startPosition;				// start position used for orientation
		int32 m_iDispVertStart;				// index into LUMP_DISP_VERTS
		int32 m_iDispTriStart;				// index into LUMP_DISP_TRIS
		int32 power;						// power - indicates size of surface (2^power + 1)
		int32 minTess;						// minimum tesselation allowed
		float smoothingAngle;				// lighting smoothing angle
		int32 contents;						// surface contents
		uint16 m_iMapFace;					// which map face this displacement comes from
		// 2 bytes of implicit padding
		int32 m_iLightmapAlphaStart;
		int32 m_iLightmapSamplePositionStart;
		CDispNeighbor m_EdgeNeighbors[4];	// indexed by NEIGHBOREDGE_ defines
		CDispCornerNeighbors m_CornerNeighbors[4];	// indexed by CORNER_ defines
		uint32 m_AllowedVerts[10];			// active vertices

		int32 NumVerts() const { return ((1 << power) + 1) * ((1 << power) + 1); }
		int32 NumTris() const { return (1 << power) * (1 << power) * 2; }
	};
	static_assert(sizeof(ddispinfo_t) == 176, "ddispinfo_t size");

	struct CDispVert
	{
		FVec3 m_vVector;	// vector field defining displacement volume
		float m_flDist;		// displacement distances
		float m_flAlpha;	// "per vertex" alpha values
	};
	static_assert(sizeof(CDispVert) == 20, "CDispVert size");

	struct CDispTri
	{
		uint16 m_uiTags;	// displacement triangle tags
	};
	static_assert(sizeof(CDispTri) == 2, "CDispTri size");

	/** Human-readable lump names for logging. */
	LAMBDASOURCE_API const TCHAR* GetLumpName(int32 LumpIndex);
}
