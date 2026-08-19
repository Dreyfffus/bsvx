#include "bsvx_world_resource.h"
#include "bsvx_gd_util.h"
#include "bsvx_gd_vfs.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <vector>

using namespace godot;
using bsvxgd::from_utf8;
using bsvxgd::read_string;
using bsvxgd::to_godot_error;
using bsvxgd::to_utf8;

// The enums above are hand-mirrored so GDScript sees named constants instead of magic numbers.
// These keep the mirror honest: a renumbering in the ABI breaks the build here rather than
// silently turning SAVE_BACKUP into SAVE_PRUNE_ORPHANS at some user's expense.
static_assert(static_cast<int>(BsvxWorld::LOAD_IGNORE_HASH_MISMATCH) == static_cast<int>(BSVX_LOAD_IGNORE_HASH_MISMATCH));
static_assert(static_cast<int>(BsvxWorld::LOAD_SKIP_TEXTURES) == static_cast<int>(BSVX_LOAD_SKIP_TEXTURES));
static_assert(static_cast<int>(BsvxWorld::LOAD_SKIP_REGIONS) == static_cast<int>(BSVX_LOAD_SKIP_REGIONS));
static_assert(static_cast<int>(BsvxWorld::SAVE_NON_ATOMIC) == static_cast<int>(BSVX_SAVE_NON_ATOMIC));
static_assert(static_cast<int>(BsvxWorld::SAVE_BACKUP) == static_cast<int>(BSVX_SAVE_BACKUP));
static_assert(static_cast<int>(BsvxWorld::SAVE_PRUNE_ORPHANS) == static_cast<int>(BSVX_SAVE_PRUNE_ORPHANS));
static_assert(static_cast<int>(BsvxWorld::SAVE_DIRTY_ONLY) == static_cast<int>(BSVX_SAVE_DIRTY_ONLY));
static_assert(static_cast<int>(BsvxWorld::SAVE_COMPACT_FIRST) == static_cast<int>(BSVX_SAVE_COMPACT_FIRST));
static_assert(static_cast<int>(BsvxWorld::SAVE_DRY_RUN) == static_cast<int>(BSVX_SAVE_DRY_RUN));
static_assert(static_cast<int>(BsvxWorld::REGISTRY_OPAQUE) == static_cast<int>(BSVX_REGISTRY_OPAQUE));
static_assert(static_cast<int>(BsvxWorld::REGISTRY_EMISSIVE) == static_cast<int>(BSVX_REGISTRY_EMISSIVE));
static_assert(static_cast<int>(BsvxWorld::REGISTRY_SPECIAL) == static_cast<int>(BSVX_REGISTRY_SPECIAL));
static_assert(static_cast<int>(BsvxWorld::REGISTRY_COLLIDABLE) == static_cast<int>(BSVX_REGISTRY_COLLIDABLE));
static_assert(static_cast<int>(BsvxWorld::AXIS_X_RIGHT_Z_UP_Y_FORWARD) == static_cast<int>(BSVX_AXIS_X_RIGHT_Z_UP_Y_FORWARD));
static_assert(static_cast<int>(BsvxWorld::AXIS_X_RIGHT_Y_UP_Z_BACK) == static_cast<int>(BSVX_AXIS_X_RIGHT_Y_UP_Z_BACK));
static_assert(static_cast<int>(BsvxWorld::SEVERITY_ERROR) == static_cast<int>(BSVX_SEVERITY_ERROR));

namespace {

constexpr uint16_t CHUNK_FLAG_EMPTY = 1u << 1;

// Godot's Color is linear float; the format stores 0xRRGGBBAA. Both conversions go through
// Color8 / the byte accessors so the sRGB round-trip is Godot's own, not a hand-rolled one.
Color color_from_rgba8(uint32_t rgba) {
	return Color(static_cast<uint8_t>(rgba >> 24) / 255.0f,
			static_cast<uint8_t>(rgba >> 16) / 255.0f,
			static_cast<uint8_t>(rgba >> 8) / 255.0f,
			static_cast<uint8_t>(rgba) / 255.0f);
}

uint32_t rgba8_from_color(const Color &c) {
	const auto q = [](float v) -> uint32_t {
		const float clamped = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
		return static_cast<uint32_t>(clamped * 255.0f + 0.5f);
	};
	return (q(c.r) << 24) | (q(c.g) << 16) | (q(c.b) << 8) | q(c.a);
}

bool chunk_coord_fits(const Vector3i &c) {
	return c.x >= 0 && c.y >= 0 && c.z >= 0 && c.x <= 0xFFFF && c.y <= 0xFFFF && c.z <= 0xFFFF;
}

} // namespace

BsvxWorld::BsvxWorld() {
	ctx = bsvx_context_create();
}

BsvxWorld::~BsvxWorld() {
	close();
	if (ctx != nullptr) {
		bsvx_context_destroy(ctx);
		ctx = nullptr;
	}
}

// ---------------------------------------------------------------------------------------------
// internals
// ---------------------------------------------------------------------------------------------

bool BsvxWorld::check(bsvx_result result, const char *what) const {
	if (result == BSVX_RESULT_OK) {
		last_error = String();
		return true;
	}

	// The context carries the library's own message for the _ex entry points; the plain ones
	// leave it untouched, so the result code is the only thing left to report.
	const char *detail = ctx != nullptr ? bsvx_context_last_error(ctx) : nullptr;
	last_error = String(what) + ": " + String(bsvx_result_string(static_cast<uint32_t>(result)));
	if (detail != nullptr && *detail != '\0') {
		last_error += " (" + String::utf8(detail) + ")";
	}
	return false;
}

bool BsvxWorld::require_open() const {
	if (world != nullptr) return true;
	last_error = "no world is open";
	return false;
}

void BsvxWorld::adopt(bsvx_world *new_world) {
	if (world != nullptr) bsvx_world_destroy(world);
	world = new_world;
	emit_changed();
}

void BsvxWorld::close() {
	if (world != nullptr) {
		bsvx_world_destroy(world);
		world = nullptr;
		emit_changed();
	}
}

bool BsvxWorld::read_bytes(const String &path, PackedByteArray &out) const {
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
	if (file.is_null()) {
		last_error = "could not open " + path;
		return false;
	}
	out = file->get_buffer(file->get_length());
	return true;
}

PackedStringArray BsvxWorld::get_warnings() const {
	PackedStringArray out;
	if (ctx == nullptr) return out;
	const size_t count = bsvx_context_warning_count(ctx);
	for (size_t i = 0; i < count; ++i) {
		const char *text = bsvx_context_warning(ctx, i);
		if (text != nullptr) out.push_back(String::utf8(text));
	}
	return out;
}

// ---------------------------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------------------------

