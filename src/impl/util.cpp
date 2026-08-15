#include "util.h"
#include <atomic>
#include <bit>
#include <cassert>
#include <sstream>
#include <stdexcept>

#if !defined(_WIN32) && (defined(__unix__) || defined(__APPLE__))
#include <fcntl.h>
#include <unistd.h>
#define BSVX_HAS_FSYNC 1
#endif

namespace bsvx {

	namespace {
		// Only has to be unique among the temp files this process has in flight at once.
		std::atomic<uint64_t> g_temp_counter{ 0 };

		void fsync_path(const std::filesystem::path& path)
		{
#if defined(BSVX_HAS_FSYNC)
			const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
			if (fd < 0) return;
			::fsync(fd);
			::close(fd);
#else
			(void)path;
#endif
		}
	}

	void write_file_direct(const std::filesystem::path& path, std::span<const std::byte> bytes)
	{
		std::ofstream os(path, std::ios::binary | std::ios::trunc);
		if (!os) throw std::runtime_error("[bsvx]: could not open for writing: " + path_to_utf8(path));
		if (!bytes.empty()) {
			os.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		}
		os.close();
		if (!os) throw std::runtime_error("[bsvx]: failed writing: " + path_to_utf8(path));
	}

	void write_file_atomic(const std::filesystem::path& path, std::span<const std::byte> bytes, bool backup)
	{
		const auto parent = path.parent_path();
		if (!parent.empty()) std::filesystem::create_directories(parent);

		std::filesystem::path temp = path;
		temp += ".bsvx-tmp-" + std::to_string(g_temp_counter.fetch_add(1, std::memory_order_relaxed));

		try {
			write_file_direct(temp, bytes);
			// The rename below only publishes the directory entry; without this the *contents* may
			// still be in the page cache when the machine loses power.
			fsync_path(temp);

			std::error_code ec;
			if (backup && std::filesystem::exists(path, ec)) {
				std::filesystem::path bak = path;
				bak += ".bak";
				std::filesystem::remove(bak, ec);
				std::filesystem::rename(path, bak, ec);
				if (ec) std::filesystem::copy_file(path, bak, std::filesystem::copy_options::overwrite_existing, ec);
			}

			std::filesystem::rename(temp, path, ec);
			if (ec) {
				// Crossing a filesystem boundary (or a Win32 sharing violation) -- fall back to an
				// in-place write rather than leaving the temp file behind and the target untouched.
				std::filesystem::remove(temp, ec);
				write_file_direct(path, bytes);
			}
		}
		catch (...) {
			std::error_code ec;
			std::filesystem::remove(temp, ec);
			throw;
		}
	}

	std::filesystem::path path_from_utf8(std::string_view utf8)
	{
		return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
	}

	std::string path_to_utf8(const std::filesystem::path& path)
	{
		const std::u8string s = path.generic_u8string();
		return std::string(reinterpret_cast<const char*>(s.data()), s.size());
	}

	std::string to_hex(std::span<const std::byte> bytes)
	{
		static constexpr char digits[] = "0123456789abcdef";
		std::string out;
		out.reserve(bytes.size() * 2u);
		for (std::byte b : bytes) {
			const uint8_t v = static_cast<uint8_t>(b);
			out.push_back(digits[v >> 4]);
			out.push_back(digits[v & 0x0Fu]);
		}
		return out;
	}

	bool from_hex(std::string_view text, std::vector<std::byte>& out)
	{
		if (text.size() % 2u != 0u) return false;

		const auto nibble = [](char c) -> int {
			if (c >= '0' && c <= '9') return c - '0';
			if (c >= 'a' && c <= 'f') return c - 'a' + 10;
			if (c >= 'A' && c <= 'F') return c - 'A' + 10;
			return -1;
			};

		out.clear();
		out.reserve(text.size() / 2u);
		for (size_t i = 0; i < text.size(); i += 2u) {
			const int hi = nibble(text[i]);
			const int lo = nibble(text[i + 1u]);
			if (hi < 0 || lo < 0) return false;
			out.push_back(static_cast<std::byte>((hi << 4) | lo));
		}
		return true;
	}

	bool is_printable_utf8(std::span<const std::byte> bytes)
	{
		size_t i = 0;
		while (i < bytes.size()) {
			const uint8_t c = static_cast<uint8_t>(bytes[i]);
			if (c < 0x20u || c == 0x7Fu) return false;   // control characters, including NUL
			if (c < 0x80u) { ++i; continue; }

			size_t extra = 0;
			if ((c & 0xE0u) == 0xC0u) extra = 1;
			else if ((c & 0xF0u) == 0xE0u) extra = 2;
			else if ((c & 0xF8u) == 0xF0u) extra = 3;
			else return false;

			if (i + extra >= bytes.size()) return false;
			for (size_t k = 1; k <= extra; ++k) {
				if ((static_cast<uint8_t>(bytes[i + k]) & 0xC0u) != 0x80u) return false;
			}
			i += extra + 1u;
		}
		return true;
	}

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