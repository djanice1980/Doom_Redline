# rl_wad — dependency-free Doom WAD reader.
# Expected to be pulled in from the project root with add_subdirectory(src/wad);
# paths are relative to this directory so it also builds from a foreign root.

set(RL_WAD_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

add_library(rl_wad STATIC
    wad.cpp
    graphics.cpp
    texture.cpp
    sound.cpp
)
target_compile_features(rl_wad PUBLIC cxx_std_20)
target_include_directories(rl_wad PUBLIC "${RL_WAD_ROOT}/src")

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(RL_WAD_WARNINGS -Wall -Wextra)
else()
    set(RL_WAD_WARNINGS)
endif()
target_compile_options(rl_wad PRIVATE ${RL_WAD_WARNINGS})

# enable_testing() is idempotent; calling it here makes add_test register even
# when the parent adds this directory before its own enable_testing() call.
enable_testing()
add_executable(wad_test "${RL_WAD_ROOT}/tests/wad_test.cpp")
target_link_libraries(wad_test PRIVATE rl_wad)
target_compile_options(wad_test PRIVATE ${RL_WAD_WARNINGS})
add_test(NAME wad_test COMMAND wad_test)

add_executable(wadinfo "${RL_WAD_ROOT}/tools/wadinfo.cpp")
target_link_libraries(wadinfo PRIVATE rl_wad)
target_compile_options(wadinfo PRIVATE ${RL_WAD_WARNINGS})
