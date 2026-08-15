#pragma once
#include <stdint.h>
#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#define BSVX_NODISCARD [[nodiscard]]
#define VALIDATION_RESULT bsvx::ValidationError;
#define MAKE_VALIDATION_RESULT(str) bsvx::Version{str}


namespace bsvx {

	enum class TextureKind : uint8_t {
		TEXTURE_2D		= 0,
		TEXTURE_3D		= 1,
		TEXEL_BUFFER	= 2
	};

	enum class SamplerFilter : uint8_t {
		NEAREST			= 0,
		LINEAR			= 1
	};

	enum class SamplerAddressMode : uint8_t {
		REPEAT			= 0,
		MIRROR_REPEAT	= 1,
		CLAMP_TO_EDGE	= 2,
		CLAMP_TO_BORDER = 3
	};

	enum class BorderColor : uint8_t {
		FLOAT_TRANSPARENT_BLACK = 0,
		INT_TRANSPARENT_BLACK	= 1,
		FLOAT_OPAQUE_BLACK		= 2,
		INT_OPAQUE_BLACK		= 3,
		FLOAT_OPAQUE_WHITE		= 4,
		INT_OPAQUE_WHITE		= 5
	};

	enum class FileFlags : uint16_t {
		NONE = 0,
		STANDALONE = 1u
	};

	enum class ChunkFlags : uint16_t {
		NONE					= 0,
		PRESENT					= 1u,
		CHUNK_EMPTY				= 1u << 1,
		HAS_VOXELS				= 1u << 2,
		HAS_SURFACE_BAKE		= 1u << 3,
		HAS_COLLISION_BAKE		= 1u << 4,
		HAS_DISTANCE_FIELD		= 1u << 5,
		HAS_LIGHT_BAKE			= 1u << 6
	};

	enum class RegistryFlags : uint32_t {
		NONE					= 0,
		OPAQUE					= 1u,
		EMISSIVE				= 1u << 1,
		SPECIAL					= 1u << 2,
		COLLIDABLE				= 1u << 3
	};

	enum class BoundsMode : uint16_t {
		UNBOUNDED		= 0,
		EXPLICIT		= 1u
	};

	enum class VoxelSchema : uint16_t {
		DENSE_U32_VOXEL_KEY = 1
	};

	// The frame a world's integer coordinates are expressed in. X_RIGHT_Y_UP_Z_FORWARD is the
	// canonical one -- conversions are defined as a signed axis permutation relative to it -- and is
	// what every world written before ABI v4 carries.
	enum class AxisConvention : uint16_t {
		X_RIGHT_Y_UP_Z_FORWARD	= 0,	// Godot: +X right, +Y up, +Z toward the viewer. Right-handed.
		X_RIGHT_Z_UP_Y_FORWARD	= 1,	// Blender, 3ds Max: +X right, +Z up, +Y away. Right-handed.
		X_RIGHT_Y_UP_Z_BACK		= 2		// Unity: +X right, +Y up, +Z away. Left-handed.
	};

	enum class FaceState : uint8_t {
		EMPTY	= 0,
		FULL	= 1,
		MIXED	= 2
	};

	enum class SectionType : uint32_t {
		WORLD_DESC		= 1,
		VOXELS			= 2,
		SURFACE			= 3,
		COLLISION		= 4,
		DISTANCE_FIELD	= 5,
		LIGHT			= 6,
		METADATA		= 7,
		USER_BASE		= 0x80000000u
	};

	// Written into SectionRecord's former padding word. A pre-v4 writer left it zero, which is why
	// FLAGS_PRESENT exists: without it there is no way to tell "not chunk-associated" from "written
	// by a build that did not record the flag", and the reader has to fall back to guessing from the
	// entry count.
	enum class SectionFlags : uint32_t {
		NONE				= 0,
		FLAGS_PRESENT		= 1u,
		CHUNK_ASSOCIATED	= 1u << 1
	};

	enum class VoxelCodec : uint16_t {
		CHUNK_INVALID			= 0,
		CHUNK_EMPTY				= 1,
		CHUNK_UNIFORM			= 2,
		PALLETE_BITPACK			= 3,
		SPARSE_LIST				= 4,
		Y_COLUMN_INTERVALS		= 5,
		RAW_DENSE				= 6,
		AUTO					= 0xFFFFu
	};

	struct ValidationError {
		std::string message;
	};

	enum class Severity : uint32_t {
		INFO	= 0,
		WARNING	= 1,
		ERROR	= 2
	};

	// Stable numbers: a host turns these into its own localized text and into "select the thing that
	// is wrong" actions, so they must not be renumbered once shipped.
	enum class ValidationCode : uint32_t {
		NONE						= 0,
		VOXEL_KEY_NOT_IN_REGISTRY	= 1,
		MATERIAL_NOT_FOUND			= 2,
		REGION_OUT_OF_BOUNDS		= 3,
		CHUNK_OUT_OF_REGION			= 4,
		DUPLICATE_REGION_COORD		= 5,
		DUPLICATE_VOXEL_KEY			= 6,
		AIR_KEY_REGISTERED			= 7,
		TEXTURE_PATH_TOO_LONG		= 8,
		TEXTURE_ARCHIVE_INVALID		= 9,
		TEXTURE_REF_UNRESOLVED		= 10,
		GEOMETRY_INVALID			= 11,
		EMPTY_REGION				= 12,
		REGISTRY_EMPTY				= 13,
		NO_TEXTURES					= 14,
		UNITS_UNSET					= 15
	};

	struct ValidationIssue final {
		Severity severity = Severity::WARNING;
		ValidationCode code = ValidationCode::NONE;
		int64_t region_index = -1;   // -1 when the issue is not region-scoped
		int64_t chunk_ordinal = -1;  // -1 when the issue is not chunk-scoped
		uint32_t voxel_key = 0;      // 0 when irrelevant
		std::string message;
	};

	// Return false to cancel. Long operations call this between units of work and unwind with a
	// CancelledError, which never leaves a partially written file behind when the write is atomic.
	using ProgressFn = std::function<bool(std::string_view stage, size_t done, size_t total)>;

	struct CancelledError final : std::runtime_error {
		CancelledError() : std::runtime_error("[bsvx]: operation cancelled by the host") {}
	};
}