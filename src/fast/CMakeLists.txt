#=================== Top-Level ===================
file(GLOB Source_Files__TopLevel RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "*.cpp")

source_group("" FILES ${Source_Files__TopLevel})
target_sources(libultraship PRIVATE ${Source_Files__TopLevel})

#=================== Debug ===================

file(GLOB Source_Files__Debug RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "debug/*.cpp")

source_group("debug" FILES ${Source_Files__Debug})
target_sources(libultraship PRIVATE ${Source_Files__Debug})

#=================== Resource ===================

file(GLOB Source_Files__Resource__Types RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "resource/type/*.h" "resource/type/*.cpp")
source_group("resource/type" FILES ${Source_Files__Resource__Types})

file(GLOB Source_Files__Resource__Factories RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "resource/factory/*.h" "resource/factory/*.cpp")
source_group("resource/factory" FILES ${Source_Files__Resource__Factories})

target_sources(libultraship PRIVATE ${Source_Files__Resource} ${Source_Files__Resource__Types} ${Source_Files__Resource__Factories} ${Source_Files__Resource__Archive})

#=================== Graphic ===================

file(GLOB Source_Files__Graphic RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "backends/*.cpp" "backends/*.c")

if (NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    list(FILTER Source_Files__Graphic EXCLUDE REGEX "graphic/Fast3D/backends/gfx_dxgi*")
    list(FILTER Source_Files__Graphic EXCLUDE REGEX "graphic/Fast3D/backends/gfx_direct3d*")
    list(FILTER Source_Files__Graphic EXCLUDE REGEX "graphic/Fast3D/backends/dxsdk/*")
endif()

if (NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    list(FILTER Source_Files__Graphic EXCLUDE REGEX "graphic/Fast3D/backends/gfx_metal*")
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
    list(FILTER Source_Files__Graphic EXCLUDE REGEX "graphic/Fast3D/backends/gfx_opengl*")
endif()

# [port] The GX2 backend only builds against devkitPro's headers, and nothing
# excluded it anywhere - it compiled fine on Wii U and broke every other target
# the moment one was attempted.
#
# Note the filters above never fire: the glob is RELATIVE, so entries look like
# "backends/gfx_dxgi.cpp" and those patterns expect "graphic/Fast3D/backends/...".
# Left alone rather than corrected, because the Wii U build currently compiles
# those files and relies on their internal #ifdefs; this rule uses the path form
# the list actually contains.
if (NOT CMAKE_SYSTEM_NAME STREQUAL "CafeOS")
    list(FILTER Source_Files__Graphic EXCLUDE REGEX "backends/gx2_")
    list(FILTER Source_Files__Graphic EXCLUDE REGEX "backends/gfx_gx2")
    list(FILTER Source_Files__Graphic EXCLUDE REGEX "backends/gfx_wiiu")
endif()

source_group("graphic" FILES ${Source_Files__Graphic})
target_sources(libultraship PRIVATE ${Source_Files__Graphic})
