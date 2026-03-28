
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
                throw std::runtime_error("[bsvx]: Parser: could not open file: " + path.string());
            }

            is.seekg(0, std::ios::end);
            const auto size = static_cast<size_t>(is.tellg());
            is.seekg(0, std::ios::beg);

            std::vector<std::byte> bytes(size);
            if (size != 0) {
                is.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                if (!is) {
                    throw std::runtime_error("[bsvx]: Parser: failed to read file: " + path.string());
                }
            }
            return bytes;
        }

        static uint64_t hash_file(const std::filesystem::path& path)
        {
            const auto bytes = read_file_bytes(path);
            return fnv1a64_bytes(std::span(bytes.data(), bytes.size()));
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
                if (*axis == "x_right_y_up_z_forward") {
                    manifest.world_desc.axis_convention = AxisConvention::X_RIGHT_Y_UP_Z_FORWARD;
                }
                else {
                    throw std::runtime_error("[bsvx]::Parser: unsupported geometry.axis_convention: " + *axis);
                }
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

        static void parse_hashes(Manifest& manifest, const toml::table& tbl)
        {
            if (auto hashes = tbl["hashes"].as_table()) {
                manifest.world_desc.registry_hash = optional_u64((*hashes)["registry"], manifest.world_desc.registry_hash);
                manifest.world_desc.manifest_hash = optional_u64((*hashes)["manifest"], manifest.world_desc.manifest_hash);
            }

            manifest.world_desc.manifest_hash = hash_file(manifest.manifest_path);
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
                }
                else {
                    entry.name_hash = optional_u64((*entry_tbl)["name_hash"], 0);
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
                    ref.id = ref.relative_path.stem().string();
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
                manifest.name = optional_string(tbl["name"]).value_or(manifest.root_dir.filename().string());
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
            switch (axis) {
            case AxisConvention::X_RIGHT_Y_UP_Z_FORWARD: return "x_right_y_up_z_forward";
            default: throw std::runtime_error("[bsvx]: Parser: unsupported AxisConvention");
            }
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

            if (!manifest.world_desc.registry_entries.empty()) {
                toml::array reg{};
                for (const bvx::RegistryEntry& entry : manifest.world_desc.registry_entries) {
                    toml::table e{};
                    e.insert("voxel_key", static_cast<int64_t>(entry.voxel_key));
                    e.insert("material_id", static_cast<int64_t>(entry.material_id));
                    e.insert("flags", registry_flags_to_array(entry.flags));
                    e.insert("name_hash", u64_hex(entry.name_hash));
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

    std::filesystem::path Parser::resolve_manifest_path(const std::filesystem::path& path)
    {
        if (std::filesystem::is_regular_file(path)) {
            if (path.extension() == ".toml") {
                return path.lexically_normal();
            }
            throw std::runtime_error("[bsvx]: Parser: expected a manifest .toml file or a root directory");
        }

        if (!std::filesystem::is_directory(path)) {
            throw std::runtime_error("[bsvx]: Parser: path does not exist: " + path.string());
        }

        const auto manifest = path / "manifest.toml";
        if (!std::filesystem::exists(manifest)) {
            throw std::runtime_error("[bsvx]: Parser: manifest.toml not found in directory: " + path.string());
        }
        return manifest.lexically_normal();
    }

    Manifest Parser::parse_manifest(const std::filesystem::path& manifest_path)
    {
        const auto absolute_manifest = std::filesystem::absolute(resolve_manifest_path(manifest_path)).lexically_normal();

        toml::table tbl;
        try {
            tbl = toml::parse_file(absolute_manifest.string());
        }
        catch (const toml::parse_error& err) {
            std::ostringstream oss;
            oss << "[bsvx]: Parser: failed to parse manifest '" << absolute_manifest.string() << "': " << err;
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
        parse_hashes(manifest, tbl);
        parse_registry(manifest, tbl);
        parse_textures(manifest, tbl);
        parse_regions(manifest, tbl);

        if (manifest.textures.empty()) {
            auto_discover_textures(manifest);
            synthesize_missing_texture_refs(manifest);
        }
        if (manifest.regions.empty()) {
            auto_discover_regions(manifest);
        }

        return manifest;
    }

    void Parser::auto_discover_textures(Manifest& manifest)
    {
        manifest.textures.clear();

        const auto texture_root = make_absolute_from_root(manifest.root_dir, manifest.textures_dir);
        if (!std::filesystem::exists(texture_root)) {
            return;
        }

        for (const auto& entry : std::filesystem::directory_iterator(texture_root)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".btx") continue;

            TextureReference ref{};
            ref.relative_path = std::filesystem::relative(entry.path(), texture_root).lexically_normal();
            ref.absolute_path = entry.path().lexically_normal();
            ref.id = ref.relative_path.stem().string();
            ref.path_hash = fnv1a64_string((manifest.textures_dir / ref.relative_path).generic_string());
            ref.content_hash = 0;
            manifest.textures.push_back(std::move(ref));
        }

        std::sort(manifest.textures.begin(), manifest.textures.end(),
            [](const TextureReference& a, const TextureReference& b) {
                return a.relative_path.generic_string() < b.relative_path.generic_string();
            });
    }

    void Parser::auto_discover_regions(Manifest& manifest)
    {
        manifest.regions.clear();

        const auto region_root = make_absolute_from_root(manifest.root_dir, manifest.regions_dir);
        if (!std::filesystem::exists(region_root)) {
            return;
        }

        for (const auto& entry : std::filesystem::directory_iterator(region_root)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".bvx") continue;

            RegionReference ref{};
            ref.relative_path = std::filesystem::relative(entry.path(), region_root).lexically_normal();
            ref.absolute_path = entry.path().lexically_normal();
            manifest.regions.push_back(std::move(ref));
        }

        std::sort(manifest.regions.begin(), manifest.regions.end(),
            [](const RegionReference& a, const RegionReference& b) {
                return a.relative_path.generic_string() < b.relative_path.generic_string();
            });
    }

    WorldPackage Parser::load_region(const std::filesystem::path& region_path)
    {
        const auto absolute_region = std::filesystem::absolute(region_path).lexically_normal();
        auto region_archive = bvx::Archive::load_from_file(absolute_region.string());
        if (!region_archive) {
            throw std::runtime_error("[bsvx]: Parser: could not load standalone region: " + absolute_region.string());
        }
        if (!region_archive->is_standalone()) {
            throw std::runtime_error("[bsvx]: Parser: region is not standalone: " + absolute_region.string());
        }

        WorldPackage pkg{};
        pkg.manifest.synthetic_from_standalone_region = true;
        pkg.manifest.manifest_path.clear();
        pkg.manifest.root_dir = absolute_region.parent_path();
        pkg.manifest.name = absolute_region.stem().string();
        pkg.manifest.format_version = 1;
        pkg.manifest.world_desc = *region_archive->standalone;
        pkg.manifest.world_desc.manifest_hash = region_archive->manifest_hash;
        pkg.manifest.world_desc.registry_hash = region_archive->registry_hash;

        RegionReference region_ref{};
        region_ref.relative_path = absolute_region.filename();
        region_ref.absolute_path = absolute_region;
        region_ref.coord = std::array<int32_t, 3>{ region_archive->region_x, region_archive->region_y, region_archive->region_z };
        pkg.manifest.regions.push_back(region_ref);

        RegionAsset region_asset{};
        region_asset.ref = region_ref;
        region_asset.archive = std::move(*region_archive);
        pkg.regions.push_back(std::move(region_asset));

        for (const bvx::BtxRef& btx_ref : pkg.manifest.world_desc.texture_refs) {
            TextureReference tex_ref{};
            tex_ref.relative_path = trim_nul_padded(btx_ref.relative_path, sizeof(btx_ref.relative_path));
            tex_ref.absolute_path = make_absolute_from_root(pkg.manifest.root_dir, tex_ref.relative_path);
            tex_ref.id = tex_ref.relative_path.stem().string();
            tex_ref.path_hash = btx_ref.path_hash;
            tex_ref.content_hash = btx_ref.content_hash;
            pkg.manifest.textures.push_back(tex_ref);
        }

        if (pkg.manifest.textures.empty()) {
            pkg.manifest.textures_dir = ".";
            auto_discover_textures(pkg.manifest);
            synthesize_missing_texture_refs(pkg.manifest);
        }

        for (const TextureReference& ref : pkg.manifest.textures) {
            auto tex_archive = btx::Archive::load_from_file(ref.absolute_path.string());
            if (!tex_archive) {
                throw std::runtime_error("[bsvx]: Parser: could not load .btx: " + ref.absolute_path.string());
            }
            TextureAsset asset{};
            asset.ref = ref;
            asset.archive = std::move(*tex_archive);
            pkg.textures.push_back(std::move(asset));
        }

        return pkg;
    }

    WorldPackage Parser::load_world(const std::filesystem::path& manifest_or_root)
    {
        if (std::filesystem::is_regular_file(manifest_or_root) && manifest_or_root.extension() == ".bvx") {
            return load_region(manifest_or_root);
        }

        WorldPackage pkg{};
        pkg.manifest = parse_manifest(manifest_or_root);

        for (const TextureReference& ref : pkg.manifest.textures) {
            auto tex_archive = btx::Archive::load_from_file(ref.absolute_path.string());
            if (!tex_archive) {
                throw std::runtime_error("[bsvx]: Parser: could not load .btx: " + ref.absolute_path.string());
            }

            TextureAsset asset{};
            asset.ref = ref;
            asset.archive = std::move(*tex_archive);
            pkg.textures.push_back(std::move(asset));
        }

        for (const RegionReference& ref : pkg.manifest.regions) {
            auto reg_archive = bvx::Archive::load_from_file(ref.absolute_path.string());
            if (!reg_archive) {
                throw std::runtime_error("[bsvx]: Parser: could not load .bvx: " + ref.absolute_path.string());
            }

            if (ref.coord) {
                const auto expected = *ref.coord;
                if (reg_archive->region_x != expected[0] ||
                    reg_archive->region_y != expected[1] ||
                    reg_archive->region_z != expected[2]) {
                    throw std::runtime_error("[bsvx]: Parser: region coordinate mismatch for " + ref.absolute_path.string());
                }
            }

            if (pkg.manifest.world_desc.manifest_hash != 0 && reg_archive->manifest_hash != 0 &&
                reg_archive->manifest_hash != pkg.manifest.world_desc.manifest_hash) {
                throw std::runtime_error("[bsvx]: Parser: manifest hash mismatch for region " + ref.absolute_path.string());
            }

            if (pkg.manifest.world_desc.registry_hash != 0 && reg_archive->registry_hash != 0 &&
                reg_archive->registry_hash != pkg.manifest.world_desc.registry_hash) {
                throw std::runtime_error("[bsvx]: Parser: registry hash mismatch for region " + ref.absolute_path.string());
            }

            RegionAsset asset{};
            asset.ref = ref;
            asset.archive = std::move(*reg_archive);
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
        if (manifest.name.empty()) manifest.name = manifest.root_dir.filename().string();
        manifest.world_desc.registry_hash = compute_registry_hash(manifest.world_desc);

        const std::string toml_text = serialize_manifest_toml(manifest);
        ensure_parent_dir(manifest.manifest_path);

        std::ofstream os(manifest.manifest_path, std::ios::binary);
        if (!os) {
            throw std::runtime_error("[bsvx]: Parser: could not open manifest for writing: " + manifest.manifest_path.string());
        }
        os.write(toml_text.data(), static_cast<std::streamsize>(toml_text.size()));
        if (!os) {
            throw std::runtime_error("[bsvx]: Parser: failed writing manifest: " + manifest.manifest_path.string());
        }
    }

    void Parser::save_world(const WorldPackage& package, const std::filesystem::path& root_or_manifest_path)
    {
        auto [root_dir_raw, manifest_path_raw] = resolve_root_and_manifest_path(root_or_manifest_path);
        const auto root_dir = std::filesystem::absolute(root_dir_raw).lexically_normal();
        const auto manifest_path = std::filesystem::absolute(manifest_path_raw).lexically_normal();

        std::filesystem::create_directories(root_dir);

        Manifest manifest = package.manifest;
        manifest.synthetic_from_standalone_region = false;
        manifest.root_dir = root_dir;
        manifest.manifest_path = manifest_path;
        if (manifest.regions_dir.empty()) manifest.regions_dir = "regions";
        if (manifest.textures_dir.empty()) manifest.textures_dir = "textures";
        if (manifest.name.empty()) manifest.name = root_dir.filename().string();

        manifest.textures.clear();
        for (size_t i = 0; i < package.textures.size(); ++i) {
            const TextureAsset& asset = package.textures[i];
            TextureReference ref = asset.ref;
            ref.relative_path = choose_texture_relative_path(asset, i);
            ref.absolute_path = (root_dir / manifest.textures_dir / ref.relative_path).lexically_normal();
            if (ref.id.empty()) {
                ref.id = ref.relative_path.stem().string();
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
            manifest.world_desc.texture_refs.push_back(make_btx_ref(path_str, ref.content_hash));
        }
        manifest.world_desc.registry_hash = compute_registry_hash(manifest.world_desc);

        const std::string toml_text = serialize_manifest_toml(manifest);
        const uint64_t manifest_hash = fnv1a64_bytes(std::as_bytes(std::span(toml_text.data(), toml_text.size())));
        manifest.world_desc.manifest_hash = manifest_hash;

        ensure_parent_dir(manifest_path);
        std::ofstream mos(manifest_path, std::ios::binary);
        if (!mos) {
            throw std::runtime_error("[bsvx]: Parser: could not open manifest for writing: " + manifest_path.string());
        }
        mos.write(toml_text.data(), static_cast<std::streamsize>(toml_text.size()));
        if (!mos) {
            throw std::runtime_error("[bsvx]: Parser: failed writing manifest: " + manifest_path.string());
        }

        const auto texture_root = (root_dir / manifest.textures_dir).lexically_normal();
        const auto region_root = (root_dir / manifest.regions_dir).lexically_normal();
        std::filesystem::create_directories(texture_root);
        std::filesystem::create_directories(region_root);

        if (package.textures.size() != manifest.textures.size()) {
            throw std::runtime_error("[bsvx]: Parser: internal texture reference mismatch during save");
        }
        for (size_t i = 0; i < package.textures.size(); ++i) {
            const auto& asset = package.textures[i];
            const auto& ref = manifest.textures[i];
            ensure_parent_dir(ref.absolute_path);
            if (!asset.archive.save_to_file(ref.absolute_path.string())) {
                throw std::runtime_error("[bsvx]: Parser: failed writing .btx: " + ref.absolute_path.string());
            }
        }

        if (package.regions.size() != manifest.regions.size()) {
            throw std::runtime_error("[bsvx]: Parser: internal region reference mismatch during save");
        }
        for (size_t i = 0; i < package.regions.size(); ++i) {
            const auto& asset = package.regions[i];
            const auto& ref = manifest.regions[i];
            bvx::Archive archive = asset.archive;
            archive.manifest_hash = manifest_hash;
            archive.registry_hash = manifest.world_desc.registry_hash;
            if (!ref.coord) {
                throw std::runtime_error("[bsvx]: Parser: region coord missing during save");
            }
            archive.region_x = (*ref.coord)[0];
            archive.region_y = (*ref.coord)[1];
            archive.region_z = (*ref.coord)[2];
            if (archive.is_standalone()) {
                archive.standalone->manifest_hash = manifest_hash;
                archive.standalone->registry_hash = manifest.world_desc.registry_hash;
            }
            ensure_parent_dir(ref.absolute_path);
            if (!archive.save_to_file(ref.absolute_path.string())) {
                throw std::runtime_error("[bsvx]: Parser: failed writing .bvx: " + ref.absolute_path.string());
            }
        }
    }

    void Parser::save_region(const WorldPackage& package, const std::filesystem::path& region_path_in)
    {
        if (package.regions.size() != 1) {
            throw std::runtime_error("[bsvx]: Parser: save_standalone_region expects exactly one region");
        }

        std::filesystem::path region_path = region_path_in;
        if (!region_path.has_extension()) {
            std::filesystem::create_directories(region_path);
            const auto default_name = choose_region_relative_path(package.regions.front(), 0).filename();
            region_path /= default_name.empty() ? std::filesystem::path("region.bvx") : default_name;
        }

        region_path = std::filesystem::absolute(region_path).lexically_normal();
        const auto root_dir = region_path.parent_path();
        std::filesystem::create_directories(root_dir);

        bvx::WorldDesc world_desc = package.manifest.world_desc;
        world_desc.manifest_hash = 0;
        world_desc.registry_hash = compute_registry_hash(world_desc);
        world_desc.texture_refs.clear();

        for (size_t i = 0; i < package.textures.size(); ++i) {
            const auto& asset = package.textures[i];
            const std::filesystem::path file_name = choose_texture_relative_path(asset, i).filename();
            world_desc.texture_refs.push_back(make_btx_ref(file_name.generic_string(), asset.ref.content_hash));

            const auto out_path = (root_dir / file_name).lexically_normal();
            if (!asset.archive.save_to_file(out_path.string())) {
                throw std::runtime_error("[bsvx]: Parser: failed writing standalone .btx: " + out_path.string());
            }
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

        ensure_parent_dir(region_path);
        if (!archive.save_to_file(region_path.string())) {
            throw std::runtime_error("[bsvx]: Parser: failed writing standalone .bvx: " + region_path.string());
        }
    }

} // namespace voxel
