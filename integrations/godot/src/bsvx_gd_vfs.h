#pragma once

#include <bsvx_dll.h>

#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

namespace bsvxgd {

// A manifest world is a directory of files, and bsvx_world_load opens them itself -- which works
// in the editor and not at all in an exported game, where res:// lives inside a .pck and has no
// filesystem path. The library's answer is bsvx_vfs: hand it the host's reader instead.
//
// Godot's FileAccess already resolves res://, user:// and pack files uniformly, so the bridge is
// thin. Paths arrive with their scheme prefix intact and always use '/'.
struct GodotVfs {
	GodotVfs();

	const bsvx_vfs *desc() const { return &vfs; }

private:
	bsvx_vfs vfs{};

	// list_dir is called with a rising index for the same directory, so re-enumerating on every
	// call would make discovery quadratic in the number of regions. One cached listing is enough
	// because the library never interleaves two directories.
	mutable godot::String cached_dir;
	mutable godot::PackedStringArray cached_files;
	mutable bool cache_valid = false;

	static int read_file(void *user, const char *path, void *out, size_t capacity, size_t *out_size);
	static int file_exists(void *user, const char *path);
	static int list_dir(void *user, const char *dir_path, size_t index, char *out_name, size_t capacity, size_t *out_size);
};

} // namespace bsvxgd
