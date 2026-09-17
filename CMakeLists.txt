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

if(MSVC)
  add_compile_options(/W3 /permissive- /utf-8 /EHsc /Zc:__cplusplus /wd4244 /wd4267)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  add_compile_options(-Wall -Wextra -Wshadow -Wno-missing-field-initializers)
endif()

# ---------------------------------------------------------------------------
# Core rules library (no dependencies)
add_library(rl_core STATIC src/core/tetris.cpp src/core/inflate.cpp src/core/png_read.cpp src/core/pixel_scale.cpp)
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
  find_program(GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin" REQUIRED)

  # Compile GLSL -> SPIR-V -> embedded C++ arrays.
  set(SHADER_SOURCES
      shaders/cube.vert shaders/cube.frag
      shaders/quad.vert shaders/quad.frag
      shaders/shadow_cube.vert shaders/shadow_quad.vert shaders/shadow_quad.frag
      shaders/mesh.vert shaders/mesh.frag shaders/shadow_mesh.vert
      shaders/cube_rt.frag shaders/mesh_rt.frag shaders/quad_rt.vert
      shaders/post.vert shaders/bright.frag shaders/blur.frag shaders/composite.frag shaders/volume.frag)
  file(GLOB SHADER_INCLUDES ${CMAKE_SOURCE_DIR}/shaders/*.glsl)
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
      DEPENDS ${CMAKE_SOURCE_DIR}/${src} ${SHADER_INCLUDES}
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
      src/render/ktx2.cpp
      src/game/assets.cpp
      src/game/procedural.cpp
      src/game/app.cpp
      src/game/fps_mode.cpp
      src/game/highscores.cpp
      src/game/trophies.cpp
      src/game/ambient.cpp
      src/game/kvx.cpp
      src/game/voxels.cpp
      src/game/playerstats.cpp
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
  if(WIN32)
    # GUI subsystem (no console window); SDL_main.h in main.cpp supplies WinMain.
    enable_language(RC)
    set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -I${CMAKE_SOURCE_DIR}/packaging/windows")
    set_target_properties(redline PROPERTIES WIN32_EXECUTABLE ON)
    target_sources(redline PRIVATE packaging/windows/redline.rc)
  endif()
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
  # Without pkg-config (vcpkg on Windows) the same libraries come as CMake packages.
  if(NOT VORBISFILE_FOUND)
    find_package(Vorbis CONFIG QUIET)
    if(TARGET Vorbis::vorbisfile)
      target_link_libraries(redline PRIVATE Vorbis::vorbisfile)
      target_compile_definitions(redline PRIVATE REDLINE_HAVE_VORBIS=1)
      message(STATUS "libvorbisfile (CMake package) found: streamed Ogg music enabled")
    else()
      message(STATUS "libvorbisfile not found: the rerelease Ogg soundtracks will be unavailable")
    endif()
  endif()
  if(NOT FLUIDSYNTH_FOUND)
    find_package(FluidSynth CONFIG QUIET)
    if(TARGET FluidSynth::libfluidsynth)
      target_link_libraries(redline PRIVATE FluidSynth::libfluidsynth)
      target_compile_definitions(redline PRIVATE REDLINE_HAVE_FLUIDSYNTH=1)
      message(STATUS "FluidSynth (CMake package) found: soundfont music backend enabled")
    endif()
  endif()

  # ---------------------------------------------------------------------------
  # Install + packages. No Doom data is ever installed: the game finds or asks
  # for the player's own WAD (see docs/packaging.md).
  include(GNUInstallDirs)
  install(TARGETS redline RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(FILES README.md LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})
  install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DOCDIR}/docs FILES_MATCHING PATTERN "*.md")
  # The Voxel Doom pack (MIT) ships with the game: next to the exe on Windows, under share/ on Linux.
  if(WIN32)
    # The DLLs of every imported dependency (SDL3, vorbis, ogg ...) go next to the exe.
    install(FILES $<TARGET_RUNTIME_DLLS:redline> DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(DIRECTORY assets/voxel-doom/ DESTINATION ${CMAKE_INSTALL_BINDIR}/voxels)
    install(DIRECTORY assets/materials/ DESTINATION ${CMAKE_INSTALL_BINDIR}/materials)
    install(DIRECTORY assets/brutal/ DESTINATION ${CMAKE_INSTALL_BINDIR}/brutal)
    install(DIRECTORY assets/font-hd/ DESTINATION ${CMAKE_INSTALL_BINDIR}/font-hd)
  elseif(UNIX AND NOT APPLE)
    install(DIRECTORY assets/voxel-doom/ DESTINATION ${CMAKE_INSTALL_DATADIR}/redline/voxel-doom)
    install(DIRECTORY assets/materials/ DESTINATION ${CMAKE_INSTALL_DATADIR}/redline/materials)
    install(DIRECTORY assets/brutal/ DESTINATION ${CMAKE_INSTALL_DATADIR}/redline/brutal)
    install(DIRECTORY assets/font-hd/ DESTINATION ${CMAKE_INSTALL_DATADIR}/redline/font-hd)
    install(FILES packaging/linux/redline.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES packaging/linux/redline.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps)
    install(FILES packaging/linux/redline-48.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/48x48/apps RENAME redline.png)
  endif()
  set(CPACK_PACKAGE_NAME "redline")
  set(CPACK_PACKAGE_VENDOR "David Janice")
  set(CPACK_PACKAGE_CONTACT "David Janice")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Tetris that tips over into a Doom fight (needs your own doom.wad)")
  set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
  set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_SOURCE_DIR}/LICENSE)
  set(CPACK_STRIP_FILES ON)
  if(WIN32)
    set(CPACK_GENERATOR "ZIP")   # the real Windows installer is packaging/windows/redline.iss (Inno Setup)
  else()
    set(CPACK_GENERATOR "TGZ")   # add DEB / RPM on the command line: cpack -G DEB
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_SECTION "games")
    set(CPACK_RPM_PACKAGE_LICENSE "MIT")
    set(CPACK_RPM_PACKAGE_GROUP "Amusements/Games")
  endif()
  include(CPack)

  if(REDLINE_BUILD_TESTS)
    add_executable(opl_test tests/opl_test.cpp src/audio/opl3.cpp)
    target_include_directories(opl_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME opl_test COMMAND opl_test)
    add_executable(music_test tests/music_test.cpp src/audio/opl3.cpp src/audio/opl_driver.cpp src/audio/genmidi.cpp src/audio/mus.cpp)
    target_include_directories(music_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME music_test COMMAND music_test)
  endif()
  if(REDLINE_BUILD_TESTS)
    add_executable(stats_test tests/stats_test.cpp src/game/playerstats.cpp)
    target_include_directories(stats_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME stats_test COMMAND stats_test)
    add_executable(kvx_test tests/kvx_test.cpp src/game/kvx.cpp)
    target_include_directories(kvx_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(kvx_test PRIVATE glm::glm)
    add_test(NAME kvx_test COMMAND kvx_test)
    add_executable(ktx2_test tests/ktx2_test.cpp src/render/ktx2.cpp)
    target_include_directories(ktx2_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(ktx2_test PRIVATE REDLINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME ktx2_test COMMAND ktx2_test)
    add_executable(png_test tests/png_test.cpp)
    target_include_directories(png_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(png_test PRIVATE rl_core)
    target_compile_definitions(png_test PRIVATE REDLINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME png_test COMMAND png_test)
  endif()
  add_executable(musrender tools/musrender.cpp src/audio/opl3.cpp src/audio/opl_driver.cpp src/audio/genmidi.cpp src/audio/mus.cpp)
  target_include_directories(musrender PRIVATE ${CMAKE_SOURCE_DIR}/src)
endif()
