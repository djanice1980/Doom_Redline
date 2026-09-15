cmake_minimum_required(VERSION 3.24)
project(redline VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(REDLINE_BUILD_GAME "Build the Vulkan game executable (needs SDL3, Vulkan, glm, glslc)" ON)
option(REDLINE_BUILD_TESTS "Build unit tests" ON)

add_compile_options(-Wall -Wextra -Wshadow -Wno-missing-field-initializers)

# ---------------------------------------------------------------------------
# Core rules library (no dependencies)
add_library(rl_core STATIC src/core/tetris.cpp)
target_include_directories(rl_core PUBLIC ${CMAKE_SOURCE_DIR}/src)

if(REDLINE_BUILD_TESTS)
  enable_testing()
endif()

# WAD reader library (no dependencies)
if(EXISTS ${CMAKE_SOURCE_DIR}/src/wad/CMakeLists.txt)
  add_subdirectory(src/wad)
  set(REDLINE_HAVE_WAD ON)
endif()

# ---------------------------------------------------------------------------
if(REDLINE_BUILD_TESTS)
  add_executable(tetris_test tests/tetris_test.cpp)
  target_link_libraries(tetris_test PRIVATE rl_core)
  add_test(NAME tetris_test COMMAND tetris_test)
endif()

# ---------------------------------------------------------------------------
if(REDLINE_BUILD_GAME)
  find_package(Vulkan REQUIRED)
  find_package(SDL3 REQUIRED CONFIG)
  find_package(glm REQUIRED CONFIG)
  find_program(GLSLC glslc REQUIRED)

  # Compile GLSL -> SPIR-V -> embedded C++ arrays.
  set(SHADER_SOURCES
      shaders/cube.vert shaders/cube.frag
      shaders/quad.vert shaders/quad.frag)
  set(SHADER_HEADERS)
  foreach(src ${SHADER_SOURCES})
    get_filename_component(name ${src} NAME)
    string(REPLACE "." "_" sym ${name})
    set(spv ${CMAKE_BINARY_DIR}/shaders/${name}.spv)
    set(hdr ${CMAKE_BINARY_DIR}/shaders/${sym}.h)
    add_custom_command(
      OUTPUT ${spv}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders
      COMMAND ${GLSLC} --target-env=vulkan1.3 -O -o ${spv} ${CMAKE_SOURCE_DIR}/${src}
      DEPENDS ${CMAKE_SOURCE_DIR}/${src} ${CMAKE_SOURCE_DIR}/shaders/common.glsl
      COMMENT "glslc ${src}")
    add_custom_command(
      OUTPUT ${hdr}
      COMMAND ${CMAKE_COMMAND} -DINPUT=${spv} -DOUTPUT=${hdr} -DSYMBOL=${sym}
              -P ${CMAKE_SOURCE_DIR}/tools/embed.cmake
      DEPENDS ${spv} ${CMAKE_SOURCE_DIR}/tools/embed.cmake
      COMMENT "embed ${name}")
    list(APPEND SHADER_HEADERS ${hdr})
  endforeach()
  add_custom_target(rl_shaders DEPENDS ${SHADER_HEADERS})

  add_executable(redline
      src/main.cpp
      src/render/vk_context.cpp
      src/render/renderer.cpp
      src/render/atlas.cpp
      src/game/assets.cpp
      src/game/procedural.cpp
      src/game/app.cpp
      src/game/fps_mode.cpp
      src/game/highscores.cpp
      src/game/trophies.cpp
      src/audio/audio.cpp
      src/audio/opl3.cpp
      src/audio/opl_driver.cpp
      src/audio/genmidi.cpp
      src/audio/mus.cpp
      src/audio/music.cpp
      src/audio/oggstream.cpp)
  add_dependencies(redline rl_shaders)
  target_include_directories(redline PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR})
  target_link_libraries(redline PRIVATE rl_core Vulkan::Vulkan SDL3::SDL3 glm::glm)
  target_compile_definitions(redline PRIVATE GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RADIANS GLM_ENABLE_EXPERIMENTAL)
  if(REDLINE_HAVE_WAD)
    target_link_libraries(redline PRIVATE rl_wad)
    target_compile_definitions(redline PRIVATE REDLINE_HAVE_WAD=1)
  endif()

  # Optional FluidSynth backend (used at runtime only when a soundfont is found).
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(FLUIDSYNTH IMPORTED_TARGET fluidsynth)
    if(FLUIDSYNTH_FOUND)
      target_link_libraries(redline PRIVATE PkgConfig::FLUIDSYNTH)
      target_compile_definitions(redline PRIVATE REDLINE_HAVE_FLUIDSYNTH=1)
      message(STATUS "FluidSynth ${FLUIDSYNTH_VERSION} found: soundfont music backend enabled")
    endif()
    pkg_check_modules(VORBISFILE IMPORTED_TARGET vorbisfile)
    if(VORBISFILE_FOUND)
      target_link_libraries(redline PRIVATE PkgConfig::VORBISFILE)
      target_compile_definitions(redline PRIVATE REDLINE_HAVE_VORBIS=1)
      message(STATUS "libvorbisfile ${VORBISFILE_VERSION} found: streamed Ogg music enabled")
    endif()
  endif()

  if(REDLINE_BUILD_TESTS)
    add_executable(opl_test tests/opl_test.cpp src/audio/opl3.cpp)
    target_include_directories(opl_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME opl_test COMMAND opl_test)
    add_executable(music_test tests/music_test.cpp src/audio/opl3.cpp src/audio/opl_driver.cpp src/audio/genmidi.cpp src/audio/mus.cpp)
    target_include_directories(music_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME music_test COMMAND music_test)
  endif()
  add_executable(musrender tools/musrender.cpp src/audio/opl3.cpp src/audio/opl_driver.cpp src/audio/genmidi.cpp src/audio/mus.cpp)
  target_include_directories(musrender PRIVATE ${CMAKE_SOURCE_DIR}/src)
endif()
