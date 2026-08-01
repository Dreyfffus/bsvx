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
#include <span>
#include <string>
#include <vector>


namespace bsvx {

	// Read-only file access, so a manifest world can be loaded out of something that is not the
	// native filesystem -- a Godot .pck behind res://, an archive, a download cache. Everything the
	// Parser reads goes through here.
	//
	// Paths handed to a FileSystem are always scheme-less and use '/' separators: a "res://"-style
	// prefix has to be split off before any path arithmetic happens, because std::filesystem::path
	// collapses the "//" in it. The implementation puts the prefix back on its own side.
	class FileSystem {
	public:
		virtual ~FileSystem() = default;

		// Manifest and region paths pass through here once, before anything is resolved against them.
		virtual std::filesystem::path normalize(const std::filesystem::path& path) const = 0;
		virtual bool is_file(const std::filesystem::path& path) const = 0;
		virtual bool is_directory(const std::filesystem::path& path) const = 0;
		virtual std::vector<std::byte> read_file(const std::filesystem::path& path) const = 0;
		// File names (not full paths) directly inside dir carrying the given extension, unsorted.
		virtual std::vector<std::filesystem::path> list_files(const std::filesystem::path& dir, std::string_view extension) const = 0;
	};

	// std::filesystem-backed FileSystem; what every Parser entry point uses unless told otherwise.
	const FileSystem& native_filesystem();

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

		static std::filesystem::path resolve_manifest_path(const std::filesystem::path& path, const FileSystem& fs = native_filesystem());
		static Manifest parse_manifest(const std::filesystem::path& path, const FileSystem& fs = native_filesystem());
		static WorldPackage load_world(const std::filesystem::path& path, const FileSystem& fs = native_filesystem());
		static WorldPackage load_region(const std::filesystem::path& path, const FileSystem& fs = native_filesystem());
		// Loads a standalone region straight out of a buffer -- no filesystem access at all unless
		// texture_root is non-empty, in which case the region's .btx references are resolved
		// relative to it.
		static WorldPackage load_region_memory(std::span<const std::byte> bytes, const std::filesystem::path& texture_root = {}, const FileSystem& fs = native_filesystem());
		// Wraps an already-parsed standalone region archive in a package, without re-reading or
		// re-serializing it.
		static WorldPackage wrap_standalone_region(bvx::Archive&& archive, const std::filesystem::path& texture_root = {}, const FileSystem& fs = native_filesystem());
		// An empty in-memory world: geometry, no regions, no textures, no registry. Nothing is
		// touched on disk until save_world().
		static WorldPackage create_world(const bvx::GeometryDesc& geometry);
		static void save_manifest(const Manifest& manifest, const std::filesystem::path& manifest_path);
		static void save_world(const WorldPackage& package, const std::filesystem::path& root_or_manifest_path);
		static void save_region(const WorldPackage& package, const std::filesystem::path& region_path);
		// The bytes save_region would have written for the .bvx itself. The package's .btx files are
		// *not* part of them -- a standalone region only stores references to those.
		static std::vector<std::byte> save_region_to_bytes(const WorldPackage& package);

	private:

		static WorldPackage make_standalone_package(bvx::Archive&& archive, const std::filesystem::path& root_dir, const std::string& name, const std::filesystem::path& region_file_name, bool load_textures, const FileSystem& fs);
		static bvx::Archive build_standalone_region(const WorldPackage& package, std::vector<std::filesystem::path>* out_texture_file_names);
		static void auto_discover_textures(Manifest& manifest, const FileSystem& fs);
		static void auto_discover_regions(Manifest& manifest, const FileSystem& fs);

	};
}