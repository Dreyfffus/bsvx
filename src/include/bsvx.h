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

		// Round-trips through the manifest's [metadata] table. Unknown keys are carried through
		// untouched, which is the contract that makes a rewrite by one tool safe for another's data.
		bvx::MetadataMap metadata;

		std::vector<TextureReference> textures;
		std::vector<RegionReference> regions;
	};

	struct TextureAsset final {
		TextureReference ref;
		btx::Archive archive;
		// Set by every mutator; cleared once the asset has been written. A save that only touches
		// voxels should not rewrite (and re-timestamp) every .btx in the world.
		bool dirty = false;
	};

	struct RegionAsset final {
		RegionReference ref;
		bvx::Archive archive;
		bool dirty = false;
	};

	struct WorldPackage final {
		Manifest manifest;
		std::vector<TextureAsset> textures;
		std::vector<RegionAsset> regions;
	};

	// Where a world-space voxel coordinate lands. Region coordinates are signed and floor correctly
	// for negative inputs, which C's truncating division does not.
	struct VoxelAddress final {
		std::array<int32_t, 3> region{};
		std::array<uint16_t, 3> chunk{};
		std::array<uint16_t, 3> local{};
		uint32_t local_index = 0;
	};

	VoxelAddress locate_voxel(const bvx::GeometryDesc& geometry, int64_t x, int64_t y, int64_t z);

	// --- axis conventions ----------------------------------------------------------------------
	// Each convention is a signed axis permutation of the canonical X_RIGHT_Y_UP_Z_FORWARD frame,
	// so converting between any two is "to canonical, then out of canonical".

	// Continuous positions: a plain permutation with sign flips.
	std::array<double, 3> convert_position(AxisConvention from, AxisConvention to, double x, double y, double z);

	// Integer *cell* indices. A flipped axis needs a one-cell offset on top of the negation -- cell
	// c occupies [c, c+1), so its mirror is -c-1, not -c. Getting this wrong shifts the world by one
	// voxel along the flipped axis, which is invisible on symmetric content.
	std::array<int64_t, 3> convert_cell(AxisConvention from, AxisConvention to, int64_t x, int64_t y, int64_t z);

	std::string_view axis_convention_name(AxisConvention convention);
	std::optional<AxisConvention> axis_convention_from_name(std::string_view name);

	// Rewrites every voxel of every region into another axis convention, re-deriving the region and
	// chunk decomposition from scratch -- a flipped axis moves voxels across region boundaries, so
	// there is no cheaper way. Baked payload sections are dropped: they describe the old frame and
	// this library cannot reinterpret them. Returns the number of voxels moved.
	size_t convert_world_axis_convention(WorldPackage& package, AxisConvention target, const ProgressFn& progress = {});

	struct LoadOptions final {
		// A manifest whose bytes changed at all -- a trailing newline, a CRLF checkout, a hand edit
		// -- no longer matches the hash every region was stamped with. Refusing to load is correct
		// for a shipping runtime and useless for an editor, which needs to open the world and fix
		// it. Mismatches become warnings instead.
		bool ignore_hash_mismatch = false;
		bool skip_textures = false;   // metadata-only open: name, bounds, registry, region list
		bool skip_regions = false;
		ProgressFn progress{};
		std::vector<std::string>* warnings = nullptr;
	};

	// Write-side counterpart of FileSystem: everything save_world writes goes through here, so a
	// world can be saved into an archive, a packed file, or a host's own VFS.
	class FileWriter {
	public:
		virtual ~FileWriter() = default;

		virtual void write_file(const std::filesystem::path& path, std::span<const std::byte> bytes, bool atomic, bool backup) = 0;
		virtual void make_directories(const std::filesystem::path& dir) = 0;
		virtual bool exists(const std::filesystem::path& path) const = 0;
		virtual void remove_file(const std::filesystem::path& path) = 0;
		// File names (not full paths) directly inside dir carrying the given extension. Used only by
		// orphan pruning; returning nothing disables it.
		virtual std::vector<std::filesystem::path> list_files(const std::filesystem::path& dir, std::string_view extension) const = 0;
		// Existing contents, for the dirty-only manifest comparison. Empty when absent.
		virtual std::vector<std::byte> read_file(const std::filesystem::path& path) const = 0;
	};

	// std::filesystem-backed FileWriter; what every save uses unless told otherwise.
	FileWriter& native_filewriter();

	struct SaveOptions final {
		bool atomic = true;      // temp file + fsync + rename; an interrupted save keeps the old world
		bool backup = false;     // keep the previous contents as "<file>.bak"
		// Deletes .bvx / .btx files in the managed directories that the manifest no longer
		// references. Off by default because a "save as" into a populated directory would otherwise
		// delete files this world never owned -- but without it, a removed region reappears on the
		// next load through auto-discovery.
		bool prune_orphans = false;
		// Writes only the assets marked dirty. Falls back to a full write when the manifest text
		// changes, because the manifest hash is stamped into every region.
		bool dirty_only = false;
		bool dry_run = false;    // compute and report, touch nothing
		ProgressFn progress{};
		// nullptr means the native filesystem. A custom writer is how a world is saved into a
		// place std::filesystem cannot reach.
		FileWriter* writer = nullptr;
	};

	struct SaveReport final {
		size_t files_written = 0;
		size_t files_removed = 0;
		size_t files_skipped = 0;
		uint64_t bytes_written = 0;
		bool manifest_written = false;
		bool full_rewrite = false;   // a dirty_only save that had to widen to everything
	};

	struct ValidateOptions final {
		// Decodes every chunk to check the voxel keys actually used against the registry. Without
		// it only the summaries' dominant keys are checked, which is cheap but partial.
		bool deep = false;
		ProgressFn progress{};
	};

	class Parser final {
	public:

		static std::filesystem::path resolve_manifest_path(const std::filesystem::path& path, const FileSystem& fs = native_filesystem());
		static Manifest parse_manifest(const std::filesystem::path& path, const FileSystem& fs = native_filesystem());
		static WorldPackage load_world(const std::filesystem::path& path, const FileSystem& fs = native_filesystem(), const LoadOptions& options = {});
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
		static SaveReport save_world(const WorldPackage& package, const std::filesystem::path& root_or_manifest_path, const SaveOptions& options = {});
		static void save_region(const WorldPackage& package, const std::filesystem::path& region_path, const SaveOptions& options = {});
		// The bytes save_region would have written for the .bvx itself. The package's .btx files are
		// *not* part of them -- a standalone region only stores references to those.
		static std::vector<std::byte> save_region_to_bytes(const WorldPackage& package);
		// The manifest.toml text save_world would write, without writing anything. This is the only
		// input to the manifest hash, so a host can compare it against the file on disk to find out
		// whether a save has to restamp every region.
		static std::string save_manifest_to_string(const WorldPackage& package, const std::filesystem::path& root_or_manifest_path);

		// Structural checks a host should run before writing. Never throws for content problems --
		// they come back as issues; it throws only if the package cannot be walked at all.
		static std::vector<ValidationIssue> validate(const WorldPackage& package, const ValidateOptions& options = {});

	private:

		static WorldPackage make_standalone_package(bvx::Archive&& archive, const std::filesystem::path& root_dir, const std::string& name, const std::filesystem::path& region_file_name, bool load_textures, const FileSystem& fs);
		static bvx::Archive build_standalone_region(const WorldPackage& package, std::vector<std::filesystem::path>* out_texture_file_names);
		static void auto_discover_textures(Manifest& manifest, const FileSystem& fs);
		static void auto_discover_regions(Manifest& manifest, const FileSystem& fs);

	};
}