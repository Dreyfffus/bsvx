#include "bsvx_gd_vfs.h"
#include "bsvx_gd_util.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>

#include <cstring>

using namespace godot;

namespace bsvxgd {

GodotVfs::GodotVfs() {
	vfs.user = this;
	vfs.read_file = &GodotVfs::read_file;
	vfs.file_exists = &GodotVfs::file_exists;
	vfs.list_dir = &GodotVfs::list_dir;
}

int GodotVfs::read_file(void *, const char *path, void *out, size_t capacity, size_t *out_size) {
	if (out_size == nullptr) return 0;
	*out_size = 0;

	const String gd_path = String::utf8(path);
	Ref<FileAccess> file = FileAccess::open(gd_path, FileAccess::READ);
	if (file.is_null()) return 0;

	const int64_t length = file->get_length();
	if (length < 0) return 0;
	*out_size = static_cast<size_t>(length);

	// The contract is deliberate: report the size and write nothing when the buffer is short.
	// The library then calls back with a buffer that fits, so this is the (NULL, 0) probe path
	// as well as the genuinely-too-small one.
	if (out == nullptr || capacity < static_cast<size_t>(length)) return 1;

	if (length > 0) {
		const PackedByteArray bytes = file->get_buffer(length);
		if (bytes.size() != length) return 0;
		std::memcpy(out, bytes.ptr(), static_cast<size_t>(length));
	}
	return 1;
}

int GodotVfs::file_exists(void *, const char *path) {
	return FileAccess::file_exists(String::utf8(path)) ? 1 : 0;
}

int GodotVfs::list_dir(void *user, const char *dir_path, size_t index, char *out_name, size_t capacity, size_t *out_size) {
	GodotVfs *self = static_cast<GodotVfs *>(user);
	if (self == nullptr || out_size == nullptr) return 0;
	*out_size = 0;

	const String dir = String::utf8(dir_path);
	if (!self->cache_valid || self->cached_dir != dir) {
		self->cached_files = DirAccess::get_files_at(dir);
		self->cached_dir = dir;
		self->cache_valid = true;
	}

	if (index >= static_cast<size_t>(self->cached_files.size())) return 0;

	const std::string name = to_utf8(self->cached_files[static_cast<int64_t>(index)]);
	*out_size = name.size() + 1u;
	if (out_name == nullptr || capacity < *out_size) return 1;

	std::memcpy(out_name, name.c_str(), *out_size);
	return 1;
}

} // namespace bsvxgd
