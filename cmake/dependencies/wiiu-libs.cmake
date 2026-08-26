find_package(SDL2 REQUIRED)

target_link_libraries(ImGui PUBLIC SDL2::SDL2-static)
target_include_directories(ImGui PUBLIC ${DEVKITPRO}/portlibs/wiiu/include)

set(ADDITIONAL_LIB_INCLUDES ${ADDITIONAL_LIB_INCLUDES}
    ${DEVKITPRO}/portlibs/wiiu/include
    ${DEVKITPRO}/portlibs/ppc/include
    PARENT_SCOPE
)

# wut has no execvp, and ImGui's default "open in shell" implementation calls it
target_compile_definitions(ImGui PUBLIC IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
