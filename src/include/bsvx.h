#pragma once


#include "btx_header.h"
#include "bvx_header.h"
#include "btx.h"
#include "bvx.h"
#include "definitions.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>


namespace bsvx {

	struct TextureReference final {
		std::string id;
		std::filesystem::path relative_path;
		std::filesystem::path absolute_path;
		uint64_t path_hash = 0ull;
		uint64_t content_hash = 0ull;
	};

	struct RegionReference final {
		std::filesystem::path relative_path;
		std::filesystem::path absolute_path;
		std::optional<std::array<int32_t, 3>> coord;
	};

	struct Manifest final {
		uint32_t format_version = 1;
		std::string name;
		std::string uuid;
		std::filesystem::path manifest_path;
		std::filesystem::path root_dir;
		std::filesystem::path regions_dir = "regions";
		std::filesystem::path textures_dir = "textures";

		bvx::WorldDesc world_desc{};
		bool synthetic_from_standalone_region = false;

		std::vector<TextureReference> textures;
		std::vector<RegionReference> regions;
	};

	struct TextureAsset final {
		TextureReference ref;
		btx::Archive archive;
	};

	struct RegionAsset final {
		RegionReference ref;
		bvx::Archive archive;
	};

	struct WorldPackage final {
		Manifest manifest;
		std::vector<TextureAsset> textures;
		std::vector<RegionAsset> regions;	
	};

	class Parser final {
	public:
		
		static std::filesystem::path resolve_manifest_path(const std::filesystem::path& path);
		static Manifest parse_manifest(const std::filesystem::path& path);
		static WorldPackage load_world(const std::filesystem::path& path);
		static WorldPackage load_region(const std::filesystem::path& path);
		static void save_manifest(const Manifest& manifest, const std::filesystem::path& manifest_path);
		static void save_world(const WorldPackage& package, const std::filesystem::path& root_or_manifest_path);
		static void save_region(const WorldPackage& package, const std::filesystem::path& region_path);

	private:

		static void auto_discover_textures(Manifest& manifest);
		static void auto_discover_regions(Manifest& manifest);

	};
}