Error BsvxWorld::create(const Vector3i &chunk_size, const Vector3i &region_size) {
	if (!chunk_coord_fits(chunk_size) || !chunk_coord_fits(region_size)) {
		last_error = "chunk and region sizes must fit in uint16";
		return ERR_INVALID_PARAMETER;
	}

	bsvx_geometry_desc geom{};
	geom.chunk_size_x = static_cast<uint16_t>(chunk_size.x);
	geom.chunk_size_y = static_cast<uint16_t>(chunk_size.y);
	geom.chunk_size_z = static_cast<uint16_t>(chunk_size.z);
	geom.region_size_x = static_cast<uint16_t>(region_size.x);
	geom.region_size_y = static_cast<uint16_t>(region_size.y);
	geom.region_size_z = static_cast<uint16_t>(region_size.z);

	bsvx_world *created = nullptr;
	const bsvx_result r = bsvx_world_create(ctx, &geom, &created);
	if (!check(r, "create")) return to_godot_error(r);

	adopt(created);
	return OK;
}

Error BsvxWorld::load_world(const String &path, int64_t flags) {
	// The VFS route ignores flags, so a caller who needs them on a real path gets the direct
	// entry point instead. Both are honest about which one ran, because the flags that matter
	// most (IGNORE_HASH_MISMATCH) are exactly the ones an editor needs.
	const String global = ProjectSettings::get_singleton()->globalize_path(path);
	const bool on_disk = !global.is_empty() && (FileAccess::file_exists(global) || FileAccess::file_exists(global.path_join("manifest.toml")));

	bsvx_world *loaded = nullptr;
	bsvx_result r;
	if (on_disk) {
		r = bsvx_world_load_ex2(ctx, to_utf8(global).c_str(), static_cast<uint32_t>(flags), &loaded);
	} else {
		bsvxgd::GodotVfs vfs;
		r = bsvx_world_load_vfs(ctx, to_utf8(path).c_str(), vfs.desc(), &loaded);
	}

	if (!check(r, "load_world")) return to_godot_error(r);
	adopt(loaded);
	return OK;
}

Error BsvxWorld::load_region(const String &path) {
	PackedByteArray bytes;
	if (!read_bytes(path, bytes)) return ERR_FILE_CANT_OPEN;

	// A standalone .bvx may still reference .btx archives beside it. That only resolves when the
	// directory is a real one; inside a .pck it is skipped and the world loads without textures.
	const String global_dir = ProjectSettings::get_singleton()->globalize_path(path).get_base_dir();
	const std::string texture_root = to_utf8(global_dir);

	bsvx_world *loaded = nullptr;
	const bsvx_result r = bsvx_world_load_region_memory_ex(ctx, bytes.ptr(), static_cast<size_t>(bytes.size()),
			texture_root.empty() ? nullptr : texture_root.c_str(), &loaded);
	if (!check(r, "load_region")) return to_godot_error(r);

	adopt(loaded);
	return OK;
}

Error BsvxWorld::load_region_bytes(const PackedByteArray &bytes) {
	bsvx_world *loaded = nullptr;
	const bsvx_result r = bsvx_world_load_region_memory(ctx, bytes.ptr(), static_cast<size_t>(bytes.size()), &loaded);
	if (!check(r, "load_region_bytes")) return to_godot_error(r);

	adopt(loaded);
	return OK;
}

Error BsvxWorld::save_world(const String &path, int64_t flags) {
	if (!require_open()) return ERR_UNCONFIGURED;

	const String global = ProjectSettings::get_singleton()->globalize_path(path);
	const bsvx_result r = bsvx_world_save_ex2(ctx, world, to_utf8(global).c_str(), static_cast<uint32_t>(flags), nullptr);
	if (!check(r, "save_world")) return to_godot_error(r);
	return OK;
}

Error BsvxWorld::save_region(const String &path) {
	if (!require_open()) return ERR_UNCONFIGURED;

	const String global = ProjectSettings::get_singleton()->globalize_path(path);
	const bsvx_result r = bsvx_world_save_region_ex(ctx, world, to_utf8(global).c_str());
	if (!check(r, "save_region")) return to_godot_error(r);
	return OK;
}

PackedByteArray BsvxWorld::save_region_bytes() {
	PackedByteArray out;
	if (!require_open()) return out;

	size_t needed = 0;
	bsvx_result r = bsvx_world_save_region_memory(ctx, world, nullptr, 0, &needed);
	if (r != BSVX_RESULT_OK && r != BSVX_RESULT_BUFFER_TOO_SMALL) {
		check(r, "save_region_bytes");
		return out;
	}

	out.resize(static_cast<int64_t>(needed));
	size_t written = 0;
	r = bsvx_world_save_region_memory(ctx, world, out.ptrw(), needed, &written);
	if (!check(r, "save_region_bytes")) {
		out.clear();
		return out;
	}
	out.resize(static_cast<int64_t>(written));
	return out;
}

// ---------------------------------------------------------------------------------------------
// description
// ---------------------------------------------------------------------------------------------

Vector3i BsvxWorld::get_chunk_size() const {
	bsvx_geometry_desc geom{};
	if (!require_open() || !check(bsvx_world_geometry(world, &geom), "get_chunk_size")) return Vector3i();
	return Vector3i(geom.chunk_size_x, geom.chunk_size_y, geom.chunk_size_z);
}

Vector3i BsvxWorld::get_region_size() const {
	bsvx_geometry_desc geom{};
	if (!require_open() || !check(bsvx_world_geometry(world, &geom), "get_region_size")) return Vector3i();
	return Vector3i(geom.region_size_x, geom.region_size_y, geom.region_size_z);
}

Vector3i BsvxWorld::get_region_extent() const {
	bsvx_geometry_desc geom{};
	if (!require_open() || !check(bsvx_world_geometry(world, &geom), "get_region_extent")) return Vector3i();
	return Vector3i(geom.chunk_size_x * geom.region_size_x,
			geom.chunk_size_y * geom.region_size_y,
			geom.chunk_size_z * geom.region_size_z);
}

String BsvxWorld::get_world_name() const {
	if (!require_open()) return String();
	return read_string([&](char *b, size_t c, size_t *n) { return bsvx_world_get_name(world, b, c, n); });
}

Error BsvxWorld::set_world_name(const String &name) {
	if (!require_open()) return ERR_UNCONFIGURED;
	const bsvx_result r = bsvx_world_set_name(world, to_utf8(name).c_str());
	return check(r, "set_world_name") ? OK : to_godot_error(r);
}

String BsvxWorld::get_uuid() const {
	if (!require_open()) return String();
	return read_string([&](char *b, size_t c, size_t *n) { return bsvx_world_get_uuid(world, b, c, n); });
}

