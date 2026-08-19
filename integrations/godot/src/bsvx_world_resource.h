#pragma once

#include <bsvx_dll.h>

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/vector3i.hpp>

namespace godot {

// The format layer, and only the format layer: open a .bsvx world or a standalone .bvx region,
// read and write its voxels, registry, units and metadata, and save it back.
//
// Nothing here bakes anything. A voxel is a uint32 key plus whatever the registry says that key
// means; turning that into geometry -- meshes, bricks, distance fields, a raymarched volume --
// is a renderer's decision, and deliberately not this class's business. That is the whole point
// of keeping the two apart: a world authored through this API is readable by any renderer, and
// by Blender, and by the library's own tools.
class BsvxWorld : public Resource {
	GDCLASS(BsvxWorld, Resource)

public:
	// Mirrors bsvx_load_flags. static_asserts in the .cpp keep the two from drifting.
	enum LoadFlags {
		LOAD_DEFAULT = 0,
		LOAD_IGNORE_HASH_MISMATCH = 1 << 0,
		LOAD_SKIP_TEXTURES = 1 << 1,
		LOAD_SKIP_REGIONS = 1 << 2,
	};

	// Mirrors bsvx_save_flags.
	enum SaveFlags {
		SAVE_DEFAULT = 0,
		SAVE_NON_ATOMIC = 1 << 0,
		SAVE_BACKUP = 1 << 1,
		SAVE_PRUNE_ORPHANS = 1 << 2,
		SAVE_DIRTY_ONLY = 1 << 3,
		SAVE_COMPACT_FIRST = 1 << 4,
		SAVE_DRY_RUN = 1 << 5,
	};

	// Mirrors bsvx_registry_flags.
	enum RegistryFlags {
		REGISTRY_OPAQUE = 1 << 0,
		REGISTRY_EMISSIVE = 1 << 1,
		REGISTRY_SPECIAL = 1 << 2,
		REGISTRY_COLLIDABLE = 1 << 3,
	};

	// Mirrors bsvx_axis_convention. Godot is the canonical frame, so a world authored here needs
	// no conversion; Blender's Z-up worlds do.
	enum AxisConvention {
		AXIS_X_RIGHT_Y_UP_Z_FORWARD = 0,
		AXIS_X_RIGHT_Z_UP_Y_FORWARD = 1,
		AXIS_X_RIGHT_Y_UP_Z_BACK = 2,
	};

	enum Severity {
		SEVERITY_INFO = 0,
		SEVERITY_WARNING = 1,
		SEVERITY_ERROR = 2,
	};

protected:
	static void _bind_methods();

public:
	BsvxWorld();
	~BsvxWorld() override;

	// --- lifecycle ---------------------------------------------------------------------------
	Error create(const Vector3i &chunk_size, const Vector3i &region_size);
	// A manifest world (a directory, or the manifest.toml inside it). Read through Godot's
	// FileAccess, so res:// works in an exported project where there is no filesystem path.
	Error load_world(const String &path, int64_t flags = LOAD_DEFAULT);
	// A standalone .bvx. Also read through FileAccess.
	Error load_region(const String &path);
	Error load_region_bytes(const PackedByteArray &bytes);

	// Saving needs a real filesystem path: the library writes through a temp file and a rename,
	// which no pack file can offer. user:// globalizes fine; res:// only in the editor.
	Error save_world(const String &path, int64_t flags = SAVE_DEFAULT);
	Error save_region(const String &path);
	PackedByteArray save_region_bytes();

	bool is_open() const { return world != nullptr; }
	void close();

	String get_last_error() const { return last_error; }
	PackedStringArray get_warnings() const;

	// --- description -------------------------------------------------------------------------
	Vector3i get_chunk_size() const;
	Vector3i get_region_size() const;
	Vector3i get_region_extent() const; // chunk_size * region_size, in voxels

	String get_world_name() const;
	Error set_world_name(const String &name);
	String get_uuid() const;
	Error set_uuid(const String &uuid);

	int64_t get_axis_convention() const;
	// Rewrites every voxel. Baked payload sections are dropped and every region index is
	// invalidated -- see the ABI note. Returns the number of voxels moved, or -1 on failure.
	int64_t convert_axis_convention(int64_t target);

	Vector3 get_voxel_size() const;
	Error set_voxel_size(const Vector3 &size);
	Vector3 get_origin() const;
	Error set_origin(const Vector3 &origin);

	// --- regions -----------------------------------------------------------------------------
	int64_t get_region_count() const;
	Vector3i get_region_coord(int64_t region_index) const;
	int64_t find_region(const Vector3i &coord) const; // -1 when absent
	int64_t add_region(const Vector3i &coord);        // new index, or -1
	Error remove_region(int64_t region_index);
	int64_t prune_empty_regions();

