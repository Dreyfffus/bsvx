#pragma once

#include <bsvx_dll.h>

#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/string.hpp>

#include <string>
#include <vector>

namespace bsvxgd {

// Every path and string crossing the C ABI is UTF-8 on every platform, Windows included, so
// String::utf8() is the only correct narrowing -- to_ascii() and the implicit char* conversion
// both lose non-ASCII characters.
inline std::string to_utf8(const godot::String &s) {
	const godot::CharString cs = s.utf8();
	return std::string(cs.get_data(), static_cast<size_t>(cs.length()));
}

inline godot::String from_utf8(const char *bytes, size_t size) {
	if (bytes == nullptr || size == 0) return godot::String();
	return godot::String::utf8(bytes, static_cast<int64_t>(size));
}

// The library's string getters share one convention: the required size *including* the NUL lands
// in *out_size, BUFFER_TOO_SMALL means nothing was written, and (NULL, 0) is a legal size probe.
// Every call site would otherwise repeat the same two-pass dance.
template <typename Getter>
godot::String read_string(Getter &&getter) {
	size_t needed = 0;
	// The probe reports BUFFER_TOO_SMALL, not OK -- zero capacity cannot hold even the NUL. It
	// only returns OK for the empty-string case, so both have to be accepted here; treating
	// BUFFER_TOO_SMALL as failure silently empties every string this binding reads.
	const bsvx_result probe = getter(nullptr, 0, &needed);
	if (probe != BSVX_RESULT_OK && probe != BSVX_RESULT_BUFFER_TOO_SMALL) return godot::String();
	if (needed <= 1) return godot::String();

	std::vector<char> buffer(needed);
	size_t written = 0;
	if (getter(buffer.data(), buffer.size(), &written) != BSVX_RESULT_OK) return godot::String();

	// written counts the terminator; the String must not.
	return from_utf8(buffer.data(), written > 0 ? written - 1 : 0);
}

// Godot surfaces failures as Error, so the ABI's result codes have to land somewhere sensible.
// The mapping is deliberately lossy in one direction only: get_last_error() still carries the
// library's own message, which is the part a user acts on.
inline godot::Error to_godot_error(bsvx_result result) {
	switch (result) {
		case BSVX_RESULT_OK: return godot::OK;
		case BSVX_RESULT_INVALID_ARGUMENT: return godot::ERR_INVALID_PARAMETER;
		case BSVX_RESULT_NOT_FOUND: return godot::ERR_DOES_NOT_EXIST;
		case BSVX_RESULT_BUFFER_TOO_SMALL: return godot::ERR_OUT_OF_MEMORY;
		case BSVX_RESULT_CANCELLED: return godot::ERR_SKIP;
		default: return godot::FAILED;
	}
}

} // namespace bsvxgd