Error BsvxWorld::set_uuid(const String &uuid) {
	if (!require_open()) return ERR_UNCONFIGURED;
	const bsvx_result r = bsvx_world_set_uuid(world, to_utf8(uuid).c_str());
	return check(r, "set_uuid") ? OK : to_godot_error(r);
}

int64_t BsvxWorld::get_axis_convention() const {
	bsvx_world_desc desc{};
	if (!require_open() || !check(bsvx_world_get_desc(world, &desc), "get_axis_convention")) return -1;
	return desc.axis_convention;
}

int64_t BsvxWorld::convert_axis_convention(int64_t target) {
	if (!require_open()) return -1;
	size_t moved = 0;
	const bsvx_result r = bsvx_world_convert_axis_convention(ctx, world, static_cast<uint32_t>(target), &moved);
	if (!check(r, "convert_axis_convention")) return -1;
	emit_changed();
	return static_cast<int64_t>(moved);
}

Vector3 BsvxWorld::get_voxel_size() const {
	bsvx_units units{};
	if (!require_open() || !check(bsvx_world_get_units(world, &units), "get_voxel_size")) return Vector3();
	return Vector3(static_cast<float>(units.voxel_size_x), static_cast<float>(units.voxel_size_y), static_cast<float>(units.voxel_size_z));
}

Error BsvxWorld::set_voxel_size(const Vector3 &size) {
	if (!require_open()) return ERR_UNCONFIGURED;
	bsvx_units units{};
	if (!check(bsvx_world_get_units(world, &units), "set_voxel_size")) return FAILED;
	units.voxel_size_x = size.x;
	units.voxel_size_y = size.y;
	units.voxel_size_z = size.z;
	const bsvx_result r = bsvx_world_set_units(world, &units);
	return check(r, "set_voxel_size") ? OK : to_godot_error(r);
}

Vector3 BsvxWorld::get_origin() const {
	bsvx_units units{};
	if (!require_open() || !check(bsvx_world_get_units(world, &units), "get_origin")) return Vector3();
	return Vector3(static_cast<float>(units.origin_x), static_cast<float>(units.origin_y), static_cast<float>(units.origin_z));
}

Error BsvxWorld::set_origin(const Vector3 &origin) {
	if (!require_open()) return ERR_UNCONFIGURED;
	bsvx_units units{};
	if (!check(bsvx_world_get_units(world, &units), "set_origin")) return FAILED;
	units.origin_x = origin.x;
	units.origin_y = origin.y;
	units.origin_z = origin.z;
	const bsvx_result r = bsvx_world_set_units(world, &units);
	return check(r, "set_origin") ? OK : to_godot_error(r);
}

// ---------------------------------------------------------------------------------------------
// regions
// ---------------------------------------------------------------------------------------------

int64_t BsvxWorld::get_region_count() const {
	if (!require_open()) return 0;
	return static_cast<int64_t>(bsvx_world_region_count(world));
}

Vector3i BsvxWorld::get_region_coord(int64_t region_index) const {
	if (!require_open() || region_index < 0) return Vector3i();
	int32_t x = 0, y = 0, z = 0;
	if (!check(bsvx_world_get_region_coord(world, static_cast<size_t>(region_index), &x, &y, &z), "get_region_coord")) return Vector3i();
	return Vector3i(x, y, z);
}

int64_t BsvxWorld::find_region(const Vector3i &coord) const {
	if (!require_open()) return -1;
	size_t index = 0;
	if (bsvx_world_find_region(world, coord.x, coord.y, coord.z, &index) != BSVX_RESULT_OK) return -1;
	return static_cast<int64_t>(index);
}

int64_t BsvxWorld::add_region(const Vector3i &coord) {
	if (!require_open()) return -1;
	size_t index = 0;
	if (!check(bsvx_world_add_region(world, coord.x, coord.y, coord.z, &index), "add_region")) return -1;
	emit_changed();
	return static_cast<int64_t>(index);
}

Error BsvxWorld::remove_region(int64_t region_index) {
	if (!require_open()) return ERR_UNCONFIGURED;
	const bsvx_result r = bsvx_world_remove_region(world, static_cast<size_t>(region_index));
	if (!check(r, "remove_region")) return to_godot_error(r);
	emit_changed();
	return OK;
}

int64_t BsvxWorld::prune_empty_regions() {
	if (!require_open()) return -1;
	size_t removed = 0;
	if (!check(bsvx_world_prune_empty_regions(world, &removed), "prune_empty_regions")) return -1;
	if (removed > 0) emit_changed();
	return static_cast<int64_t>(removed);
}

// ---------------------------------------------------------------------------------------------
// chunks
// ---------------------------------------------------------------------------------------------

int64_t BsvxWorld::get_chunk_count(int64_t region_index) const {
	if (!require_open() || region_index < 0) return 0;
	return static_cast<int64_t>(bsvx_region_chunk_count(world, static_cast<size_t>(region_index)));
}

TypedArray<Vector3i> BsvxWorld::get_chunk_coords(int64_t region_index) const {
	TypedArray<Vector3i> out;
	if (!require_open() || region_index < 0) return out;

	const size_t region = static_cast<size_t>(region_index);
	const size_t count = bsvx_region_chunk_count(world, region);
	if (count == 0) return out;

	// One bulk crossing instead of one per chunk: a populated region holds thousands.
	std::vector<bsvx_chunk_info> infos(count);
	size_t written = 0;
	if (!check(bsvx_region_get_chunk_infos(world, region, 0, count, infos.data(), &written), "get_chunk_coords")) return out;

	out.resize(static_cast<int64_t>(written));
	for (size_t i = 0; i < written; ++i) {
		out[static_cast<int64_t>(i)] = Vector3i(infos[i].local_chunk_x, infos[i].local_chunk_y, infos[i].local_chunk_z);
	}
	return out;
}

bool BsvxWorld::has_chunk(int64_t region_index, const Vector3i &chunk) const {
	if (!require_open() || region_index < 0 || !chunk_coord_fits(chunk)) return false;
	size_t ordinal = 0;
	return bsvx_region_find_chunk(world, static_cast<size_t>(region_index),
				   static_cast<uint16_t>(chunk.x), static_cast<uint16_t>(chunk.y), static_cast<uint16_t>(chunk.z), &ordinal) == BSVX_RESULT_OK;
}

