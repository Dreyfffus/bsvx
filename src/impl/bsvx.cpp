
#include "bsvx.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <charconv>

#define TOML_HEADER_ONLY 0
#include <toml.hpp>

namespace bsvx {
    namespace {

        using I32x3 = std::array<int32_t, 3>;
        using U16x3 = std::array<uint16_t, 3>;

        static uint64_t fnv1a64_bytes(std::span<const std::byte> bytes)
        {
            uint64_t h = 14695981039346656037ull;
            for (std::byte b : bytes) {
                h ^= static_cast<uint8_t>(b);
                h *= 1099511628211ull;
            }
            return h;
        }

        static uint64_t fnv1a64_string(std::string_view s)
        {
            return fnv1a64_bytes(std::as_bytes(std::span(s.data(), s.size())));
        }

        static std::vector<std::byte> read_file_bytes(const std::filesystem::path& path)
        {
            std::ifstream is(path, std::ios::binary);
            if (!is) {
                throw std::runtime_error("[bsvx]: Parser: could not open file: " + path_to_utf8(path));
            }

            is.seekg(0, std::ios::end);
            const auto size = static_cast<size_t>(is.tellg());
            is.seekg(0, std::ios::beg);

            std::vector<std::byte> bytes(size);
            if (size != 0) {
                is.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                if (!is) {
                    throw std::runtime_error("[bsvx]: Parser: failed to read file: " + path_to_utf8(path));
                }
            }
            return bytes;
        }

        class NativeFileSystem final : public FileSystem {
        public:
            std::filesystem::path normalize(const std::filesystem::path& path) const override
            {
                return std::filesystem::absolute(path).lexically_normal();
            }

            bool is_file(const std::filesystem::path& path) const override
            {
                std::error_code ec;
                return std::filesystem::is_regular_file(path, ec);
            }

            bool is_directory(const std::filesystem::path& path) const override
            {
                std::error_code ec;
                return std::filesystem::is_directory(path, ec);
            }

            std::vector<std::byte> read_file(const std::filesystem::path& path) const override
            {
                return read_file_bytes(path);
            }

            std::vector<std::filesystem::path> list_files(const std::filesystem::path& dir, std::string_view extension) const override
            {
                std::vector<std::filesystem::path> out;
                std::error_code ec;
                if (!std::filesystem::is_directory(dir, ec)) return out;

                for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
                    if (ec) break;
                    if (!entry.is_regular_file()) continue;
                    if (entry.path().extension() != extension) continue;
                    out.push_back(entry.path().filename());
                }
                return out;
            }
        };

        class NativeFileWriter final : public FileWriter {
        public:
            void write_file(const std::filesystem::path& path, std::span<const std::byte> bytes, bool atomic, bool backup) override
            {
                if (atomic) write_file_atomic(path, bytes, backup);
                else write_file_direct(path, bytes);
            }

            void make_directories(const std::filesystem::path& dir) override
            {
                if (!dir.empty()) std::filesystem::create_directories(dir);
            }

            bool exists(const std::filesystem::path& path) const override
            {
                std::error_code ec;
                return std::filesystem::exists(path, ec);
            }

            void remove_file(const std::filesystem::path& path) override
            {
                std::error_code ec;
                std::filesystem::remove(path, ec);
            }

            std::vector<std::filesystem::path> list_files(const std::filesystem::path& dir, std::string_view extension) const override
            {
                std::vector<std::filesystem::path> out;
                std::error_code ec;
                if (!std::filesystem::is_directory(dir, ec)) return out;

                for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
                    if (ec) break;
                    if (!entry.is_regular_file()) continue;
                    if (entry.path().extension() != extension) continue;
                    out.push_back(entry.path().filename());
                }
                return out;
            }

