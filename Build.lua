-- Premake build for bsvx.
--
-- CMakeLists.txt is the primary build and the one CI runs. This exists so a Visual Studio
-- solution can be generated without CMake in the picture, and it deliberately mirrors the same
-- decisions rather than inventing its own -- in particular the MSVC flags, which are not optional:
-- without /utf-8 the UTF-8 string literals in the library and in test_non_ascii_paths are decoded
-- in the active code page and the test suite fails on exactly the paths it exists to check.
--
--   premake\premake5.exe --file=Build.lua vs2022      (Windows, what build.bat runs)
--   premake5 --file=Build.lua gmake2 && make -j       (Linux/macOS)
--
-- Options:
--   --static-lib            build bsvx as a static archive instead of a DLL
--   --static-runtime        link the C++ runtime statically
--   --no-tests              skip the test runner
--   --no-cli                skip the bsvx command-line tool
--   --no-python-stage       do not copy the built library into python/bsvx/bin/<platform>/
--   --godot-cpp=DIR         godot-cpp source tree, enabling the GDExtension project
--   --godot-cpp-gen=DIR     godot-cpp's generated headers (its build dir's gen/include)
--   --godot-cpp-lib=FILE    the prebuilt godot-cpp static library to link

newoption {
    trigger     = "static-lib",
    description = "Build bsvx as a static archive. A host that links it into one loadable module " ..
                  "of its own wants a single file to ship; the flat C ABI is unchanged either way"
}

newoption {
    trigger     = "static-runtime",
    description = "Link the C++ runtime statically, so a host process's own older runtime cannot clash"
}

newoption {
    trigger     = "no-tests",
    description = "Do not generate the test runner project"
}

newoption {
    trigger     = "no-cli",
    description = "Do not generate the bsvx command-line tool project"
}

newoption {
    trigger     = "no-python-stage",
    description = "Do not copy the built library into python/bsvx/bin/<platform>/"
}

newoption {
    trigger     = "godot-cpp",
    value       = "DIR",
    description = "godot-cpp source tree. Enables the GDExtension project; needs --godot-cpp-gen and --godot-cpp-lib too"
}

newoption {
    trigger     = "godot-cpp-gen",
    value       = "DIR",
    description = "godot-cpp's generated headers, i.e. its build directory's gen/include"
}

newoption {
    trigger     = "godot-cpp-lib",
    value       = "FILE",
    description = "The prebuilt godot-cpp static library to link against"
}

local SHARED = not _OPTIONS["static-lib"]

-- The ctypes binding looks for the library under bin/<platform>/, and the Blender add-on is that
-- binding plus bpy glue -- so staging here is what makes a premake build usable from Blender.
local PYTHON_PLATFORM = ({
    windows = "windows_x86_64",
    macosx  = "macos_universal",
})[os.target()] or ("linux_" .. os.hostarch())

-- Godot loads a GDExtension by the exact file name its .gdextension lists.
local GODOT_PLATFORM = ({ windows = "windows", macosx = "macos" })[os.target()] or "linux"

workspace "bsvx"
    architecture "x86_64"
    configurations { "Debug", "Release" }
    startproject (_OPTIONS["no-tests"] and "bsvx" or "bsvx_tests")

OutputDir = "%{cfg.system}-%{cfg.architecture}/%{cfg.buildcfg}"

-- ------------------------------------------------------------------------------------------------
-- The library
-- ------------------------------------------------------------------------------------------------