Dictionary BsvxWorld::get_chunk_info(int64_t region_index, const Vector3i &chunk) const {
	Dictionary out;
	if (!require_open() || region_index < 0 || !chunk_coord_fits(chunk)) return out;

	const size_t region = static_cast<size_t>(region_index);
	size_t ordinal = 0;
	if (bsvx_region_find_chunk(world, region, static_cast<uint16_t>(chunk.x), static_cast<uint16_t>(chunk.y), static_cast<uint16_t>(chunk.z), &ordinal) != BSVX_RESULT_OK) {
		return out;
	}

	bsvx_chunk_info info{};
	if (!check(bsvx_region_get_chunk_info(world, region, ordinal, &info), "get_chunk_info")) return out;

	out["coord"] = Vector3i(info.local_chunk_x, info.local_chunk_y, info.local_chunk_z);
	out["ordinal"] = static_cast<int64_t>(ordinal);
	out["flags"] = static_cast<int64_t>(info.flags);
	out["empty"] = (info.flags & CHUNK_FLAG_EMPTY) != 0 || info.summary.non_air_count == 0;
	out["non_air_count"] = static_cast<int64_t>(info.summary.non_air_count);
	out["opaque_count"] = static_cast<int64_t>(info.summary.opaque_count);
	out["aabb_min"] = Vector3i(info.summary.aabb_min_x, info.summary.aabb_min_y, info.summary.aabb_min_z);
	out["aabb_max"] = Vector3i(info.summary.aabb_max_x, info.summary.aabb_max_y, info.summary.aabb_max_z);

	PackedInt32Array top_keys;
	top_keys.push_back(static_cast<int32_t>(info.summary.top_id_0));
	top_keys.push_back(static_cast<int32_t>(info.summary.top_id_1));
	top_keys.push_back(static_cast<int32_t>(info.summary.top_id_2));
	top_keys.push_back(static_cast<int32_t>(info.summary.top_id_3));
	out["top_keys"] = top_keys;
	return out;
}

PackedInt32Array BsvxWorld::get_chunk(int64_t region_index, const Vector3i &chunk) const {
	PackedInt32Array out;
	if (!require_open() || region_index < 0 || !chunk_coord_fits(chunk)) return out;

	const size_t region = static_cast<size_t>(region_index);
	const size_t capacity = bsvx_region_required_voxel_count(world, region);
	if (capacity == 0) return out;

	out.resize(static_cast<int64_t>(capacity));
	size_t written = 0;
	// PackedInt32Array is int32 and voxel keys are uint32; the reinterpret is bit-exact, so a
	// key above 2^31 reads back negative in GDScript rather than wrong. Registries that large
	// do not occur in practice, and the alternative doubles the memory of every chunk read.
	const bsvx_result r = bsvx_region_decode_chunk_u32_ex(ctx, world, region,
			static_cast<uint16_t>(chunk.x), static_cast<uint16_t>(chunk.y), static_cast<uint16_t>(chunk.z),
			reinterpret_cast<uint32_t *>(out.ptrw()), capacity, &written);
	if (!check(r, "get_chunk")) {
		out.clear();
		return out;
	}
	out.resize(static_cast<int64_t>(written));
	return out;
}

Error BsvxWorld::set_chunk(int64_t region_index, const Vector3i &chunk, const PackedInt32Array &voxels) {
	if (!require_open()) return ERR_UNCONFIGURED;
	if (region_index < 0 || !chunk_coord_fits(chunk)) return ERR_INVALID_PARAMETER;

	// 0xFFFF asks the library to pick a codec from the content, which is what an authoring tool
	// wants: dense chunks stay dense, uniform ones collapse.
	constexpr uint16_t CODEC_AUTO = 0xFFFFu;
	const bsvx_result r = bsvx_region_set_chunk_u32_ex(ctx, world, static_cast<size_t>(region_index),
			static_cast<uint16_t>(chunk.x), static_cast<uint16_t>(chunk.y), static_cast<uint16_t>(chunk.z),
			reinterpret_cast<const uint32_t *>(voxels.ptr()), static_cast<size_t>(voxels.size()), CODEC_AUTO);
	if (!check(r, "set_chunk")) return to_godot_error(r);
	emit_changed();
	return OK;
}

Error BsvxWorld::clear_chunk(int64_t region_index, const Vector3i &chunk) {
	if (!require_open()) return ERR_UNCONFIGURED;
	if (region_index < 0 || !chunk_coord_fits(chunk)) return ERR_INVALID_PARAMETER;
	const bsvx_result r = bsvx_region_clear_chunk(world, static_cast<size_t>(region_index),
			static_cast<uint16_t>(chunk.x), static_cast<uint16_t>(chunk.y), static_cast<uint16_t>(chunk.z));
	if (!check(r, "clear_chunk")) return to_godot_error(r);
	emit_changed();
	return OK;
}

Error BsvxWorld::remove_chunk(int64_t region_index, const Vector3i &chunk) {
	if (!require_open()) return ERR_UNCONFIGURED;
	if (region_index < 0 || !chunk_coord_fits(chunk)) return ERR_INVALID_PARAMETER;
	const bsvx_result r = bsvx_region_remove_chunk(world, static_cast<size_t>(region_index),
			static_cast<uint16_t>(chunk.x), static_cast<uint16_t>(chunk.y), static_cast<uint16_t>(chunk.z));
	if (!check(r, "remove_chunk")) return to_godot_error(r);
	emit_changed();
	return OK;
}

PackedInt32Array BsvxWorld::decode_region(int64_t region_index) const {
	PackedInt32Array out;
	if (!require_open() || region_index < 0) return out;

	const size_t region = static_cast<size_t>(region_index);
	size_t needed = 0;
	bsvx_result r = bsvx_region_decode_all_u32(ctx, world, region, BSVX_LAYOUT_REGION_LINEAR, nullptr, 0, &needed);
	if (r != BSVX_RESULT_OK && r != BSVX_RESULT_BUFFER_TOO_SMALL) {
		check(r, "decode_region");
		return out;
	}
	if (needed == 0) return out;

	out.resize(static_cast<int64_t>(needed));
	size_t written = 0;
	r = bsvx_region_decode_all_u32(ctx, world, region, BSVX_LAYOUT_REGION_LINEAR,
			reinterpret_cast<uint32_t *>(out.ptrw()), needed, &written);
	if (!check(r, "decode_region")) {
		out.clear();
		return out;
	}
	out.resize(static_cast<int64_t>(written));
	return out;
}

// ---------------------------------------------------------------------------------------------
// voxels in world coordinates
// ---------------------------------------------------------------------------------------------

int64_t BsvxWorld::get_voxel(const Vector3i &position) const {
	if (!require_open()) return 0;
	const int64_t xs[1] = { position.x };
	const int64_t ys[1] = { position.y };
	const int64_t zs[1] = { position.z };
	uint32_t key = 0;
	if (!check(bsvx_world_get_voxels(ctx, world, xs, ys, zs, &key, 1), "get_voxel")) return 0;
	return static_cast<int64_t>(key);
}

