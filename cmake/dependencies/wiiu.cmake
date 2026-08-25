include(FetchContent)

# Included ahead of common.cmake: FetchContent honours the first declaration of
# a dependency, and the Wii U needs spdlog and ThreadPool patched before
# common.cmake can declare them unpatched.

#=================== nlohmann-json ===================
find_package(nlohmann_json QUIET)
if (NOT ${nlohmann_json_FOUND})
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
        OVERRIDE_FIND_PACKAGE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

#=================== spdlog ===================
# wut has no pthreads for CMake's Threads package to find
set(spdlog_patch_file ${CMAKE_CURRENT_SOURCE_DIR}/cmake/dependencies/patches/spdlog-wiiu.patch)
set(spdlog_apply_patch_command ${CMAKE_COMMAND} -Dpatch_file=${spdlog_patch_file} -Dwith_reset=TRUE -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/dependencies/git-patch.cmake)

find_package(spdlog QUIET)
if (NOT ${spdlog_FOUND})
    FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.15.0
        OVERRIDE_FIND_PACKAGE
        PATCH_COMMAND ${spdlog_apply_patch_command}
    )

    option(SPDLOG_BUILD_EXAMPLE "" OFF)
    option(SPDLOG_BUILD_TESTS "" OFF)
    option(SPDLOG_INSTALL "" OFF)

    # Disable TLS and thread id on Wii U
    option(SPDLOG_NO_TLS "" ON)
    option(SPDLOG_NO_THREAD_ID "" ON)

    FetchContent_MakeAvailable(spdlog)
endif()

#======== thread-pool ========
# thread_local is unavailable on this target
set(threadpool_patch_file ${CMAKE_CURRENT_SOURCE_DIR}/cmake/dependencies/patches/threadpool-wiiu.patch)
set(threadpool_apply_patch_command ${CMAKE_COMMAND} -Dpatch_file=${threadpool_patch_file} -Dwith_reset=TRUE -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/dependencies/git-patch.cmake)

FetchContent_Declare(
    ThreadPool
    GIT_REPOSITORY https://github.com/bshoshany/thread-pool.git
    GIT_TAG v4.1.0
    PATCH_COMMAND ${threadpool_apply_patch_command}
)
FetchContent_MakeAvailable(ThreadPool)
list(APPEND ADDITIONAL_LIB_INCLUDES ${threadpool_SOURCE_DIR}/include)
