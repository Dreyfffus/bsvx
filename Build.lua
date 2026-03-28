workspace "bsvx"
    architecture "x86_64"
    configurations {"Debug", "Release"}
    startproject "bsvx_tests"


OutputDir = "%{cfg.system}-%{cfg.architecture}/%{cfg.buildcfg}"

project "bsvx"
    kind "SharedLib"
    language "C++"
    cppdialect "C++latest"
    staticruntime "off"
    toolset "msc"

    targetdir ("Binaries/" .. OutputDir .. "/%{prj.name}")
    objdir    ("Binaries/Intermediates/" .. OutputDir .. "/%{prj.name}")
    implibdir ("Binaries/" .. OutputDir .. "/%{prj.name}")
    implibname ("%{prj.name}")

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

    defines {
        "BSVX_ASSETS_BUILD_DLL",
        "TOML_HEADER_ONLY=0"
    }

    filter "system:windows"
        systemversion "latest"
        defines { "WINDOWS", "NOMINMAX", "_CRT_SECURE_NO_WARNINGS" }

    filter "configurations:Debug"
        defines { "DEBUG" }
        symbols "On"

    filter "configurations:Release"
        defines { "RELEASE" }
        optimize "Speed"

    filter{}

project "bsvx_tests"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++latest"
    staticruntime "off"

    targetdir ("Binaries/" .. OutputDir .. "/%{prj.name}")
    objdir    ("Binaries/Intermediates/" .. OutputDir .. "/%{prj.name}")

    files {
        "test/**.cpp",
        "src/**.h"
    }

    includedirs {
        "src/include"
    }

    links { "bsvx" }
    dependson { "bsvx" }

    defines {
        "BSVX_ASSETS_USE_DLL",
    }

    postbuildcommands {
    '{COPY} "%{wks.location}/Binaries/' .. OutputDir .. '/bsvx/bsvx.dll" "%{cfg.targetdir}"'
    }

    filter "system:windows"
        systemversion "latest"
        defines { "NOMINMAX", "_CRT_SECURE_NO_WARNINGS" }

    filter "configurations:Debug"
        symbols "On"

    filter "configurations:Release"
        optimize "Speed"

    filter{}
 