Error BsvxWorld::set_voxel(const Vector3i &position, int64_t key, bool create_missing) {
	if (!require_open()) return ERR_UNCONFIGURED;
	const int64_t xs[1] = { position.x };
	const int64_t ys[1] = { position.y };
	const int64_t zs[1] = { position.z };
	const uint32_t keys[1] = { static_cast<uint32_t>(key) };
	size_t written = 0;
	const bsvx_result r = bsvx_world_set_voxels(ctx, world, xs, ys, zs, keys, 1, create_missing ? 1 : 0, &written);
	if (!check(r, "set_voxel")) return to_godot_error(r);
	if (written == 0) return ERR_DOES_NOT_EXIST; // no region there and create_missing was false
	emit_changed();
	return OK;
}

PackedInt32Array BsvxWorld::get_voxels(const PackedInt64Array &positions) const {
	PackedInt32Array out;
	if (!require_open()) return out;
	if (positions.size() % 3 != 0) {
		last_error = "positions must hold x, y, z triples";
		return out;
	}

	const size_t count = static_cast<size_t>(positions.size() / 3);
	if (count == 0) return out;

	// The ABI takes three parallel arrays; the flat triple form is what a scripting language can
	// build cheaply, so the split happens here rather than at every call site.
	std::vector<int64_t> xs(count), ys(count), zs(count);
	const int64_t *src = positions.ptr();
	for (size_t i = 0; i < count; ++i) {
		xs[i] = src[i * 3 + 0];
		ys[i] = src[i * 3 + 1];
		zs[i] = src[i * 3 + 2];
	}

	out.resize(static_cast<int64_t>(count));
	if (!check(bsvx_world_get_voxels(ctx, world, xs.data(), ys.data(), zs.data(), reinterpret_cast<uint32_t *>(out.ptrw()), count), "get_voxels")) {
		out.clear();
	}
	return out;
}

int64_t BsvxWorld::set_voxels(const PackedInt64Array &positions, const PackedInt32Array &keys, bool create_missing) {
	if (!require_open()) return -1;
	if (positions.size() % 3 != 0) {
		last_error = "positions must hold x, y, z triples";
		return -1;
	}

	const size_t count = static_cast<size_t>(positions.size() / 3);
	if (static_cast<size_t>(keys.size()) != count) {
		last_error = "keys must hold one entry per position triple";
		return -1;
	}
	if (count == 0) return 0;

	std::vector<int64_t> xs(count), ys(count), zs(count);
	const int64_t *src = positions.ptr();
	for (size_t i = 0; i < count; ++i) {
		xs[i] = src[i * 3 + 0];
		ys[i] = src[i * 3 + 1];
		zs[i] = src[i * 3 + 2];
	}

	size_t written = 0;
	const bsvx_result r = bsvx_world_set_voxels(ctx, world, xs.data(), ys.data(), zs.data(),
			reinterpret_cast<const uint32_t *>(keys.ptr()), count, create_missing ? 1 : 0, &written);
	if (!check(r, "set_voxels")) return -1;
	if (written > 0) emit_changed();
	return static_cast<int64_t>(written);
}

int64_t BsvxWorld::fill_box(const Vector3i &from, const Vector3i &to, int64_t key, bool create_missing) {
	if (!require_open()) return -1;
	size_t written = 0;
	const bsvx_result r = bsvx_world_fill_box(ctx, world,
			MIN(from.x, to.x), MIN(from.y, to.y), MIN(from.z, to.z),
			MAX(from.x, to.x), MAX(from.y, to.y), MAX(from.z, to.z),
			static_cast<uint32_t>(key), create_missing ? 1 : 0, &written);
	if (!check(r, "fill_box")) return -1;
	if (written > 0) emit_changed();
	return static_cast<int64_t>(written);
}

Dictionary BsvxWorld::locate_voxel(const Vector3i &position) const {
	Dictionary out;
	if (!require_open()) return out;

	bsvx_voxel_address address{};
	if (!check(bsvx_world_locate_voxel(world, position.x, position.y, position.z, &address), "locate_voxel")) return out;

	out["region"] = Vector3i(address.region_x, address.region_y, address.region_z);
	out["chunk"] = Vector3i(address.chunk_x, address.chunk_y, address.chunk_z);
	out["local"] = Vector3i(address.local_x, address.local_y, address.local_z);
	out["local_index"] = static_cast<int64_t>(address.local_index);
	return out;
}

// ---------------------------------------------------------------------------------------------
// registry
// ---------------------------------------------------------------------------------------------

PackedInt32Array BsvxWorld::get_registry_keys() const {
	PackedInt32Array out;
	if (!require_open()) return out;

	const size_t count = bsvx_world_registry_entry_count(world);
	if (count == 0) return out;

	std::vector<bsvx_registry_entry> entries(count);
	size_t written = 0;
	if (!check(bsvx_world_get_registry_entries(world, 0, count, entries.data(), &written), "get_registry_keys")) return out;

	out.resize(static_cast<int64_t>(written));
	int32_t *dst = out.ptrw();
	for (size_t i = 0; i < written; ++i) dst[i] = static_cast<int32_t>(entries[i].voxel_key);
	return out;
}

Dictionary BsvxWorld::get_registry_entry(int64_t voxel_key) const {
	Dictionary out;
	if (!require_open()) return out;

	const size_t count = bsvx_world_registry_entry_count(world);
	for (size_t i = 0; i < count; ++i) {
		bsvx_registry_entry entry{};
		if (bsvx_world_get_registry_entry(world, i, &entry) != BSVX_RESULT_OK) continue;
		if (entry.voxel_key != static_cast<uint32_t>(voxel_key)) continue;

		out["voxel_key"] = static_cast<int64_t>(entry.voxel_key);
		out["material_id"] = static_cast<int64_t>(entry.material_id);
		out["flags"] = static_cast<int64_t>(entry.flags);
		out["name"] = get_voxel_name(voxel_key);
		out["color"] = get_voxel_color(voxel_key);
		return out;
	}
	last_error = "voxel key " + itos(voxel_key) + " is not in the registry";
	return out;
}

Error BsvxWorld::set_registry_entry(int64_t voxel_key, int64_t material_id, int64_t flags) {
	if (!require_open()) return ERR_UNCONFIGURED;
	bsvx_registry_entry entry{};
	entry.voxel_key = static_cast<uint32_t>(voxel_key);
	entry.material_id = static_cast<uint32_t>(material_id);
	entry.flags = static_cast<uint32_t>(flags);
	// name_hash is derived from the name the library stores beside the entry; setting it by hand
	// would desynchronize the two, so it is deliberately left at zero here.
	const bsvx_result r = bsvx_world_set_registry_entry(world, &entry);
	if (!check(r, "set_registry_entry")) return to_godot_error(r);
	emit_changed();
	return OK;
}

Error BsvxWorld::remove_registry_entry(int64_t voxel_key) {
	if (!require_open()) return ERR_UNCONFIGURED;
	const bsvx_result r = bsvx_world_remove_registry_entry(world, static_cast<uint32_t>(voxel_key));
	if (!check(r, "remove_registry_entry")) return to_godot_error(r);
	emit_changed();
	return OK;
}