project "bsvx"
    kind (SHARED and "SharedLib" or "StaticLib")
    language "C++"
    -- The library is written to C++23 and also builds as C++20; C++latest gets MSVC there without
    -- pinning a dialect that older toolsets reject.
    cppdialect "C++latest"
    staticruntime (_OPTIONS["static-runtime"] and "on" or "off")
    visibility "Hidden"
    pic "On"

    targetdir ("Binaries/" .. OutputDir .. "/%{prj.name}")
    objdir    ("Binaries/Intermediates/" .. OutputDir .. "/%{prj.name}")

    files
    {
        "src/include/**.h",
        "src/impl/**.hpp",
        "src/impl/**.cpp"
    }

    includedirs
    {
        "src/include",
        "src/impl"
    }

    defines { "TOML_HEADER_ONLY=0" }

    filter "kind:SharedLib"
        implibdir  ("Binaries/" .. OutputDir .. "/%{prj.name}")
        implibname ("%{prj.name}")
        -- Only meaningful on Windows, where it turns BSVX_API into __declspec(dllexport). A static
        -- bsvx must not carry it: consumers include the same header without BSVX_ASSETS_USE_DLL,
        -- and an exported-but-not-imported symbol is a link error rather than a warning.
        defines { "BSVX_ASSETS_BUILD_DLL" }

    filter "toolset:msc"
        -- These three are the reason this file needs to track CMakeLists.txt rather than drift
        -- from it. /utf-8 makes cl read the UTF-8 source and emit UTF-8 execution strings, which
        -- is what the "every path is UTF-8" contract assumes; without it the non-ASCII path tests
        -- fail and the library warns C4819 on its own source. /bigobj is needed because toml++
        -- exhausts the default object-file section limit. /Zc:__cplusplus stops cl reporting
        -- __cplusplus as 199711L, which would make bsvx_build_info() print a lie.
        buildoptions { "/utf-8", "/bigobj", "/Zc:__cplusplus" }

    filter { "files:src/impl/toml_impl.cpp", "toolset:gcc or clang" }
        -- Vendored toml++ uses `operator"" _toml`, whose space before the suffix is deprecated in
        -- C++23. Not actionable in third-party code. MSVC must not see this: cl translates -W into
        -- its own warning-*level* flag and rejects the rest as a non-numeric level (D8021).
        buildoptions { "-Wno-deprecated-literal-operator" }

    filter "system:windows"
        systemversion "latest"
        defines { "WINDOWS", "NOMINMAX", "_CRT_SECURE_NO_WARNINGS" }

    filter "configurations:Debug"
        defines { "DEBUG" }
        symbols "On"

    filter "configurations:Release"
        defines { "RELEASE" }
        optimize "Speed"

    filter {}

    if SHARED and not _OPTIONS["no-python-stage"] then
        postbuildcommands {
            '{MKDIR} "%{wks.location}/python/bsvx/bin/' .. PYTHON_PLATFORM .. '"',
            '{COPYFILE} "%{cfg.buildtarget.abspath}" "%{wks.location}/python/bsvx/bin/' .. PYTHON_PLATFORM .. '/%{cfg.buildtarget.name}"'
        }
    end

-- ------------------------------------------------------------------------------------------------
-- Tests
-- ------------------------------------------------------------------------------------------------

if not _OPTIONS["no-tests"] then

project "bsvx_tests"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++latest"
    staticruntime (_OPTIONS["static-runtime"] and "on" or "off")

    targetdir ("Binaries/" .. OutputDir .. "/%{prj.name}")
    objdir    ("Binaries/Intermediates/" .. OutputDir .. "/%{prj.name}")

    files {
        "test/**.cpp",
        "src/include/**.h"
    }

    includedirs { "src/include" }

    links { "bsvx" }
    dependson { "bsvx" }

    -- The suite reads test/data through relative paths, so it has to run from the repository root.
    debugdir "%{wks.location}"

    filter "toolset:msc"
        -- test_non_ascii_paths carries u8"" literals with non-ASCII text; see the library above.
        buildoptions { "/utf-8", "/Zc:__cplusplus" }

    filter "system:windows"
        systemversion "latest"
        defines { "NOMINMAX", "_CRT_SECURE_NO_WARNINGS" }

    filter "configurations:Debug"
        symbols "On"

    filter "configurations:Release"
        optimize "Speed"

    filter {}

    if SHARED then
        defines { "BSVX_ASSETS_USE_DLL" }
        filter "system:windows"
            -- Windows resolves a DLL beside the executable; elsewhere the linker's rpath does it.
            postbuildcommands {
                '{COPYFILE} "%{wks.location}/Binaries/' .. OutputDir .. '/bsvx/bsvx.dll" "%{cfg.targetdir}"'
            }
        filter "system:not windows"
            linkoptions { "-Wl,-rpath,'$$ORIGIN/../bsvx'" }
        filter {}
    end

end

-- ------------------------------------------------------------------------------------------------
-- Command-line tool
-- ------------------------------------------------------------------------------------------------