            std::vector<std::byte> read_file(const std::filesystem::path& path) const override
            {
                std::error_code ec;
                if (!std::filesystem::is_regular_file(path, ec)) return {};
                return read_file_bytes(path);
            }
        };

        static btx::Archive load_btx(const FileSystem& fs, const std::filesystem::path& path)
        {
            try {
                const auto bytes = fs.read_file(path);
                return btx::Archive::load_from_memory(std::span(bytes.data(), bytes.size()));
            }
            catch (const std::exception& e) {
                throw std::runtime_error("[bsvx]: Parser: could not load .btx '" + path_to_utf8(path) + "': " + e.what());
            }
        }

        static std::string trim_nul_padded(const char* cstr, size_t max_len)
        {
            size_t n = 0;
            while (n < max_len && cstr[n] != '\0') {
                ++n;
            }
            return std::string(cstr, cstr + n);
        }

        static std::filesystem::path make_absolute_from_root(const std::filesystem::path& root,
            const std::filesystem::path& relative_or_absolute)
        {
            if (relative_or_absolute.is_absolute()) {
                return relative_or_absolute.lexically_normal();
            }
            return (root / relative_or_absolute).lexically_normal();
        }

        static std::string require_string(const toml::node_view<const toml::node>& view, std::string_view what)
        {
            if (auto value = view.value<std::string>()) {
                return *value;
            }
            if (auto value_sv = view.value<std::string_view>()) {
                return std::string(*value_sv);
            }
            throw std::runtime_error("[bsvx]: Parser: expected string for " + std::string(what));
        }

        static std::optional<std::string> optional_string(const toml::node_view<const toml::node>& view)
        {
            if (auto value = view.value<std::string>()) {
                return *value;
            }
            if (auto value_sv = view.value<std::string_view>()) {
                return std::string(*value_sv);
            }
            return std::nullopt;
        }

        static int64_t require_i64(const toml::node_view<const toml::node>& view, std::string_view what)
        {
            if (auto value = view.value<int64_t>()) {
                return *value;
            }
            throw std::runtime_error("[bsvx]: Parser: expected integer for " + std::string(what));
        }

        template <typename NodeType>
        static uint64_t optional_u64(const toml::node_view<NodeType>& view, uint64_t fallback = 0)
        {
            if (auto value = view.template value<int64_t>()) {
                return static_cast<uint64_t>(*value);
            }
            if (auto value = view.template value<uint64_t>()) {
                return *value;
            }
            if (auto value = view.template value<std::string_view>()) {
                uint64_t out = 0;
                auto text = *value;
                if (text.starts_with("0x") || text.starts_with("0X")) text.remove_prefix(2);

                const auto first = text.data();
                const auto* last = text.data() + text.size();
                auto res = std::from_chars(first, last, out, 16);
                if (res.ec == std::errc{} && res.ptr == last) return out;
            }
            return fallback;
        }

        template <typename T, size_t N>
        static std::array<T, N> require_int_array(const toml::node_view<const toml::node>& view, std::string_view what)
        {
            auto* arr = view.as_array();
            if (!arr || arr->size() != N) {
                throw std::runtime_error("[bsvx]: Parser: expected array[" + std::to_string(N) + "] for " + std::string(what));
            }

            std::array<T, N> out{};
            for (size_t i = 0; i < N; ++i) {
                auto maybe = (*arr)[i].template value<int64_t>();
                if (!maybe) {
                    throw std::runtime_error("[bsvx]: Parser: expected integer elements in " + std::string(what));
                }
                out[i] = static_cast<T>(*maybe);
            }
            return out;
        }

        static uint32_t parse_registry_flags(const toml::node_view<const toml::node>& view)
        {
            if (auto direct = view.value<int64_t>()) {
                return static_cast<uint32_t>(*direct);
            }

            uint32_t flags = 0;
            if (auto* arr = view.as_array()) {
                for (const auto& node : *arr) {
                    const auto maybe = node.value<std::string_view>();
                    if (!maybe) {
                        throw std::runtime_error("[bsvx]: Parser: registry flags array must contain strings");
                    }
                    const std::string_view s = *maybe;
                    if (s == "opaque") flags |= to_underlying(RegistryFlags::OPAQUE);
                    else if (s == "emissive") flags |= to_underlying(RegistryFlags::EMISSIVE);
                    else if (s == "special") flags |= to_underlying(RegistryFlags::SPECIAL);
                    else if (s == "collidable") flags |= to_underlying(RegistryFlags::COLLIDABLE);
                    else throw std::runtime_error("[bsvx]: Parser: unknown registry flag string: " + std::string(s));
                }
            }
            return flags;
        }

        static void parse_geometry(Manifest& manifest, const toml::table& tbl)
        {
            auto geometry = tbl["geometry"];
            if (!geometry) {
                throw std::runtime_error("[bsvx]: Parser: manifest is missing [geometry]");
            }

            const auto chunk_size = require_int_array<uint16_t, 3>(geometry["chunk_size"], "geometry.chunk_size");
            const auto region_size = require_int_array<uint16_t, 3>(geometry["region_size"], "geometry.region_size");

            manifest.world_desc.geometry.chunk_size_x = chunk_size[0];
            manifest.world_desc.geometry.chunk_size_y = chunk_size[1];
            manifest.world_desc.geometry.chunk_size_z = chunk_size[2];

            manifest.world_desc.geometry.region_size_x = region_size[0];
            manifest.world_desc.geometry.region_size_y = region_size[1];
            manifest.world_desc.geometry.region_size_z = region_size[2];

            if (auto axis = optional_string(geometry["axis_convention"])) {
                const auto parsed = axis_convention_from_name(*axis);
                if (!parsed) {
                    throw std::runtime_error("[bsvx]::Parser: unsupported geometry.axis_convention: " + *axis);
                }
                manifest.world_desc.axis_convention = *parsed;
            }

            if (auto schema = optional_string(geometry["voxel_schema"])) {
                if (*schema == "dense_u32_voxel_key") {
                    manifest.world_desc.voxel_schema = VoxelSchema::DENSE_U32_VOXEL_KEY;
                }
                else {
                    throw std::runtime_error("[bsvx]::Parser: unsupported geometry.voxel_schema: " + *schema);
                }
            }
        }

        static void parse_bounds(Manifest& manifest, const toml::table& tbl)
        {
            auto bounds = tbl["bounds"];
            if (!bounds) {
                manifest.world_desc.bounds_mode = BoundsMode::UNBOUNDED;
                return;
            }

            const std::string mode = optional_string(bounds["mode"]).value_or("unbounded");
            if (mode == "unbounded") {
                manifest.world_desc.bounds_mode = BoundsMode::UNBOUNDED;
            }
            else if (mode == "explicit") {
                manifest.world_desc.bounds_mode = BoundsMode::EXPLICIT;
                const auto mn = require_int_array<int32_t, 3>(bounds["min_region"], "bounds.min_region");
                const auto mx = require_int_array<int32_t, 3>(bounds["max_region"], "bounds.max_region");
                manifest.world_desc.world_min_region_x = mn[0];
                manifest.world_desc.world_min_region_y = mn[1];
                manifest.world_desc.world_min_region_z = mn[2];
                manifest.world_desc.world_max_region_x = mx[0];
                manifest.world_desc.world_max_region_y = mx[1];
                manifest.world_desc.world_max_region_z = mx[2];
            }
            else {
                throw std::runtime_error("[bsvx]: Parser: bounds.mode must be 'unbounded' or 'explicit'");
            }
        }

        static void parse_paths(Manifest& manifest, const toml::table& tbl)
        {
            if (auto paths = tbl["paths"].as_table()) {
                if (auto value = optional_string((*paths)["regions_dir"])) {
                    manifest.regions_dir = *value;
                }
                if (auto value = optional_string((*paths)["textures_dir"])) {
                    manifest.textures_dir = *value;
                }
            }

            if (auto value = optional_string(tbl["regions_dir"])) {
                manifest.regions_dir = *value;
            }
            if (auto value = optional_string(tbl["textures_dir"])) {
                manifest.textures_dir = *value;
            }
        }

        // manifest_hash is always the hash of the manifest's raw bytes, whatever [hashes] claims --
        // every .bvx is validated against it, so it has to describe the file that was actually read.
        static void parse_hashes(Manifest& manifest, const toml::table& tbl, uint64_t manifest_bytes_hash)
        {
            if (auto hashes = tbl["hashes"].as_table()) {
                manifest.world_desc.registry_hash = optional_u64((*hashes)["registry"], manifest.world_desc.registry_hash);
                manifest.world_desc.manifest_hash = optional_u64((*hashes)["manifest"], manifest.world_desc.manifest_hash);
            }

            manifest.world_desc.manifest_hash = manifest_bytes_hash;
        }

        static std::optional<double> optional_double(const toml::node_view<const toml::node>& view)
        {
            if (auto value = view.value<double>()) return *value;
            if (auto value = view.value<int64_t>()) return static_cast<double>(*value);
            return std::nullopt;
        }

        // voxel_size accepts a scalar (uniform) or a 3-array (non-uniform); origin is always a
        // 3-array of world-space units.
        static void parse_units(Manifest& manifest, const toml::table& tbl)
        {
            auto units = tbl["units"];
            if (!units) return;

            const auto element = [](const toml::array& arr, size_t index, double fallback) {
                const auto* node = arr.get(index);
                if (!node) return fallback;
                if (auto value = node->value<double>()) return *value;
                if (auto value = node->value<int64_t>()) return static_cast<double>(*value);
                return fallback;
                };

            if (auto scalar = optional_double(units["voxel_size"])) {
                manifest.world_desc.units.voxel_size_x = *scalar;
                manifest.world_desc.units.voxel_size_y = *scalar;
                manifest.world_desc.units.voxel_size_z = *scalar;
            }
            else if (auto* arr = units["voxel_size"].as_array()) {
                if (arr->size() != 3) throw std::runtime_error("[bsvx]: Parser: units.voxel_size must be a number or an array[3]");
                manifest.world_desc.units.voxel_size_x = element(*arr, 0, 1.0);
                manifest.world_desc.units.voxel_size_y = element(*arr, 1, 1.0);
                manifest.world_desc.units.voxel_size_z = element(*arr, 2, 1.0);
            }

            if (auto* arr = units["origin"].as_array()) {
                if (arr->size() != 3) throw std::runtime_error("[bsvx]: Parser: units.origin must be an array[3]");
                manifest.world_desc.units.origin_x = element(*arr, 0, 0.0);
                manifest.world_desc.units.origin_y = element(*arr, 1, 0.0);
                manifest.world_desc.units.origin_z = element(*arr, 2, 0.0);
            }
        }

        // Values are opaque bytes. Printable UTF-8 is written as a plain string so a human can read
        // and edit it; anything else round-trips through { hex = "..." } without loss.
        static void parse_metadata(Manifest& manifest, const toml::table& tbl)
        {
            manifest.metadata.clear();

            auto* meta = tbl["metadata"].as_table();
            if (!meta) return;

            for (const auto& [key, node] : *meta) {
                std::vector<std::byte> value;

                if (auto text = optional_string(toml::node_view<const toml::node>(node))) {
                    const auto bytes = std::as_bytes(std::span(text->data(), text->size()));
                    value.assign(bytes.begin(), bytes.end());
                }
                else if (auto* inner = node.as_table()) {
                    const auto hex = optional_string((*inner)["hex"]);
                    if (!hex || !from_hex(*hex, value)) {
                        throw std::runtime_error("[bsvx]: Parser: metadata." + std::string(key.str()) + " must be a string or { hex = \"...\" }");
                    }
                }
                else {
                    throw std::runtime_error("[bsvx]: Parser: metadata." + std::string(key.str()) + " must be a string or { hex = \"...\" }");
                }

                manifest.metadata.emplace(std::string(key.str()), std::move(value));
            }
        }

        static void parse_registry(Manifest& manifest, const toml::table& tbl)
        {
            auto registry = tbl["registry"];
            if (!registry) {
                return;
            }

            auto* arr = registry.as_array();
            if (!arr) {
                throw std::runtime_error("[bsvx]: Parser: [[registry]] must be an array of tables");
            }

            manifest.world_desc.registry_entries.clear();
            manifest.world_desc.registry_entries.reserve(arr->size());

            uint64_t rolling_hash = 14695981039346656037ull;

            for (const auto& node : *arr) {
                auto* entry_tbl = node.as_table();
                if (!entry_tbl) {
                    throw std::runtime_error("[bsvx]: Parser: [[registry]] entries must be tables");
                }

                bvx::RegistryEntry entry{};
                entry.voxel_key = static_cast<uint32_t>(require_i64((*entry_tbl)["voxel_key"], "registry.voxel_key"));
                entry.material_id = static_cast<uint32_t>(optional_u64((*entry_tbl)["material_id"], 0));
                entry.flags = parse_registry_flags((*entry_tbl)["flags"]);
                if (auto name = optional_string((*entry_tbl)["name"])) {
                    entry.name_hash = fnv1a64_string(*name);
                    // Kept, not merely hashed. FNV-1a is not invertible, so hashing and discarding
                    // destroyed every voxel type's name on the first save.
                    if (!name->empty()) manifest.world_desc.registry_names[entry.voxel_key] = *name;
                }
                else {
                    entry.name_hash = optional_u64((*entry_tbl)["name_hash"], 0);
                }

                // A display colour that does not depend on a .btx existing yet -- authoring tools
                // are colour-first, and a world can be built long before it has textures.
                if ((*entry_tbl)["color"]) {
                    const uint32_t color = static_cast<uint32_t>(optional_u64((*entry_tbl)["color"], 0));
                    if (color != 0u) manifest.world_desc.registry_colors[entry.voxel_key] = color;
                }

                manifest.world_desc.registry_entries.push_back(entry);

                const std::byte* raw = reinterpret_cast<const std::byte*>(&entry);
                for (size_t i = 0; i < sizeof(entry); ++i) {
                    rolling_hash ^= static_cast<uint8_t>(raw[i]);
                    rolling_hash *= 1099511628211ull;
                }
            }

            if (manifest.world_desc.registry_hash == 0) {
                manifest.world_desc.registry_hash = rolling_hash;
            }
        }

        static void parse_textures(Manifest& manifest, const toml::table& tbl)
        {
            manifest.textures.clear();
            manifest.world_desc.texture_refs.clear();

            auto textures = tbl["textures"];
            if (!textures) {
                return;
            }

            auto* arr = textures.as_array();
            if (!arr) {
                throw std::runtime_error("[bsvx]: Parser: [[textures]] must be an array of tables");
            }

            manifest.textures.reserve(arr->size());
            manifest.world_desc.texture_refs.reserve(arr->size());

            for (const auto& node : *arr) {
                auto* tex_tbl = node.as_table();
                if (!tex_tbl) {
                    throw std::runtime_error("[bsvx]: Parser: [[textures]] entries must be tables");
                }

                TextureReference ref{};
                ref.id = optional_string((*tex_tbl)["id"]).value_or("");
                ref.relative_path = require_string((*tex_tbl)["path"], "textures.path");
                ref.absolute_path = make_absolute_from_root(manifest.root_dir, manifest.textures_dir / ref.relative_path);
                if (ref.id.empty()) {
                    ref.id = path_to_utf8(ref.relative_path.stem());
                }
                ref.path_hash = fnv1a64_string(ref.relative_path.generic_string());
                ref.content_hash = optional_u64((*tex_tbl)["content_hash"], 0);

                manifest.textures.push_back(ref);

                bvx::BtxRef btx_ref{};
                const std::string path_str = (manifest.textures_dir / ref.relative_path).generic_string();
                const size_t count = std::min(path_str.size(), sizeof(btx_ref.relative_path) - 1);
                std::memcpy(btx_ref.relative_path, path_str.data(), count);
                btx_ref.relative_path[count] = '\0';
                btx_ref.path_hash = ref.path_hash;
                btx_ref.content_hash = ref.content_hash;
                manifest.world_desc.texture_refs.push_back(btx_ref);
            }
        }

        static void parse_regions(Manifest& manifest, const toml::table& tbl)
        {
            manifest.regions.clear();

            auto regions = tbl["regions"];
            if (!regions) {
                return;
            }

            auto* arr = regions.as_array();
            if (!arr) {
                throw std::runtime_error("[bsvx]: Parser: [[regions]] must be an array of tables");
            }

            manifest.regions.reserve(arr->size());

            for (const auto& node : *arr) {
                auto* reg_tbl = node.as_table();
                if (!reg_tbl) {
                    throw std::runtime_error("[bsvx]: Parser: [[regions]] entries must be tables");
                }

                RegionReference ref{};
                ref.relative_path = require_string((*reg_tbl)["path"], "regions.path");
                ref.absolute_path = make_absolute_from_root(manifest.root_dir, manifest.regions_dir / ref.relative_path);

                if ((*reg_tbl)["coord"]) {
                    const auto coord = require_int_array<int32_t, 3>((*reg_tbl)["coord"], "regions.coord");
                    ref.coord = coord;
                }

                manifest.regions.push_back(std::move(ref));
            }
        }

        static void parse_world_metadata(Manifest& manifest, const toml::table& tbl)
        {
            if (auto world = tbl["world"].as_table()) {
                manifest.name = optional_string((*world)["name"]).value_or(manifest.name);
                manifest.uuid = optional_string((*world)["uuid"]).value_or(manifest.uuid);
            }

            if (manifest.name.empty()) {
                manifest.name = optional_string(tbl["name"]).value_or(path_to_utf8(manifest.root_dir.filename()));
            }
            if (manifest.uuid.empty()) {
                manifest.uuid = optional_string(tbl["uuid"]).value_or("");
            }

            manifest.world_desc.asset_name_hash = fnv1a64_string(manifest.name);
        }

        static void synthesize_missing_texture_refs(Manifest& manifest)
        {
            manifest.world_desc.texture_refs.clear();
            manifest.world_desc.texture_refs.reserve(manifest.textures.size());

            for (const TextureReference& ref : manifest.textures) {
                bvx::BtxRef btx_ref{};
                const std::string path_str = (manifest.textures_dir / ref.relative_path).generic_string();
                const size_t count = std::min(path_str.size(), sizeof(btx_ref.relative_path) - 1);
                std::memcpy(btx_ref.relative_path, path_str.data(), count);
                btx_ref.relative_path[count] = '\0';
                btx_ref.path_hash = ref.path_hash;
                btx_ref.content_hash = ref.content_hash;
                manifest.world_desc.texture_refs.push_back(btx_ref);
            }
        }



        static uint64_t compute_registry_hash(const bvx::WorldDesc& world_desc)
        {
            if (world_desc.registry_hash != 0) {
                return world_desc.registry_hash;
            }

            uint64_t rolling_hash = 14695981039346656037ull;
            for (const bvx::RegistryEntry& entry : world_desc.registry_entries) {
                const std::byte* raw = reinterpret_cast<const std::byte*>(&entry);
                for (size_t i = 0; i < sizeof(entry); ++i) {
                    rolling_hash ^= static_cast<uint8_t>(raw[i]);
                    rolling_hash *= 1099511628211ull;
                }
            }
            return rolling_hash;
        }

        static std::string to_string(AxisConvention axis)
        {
            return std::string(axis_convention_name(axis));
        }

        static std::string to_string(VoxelSchema schema)
        {
            switch (schema) {
            case VoxelSchema::DENSE_U32_VOXEL_KEY: return "dense_u32_voxel_key";
            default: throw std::runtime_error("[bsvx]: Parser: unsupported VoxelSchema");
            }
        }

        static std::string to_string(BoundsMode mode)
        {
            switch (mode) {
            case BoundsMode::UNBOUNDED: return "unbounded";
            case BoundsMode::EXPLICIT:  return "explicit";
            default: throw std::runtime_error("[bsvx]: Parser: unsupported BoundsMode");
            }
        }

        static toml::array registry_flags_to_array(uint32_t flags)
        {
            toml::array out{};
            if (flags & to_underlying(RegistryFlags::OPAQUE))     out.push_back("opaque");
            if (flags & to_underlying(RegistryFlags::EMISSIVE))   out.push_back("emissive");
            if (flags & to_underlying(RegistryFlags::SPECIAL))    out.push_back("special");
            if (flags & to_underlying(RegistryFlags::COLLIDABLE)) out.push_back("collidable");
            return out;
        }

        static std::string trim_btx_relative_path(const bvx::BtxRef& ref)
        {
            return trim_nul_padded(ref.relative_path, sizeof(ref.relative_path));
        }

        static std::filesystem::path choose_texture_relative_path(const TextureAsset& asset, size_t index)
        {
            if (!asset.ref.relative_path.empty()) {
                return asset.ref.relative_path.lexically_normal();
            }
            if (!asset.ref.id.empty()) {
                return std::filesystem::path(asset.ref.id + ".btx");
            }
            return std::filesystem::path("texture_" + std::to_string(index) + ".btx");
        }

        static std::filesystem::path choose_region_relative_path(const RegionAsset& asset, size_t index)
        {
            if (!asset.ref.relative_path.empty()) {
                return asset.ref.relative_path.lexically_normal();
            }
            const auto coord = asset.ref.coord.value_or(std::array<int32_t, 3>{ asset.archive.region_x, asset.archive.region_y, asset.archive.region_z });
            return std::filesystem::path(
                "r_" + std::to_string(coord[0]) + "_" + std::to_string(coord[1]) + "_" + std::to_string(coord[2]) + ".bvx");
        }

        static bvx::BtxRef make_btx_ref(std::string_view relative_path, uint64_t content_hash = 0)
        {
            bvx::BtxRef btx_ref{};
            const size_t count = std::min(relative_path.size(), sizeof(btx_ref.relative_path) - 1);
            std::memcpy(btx_ref.relative_path, relative_path.data(), count);
            btx_ref.relative_path[count] = '\0';
            btx_ref.path_hash = fnv1a64_string(relative_path);
            btx_ref.content_hash = content_hash;
            return btx_ref;
        }

        static toml::table build_manifest_table(const Manifest& manifest)
        {
            toml::table tbl{};

            tbl.insert("format_version", static_cast<int64_t>(manifest.format_version));
            if (!manifest.name.empty()) tbl.insert("name", manifest.name);
            if (!manifest.uuid.empty()) tbl.insert("uuid", manifest.uuid);

            toml::table geometry{};
            geometry.insert("chunk_size", toml::array{
                static_cast<int64_t>(manifest.world_desc.geometry.chunk_size_x),
                static_cast<int64_t>(manifest.world_desc.geometry.chunk_size_y),
                static_cast<int64_t>(manifest.world_desc.geometry.chunk_size_z)
                });
            geometry.insert("region_size", toml::array{
                static_cast<int64_t>(manifest.world_desc.geometry.region_size_x),
                static_cast<int64_t>(manifest.world_desc.geometry.region_size_y),
                static_cast<int64_t>(manifest.world_desc.geometry.region_size_z)
                });
            geometry.insert("voxel_schema", to_string(manifest.world_desc.voxel_schema));
            geometry.insert("axis_convention", to_string(manifest.world_desc.axis_convention));
            tbl.insert("geometry", geometry);

            toml::table bounds{};
            bounds.insert("mode", to_string(manifest.world_desc.bounds_mode));
            if (manifest.world_desc.bounds_mode == BoundsMode::EXPLICIT) {
                bounds.insert("min_region", toml::array{
                    static_cast<int64_t>(manifest.world_desc.world_min_region_x),
                    static_cast<int64_t>(manifest.world_desc.world_min_region_y),
                    static_cast<int64_t>(manifest.world_desc.world_min_region_z)
                    });
                bounds.insert("max_region", toml::array{
                    static_cast<int64_t>(manifest.world_desc.world_max_region_x),
                    static_cast<int64_t>(manifest.world_desc.world_max_region_y),
                    static_cast<int64_t>(manifest.world_desc.world_max_region_z)
                    });
            }
            tbl.insert("bounds", bounds);

            toml::table paths{};
            paths.insert("regions_dir", manifest.regions_dir.generic_string());
            paths.insert("textures_dir", manifest.textures_dir.generic_string());
            tbl.insert("paths", paths);

            toml::table hashes{};
            hashes.insert("registry", u64_hex(compute_registry_hash(manifest.world_desc)));
            tbl.insert("hashes", hashes);

            if (!manifest.world_desc.units.is_default()) {
                toml::table units{};
                const auto& u = manifest.world_desc.units;
                if (u.voxel_size_x == u.voxel_size_y && u.voxel_size_y == u.voxel_size_z) {
                    units.insert("voxel_size", u.voxel_size_x);
                }
                else {
                    units.insert("voxel_size", toml::array{ u.voxel_size_x, u.voxel_size_y, u.voxel_size_z });
                }
                units.insert("origin", toml::array{ u.origin_x, u.origin_y, u.origin_z });
                tbl.insert("units", std::move(units));
            }

            if (!manifest.metadata.empty()) {
                toml::table metadata{};
                for (const auto& [key, value] : manifest.metadata) {
                    const std::span<const std::byte> bytes(value.data(), value.size());
                    if (is_printable_utf8(bytes)) {
                        metadata.insert(key, std::string(reinterpret_cast<const char*>(value.data()), value.size()));
                    }
                    else {
                        toml::table wrapper{};
                        wrapper.insert("hex", to_hex(bytes));
                        metadata.insert(key, std::move(wrapper));
                    }
                }
                tbl.insert("metadata", std::move(metadata));
            }

            if (!manifest.world_desc.registry_entries.empty()) {
                toml::array reg{};
                for (const bvx::RegistryEntry& entry : manifest.world_desc.registry_entries) {
                    toml::table e{};
                    e.insert("voxel_key", static_cast<int64_t>(entry.voxel_key));
                    e.insert("material_id", static_cast<int64_t>(entry.material_id));
                    e.insert("flags", registry_flags_to_array(entry.flags));

                    const std::string name = manifest.world_desc.registry_name(entry.voxel_key);
                    if (!name.empty()) e.insert("name", name);
                    e.insert("name_hash", u64_hex(entry.name_hash));

                    if (const uint32_t color = manifest.world_desc.registry_color(entry.voxel_key); color != 0u) {
                        e.insert("color", u64_hex(color));
                    }

                    reg.push_back(std::move(e));
                }
                tbl.insert("registry", std::move(reg));
            }

            if (!manifest.textures.empty()) {
                toml::array textures{};
                for (const TextureReference& ref : manifest.textures) {
                    toml::table t{};
                    if (!ref.id.empty()) t.insert("id", ref.id);
                    t.insert("path", ref.relative_path.generic_string());
                    if (ref.content_hash != 0) t.insert("content_hash", u64_hex(ref.content_hash));
                    textures.push_back(std::move(t));
                }
                tbl.insert("textures", std::move(textures));
            }

            if (!manifest.regions.empty()) {
                toml::array regions{};
                for (const RegionReference& ref : manifest.regions) {
                    toml::table r{};
                    r.insert("path", ref.relative_path.generic_string());
                    if (ref.coord) {
                        r.insert("coord", toml::array{
                            static_cast<int64_t>((*ref.coord)[0]),
                            static_cast<int64_t>((*ref.coord)[1]),
                            static_cast<int64_t>((*ref.coord)[2])
                            });
                    }
                    regions.push_back(std::move(r));
                }
                tbl.insert("regions", std::move(regions));
            }

            return tbl;
        }

        static std::string serialize_manifest_toml(const Manifest& manifest)
        {
            std::ostringstream oss;
            oss << build_manifest_table(manifest);
            return oss.str();
        }

        static void ensure_parent_dir(const std::filesystem::path& file_path)
        {
            const auto parent = file_path.parent_path();
            if (!parent.empty()) {
                std::filesystem::create_directories(parent);
            }
        }

        // Everything save_world does to a package before a single byte is written: fills in default
        // directories, assigns the relative path each asset will live at, and rebuilds the texture
        // refs and registry hash. Factored out so save_manifest_to_string produces exactly the text
        // save_world would -- the manifest hash is taken over those bytes, so "exactly" matters.
        static Manifest normalize_for_save(const WorldPackage& package, const std::filesystem::path& root_dir, const std::filesystem::path& manifest_path)
        {
            Manifest manifest = package.manifest;
            manifest.synthetic_from_standalone_region = false;
            manifest.root_dir = root_dir;
            manifest.manifest_path = manifest_path;
            if (manifest.regions_dir.empty()) manifest.regions_dir = "regions";
            if (manifest.textures_dir.empty()) manifest.textures_dir = "textures";
            if (manifest.name.empty()) manifest.name = path_to_utf8(root_dir.filename());

            manifest.textures.clear();
            for (size_t i = 0; i < package.textures.size(); ++i) {
                const TextureAsset& asset = package.textures[i];
                TextureReference ref = asset.ref;
                ref.relative_path = choose_texture_relative_path(asset, i);
                ref.absolute_path = (root_dir / manifest.textures_dir / ref.relative_path).lexically_normal();
                if (ref.id.empty()) {
                    ref.id = path_to_utf8(ref.relative_path.stem());
                }
                ref.path_hash = fnv1a64_string((manifest.textures_dir / ref.relative_path).generic_string());
                manifest.textures.push_back(std::move(ref));
            }

            manifest.regions.clear();
            for (size_t i = 0; i < package.regions.size(); ++i) {
                const RegionAsset& asset = package.regions[i];
                RegionReference ref = asset.ref;
                ref.relative_path = choose_region_relative_path(asset, i);
                ref.absolute_path = (root_dir / manifest.regions_dir / ref.relative_path).lexically_normal();
                if (!ref.coord) {
                    ref.coord = std::array<int32_t, 3>{ asset.archive.region_x, asset.archive.region_y, asset.archive.region_z };
                }
                manifest.regions.push_back(std::move(ref));
            }

            manifest.world_desc.texture_refs.clear();
            manifest.world_desc.texture_refs.reserve(manifest.textures.size());
            for (const TextureReference& ref : manifest.textures) {
                const std::string path_str = (manifest.textures_dir / ref.relative_path).generic_string();
                // A BtxRef holds 127 characters. Truncating produced a reference that silently
                // never resolved, so refuse instead -- the caller can shorten the path or the id.
                if (path_str.size() > bvx::BTX_REF_PATH_CAPACITY) {
                    throw std::runtime_error("[bsvx]: Parser: texture path exceeds " + std::to_string(bvx::BTX_REF_PATH_CAPACITY) + " characters: " + path_str);
                }
                manifest.world_desc.texture_refs.push_back(make_btx_ref(path_str, ref.content_hash));
            }
            manifest.world_desc.registry_hash = compute_registry_hash(manifest.world_desc);
            return manifest;
        }

        // Files of the given extension sitting in a managed directory that the manifest no longer
        // references. Without this, a removed region is re-discovered on the next load and the
        // deletion undoes itself.
        static size_t prune_orphans(const Manifest& manifest, FileWriter& writer, const std::filesystem::path& dir, std::string_view extension, bool dry_run)
        {
            std::unordered_set<std::string> keep;
            for (const RegionReference& ref : manifest.regions) keep.insert(ref.absolute_path.generic_string());
            for (const TextureReference& ref : manifest.textures) keep.insert(ref.absolute_path.generic_string());

            size_t removed = 0;
            for (const auto& name : writer.list_files(dir, extension)) {
                const auto full = (dir / name).lexically_normal();
                if (keep.contains(full.generic_string())) continue;

                ++removed;
                if (!dry_run) writer.remove_file(full);
            }
            return removed;
        }

        static std::pair<std::filesystem::path, std::filesystem::path>
            resolve_root_and_manifest_path(const std::filesystem::path& root_or_manifest_path)
        {
            if (root_or_manifest_path.has_extension() && root_or_manifest_path.extension() == ".toml") {
                auto manifest_path = root_or_manifest_path.lexically_normal();
                return { manifest_path.parent_path(), manifest_path };
            }

            auto root = root_or_manifest_path.lexically_normal();
            return { root, root / "manifest.toml" };
        }

    } // namespace

    const FileSystem& native_filesystem()
    {
        static const NativeFileSystem instance{};
        return instance;
    }

    FileWriter& native_filewriter()
    {
        // Stateless, so one shared instance is safe for concurrent saves to different worlds.
        static NativeFileWriter instance{};
        return instance;
    }

    std::filesystem::path Parser::resolve_manifest_path(const std::filesystem::path& path, const FileSystem& fs)
    {
        if (fs.is_file(path)) {
            if (path.extension() == ".toml") {
                return path.lexically_normal();
            }
            throw std::runtime_error("[bsvx]: Parser: expected a manifest .toml file or a root directory");
        }

        if (!fs.is_directory(path)) {
            throw std::runtime_error("[bsvx]: Parser: path does not exist: " + path_to_utf8(path));
        }

        const auto manifest = path / "manifest.toml";
        if (!fs.is_file(manifest)) {
            throw std::runtime_error("[bsvx]: Parser: manifest.toml not found in directory: " + path_to_utf8(path));
        }
        return manifest.lexically_normal();
    }

    Manifest Parser::parse_manifest(const std::filesystem::path& manifest_path, const FileSystem& fs)
    {
        const auto absolute_manifest = fs.normalize(resolve_manifest_path(manifest_path, fs));

        // Read once and parse the bytes: the manifest hash has to be taken over exactly what was
        // parsed, and a VFS has no path for toml::parse_file to open.
        const auto manifest_bytes = fs.read_file(absolute_manifest);
        const std::string_view manifest_text(reinterpret_cast<const char*>(manifest_bytes.data()), manifest_bytes.size());

        toml::table tbl;
        try {
            tbl = toml::parse(manifest_text, path_to_utf8(absolute_manifest));
        }
        catch (const toml::parse_error& err) {
            std::ostringstream oss;
            oss << "[bsvx]: Parser: failed to parse manifest '" << path_to_utf8(absolute_manifest) << "': " << err;
            throw std::runtime_error(oss.str());
        }

        Manifest manifest{};
        manifest.manifest_path = absolute_manifest;
        manifest.root_dir = absolute_manifest.parent_path();
        manifest.format_version = static_cast<uint32_t>(optional_u64(tbl["format_version"], 1));

        parse_world_metadata(manifest, tbl);
        parse_geometry(manifest, tbl);
        parse_bounds(manifest, tbl);
        parse_paths(manifest, tbl);
        parse_hashes(manifest, tbl, fnv1a64_bytes(std::span(manifest_bytes.data(), manifest_bytes.size())));
        parse_units(manifest, tbl);
        parse_metadata(manifest, tbl);
        parse_registry(manifest, tbl);
        parse_textures(manifest, tbl);
        parse_regions(manifest, tbl);

        if (manifest.textures.empty()) {
            auto_discover_textures(manifest, fs);
            synthesize_missing_texture_refs(manifest);
        }
        if (manifest.regions.empty()) {
            auto_discover_regions(manifest, fs);
        }

        return manifest;
    }

    void Parser::auto_discover_textures(Manifest& manifest, const FileSystem& fs)
    {
        manifest.textures.clear();

        const auto texture_root = make_absolute_from_root(manifest.root_dir, manifest.textures_dir);
        for (const auto& file_name : fs.list_files(texture_root, ".btx")) {
            TextureReference ref{};
            ref.relative_path = file_name;
            ref.absolute_path = (texture_root / file_name).lexically_normal();
            ref.id = path_to_utf8(ref.relative_path.stem());
            ref.path_hash = fnv1a64_string((manifest.textures_dir / ref.relative_path).generic_string());
            ref.content_hash = 0;
            manifest.textures.push_back(std::move(ref));
        }

        std::sort(manifest.textures.begin(), manifest.textures.end(),
            [](const TextureReference& a, const TextureReference& b) {
                return a.relative_path.generic_string() < b.relative_path.generic_string();
            });
    }

    void Parser::auto_discover_regions(Manifest& manifest, const FileSystem& fs)
    {
        manifest.regions.clear();

        const auto region_root = make_absolute_from_root(manifest.root_dir, manifest.regions_dir);
        for (const auto& file_name : fs.list_files(region_root, ".bvx")) {
            RegionReference ref{};
            ref.relative_path = file_name;
            ref.absolute_path = (region_root / file_name).lexically_normal();
            manifest.regions.push_back(std::move(ref));
        }

        std::sort(manifest.regions.begin(), manifest.regions.end(),
            [](const RegionReference& a, const RegionReference& b) {
                return a.relative_path.generic_string() < b.relative_path.generic_string();
            });
    }

    WorldPackage Parser::make_standalone_package(bvx::Archive&& archive,
        const std::filesystem::path& root_dir,
        const std::string& name,
        const std::filesystem::path& region_file_name,
        bool load_textures,
        const FileSystem& fs)
    {
        WorldPackage pkg{};
        pkg.manifest.synthetic_from_standalone_region = true;
        pkg.manifest.manifest_path.clear();
        pkg.manifest.root_dir = root_dir;
        pkg.manifest.name = name;
        pkg.manifest.format_version = 1;
        pkg.manifest.world_desc = *archive.standalone;
        pkg.manifest.world_desc.manifest_hash = archive.manifest_hash;
        pkg.manifest.world_desc.registry_hash = archive.registry_hash;

        RegionReference region_ref{};
        region_ref.relative_path = region_file_name;
        if (!region_file_name.empty() && !root_dir.empty()) {
            region_ref.absolute_path = (root_dir / region_file_name).lexically_normal();
        }
        region_ref.coord = std::array<int32_t, 3>{ archive.region_x, archive.region_y, archive.region_z };
        pkg.manifest.regions.push_back(region_ref);

        RegionAsset region_asset{};
        region_asset.ref = region_ref;
        region_asset.archive = std::move(archive);
        pkg.regions.push_back(std::move(region_asset));

        for (const bvx::BtxRef& btx_ref : pkg.manifest.world_desc.texture_refs) {
            TextureReference tex_ref{};
            tex_ref.relative_path = trim_nul_padded(btx_ref.relative_path, sizeof(btx_ref.relative_path));
            tex_ref.absolute_path = make_absolute_from_root(pkg.manifest.root_dir, tex_ref.relative_path);
            tex_ref.id = path_to_utf8(tex_ref.relative_path.stem());
            tex_ref.path_hash = btx_ref.path_hash;
            tex_ref.content_hash = btx_ref.content_hash;
            pkg.manifest.textures.push_back(tex_ref);
        }

        if (!load_textures) {
            return pkg;
        }

        if (pkg.manifest.textures.empty()) {
            pkg.manifest.textures_dir = ".";
            auto_discover_textures(pkg.manifest, fs);
            synthesize_missing_texture_refs(pkg.manifest);
        }

        for (const TextureReference& ref : pkg.manifest.textures) {
            TextureAsset asset{};
            asset.ref = ref;
            asset.archive = load_btx(fs, ref.absolute_path);
            pkg.textures.push_back(std::move(asset));
        }

        return pkg;
    }

    WorldPackage Parser::load_region(const std::filesystem::path& region_path, const FileSystem& fs)
    {
        const auto absolute_region = fs.normalize(region_path);
        auto region_archive = bvx::Archive::load_from_memory(fs.read_file(absolute_region));
        if (!region_archive.is_standalone()) {
            throw std::runtime_error("[bsvx]: Parser: region is not standalone: " + path_to_utf8(absolute_region));
        }

        return make_standalone_package(std::move(region_archive),
            absolute_region.parent_path(),
            path_to_utf8(absolute_region.stem()),
            absolute_region.filename(),
            true,
            fs);
    }

    WorldPackage Parser::load_region_memory(std::span<const std::byte> bytes, const std::filesystem::path& texture_root, const FileSystem& fs)
    {
        return wrap_standalone_region(bvx::Archive::load_from_memory(bytes), texture_root, fs);
    }

    WorldPackage Parser::wrap_standalone_region(bvx::Archive&& archive, const std::filesystem::path& texture_root, const FileSystem& fs)
    {
        if (!archive.is_standalone()) {
            throw std::runtime_error("[bsvx]: Parser: region is not standalone");
        }

        const bool load_textures = !texture_root.empty();
        const auto root_dir = load_textures ? fs.normalize(texture_root) : std::filesystem::path{};

        return make_standalone_package(std::move(archive), root_dir, "memory_region", {}, load_textures, fs);
    }

    WorldPackage Parser::create_world(const bvx::GeometryDesc& geometry)
    {
        if (geometry.chunk_size_x == 0 || geometry.chunk_size_y == 0 || geometry.chunk_size_z == 0 ||
            geometry.region_size_x == 0 || geometry.region_size_y == 0 || geometry.region_size_z == 0) {
            throw std::invalid_argument("[bsvx]: Parser: geometry dimensions must all be non-zero");
        }
        // Chunk-local AABBs in a ChunkSummary are uint8_t.
        if (geometry.chunk_size_x > 255 || geometry.chunk_size_y > 255 || geometry.chunk_size_z > 255) {
            throw std::invalid_argument("[bsvx]: Parser: chunk dimensions must be <= 255");
        }

        WorldPackage pkg{};
        pkg.manifest.format_version = 1;
        pkg.manifest.regions_dir = "regions";
        pkg.manifest.textures_dir = "textures";
        pkg.manifest.world_desc.geometry = geometry;
        pkg.manifest.world_desc.voxel_schema = VoxelSchema::DENSE_U32_VOXEL_KEY;
        pkg.manifest.world_desc.axis_convention = AxisConvention::X_RIGHT_Y_UP_Z_FORWARD;
        pkg.manifest.world_desc.bounds_mode = BoundsMode::UNBOUNDED;
        return pkg;
    }

    WorldPackage Parser::load_world(const std::filesystem::path& manifest_or_root, const FileSystem& fs, const LoadOptions& options)
    {
        if (manifest_or_root.extension() == ".bvx" && fs.is_file(manifest_or_root)) {
            return load_region(manifest_or_root, fs);
        }

        const auto warn = [&](std::string message) {
            if (options.warnings) options.warnings->push_back(std::move(message));
            };
        const auto tick = [&](std::string_view stage, size_t done, size_t total) {
            if (options.progress && !options.progress(stage, done, total)) throw CancelledError{};
            };

        WorldPackage pkg{};
        pkg.manifest = parse_manifest(manifest_or_root, fs);

        if (!options.skip_textures) {
            for (size_t i = 0; i < pkg.manifest.textures.size(); ++i) {
                tick("textures", i, pkg.manifest.textures.size());

                const TextureReference& ref = pkg.manifest.textures[i];
                TextureAsset asset{};
                asset.ref = ref;
                asset.archive = load_btx(fs, ref.absolute_path);
                pkg.textures.push_back(std::move(asset));
            }
        }

        if (options.skip_regions) return pkg;

        for (size_t i = 0; i < pkg.manifest.regions.size(); ++i) {
            tick("regions", i, pkg.manifest.regions.size());

            const RegionReference& ref = pkg.manifest.regions[i];
            bvx::Archive reg_archive = bvx::Archive::load_from_memory(fs.read_file(ref.absolute_path));

            if (ref.coord) {
                const auto expected = *ref.coord;
                if (reg_archive.region_x != expected[0] ||
                    reg_archive.region_y != expected[1] ||
                    reg_archive.region_z != expected[2]) {
                    throw std::runtime_error("[bsvx]: Parser: region coordinate mismatch for " + path_to_utf8(ref.absolute_path));
                }
            }

            // The manifest hash is FNV over the manifest's raw bytes, so a trailing newline or a
            // CRLF checkout invalidates every region in the world. A runtime is right to refuse; an
            // editor has to be able to open the world in order to repair it.
            if (pkg.manifest.world_desc.manifest_hash != 0 && reg_archive.manifest_hash != 0 &&
                reg_archive.manifest_hash != pkg.manifest.world_desc.manifest_hash) {
                const std::string message = "[bsvx]: Parser: manifest hash mismatch for region " + path_to_utf8(ref.absolute_path);
                if (!options.ignore_hash_mismatch) throw std::runtime_error(message);
                warn(message);
            }

            if (pkg.manifest.world_desc.registry_hash != 0 && reg_archive.registry_hash != 0 &&
                reg_archive.registry_hash != pkg.manifest.world_desc.registry_hash) {
                const std::string message = "[bsvx]: Parser: registry hash mismatch for region " + path_to_utf8(ref.absolute_path);
                if (!options.ignore_hash_mismatch) throw std::runtime_error(message);
                warn(message);
            }

            RegionAsset asset{};
            asset.ref = ref;
            asset.archive = std::move(reg_archive);
            pkg.regions.push_back(std::move(asset));
        }

        return pkg;
    }


    void Parser::save_manifest(const Manifest& manifest_in, const std::filesystem::path& manifest_path_in)
    {
        auto [root_dir, manifest_path] = resolve_root_and_manifest_path(manifest_path_in);
        Manifest manifest = manifest_in;
        manifest.root_dir = std::filesystem::absolute(root_dir).lexically_normal();
        manifest.manifest_path = std::filesystem::absolute(manifest_path).lexically_normal();

        if (manifest.regions_dir.empty()) manifest.regions_dir = "regions";
        if (manifest.textures_dir.empty()) manifest.textures_dir = "textures";
        if (manifest.name.empty()) manifest.name = path_to_utf8(manifest.root_dir.filename());
        manifest.world_desc.registry_hash = compute_registry_hash(manifest.world_desc);

        const std::string toml_text = serialize_manifest_toml(manifest);
        ensure_parent_dir(manifest.manifest_path);

        std::ofstream os(manifest.manifest_path, std::ios::binary);
        if (!os) {
            throw std::runtime_error("[bsvx]: Parser: could not open manifest for writing: " + path_to_utf8(manifest.manifest_path));
        }
        os.write(toml_text.data(), static_cast<std::streamsize>(toml_text.size()));
        if (!os) {
            throw std::runtime_error("[bsvx]: Parser: failed writing manifest: " + path_to_utf8(manifest.manifest_path));
        }
    }

    std::string Parser::save_manifest_to_string(const WorldPackage& package, const std::filesystem::path& root_or_manifest_path)
    {
        auto [root_dir_raw, manifest_path_raw] = resolve_root_and_manifest_path(root_or_manifest_path);
        const auto root_dir = std::filesystem::absolute(root_dir_raw).lexically_normal();
        const auto manifest_path = std::filesystem::absolute(manifest_path_raw).lexically_normal();
        return serialize_manifest_toml(normalize_for_save(package, root_dir, manifest_path));
    }

    SaveReport Parser::save_world(const WorldPackage& package, const std::filesystem::path& root_or_manifest_path, const SaveOptions& options)
    {
        auto [root_dir_raw, manifest_path_raw] = resolve_root_and_manifest_path(root_or_manifest_path);

        // Only a native path can be made absolute. A host VFS has its own root, so absolute() would
        // prepend this process's working directory to it and write the world somewhere nobody asked.
        const bool native = options.writer == nullptr;
        const auto root_dir = native ? std::filesystem::absolute(root_dir_raw).lexically_normal() : root_dir_raw.lexically_normal();
        const auto manifest_path = native ? std::filesystem::absolute(manifest_path_raw).lexically_normal() : manifest_path_raw.lexically_normal();

        Manifest manifest = normalize_for_save(package, root_dir, manifest_path);

        const std::string toml_text = serialize_manifest_toml(manifest);
        const uint64_t manifest_hash = fnv1a64_bytes(std::as_bytes(std::span(toml_text.data(), toml_text.size())));
        manifest.world_desc.manifest_hash = manifest_hash;

        SaveReport report{};
        FileWriter& writer = options.writer ? *options.writer : native_filewriter();

        const auto tick = [&](std::string_view stage, size_t done, size_t total) {
            if (options.progress && !options.progress(stage, done, total)) throw CancelledError{};
            };

        // A region stores the manifest hash it was baked against, so the moment the manifest text
        // changes every region has to be restamped -- an incremental save is only possible while the
        // manifest is byte-identical to what is already on disk.
        bool write_manifest = true;
        if (options.dirty_only) {
            const auto existing = writer.read_file(manifest_path);
            if (!existing.empty()) {
                const std::string_view existing_text(reinterpret_cast<const char*>(existing.data()), existing.size());
                write_manifest = (existing_text != toml_text);
            }
        }
        const bool full_rewrite = !options.dirty_only || write_manifest;
        report.full_rewrite = options.dirty_only && full_rewrite;

        const auto texture_root = (root_dir / manifest.textures_dir).lexically_normal();
        const auto region_root = (root_dir / manifest.regions_dir).lexically_normal();

        if (!options.dry_run) {
            writer.make_directories(root_dir);
            writer.make_directories(texture_root);
            writer.make_directories(region_root);
        }

        const auto write_bytes = [&](const std::filesystem::path& path, std::span<const std::byte> bytes) {
            report.files_written += 1u;
            report.bytes_written += bytes.size();
            if (options.dry_run) return;

            writer.make_directories(path.parent_path());
            writer.write_file(path, bytes, options.atomic, options.backup);
            };

        tick("manifest", 0, 1);
        if (write_manifest) {
            write_bytes(manifest_path, std::as_bytes(std::span(toml_text.data(), toml_text.size())));
            report.manifest_written = true;
        }
        else {
            report.files_skipped += 1u;
        }

        if (package.textures.size() != manifest.textures.size()) {
            throw std::runtime_error("[bsvx]: Parser: internal texture reference mismatch during save");
        }
        for (size_t i = 0; i < package.textures.size(); ++i) {
            tick("textures", i, package.textures.size());

            const auto& asset = package.textures[i];
            const auto& ref = manifest.textures[i];
            // A .btx carries no manifest hash, so an unchanged one never has to be rewritten --
            // even when the manifest itself did change.
            if (options.dirty_only && !asset.dirty && writer.exists(ref.absolute_path)) {
                report.files_skipped += 1u;
                continue;
            }
            const std::vector<std::byte> bytes = asset.archive.serialize_to_bytes();
            write_bytes(ref.absolute_path, std::span<const std::byte>(bytes.data(), bytes.size()));
        }

        if (package.regions.size() != manifest.regions.size()) {
            throw std::runtime_error("[bsvx]: Parser: internal region reference mismatch during save");
        }
        for (size_t i = 0; i < package.regions.size(); ++i) {
            tick("regions", i, package.regions.size());

            const auto& asset = package.regions[i];
            const auto& ref = manifest.regions[i];
            if (!ref.coord) {
                throw std::runtime_error("[bsvx]: Parser: region coord missing during save");
            }
            if (!full_rewrite && !asset.dirty && writer.exists(ref.absolute_path)) {
                report.files_skipped += 1u;
                continue;
            }

            bvx::Archive archive = asset.archive;
            archive.manifest_hash = manifest_hash;
            archive.registry_hash = manifest.world_desc.registry_hash;
            archive.region_x = (*ref.coord)[0];
            archive.region_y = (*ref.coord)[1];
            archive.region_z = (*ref.coord)[2];
            if (archive.is_standalone()) {
                archive.standalone->manifest_hash = manifest_hash;
                archive.standalone->registry_hash = manifest.world_desc.registry_hash;
            }
            const std::vector<std::byte> bytes = archive.serialize_to_bytes();
            write_bytes(ref.absolute_path, std::span<const std::byte>(bytes.data(), bytes.size()));
        }

        if (options.prune_orphans) {
            tick("prune", 0, 1);
            report.files_removed += prune_orphans(manifest, writer, region_root, ".bvx", options.dry_run);
            report.files_removed += prune_orphans(manifest, writer, texture_root, ".btx", options.dry_run);
        }

        return report;
    }

    bvx::Archive Parser::build_standalone_region(const WorldPackage& package, std::vector<std::filesystem::path>* out_texture_file_names)
    {
        if (package.regions.size() != 1) {
            throw std::runtime_error("[bsvx]: Parser: save_standalone_region expects exactly one region");
        }

        bvx::WorldDesc world_desc = package.manifest.world_desc;
        world_desc.manifest_hash = 0;
        world_desc.registry_hash = compute_registry_hash(world_desc);
        world_desc.texture_refs.clear();

        for (size_t i = 0; i < package.textures.size(); ++i) {
            const std::filesystem::path file_name = choose_texture_relative_path(package.textures[i], i).filename();
            world_desc.texture_refs.push_back(make_btx_ref(file_name.generic_string(), package.textures[i].ref.content_hash));
            if (out_texture_file_names) out_texture_file_names->push_back(file_name);
        }

        bvx::Archive archive = package.regions.front().archive;
        archive.set_world_desc(world_desc);
        if (package.regions.front().ref.coord) {
            archive.region_x = (*package.regions.front().ref.coord)[0];
            archive.region_y = (*package.regions.front().ref.coord)[1];
            archive.region_z = (*package.regions.front().ref.coord)[2];
        }
        archive.manifest_hash = 0;
        archive.registry_hash = world_desc.registry_hash;
        return archive;
    }

    std::vector<std::byte> Parser::save_region_to_bytes(const WorldPackage& package)
    {
        return build_standalone_region(package, nullptr).serialize_to_bytes();
    }

    VoxelAddress locate_voxel(const bvx::GeometryDesc& geometry, int64_t x, int64_t y, int64_t z)
    {
        if (geometry.chunk_size_x == 0 || geometry.chunk_size_y == 0 || geometry.chunk_size_z == 0 ||
            geometry.region_size_x == 0 || geometry.region_size_y == 0 || geometry.region_size_z == 0) {
            throw std::invalid_argument("[bsvx]: locate_voxel: geometry dimensions must all be non-zero");
        }

        // Flooring division, not C's truncation: voxel -1 belongs to region -1, not region 0.
        const auto floor_div = [](int64_t value, int64_t divisor) {
            const int64_t quotient = value / divisor;
            return (value % divisor != 0 && ((value < 0) != (divisor < 0))) ? quotient - 1 : quotient;
            };
        const auto floor_mod = [&](int64_t value, int64_t divisor) {
            return value - floor_div(value, divisor) * divisor;
            };

        const int64_t chunk_size[3] = { geometry.chunk_size_x, geometry.chunk_size_y, geometry.chunk_size_z };
        const int64_t region_span[3] = {
            chunk_size[0] * geometry.region_size_x,
            chunk_size[1] * geometry.region_size_y,
            chunk_size[2] * geometry.region_size_z,
        };
        const int64_t world[3] = { x, y, z };

        VoxelAddress out{};
        for (int axis = 0; axis < 3; ++axis) {
            const int64_t region = floor_div(world[axis], region_span[axis]);
            if (region < INT32_MIN || region > INT32_MAX) throw std::out_of_range("[bsvx]: locate_voxel: region coordinate out of range");

            const int64_t within_region = floor_mod(world[axis], region_span[axis]);
            out.region[axis] = static_cast<int32_t>(region);
            out.chunk[axis] = static_cast<uint16_t>(within_region / chunk_size[axis]);
            out.local[axis] = static_cast<uint16_t>(within_region % chunk_size[axis]);
        }

        out.local_index = static_cast<uint32_t>(out.local[0] +
            static_cast<uint32_t>(geometry.chunk_size_x) * (out.local[1] + static_cast<uint32_t>(geometry.chunk_size_y) * out.local[2]));
        return out;
    }

    namespace {
        // For each convention: canonical[i] = sign[i] * native[axis[i]]. The canonical frame is
        // X_RIGHT_Y_UP_Z_FORWARD, so its own entry is the identity.
        struct AxisMapping final {
            std::array<uint8_t, 3> axis{ 0, 1, 2 };
            std::array<int8_t, 3> sign{ 1, 1, 1 };
        };

        AxisMapping mapping_for(AxisConvention convention)
        {
            switch (convention) {
            case AxisConvention::X_RIGHT_Y_UP_Z_FORWARD:
                return AxisMapping{ { 0, 1, 2 }, { 1, 1, 1 } };
            case AxisConvention::X_RIGHT_Z_UP_Y_FORWARD:
                // Blender (x, y, z) -> canonical (x, z, -y).
                return AxisMapping{ { 0, 2, 1 }, { 1, 1, -1 } };
            case AxisConvention::X_RIGHT_Y_UP_Z_BACK:
                // Unity (x, y, z) -> canonical (x, y, -z).
                return AxisMapping{ { 0, 1, 2 }, { 1, 1, -1 } };
            default:
                throw std::invalid_argument("[bsvx]: unsupported axis convention");
            }
        }
    }

    std::array<double, 3> convert_position(AxisConvention from, AxisConvention to, double x, double y, double z)
    {
        const AxisMapping in = mapping_for(from);
        const AxisMapping out = mapping_for(to);
        const double native[3] = { x, y, z };

        double canonical[3]{};
        for (int i = 0; i < 3; ++i) canonical[i] = in.sign[i] * native[in.axis[i]];

        std::array<double, 3> result{};
        for (int i = 0; i < 3; ++i) result[out.axis[i]] = out.sign[i] * canonical[i];
        return result;
    }

    std::array<int64_t, 3> convert_cell(AxisConvention from, AxisConvention to, int64_t x, int64_t y, int64_t z)
    {
        const AxisMapping in = mapping_for(from);
        const AxisMapping out = mapping_for(to);
        const int64_t native[3] = { x, y, z };

        // Mirroring a cell index is -c-1: cell c covers [c, c+1), whose mirror is [-c-1, -c).
        int64_t canonical[3]{};
        for (int i = 0; i < 3; ++i) {
            const int64_t value = native[in.axis[i]];
            canonical[i] = in.sign[i] > 0 ? value : -value - 1;
        }

        std::array<int64_t, 3> result{};
        for (int i = 0; i < 3; ++i) {
            result[out.axis[i]] = out.sign[i] > 0 ? canonical[i] : -canonical[i] - 1;
        }
        return result;
    }

    std::string_view axis_convention_name(AxisConvention convention)
    {
        switch (convention) {
        case AxisConvention::X_RIGHT_Y_UP_Z_FORWARD: return "x_right_y_up_z_forward";
        case AxisConvention::X_RIGHT_Z_UP_Y_FORWARD: return "x_right_z_up_y_forward";
        case AxisConvention::X_RIGHT_Y_UP_Z_BACK:    return "x_right_y_up_z_back";
        default: throw std::runtime_error("[bsvx]: unsupported AxisConvention");
        }
    }

    std::optional<AxisConvention> axis_convention_from_name(std::string_view name)
    {
        if (name == "x_right_y_up_z_forward") return AxisConvention::X_RIGHT_Y_UP_Z_FORWARD;
        if (name == "x_right_z_up_y_forward" || name == "blender" || name == "z_up") return AxisConvention::X_RIGHT_Z_UP_Y_FORWARD;
        if (name == "x_right_y_up_z_back" || name == "unity") return AxisConvention::X_RIGHT_Y_UP_Z_BACK;
        return std::nullopt;
    }

    size_t convert_world_axis_convention(WorldPackage& package, AxisConvention target, const ProgressFn& progress)
    {
        const AxisConvention source = package.manifest.world_desc.axis_convention;
        if (source == target) return 0u;

        const bvx::GeometryDesc geometry = package.manifest.world_desc.geometry;
        const bvx::RegistryLookup registry = bvx::build_registry_lookup(package.manifest.world_desc);

        // A flipped axis moves voxels across region and chunk boundaries, so the decomposition has
        // to be rebuilt rather than relabelled. Gather everything, then re-scatter.
        struct ChunkKey final {
            std::array<int32_t, 3> region{};
            std::array<uint16_t, 3> chunk{};
            bool operator<(const ChunkKey& other) const noexcept
            {
                if (region != other.region) return region < other.region;
                return chunk < other.chunk;
            }
        };

        std::map<ChunkKey, std::vector<uint32_t>> rebuilt;
        const size_t per_chunk = bvx::Archive::chunk_voxel_count(geometry);
        size_t moved = 0;

        for (size_t r = 0; r < package.regions.size(); ++r) {
            if (progress && !progress("convert", r, package.regions.size())) throw CancelledError{};

            const bvx::Archive& archive = package.regions[r].archive;
            for (const bvx::ChunkMapEntry& chunk : archive.chunk_map) {
                const auto dense = archive.decode_chunk_voxels(chunk.local_chunk_x, chunk.local_chunk_y, chunk.local_chunk_z, &geometry);

                for (uint16_t z = 0; z < geometry.chunk_size_z; ++z) {
                    for (uint16_t y = 0; y < geometry.chunk_size_y; ++y) {
                        for (uint16_t x = 0; x < geometry.chunk_size_x; ++x) {
                            const size_t index = x + static_cast<size_t>(geometry.chunk_size_x) * (y + static_cast<size_t>(geometry.chunk_size_y) * z);
                            const uint32_t key = dense[index];
                            if (key == 0u) continue;

                            const int64_t world_x = static_cast<int64_t>(archive.region_x) * geometry.chunk_size_x * geometry.region_size_x
                                + static_cast<int64_t>(chunk.local_chunk_x) * geometry.chunk_size_x + x;
                            const int64_t world_y = static_cast<int64_t>(archive.region_y) * geometry.chunk_size_y * geometry.region_size_y
                                + static_cast<int64_t>(chunk.local_chunk_y) * geometry.chunk_size_y + y;
                            const int64_t world_z = static_cast<int64_t>(archive.region_z) * geometry.chunk_size_z * geometry.region_size_z
                                + static_cast<int64_t>(chunk.local_chunk_z) * geometry.chunk_size_z + z;

                            const auto converted = convert_cell(source, target, world_x, world_y, world_z);
                            const VoxelAddress address = locate_voxel(geometry, converted[0], converted[1], converted[2]);

                            ChunkKey target_key{};
                            target_key.region = address.region;
                            target_key.chunk = address.chunk;

                            auto& buffer = rebuilt[target_key];
                            if (buffer.empty()) buffer.assign(per_chunk, 0u);
                            buffer[address.local_index] = key;
                            ++moved;
                        }
                    }
                }
            }
        }

        // Payload sections describe the old frame; there is no way to reinterpret a surface bake or
        // a distance field under a permuted axis, so they go rather than silently lying.
        package.regions.clear();

        for (const auto& [key, dense] : rebuilt) {
            auto found = std::find_if(package.regions.begin(), package.regions.end(), [&](const RegionAsset& asset) {
                return asset.archive.region_x == key.region[0] && asset.archive.region_y == key.region[1] && asset.archive.region_z == key.region[2];
                });

            if (found == package.regions.end()) {
                RegionAsset asset{};
                asset.ref.coord = key.region;
                asset.archive.region_x = key.region[0];
                asset.archive.region_y = key.region[1];
                asset.archive.region_z = key.region[2];
                asset.dirty = true;
                package.regions.push_back(std::move(asset));
                found = std::prev(package.regions.end());
            }

            found->archive.set_chunk_voxels_dense(key.chunk[0], key.chunk[1], key.chunk[2],
                std::span<const uint32_t>(dense.data(), dense.size()), &geometry, VoxelCodec::AUTO,
                registry.empty() ? nullptr : &registry);
            found->dirty = true;
        }

        package.manifest.world_desc.axis_convention = target;

        // Bounds are region coordinates in the old frame, and stale bounds are worse than none.
        if (package.manifest.world_desc.bounds_mode == BoundsMode::EXPLICIT) {
            bool first = true;
            for (const RegionAsset& asset : package.regions) {
                const auto& d = package.manifest.world_desc;
                (void)d;
                auto& out = package.manifest.world_desc;
                if (first) {
                    out.world_min_region_x = out.world_max_region_x = asset.archive.region_x;
                    out.world_min_region_y = out.world_max_region_y = asset.archive.region_y;
                    out.world_min_region_z = out.world_max_region_z = asset.archive.region_z;
                    first = false;
                    continue;
                }
                out.world_min_region_x = std::min(out.world_min_region_x, asset.archive.region_x);
                out.world_min_region_y = std::min(out.world_min_region_y, asset.archive.region_y);
                out.world_min_region_z = std::min(out.world_min_region_z, asset.archive.region_z);
                out.world_max_region_x = std::max(out.world_max_region_x, asset.archive.region_x);
                out.world_max_region_y = std::max(out.world_max_region_y, asset.archive.region_y);
                out.world_max_region_z = std::max(out.world_max_region_z, asset.archive.region_z);
            }
            if (first) package.manifest.world_desc.bounds_mode = BoundsMode::UNBOUNDED;
        }

        return moved;
    }

    std::vector<ValidationIssue> Parser::validate(const WorldPackage& package, const ValidateOptions& options)
    {
        std::vector<ValidationIssue> issues;
        const auto& desc = package.manifest.world_desc;

        const auto add = [&](Severity severity, ValidationCode code, std::string message, int64_t region = -1, int64_t chunk = -1, uint32_t key = 0) {
            issues.push_back(ValidationIssue{ severity, code, region, chunk, key, std::move(message) });
            };
        const auto tick = [&](std::string_view stage, size_t done, size_t total) {
            if (options.progress && !options.progress(stage, done, total)) throw CancelledError{};
            };

        const auto& g = desc.geometry;
        if (g.chunk_size_x == 0 || g.chunk_size_y == 0 || g.chunk_size_z == 0 || g.region_size_x == 0 || g.region_size_y == 0 || g.region_size_z == 0) {
            add(Severity::ERROR, ValidationCode::GEOMETRY_INVALID, "geometry has a zero dimension");
            return issues;   // nothing below can be checked meaningfully
        }
        if (g.chunk_size_x > 255 || g.chunk_size_y > 255 || g.chunk_size_z > 255) {
            add(Severity::ERROR, ValidationCode::GEOMETRY_INVALID, "chunk dimensions must be <= 255 (summary AABBs are 8-bit)");
        }

        // --- registry ------------------------------------------------------------------------
        std::unordered_set<uint32_t> known_keys;
        for (const bvx::RegistryEntry& entry : desc.registry_entries) {
            if (entry.voxel_key == 0u) {
                add(Severity::WARNING, ValidationCode::AIR_KEY_REGISTERED, "voxel key 0 is air by convention and should not be registered", -1, -1, 0);
            }
            if (!known_keys.insert(entry.voxel_key).second) {
                add(Severity::ERROR, ValidationCode::DUPLICATE_VOXEL_KEY, "duplicate registry entry for voxel key " + std::to_string(entry.voxel_key), -1, -1, entry.voxel_key);
            }
        }
        if (desc.registry_entries.empty()) {
            add(Severity::WARNING, ValidationCode::REGISTRY_EMPTY, "world has no registry: every non-air voxel will be summarised as opaque");
        }
        if (desc.units.is_default()) {
            add(Severity::INFO, ValidationCode::UNITS_UNSET, "world uses the default 1.0 voxel size and a zero origin");
        }

        // --- textures and materials ------------------------------------------------------------
        std::unordered_set<uint32_t> known_materials;
        for (size_t i = 0; i < package.textures.size(); ++i) {
            tick("textures", i, package.textures.size());

            const TextureAsset& asset = package.textures[i];
            for (const ValidationError& err : asset.archive.validate()) {
                add(Severity::ERROR, ValidationCode::TEXTURE_ARCHIVE_INVALID, "texture '" + asset.ref.id + "': " + err.message);
            }
            for (const auto& material : asset.archive.materials) known_materials.insert(material.material_id);

            const std::string path_str = (package.manifest.textures_dir / asset.ref.relative_path).generic_string();
            if (path_str.size() > bvx::BTX_REF_PATH_CAPACITY) {
                add(Severity::ERROR, ValidationCode::TEXTURE_PATH_TOO_LONG,
                    "texture path is longer than " + std::to_string(bvx::BTX_REF_PATH_CAPACITY) + " characters and cannot be referenced: " + path_str);
            }
        }
        if (package.textures.empty() && !desc.registry_entries.empty()) {
            add(Severity::INFO, ValidationCode::NO_TEXTURES, "world has registry entries but no .btx: voxel keys resolve to no material");
        }
        for (const bvx::RegistryEntry& entry : desc.registry_entries) {
            if (entry.voxel_key == 0u || package.textures.empty()) continue;
            if (!known_materials.contains(entry.material_id)) {
                add(Severity::WARNING, ValidationCode::MATERIAL_NOT_FOUND,
                    "voxel key " + std::to_string(entry.voxel_key) + " refers to material_id " + std::to_string(entry.material_id) + ", which no .btx defines",
                    -1, -1, entry.voxel_key);
            }
        }

        // --- regions and chunks -----------------------------------------------------------------
        std::map<std::array<int32_t, 3>, size_t> seen_coords;
        for (size_t r = 0; r < package.regions.size(); ++r) {
            tick("regions", r, package.regions.size());

            const bvx::Archive& archive = package.regions[r].archive;
            const std::array<int32_t, 3> coord{ archive.region_x, archive.region_y, archive.region_z };

            if (const auto found = seen_coords.find(coord); found != seen_coords.end()) {
                add(Severity::ERROR, ValidationCode::DUPLICATE_REGION_COORD,
                    "region " + std::to_string(r) + " sits at the same coordinate as region " + std::to_string(found->second),
                    static_cast<int64_t>(r));
            }
            else {
                seen_coords.emplace(coord, r);
            }

            if (desc.bounds_mode == BoundsMode::EXPLICIT) {
                const bool inside =
                    coord[0] >= desc.world_min_region_x && coord[0] <= desc.world_max_region_x &&
                    coord[1] >= desc.world_min_region_y && coord[1] <= desc.world_max_region_y &&
                    coord[2] >= desc.world_min_region_z && coord[2] <= desc.world_max_region_z;
                if (!inside) {
                    add(Severity::WARNING, ValidationCode::REGION_OUT_OF_BOUNDS,
                        "region (" + std::to_string(coord[0]) + ", " + std::to_string(coord[1]) + ", " + std::to_string(coord[2]) + ") lies outside the declared bounds",
                        static_cast<int64_t>(r));
                }
            }

            if (archive.chunk_map.empty()) {
                add(Severity::INFO, ValidationCode::EMPTY_REGION, "region " + std::to_string(r) + " holds no chunks", static_cast<int64_t>(r));
                continue;
            }

            for (size_t c = 0; c < archive.chunk_map.size(); ++c) {
                const bvx::ChunkMapEntry& chunk = archive.chunk_map[c];
                if (chunk.local_chunk_x >= g.region_size_x || chunk.local_chunk_y >= g.region_size_y || chunk.local_chunk_z >= g.region_size_z) {
                    add(Severity::ERROR, ValidationCode::CHUNK_OUT_OF_REGION,
                        "chunk (" + std::to_string(chunk.local_chunk_x) + ", " + std::to_string(chunk.local_chunk_y) + ", " + std::to_string(chunk.local_chunk_z) + ") lies outside the region's chunk grid",
                        static_cast<int64_t>(r), static_cast<int64_t>(c));
                }

                // The cheap pass only sees the four dominant keys a summary records. The deep pass
                // decodes and catches every key actually present.
                std::vector<uint32_t> keys;
                if (options.deep) {
                    keys = archive.decode_chunk_voxels(chunk.local_chunk_x, chunk.local_chunk_y, chunk.local_chunk_z, &g);
                }
                else if (chunk.summary_index < archive.chunk_summaries.size()) {
                    const bvx::ChunkSummary& sum = archive.chunk_summaries[chunk.summary_index];
                    keys = { sum.top_id_0, sum.top_id_1, sum.top_id_2, sum.top_id_3 };
                }

                std::unordered_set<uint32_t> reported;
                for (uint32_t key : keys) {
                    if (key == 0u || !reported.insert(key).second) continue;
                    if (known_keys.contains(key)) continue;
                    add(Severity::WARNING, ValidationCode::VOXEL_KEY_NOT_IN_REGISTRY,
                        "voxel key " + std::to_string(key) + " is used but not registered",
                        static_cast<int64_t>(r), static_cast<int64_t>(c), key);
                }
            }
        }

        return issues;
    }

    void Parser::save_region(const WorldPackage& package, const std::filesystem::path& region_path_in, const SaveOptions& options)
    {
        std::vector<std::filesystem::path> texture_file_names;
        const bvx::Archive archive = build_standalone_region(package, &texture_file_names);

        FileWriter& writer = options.writer ? *options.writer : native_filewriter();
        const bool native = options.writer == nullptr;

        std::filesystem::path region_path = region_path_in;
        if (!region_path.has_extension()) {
            const auto default_name = choose_region_relative_path(package.regions.front(), 0).filename();
            region_path /= default_name.empty() ? std::filesystem::path("region.bvx") : default_name;
        }

        // Only a native path can be made absolute -- a host VFS path has its own root, and
        // std::filesystem::absolute would prepend this process's working directory to it.
        if (native) region_path = std::filesystem::absolute(region_path).lexically_normal();
        const auto root_dir = region_path.parent_path();

        if (options.dry_run) return;

        writer.make_directories(root_dir);

        for (size_t i = 0; i < package.textures.size(); ++i) {
            const auto out_path = (root_dir / texture_file_names[i]).lexically_normal();
            const auto bytes = package.textures[i].archive.serialize_to_bytes();
            writer.write_file(out_path, std::span<const std::byte>(bytes.data(), bytes.size()), options.atomic, options.backup);
        }

        writer.make_directories(region_path.parent_path());
        const auto bytes = archive.serialize_to_bytes();
        writer.write_file(region_path, std::span<const std::byte>(bytes.data(), bytes.size()), options.atomic, options.backup);
    }

} // namespace voxel