String BsvxWorld::get_voxel_name(int64_t voxel_key) const {
	if (!require_open()) return String();
	return read_string([&](char *b, size_t c, size_t *n) {
		return bsvx_world_get_registry_name(world, static_cast<uint32_t>(voxel_key), b, c, n);
	});
}

Error BsvxWorld::set_voxel_name(int64_t voxel_key, const String &name) {
	if (!require_open()) return ERR_UNCONFIGURED;
	const bsvx_result r = bsvx_world_set_registry_name(world, static_cast<uint32_t>(voxel_key), to_utf8(name).c_str());
	if (!check(r, "set_voxel_name")) return to_godot_error(r);
	emit_changed();
	return OK;
}

Color BsvxWorld::get_voxel_color(int64_t voxel_key) const {
	if (!require_open()) return Color(0, 0, 0, 0);

	// Material tint first: when a .btx is attached it is the authoritative appearance, and the
	// registry colour is the fallback that lets a palette mean something before any texture
	// archive exists. This ordering is the format's, not a choice made here -- which is exactly
	// why it belongs in the format layer rather than in each renderer.
	if (bsvx_world_texture_count(world) > 0) {
		const size_t count = bsvx_world_registry_entry_count(world);
		for (size_t i = 0; i < count; ++i) {
			bsvx_registry_entry entry{};
			if (bsvx_world_get_registry_entry(world, i, &entry) != BSVX_RESULT_OK) continue;
			if (entry.voxel_key != static_cast<uint32_t>(voxel_key)) continue;

			size_t row = 0;
			if (bsvx_texture_find_material(world, 0, entry.material_id, &row) != BSVX_RESULT_OK) break;
			bsvx_material_desc material{};
			if (bsvx_texture_get_material(world, 0, row, &material) != BSVX_RESULT_OK) break;
			if (material.tint_rgba8 != 0) return color_from_rgba8(material.tint_rgba8);
			break;
		}
	}

	uint32_t rgba = 0;
	if (bsvx_world_get_registry_color(world, static_cast<uint32_t>(voxel_key), &rgba) != BSVX_RESULT_OK) return Color(0, 0, 0, 0);
	// 0 means unset, which is why a fully transparent black is not expressible in the format.
	if (rgba == 0) return Color(0, 0, 0, 0);
	return color_from_rgba8(rgba);
}

Error BsvxWorld::set_voxel_color(int64_t voxel_key, const Color &color) {
	if (!require_open()) return ERR_UNCONFIGURED;
	const bsvx_result r = bsvx_world_set_registry_color(world, static_cast<uint32_t>(voxel_key), rgba8_from_color(color));
	if (!check(r, "set_voxel_color")) return to_godot_error(r);
	emit_changed();
	return OK;
}

PackedColorArray BsvxWorld::get_palette() const {
	PackedColorArray out;
	if (!require_open()) return out;

	const PackedInt32Array keys = get_registry_keys();
	int64_t highest = 0;
	for (int64_t i = 0; i < keys.size(); ++i) highest = MAX(highest, static_cast<int64_t>(keys[i]));
	if (highest <= 0) return out;

	// Indexed by voxel key so palette[key] just works, with index 0 -- air -- always transparent.
	out.resize(highest + 1);
	Color *dst = out.ptrw();
	dst[0] = Color(0, 0, 0, 0);
	for (int64_t i = 1; i <= highest; ++i) dst[i] = Color(0, 0, 0, 0);
	for (int64_t i = 0; i < keys.size(); ++i) {
		const int64_t key = static_cast<int64_t>(keys[i]);
		if (key > 0 && key <= highest) dst[key] = get_voxel_color(key);
	}
	return out;
}

Error BsvxWorld::make_palette(const PackedColorArray &colors, const String &texture_id, int64_t flags_for_all) {
	if (!require_open()) return ERR_UNCONFIGURED;
	if (colors.is_empty()) return ERR_INVALID_PARAMETER;

	std::vector<uint32_t> rgba(static_cast<size_t>(colors.size()));
	for (int64_t i = 0; i < colors.size(); ++i) rgba[static_cast<size_t>(i)] = rgba8_from_color(colors[i]);

	size_t tex_index = 0;
	const bsvx_result r = bsvx_world_make_palette(ctx, world, rgba.data(), rgba.size(),
			to_utf8(texture_id).c_str(), static_cast<uint32_t>(flags_for_all), &tex_index);
	if (!check(r, "make_palette")) return to_godot_error(r);
	emit_changed();
	return OK;
}

// ---------------------------------------------------------------------------------------------
// textures
// ---------------------------------------------------------------------------------------------

int64_t BsvxWorld::get_texture_count() const {
	if (!require_open()) return 0;
	return static_cast<int64_t>(bsvx_world_texture_count(world));
}

String BsvxWorld::get_texture_id(int64_t texture_index) const {
	if (!require_open() || texture_index < 0) return String();
	return read_string([&](char *b, size_t c, size_t *n) {
		return bsvx_world_get_texture_id(world, static_cast<size_t>(texture_index), b, c, n);
	});
}

String BsvxWorld::get_texture_path(int64_t texture_index) const {
	if (!require_open() || texture_index < 0) return String();
	return read_string([&](char *b, size_t c, size_t *n) {
		return bsvx_world_get_texture_path(world, static_cast<size_t>(texture_index), b, c, n);
	});
}

// ---------------------------------------------------------------------------------------------
// metadata
// ---------------------------------------------------------------------------------------------

PackedStringArray BsvxWorld::get_metadata_keys() const {
	PackedStringArray out;
	if (!require_open()) return out;

	const size_t count = bsvx_world_metadata_count(world);
	for (size_t i = 0; i < count; ++i) {
		const String key = read_string([&](char *b, size_t c, size_t *n) { return bsvx_world_get_metadata_key(world, i, b, c, n); });
		if (!key.is_empty()) out.push_back(key);
	}
	return out;
}

PackedByteArray BsvxWorld::get_metadata(const String &key) const {
	PackedByteArray out;
	if (!require_open()) return out;

	const std::string utf8_key = to_utf8(key);
	size_t needed = 0;
	bsvx_result r = bsvx_world_get_metadata(world, utf8_key.c_str(), nullptr, 0, &needed);
	if (r != BSVX_RESULT_OK && r != BSVX_RESULT_BUFFER_TOO_SMALL) {
		check(r, "get_metadata");
		return out;
	}
	if (needed == 0) return out;

	out.resize(static_cast<int64_t>(needed));
	size_t written = 0;
	r = bsvx_world_get_metadata(world, utf8_key.c_str(), out.ptrw(), needed, &written);
	if (!check(r, "get_metadata")) {
		out.clear();
		return out;
	}
	out.resize(static_cast<int64_t>(written));
	return out;
}