if not _OPTIONS["no-cli"] then

project "bsvx_cli"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++latest"
    staticruntime (_OPTIONS["static-runtime"] and "on" or "off")
    -- The executable is called bsvx like the library; the project cannot be.
    targetname "bsvx"

    targetdir ("Binaries/" .. OutputDir .. "/%{prj.name}")
    objdir    ("Binaries/Intermediates/" .. OutputDir .. "/%{prj.name}")

    files {
        "tools/bsvx_cli.cpp",
        "tools/vox.hpp",
        "src/include/**.h"
    }

    includedirs { "src/include", "tools" }

    links { "bsvx" }
    dependson { "bsvx" }

    filter "toolset:msc"
        buildoptions { "/utf-8", "/Zc:__cplusplus" }

    filter "system:windows"
        systemversion "latest"
        defines { "NOMINMAX", "_CRT_SECURE_NO_WARNINGS" }

    filter "configurations:Debug"
        symbols "On"

    filter "configurations:Release"
        optimize "Speed"

    filter {}

    if SHARED then
        defines { "BSVX_ASSETS_USE_DLL" }
        filter "system:windows"
            postbuildcommands {
                '{COPYFILE} "%{wks.location}/Binaries/' .. OutputDir .. '/bsvx/bsvx.dll" "%{cfg.targetdir}"'
            }
        filter "system:not windows"
            linkoptions { "-Wl,-rpath,'$$ORIGIN/../bsvx'" }
        filter {}
    end

end

-- ------------------------------------------------------------------------------------------------
-- Godot GDExtension (optional)
-- ------------------------------------------------------------------------------------------------
--
-- godot-cpp has no premake build and generates ~2000 source files from extension_api.json through
-- its own CMake/SCons scripts. Reimplementing that here would be a second build definition of
-- somebody else's project, which is the exact failure mode this file exists to avoid on our own.
-- So godot-cpp stays a prebuilt input: build it once with integrations/godot/CMakeLists.txt, then
-- point these options at the result.

local godot_src = _OPTIONS["godot-cpp"]
local godot_gen = _OPTIONS["godot-cpp-gen"]
local godot_lib = _OPTIONS["godot-cpp-lib"]

if godot_src then
    if not (godot_gen and godot_lib) then
        error("--godot-cpp also needs --godot-cpp-gen and --godot-cpp-lib; " ..
              "build integrations/godot with CMake first, then point them at its build directory")
    end
    if SHARED then
        -- A GDExtension should be one file to ship. Linking a shared bsvx would put a second
        -- library beside it that Godot knows nothing about and the loader has to be taught to
        -- find, which is the rpath dance the static build exists to avoid.
        error("--godot-cpp requires --static-lib, so the extension carries bsvx inside itself")
    end

project "bsvx_godot"
    kind "SharedLib"
    language "C++"
    cppdialect "C++20"
    staticruntime (_OPTIONS["static-runtime"] and "on" or "off")
    visibility "Hidden"
    pic "On"

    targetdir "integrations/godot/project/addons/bsvx/bin"
    objdir    ("Binaries/Intermediates/" .. OutputDir .. "/%{prj.name}")

    files { "integrations/godot/src/**.cpp", "integrations/godot/src/**.h" }

    includedirs {
        "integrations/godot/src",
        "src/include",
        godot_gen,
        path.join(godot_src, "include"),
        path.join(godot_src, "gdextension")
    }

    links { "bsvx" }
    dependson { "bsvx" }
    linkoptions { godot_lib }

    -- Godot loads the file its .gdextension names, so the name is not ours to choose.
    targetprefix "lib"

    filter "configurations:Debug"
        targetname ("bsvx_godot." .. GODOT_PLATFORM .. ".template_debug.x86_64")
        symbols "On"

    filter "configurations:Release"
        targetname ("bsvx_godot." .. GODOT_PLATFORM .. ".template_release.x86_64")
        optimize "Speed"

    filter "toolset:msc"
        buildoptions { "/utf-8", "/bigobj", "/Zc:__cplusplus" }

    filter "system:windows"
        systemversion "latest"
        defines { "NOMINMAX", "_CRT_SECURE_NO_WARNINGS" }

    filter {}

end