	// --- chunks ------------------------------------------------------------------------------
	int64_t get_chunk_count(int64_t region_index) const;
	TypedArray<Vector3i> get_chunk_coords(int64_t region_index) const;
	bool has_chunk(int64_t region_index, const Vector3i &chunk) const;
	Dictionary get_chunk_info(int64_t region_index, const Vector3i &chunk) const;

	// Dense voxel keys in the format's own order: index = x + sx * (y + sy * z).
	PackedInt32Array get_chunk(int64_t region_index, const Vector3i &chunk) const;
	Error set_chunk(int64_t region_index, const Vector3i &chunk, const PackedInt32Array &voxels);
	Error clear_chunk(int64_t region_index, const Vector3i &chunk);
	Error remove_chunk(int64_t region_index, const Vector3i &chunk);

	// The whole region as one dense array, unauthored chunks reading as air.
	PackedInt32Array decode_region(int64_t region_index) const;

	// --- voxels in world coordinates ---------------------------------------------------------
	int64_t get_voxel(const Vector3i &position) const;
	Error set_voxel(const Vector3i &position, int64_t key, bool create_missing = true);

	// Coordinates are packed as x, y, z triples so a large edit crosses the boundary once.
	// Vector3 would work too and would quietly lose precision past 2^24 voxels.
	PackedInt32Array get_voxels(const PackedInt64Array &positions) const;
	int64_t set_voxels(const PackedInt64Array &positions, const PackedInt32Array &keys, bool create_missing = true);
	int64_t fill_box(const Vector3i &from, const Vector3i &to, int64_t key, bool create_missing = true);

	Dictionary locate_voxel(const Vector3i &position) const;

	// --- registry ----------------------------------------------------------------------------
	PackedInt32Array get_registry_keys() const;
	Dictionary get_registry_entry(int64_t voxel_key) const;
	Error set_registry_entry(int64_t voxel_key, int64_t material_id, int64_t flags);
	Error remove_registry_entry(int64_t voxel_key);

	String get_voxel_name(int64_t voxel_key) const;
	Error set_voxel_name(int64_t voxel_key, const String &name);

	// Resolved display colour, in the order the format defines: a material tint from an attached
	// .btx wins, the registry colour is the fallback, and an unset key is transparent black.
	Color get_voxel_color(int64_t voxel_key) const;
	Error set_voxel_color(int64_t voxel_key, const Color &color);

	// Indexed by voxel key, so palette[key] is that key's colour and index 0 is always air.
	PackedColorArray get_palette() const;
	// The colour-first authoring path: one 1x1 texture layer, material and registry entry per
	// colour, with key i+1 mapped to colour i.
	Error make_palette(const PackedColorArray &colors, const String &texture_id, int64_t flags_for_all = REGISTRY_OPAQUE);

	// --- textures (introspection only) -------------------------------------------------------
	int64_t get_texture_count() const;
	String get_texture_id(int64_t texture_index) const;
	String get_texture_path(int64_t texture_index) const;

	// --- metadata ----------------------------------------------------------------------------
	PackedStringArray get_metadata_keys() const;
	PackedByteArray get_metadata(const String &key) const;
	Error set_metadata(const String &key, const PackedByteArray &value);
	Error remove_metadata(const String &key);

	// --- validation and housekeeping ---------------------------------------------------------
	TypedArray<Dictionary> validate(bool deep = false);
	bool is_dirty() const;
	Error clear_dirty();
	int64_t compact();

	// --- library info ------------------------------------------------------------------------
	static int64_t get_abi_version();
	static String get_build_info();
	static String axis_convention_name(int64_t convention);

private:
	// Fails the call and records why, so GDScript gets both an Error and a message.
	bool check(bsvx_result result, const char *what) const;
	bool require_open() const;
	// Reads a whole file through FileAccess, which is the only way res:// works after export.
	bool read_bytes(const String &path, PackedByteArray &out) const;
	void adopt(bsvx_world *new_world);

	bsvx_context *ctx = nullptr;
	bsvx_world *world = nullptr;
	mutable String last_error;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::BsvxWorld::LoadFlags);
VARIANT_ENUM_CAST(godot::BsvxWorld::SaveFlags);
VARIANT_ENUM_CAST(godot::BsvxWorld::RegistryFlags);
VARIANT_ENUM_CAST(godot::BsvxWorld::AxisConvention);
VARIANT_ENUM_CAST(godot::BsvxWorld::Severity);