Error BsvxWorld::set_metadata(const String &key, const PackedByteArray &value) {
	if (!require_open()) return ERR_UNCONFIGURED;
	const bsvx_result r = bsvx_world_set_metadata(world, to_utf8(key).c_str(), value.ptr(), static_cast<size_t>(value.size()));
	if (!check(r, "set_metadata")) return to_godot_error(r);
	emit_changed();
	return OK;
}

Error BsvxWorld::remove_metadata(const String &key) {
	if (!require_open()) return ERR_UNCONFIGURED;
	const bsvx_result r = bsvx_world_remove_metadata(world, to_utf8(key).c_str());
	if (!check(r, "remove_metadata")) return to_godot_error(r);
	emit_changed();
	return OK;
}

// ---------------------------------------------------------------------------------------------
// validation and housekeeping
// ---------------------------------------------------------------------------------------------

TypedArray<Dictionary> BsvxWorld::validate(bool deep) {
	TypedArray<Dictionary> out;
	if (!require_open()) return out;

	size_t count = 0;
	const uint32_t flags = deep ? BSVX_VALIDATE_DEEP : BSVX_VALIDATE_DEFAULT;
	// OK here means the check ran, not that the world is clean -- the issues are the payload.
	if (!check(bsvx_world_validate(ctx, world, flags, &count), "validate")) return out;

	for (size_t i = 0; i < count; ++i) {
		bsvx_validation_issue issue{};
		if (bsvx_world_get_validation_issue(ctx, i, &issue) != BSVX_RESULT_OK) continue;

		Dictionary entry;
		entry["severity"] = static_cast<int64_t>(issue.severity);
		entry["code"] = static_cast<int64_t>(issue.code);
		entry["region_index"] = static_cast<int64_t>(issue.region_index);
		entry["chunk_ordinal"] = static_cast<int64_t>(issue.chunk_ordinal);
		entry["voxel_key"] = static_cast<int64_t>(issue.voxel_key);
		entry["message"] = read_string([&](char *b, size_t c, size_t *n) { return bsvx_world_get_validation_message(ctx, i, b, c, n); });
		out.push_back(entry);
	}
	return out;
}

bool BsvxWorld::is_dirty() const {
	if (!require_open()) return false;
	return bsvx_world_is_dirty(world) != 0;
}

Error BsvxWorld::clear_dirty() {
	if (!require_open()) return ERR_UNCONFIGURED;
	const bsvx_result r = bsvx_world_clear_dirty(world);
	return check(r, "clear_dirty") ? OK : to_godot_error(r);
}

int64_t BsvxWorld::compact() {
	if (!require_open()) return -1;
	size_t reclaimed = 0;
	if (!check(bsvx_world_compact(world, &reclaimed), "compact")) return -1;
	return static_cast<int64_t>(reclaimed);
}

// ---------------------------------------------------------------------------------------------
// library info
// ---------------------------------------------------------------------------------------------

int64_t BsvxWorld::get_abi_version() {
	return static_cast<int64_t>(bsvx_abi_version());
}

String BsvxWorld::get_build_info() {
	return String::utf8(bsvx_build_info());
}

String BsvxWorld::axis_convention_name(int64_t convention) {
	return String::utf8(bsvx_axis_convention_name(static_cast<uint32_t>(convention)));
}

// ---------------------------------------------------------------------------------------------
// binding
// ---------------------------------------------------------------------------------------------

