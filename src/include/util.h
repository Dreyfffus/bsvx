#pragma once
#include <cstring>
#include <fstream>
#include <type_traits>
#include <vector>
#include <span>

namespace bsvx {

	template <typename T>
	concept TriviallySerializable = std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>;


	template <TriviallySerializable T>
	void write_pod(std::ostream& os, const T& value) {
		os.write(reinterpret_cast<const char*>(&value), sizeof(T));
		if (!os) throw std::runtime_error("[btx] : failed to write POD");
	}

	template<TriviallySerializable T>
	void append_pod(std::vector<std::byte>& bytes, const T& value) {
		const size_t old = bytes.size();
		bytes.resize(old + sizeof(T));
		std::memcpy(bytes.data() + old, &value, sizeof(T));
	}

	template <TriviallySerializable T>
	void read_pod(std::istream& is, T& value) {
		is.read(reinterpret_cast<char*>(&value), sizeof(T));
		if (!is) throw std::runtime_error("[btx] : failed to read POD");
	}

	template<TriviallySerializable T>
	T read_pod(std::span<const std::byte> bytes, size_t& cursor) {
		if (cursor + sizeof(T) > bytes.size()) throw std::runtime_error("[bvx]: blob read out of bounds");
		T out{};
		std::memcpy(&out, bytes.data() + cursor, sizeof(T));
		cursor += sizeof(T);
		return out;
	}

	template <TriviallySerializable T>
	void write_raw(std::ostream& os, const std::vector<T>& values) {
		if (!values.empty()) {
			os.write(reinterpret_cast<const char*>(values.data()), static_cast<std::streamsize>(values.size() * sizeof(T)));
			if (!os) throw std::runtime_error("[btx] : failed to write vector");
		}
	}

	template<TriviallySerializable T>
	void append_raw(std::vector<std::byte>& out, std::span<const T> value) {
		if (value.empty()) return;
		const size_t old = out.size();
		out.resize(old + value.size_bytes());
		std::memcpy(out.data() + old, value.data(), value.size_bytes());
	}

	template <TriviallySerializable T>
	void read_raw(std::istream& is, std::vector<T>& values, size_t count) {
		values.resize(count);
		if (count != 0) {
			is.read(reinterpret_cast<char*>(values.data()), static_cast<std::streamsize>(count * sizeof(T)));
			if (!is) throw std::runtime_error("[btx] : failed to read vector");
		}
	}

	void append_bytes(std::vector<std::byte>& out, std::span<const std::byte> bytes);

	void append_bytes(std::vector<std::byte>& out, std::vector<std::byte> bytes);

	
	void resize_with_zeroes(std::vector<std::byte>& out, uint64_t new_size);

	uint64_t align64(uint64_t value, uint64_t alignment);

	void seek_abs(std::ostream& os, uint64_t offset);

	void seek_abs(std::istream& is, uint64_t offset);

	uint64_t fnv1a64(std::span<const std::byte> bytes);

	uint64_t fnv1a64(std::string_view s);

	uint8_t bit_width_u32(uint32_t v);

	std::string u64_hex(uint64_t v);


	template <class Enum>
	constexpr std::underlying_type_t<Enum> to_underlying(Enum e) noexcept
	{
		static_assert(std::is_enum_v<Enum>, "bsvx::to_underlying requires an enum type");
		return static_cast<std::underlying_type_t<Enum>>(e);
	}

	template<typename Enum>
	constexpr Enum bit_or(Enum a, Enum b) noexcept {
		return static_cast<Enum>(to_underlying(a) | to_underlying(b));
	}

	template<typename Enum>
	constexpr bool has_bits(Enum a, Enum mask) noexcept {
		return (to_underlying(a) & to_underlying(mask)) == to_underlying(mask);
	}

}


