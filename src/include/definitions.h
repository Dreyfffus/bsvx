#pragma once
#include <stdint.h>
#include <map>
#include <string>
#include <filesystem>
#include <array>

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

	enum class AxisConvention : uint16_t {
		X_RIGHT_Y_UP_Z_FORWARD = 0
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
		USER_BASE		= 0x80000000u
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
}