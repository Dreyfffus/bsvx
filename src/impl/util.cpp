#include "util.h"
#include <cassert>
#include <stdexcept>
#include <bit>
#include <sstream>

namespace bsvx {

	void append_bytes(std::vector<std::byte>& out, std::span<const std::byte> bytes) {
		out.insert(out.end(), bytes.begin(), bytes.end());
	}

	void append_bytes(std::vector<std::byte>& out, std::vector<std::byte> bytes) {
		out.insert(out.end(), bytes.begin(), bytes.end());
	}

	void resize_with_zeroes(std::vector<std::byte>& out, uint64_t new_size) {
		if (new_size > out.size()) {
			out.resize(static_cast<size_t>(new_size), std::byte(0));
		}
	}

	uint64_t align64(uint64_t value, uint64_t alignment) {
		assert(alignment != 0);
		const uint64_t mask = alignment - 1;
		return (value + mask) & ~mask;
	}

	void seek_abs(std::ostream& os, uint64_t offset) {
		os.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
		if (!os) throw std::runtime_error("[btx] : seekp failed");
	}

	void seek_abs(std::istream& is, uint64_t offset) {
		is.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
		if (!is) throw std::runtime_error("[btx] : seekg failed");
	}

	uint64_t fnv1a64(std::span<const std::byte> bytes) {
		uint64_t hash = 14695981039346656037ull;
		for (std::byte b : bytes) {
			hash ^= static_cast<uint8_t>(b);
			hash *= 1099511628211ull;
		}
		return hash;
	}

	uint64_t fnv1a64(std::string_view s) {
		return fnv1a64(std::as_bytes(std::span(s.data(), s.size())));
	}

	uint8_t bit_width_u32(uint32_t v)
	{
		if (v <= 1u) return 1u;
#if __cpp_lib_int_pow2 >= 202002L
		return static_cast<uint8_t>(std::bit_width(v));
#else
		uint8_t bits = 0;
		uint32_t x = v - 1u;
		while (x != 0u) {
			x >>= 1u;
			++bits;
		}
		return bits == 0 ? 1u : bits;
#endif
	}

	std::string u64_hex(uint64_t v)
	{
		std::ostringstream oss;
		oss << "0x" << std::hex << std::uppercase << v;
		return oss.str();
	}

}