void BsvxWorld::_bind_methods() {
	ClassDB::bind_method(D_METHOD("create", "chunk_size", "region_size"), &BsvxWorld::create);
	ClassDB::bind_method(D_METHOD("load_world", "path", "flags"), &BsvxWorld::load_world, DEFVAL(static_cast<int64_t>(LOAD_DEFAULT)));
	ClassDB::bind_method(D_METHOD("load_region", "path"), &BsvxWorld::load_region);
	ClassDB::bind_method(D_METHOD("load_region_bytes", "bytes"), &BsvxWorld::load_region_bytes);
	ClassDB::bind_method(D_METHOD("save_world", "path", "flags"), &BsvxWorld::save_world, DEFVAL(static_cast<int64_t>(SAVE_DEFAULT)));
	ClassDB::bind_method(D_METHOD("save_region", "path"), &BsvxWorld::save_region);
	ClassDB::bind_method(D_METHOD("save_region_bytes"), &BsvxWorld::save_region_bytes);
	ClassDB::bind_method(D_METHOD("is_open"), &BsvxWorld::is_open);
	ClassDB::bind_method(D_METHOD("close"), &BsvxWorld::close);
	ClassDB::bind_method(D_METHOD("get_last_error"), &BsvxWorld::get_last_error);
	ClassDB::bind_method(D_METHOD("get_warnings"), &BsvxWorld::get_warnings);

	ClassDB::bind_method(D_METHOD("get_chunk_size"), &BsvxWorld::get_chunk_size);
	ClassDB::bind_method(D_METHOD("get_region_size"), &BsvxWorld::get_region_size);
	ClassDB::bind_method(D_METHOD("get_region_extent"), &BsvxWorld::get_region_extent);
	ClassDB::bind_method(D_METHOD("get_world_name"), &BsvxWorld::get_world_name);
	ClassDB::bind_method(D_METHOD("set_world_name", "name"), &BsvxWorld::set_world_name);
	ClassDB::bind_method(D_METHOD("get_uuid"), &BsvxWorld::get_uuid);
	ClassDB::bind_method(D_METHOD("set_uuid", "uuid"), &BsvxWorld::set_uuid);
	ClassDB::bind_method(D_METHOD("get_axis_convention"), &BsvxWorld::get_axis_convention);
	ClassDB::bind_method(D_METHOD("convert_axis_convention", "target"), &BsvxWorld::convert_axis_convention);
	ClassDB::bind_method(D_METHOD("get_voxel_size"), &BsvxWorld::get_voxel_size);
	ClassDB::bind_method(D_METHOD("set_voxel_size", "size"), &BsvxWorld::set_voxel_size);
	ClassDB::bind_method(D_METHOD("get_origin"), &BsvxWorld::get_origin);
	ClassDB::bind_method(D_METHOD("set_origin", "origin"), &BsvxWorld::set_origin);

	ClassDB::bind_method(D_METHOD("get_region_count"), &BsvxWorld::get_region_count);
	ClassDB::bind_method(D_METHOD("get_region_coord", "region_index"), &BsvxWorld::get_region_coord);
	ClassDB::bind_method(D_METHOD("find_region", "coord"), &BsvxWorld::find_region);
	ClassDB::bind_method(D_METHOD("add_region", "coord"), &BsvxWorld::add_region);
	ClassDB::bind_method(D_METHOD("remove_region", "region_index"), &BsvxWorld::remove_region);
	ClassDB::bind_method(D_METHOD("prune_empty_regions"), &BsvxWorld::prune_empty_regions);

	ClassDB::bind_method(D_METHOD("get_chunk_count", "region_index"), &BsvxWorld::get_chunk_count);
	ClassDB::bind_method(D_METHOD("get_chunk_coords", "region_index"), &BsvxWorld::get_chunk_coords);
	ClassDB::bind_method(D_METHOD("has_chunk", "region_index", "chunk"), &BsvxWorld::has_chunk);
	ClassDB::bind_method(D_METHOD("get_chunk_info", "region_index", "chunk"), &BsvxWorld::get_chunk_info);
	ClassDB::bind_method(D_METHOD("get_chunk", "region_index", "chunk"), &BsvxWorld::get_chunk);
	ClassDB::bind_method(D_METHOD("set_chunk", "region_index", "chunk", "voxels"), &BsvxWorld::set_chunk);
	ClassDB::bind_method(D_METHOD("clear_chunk", "region_index", "chunk"), &BsvxWorld::clear_chunk);
	ClassDB::bind_method(D_METHOD("remove_chunk", "region_index", "chunk"), &BsvxWorld::remove_chunk);
	ClassDB::bind_method(D_METHOD("decode_region", "region_index"), &BsvxWorld::decode_region);

	ClassDB::bind_method(D_METHOD("get_voxel", "position"), &BsvxWorld::get_voxel);
	ClassDB::bind_method(D_METHOD("set_voxel", "position", "key", "create_missing"), &BsvxWorld::set_voxel, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("get_voxels", "positions"), &BsvxWorld::get_voxels);
	ClassDB::bind_method(D_METHOD("set_voxels", "positions", "keys", "create_missing"), &BsvxWorld::set_voxels, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("fill_box", "from", "to", "key", "create_missing"), &BsvxWorld::fill_box, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("locate_voxel", "position"), &BsvxWorld::locate_voxel);

	ClassDB::bind_method(D_METHOD("get_registry_keys"), &BsvxWorld::get_registry_keys);
	ClassDB::bind_method(D_METHOD("get_registry_entry", "voxel_key"), &BsvxWorld::get_registry_entry);
	ClassDB::bind_method(D_METHOD("set_registry_entry", "voxel_key", "material_id", "flags"), &BsvxWorld::set_registry_entry);
	ClassDB::bind_method(D_METHOD("remove_registry_entry", "voxel_key"), &BsvxWorld::remove_registry_entry);
	ClassDB::bind_method(D_METHOD("get_voxel_name", "voxel_key"), &BsvxWorld::get_voxel_name);
	ClassDB::bind_method(D_METHOD("set_voxel_name", "voxel_key", "name"), &BsvxWorld::set_voxel_name);
	ClassDB::bind_method(D_METHOD("get_voxel_color", "voxel_key"), &BsvxWorld::get_voxel_color);
	ClassDB::bind_method(D_METHOD("set_voxel_color", "voxel_key", "color"), &BsvxWorld::set_voxel_color);
	ClassDB::bind_method(D_METHOD("get_palette"), &BsvxWorld::get_palette);
	ClassDB::bind_method(D_METHOD("make_palette", "colors", "texture_id", "flags_for_all"), &BsvxWorld::make_palette,
			DEFVAL(static_cast<int64_t>(REGISTRY_OPAQUE)));

	ClassDB::bind_method(D_METHOD("get_texture_count"), &BsvxWorld::get_texture_count);
	ClassDB::bind_method(D_METHOD("get_texture_id", "texture_index"), &BsvxWorld::get_texture_id);
	ClassDB::bind_method(D_METHOD("get_texture_path", "texture_index"), &BsvxWorld::get_texture_path);

	ClassDB::bind_method(D_METHOD("get_metadata_keys"), &BsvxWorld::get_metadata_keys);
	ClassDB::bind_method(D_METHOD("get_metadata", "key"), &BsvxWorld::get_metadata);
	ClassDB::bind_method(D_METHOD("set_metadata", "key", "value"), &BsvxWorld::set_metadata);
	ClassDB::bind_method(D_METHOD("remove_metadata", "key"), &BsvxWorld::remove_metadata);

	ClassDB::bind_method(D_METHOD("validate", "deep"), &BsvxWorld::validate, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("is_dirty"), &BsvxWorld::is_dirty);
	ClassDB::bind_method(D_METHOD("clear_dirty"), &BsvxWorld::clear_dirty);
	ClassDB::bind_method(D_METHOD("compact"), &BsvxWorld::compact);

	ClassDB::bind_static_method("BsvxWorld", D_METHOD("get_abi_version"), &BsvxWorld::get_abi_version);
	ClassDB::bind_static_method("BsvxWorld", D_METHOD("get_build_info"), &BsvxWorld::get_build_info);
	ClassDB::bind_static_method("BsvxWorld", D_METHOD("axis_convention_name", "convention"), &BsvxWorld::axis_convention_name);

	BIND_ENUM_CONSTANT(LOAD_DEFAULT);
	BIND_ENUM_CONSTANT(LOAD_IGNORE_HASH_MISMATCH);
	BIND_ENUM_CONSTANT(LOAD_SKIP_TEXTURES);
	BIND_ENUM_CONSTANT(LOAD_SKIP_REGIONS);

	BIND_ENUM_CONSTANT(SAVE_DEFAULT);
	BIND_ENUM_CONSTANT(SAVE_NON_ATOMIC);
	BIND_ENUM_CONSTANT(SAVE_BACKUP);
	BIND_ENUM_CONSTANT(SAVE_PRUNE_ORPHANS);
	BIND_ENUM_CONSTANT(SAVE_DIRTY_ONLY);
	BIND_ENUM_CONSTANT(SAVE_COMPACT_FIRST);
	BIND_ENUM_CONSTANT(SAVE_DRY_RUN);

	BIND_ENUM_CONSTANT(REGISTRY_OPAQUE);
	BIND_ENUM_CONSTANT(REGISTRY_EMISSIVE);
	BIND_ENUM_CONSTANT(REGISTRY_SPECIAL);
	BIND_ENUM_CONSTANT(REGISTRY_COLLIDABLE);

	BIND_ENUM_CONSTANT(AXIS_X_RIGHT_Y_UP_Z_FORWARD);
	BIND_ENUM_CONSTANT(AXIS_X_RIGHT_Z_UP_Y_FORWARD);
	BIND_ENUM_CONSTANT(AXIS_X_RIGHT_Y_UP_Z_BACK);

	BIND_ENUM_CONSTANT(SEVERITY_INFO);
	BIND_ENUM_CONSTANT(SEVERITY_WARNING);
	BIND_ENUM_CONSTANT(SEVERITY_ERROR);
}
