cmake_minimum_required(VERSION 3.28)

project(attome VERSION 0.1.0 LANGUAGES CXX)

option(ATTOME_PROFILING "Compile the zone profiler (ATM_PROFILE_SCOPE); OFF compiles it out" ON)
option(ATM_BUILD_TESTS "Build the unit tests" ON)
option(ATM_BUILD_BENCH "Build the local benchmarks (never gate CI)" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(tl-expected CONFIG REQUIRED)
find_package(BLAKE3 CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
find_package(Threads REQUIRED)

# Warnings are errors (MODULES §A.2.1).
add_library(atm_options INTERFACE)
if(MSVC)
  target_compile_options(atm_options INTERFACE /W4 /WX /permissive- /utf-8)
  target_compile_definitions(atm_options INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(atm_options INTERFACE -Wall -Wextra -Wpedantic -Werror)
endif()

# atm_module(<name> DEPS <targets…>): a static library core/<name> with public headers in include/atm/<mod>/.
# A module may link only the modules it lists (MODULES §B.0).
function(atm_module name)
  cmake_parse_arguments(ARG "" "" "DEPS" ${ARGN})
  file(GLOB sources CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/core/${name}/src/*.cpp)
  add_library(${name} STATIC ${sources})
  add_library(atm::${name} ALIAS ${name})
  target_include_directories(${name} PUBLIC ${CMAKE_SOURCE_DIR}/core/${name}/include)
  target_link_libraries(${name} PUBLIC ${ARG_DEPS} PRIVATE atm_options)
endfunction()

atm_module(atm_base DEPS nlohmann_json::nlohmann_json tl::expected)
target_link_libraries(atm_base PRIVATE BLAKE3::blake3 Threads::Threads)
target_compile_definitions(atm_base PUBLIC ATM_PROFILING=$<BOOL:${ATTOME_PROFILING}>)
atm_module(atm_storage DEPS atm_base)
# HTTP for downloads: WinHTTP on Windows (no dependency); other systems get their own transport later.
atm_module(atm_net DEPS atm_base)
if(WIN32)
  target_link_libraries(atm_net PRIVATE winhttp)
endif()
# The model store: catalog, resumable downloads, hash checks (ADR-031, M19).
atm_module(atm_models DEPS atm_net)
atm_module(atm_doc DEPS atm_base)
atm_module(atm_eval DEPS atm_base)
atm_module(atm_gen DEPS atm_base)
atm_module(atm_patch DEPS atm_doc atm_eval atm_gen)
# Media decode and encode: "mf" uses Windows Media Foundation, "ffmpeg" the portable LGPL FFmpeg backend (F1 plan).
set(ATM_MEDIA_BACKEND "auto" CACHE STRING "Media backend: auto (mf on Windows, ffmpeg elsewhere), mf or ffmpeg")
set_property(CACHE ATM_MEDIA_BACKEND PROPERTY STRINGS auto mf ffmpeg)
if(ATM_MEDIA_BACKEND STREQUAL "auto")
  if(WIN32)
    set(ATM_MEDIA_BACKEND_RESOLVED mf)
  else()
    set(ATM_MEDIA_BACKEND_RESOLVED ffmpeg)
  endif()
else()
  set(ATM_MEDIA_BACKEND_RESOLVED ${ATM_MEDIA_BACKEND})
endif()
message(STATUS "Attome media backend: ${ATM_MEDIA_BACKEND_RESOLVED}")
atm_module(atm_media DEPS atm_base)
if(WIN32)
  target_link_libraries(atm_media PRIVATE mfplat mfreadwrite mfuuid mf ole32 propsys d3d11 d2d1 dwrite windowscodecs)
endif()
# Text: "directwrite" on Windows, or the portable FreeType + HarfBuzz + SheenBidi with the bundled Noto fonts.
set(ATM_TEXT_BACKEND "auto" CACHE STRING "Text backend: auto (directwrite on Windows, freetype elsewhere), directwrite or freetype")
set_property(CACHE ATM_TEXT_BACKEND PROPERTY STRINGS auto directwrite freetype)
if(ATM_TEXT_BACKEND STREQUAL "auto")
  if(WIN32)
    set(ATM_TEXT_BACKEND_RESOLVED directwrite)
  else()
    set(ATM_TEXT_BACKEND_RESOLVED freetype)
  endif()
else()
  set(ATM_TEXT_BACKEND_RESOLVED ${ATM_TEXT_BACKEND})
endif()
message(STATUS "Attome text backend: ${ATM_TEXT_BACKEND_RESOLVED}")
if(ATM_TEXT_BACKEND_RESOLVED STREQUAL "freetype")
  find_package(Freetype REQUIRED)
  find_package(harfbuzz CONFIG REQUIRED)
  find_package(SheenBidi CONFIG REQUIRED)
  target_link_libraries(atm_media PRIVATE Freetype::Freetype harfbuzz::harfbuzz SheenBidi::SheenBidi)
  target_compile_definitions(atm_media PUBLIC ATM_TEXT_FREETYPE=1)
endif()
# Still images (see.frames, contact sheets): "wic" uses the Windows Imaging Component, "stb" the portable
# stb_image_write.
set(ATM_IMAGE_BACKEND "auto" CACHE STRING "Still image backend: auto (wic on Windows, stb elsewhere), wic or stb")
set_property(CACHE ATM_IMAGE_BACKEND PROPERTY STRINGS auto wic stb)
if(ATM_IMAGE_BACKEND STREQUAL "auto")
  if(WIN32)
    set(ATM_IMAGE_BACKEND_RESOLVED wic)
  else()
    set(ATM_IMAGE_BACKEND_RESOLVED stb)
  endif()
else()
  set(ATM_IMAGE_BACKEND_RESOLVED ${ATM_IMAGE_BACKEND})
endif()
message(STATUS "Attome image backend: ${ATM_IMAGE_BACKEND_RESOLVED}")
# stb_image reads still pictures on every system; stb_image_write is the portable still writer.
find_package(Stb REQUIRED)
target_include_directories(atm_media PRIVATE ${Stb_INCLUDE_DIR})
if(ATM_IMAGE_BACKEND_RESOLVED STREQUAL "stb")
  target_compile_definitions(atm_media PUBLIC ATM_IMAGE_STB=1)
endif()
# The bundled fonts (SIL OFL) sit in fonts/ next to the programs.
file(COPY ${CMAKE_SOURCE_DIR}/assets/fonts DESTINATION ${CMAKE_BINARY_DIR}/bin)
if(ATM_MEDIA_BACKEND_RESOLVED STREQUAL "ffmpeg")
  find_package(FFMPEG REQUIRED)
  target_include_directories(atm_media PRIVATE ${FFMPEG_INCLUDE_DIRS})
  target_link_libraries(atm_media PRIVATE ${FFMPEG_LIBRARIES})
  target_compile_definitions(atm_media PUBLIC ATM_MEDIA_FFMPEG=1)
endif()
atm_module(atm_render DEPS atm_media atm_eval)
# Skills: how-to guides for agents, built into the program. Every file below core/atm_skills/skills is embedded as bytes in a source
# generated here (at configure time; a change to a file re-runs configure).
atm_module(atm_skills DEPS atm_base)
file(GLOB_RECURSE ATM_SKILL_FILES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/core/atm_skills/skills/*)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${ATM_SKILL_FILES})
set(ATM_SKILLS_CPP ${CMAKE_BINARY_DIR}/gen/atm_skills_data.cpp)
set(ATM_SKILLS_BODY "")
set(ATM_SKILLS_TABLE "")
set(ATM_SKILL_N 0)
foreach(f ${ATM_SKILL_FILES})
  file(RELATIVE_PATH rel ${CMAKE_SOURCE_DIR}/core/atm_skills/skills ${f})
  file(READ ${f} hex HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
  string(APPEND ATM_SKILLS_BODY "static const unsigned char kSkill${ATM_SKILL_N}[] = {${bytes}0};
")
  string(APPEND ATM_SKILLS_TABLE "    {\"${rel}\", std::string_view(reinterpret_cast<const char *>(kSkill${ATM_SKILL_N}), sizeof(kSkill${ATM_SKILL_N}) - 1)},
")
  math(EXPR ATM_SKILL_N "${ATM_SKILL_N} + 1")
endforeach()
file(WRITE ${ATM_SKILLS_CPP}.tmp "// Generated by CMake from core/atm_skills/skills. Do not edit.
#include \"atm/skills/skills.hpp\"
namespace atm::skills {
namespace {
${ATM_SKILLS_BODY}const File kFiles[] = {
${ATM_SKILLS_TABLE}    {\"\", {}}};
} // namespace
std::span<const File> files() { return std::span<const File>(kFiles, sizeof(kFiles) / sizeof(kFiles[0]) - 1); }
} // namespace atm::skills
")
file(COPY_FILE ${ATM_SKILLS_CPP}.tmp ${ATM_SKILLS_CPP} ONLY_IF_DIFFERENT)
target_sources(atm_skills PRIVATE ${ATM_SKILLS_CPP})
atm_module(atm_api DEPS atm_patch atm_storage atm_render atm_models atm_skills)

add_executable(attomed core/daemon/main.cpp)
target_link_libraries(attomed PRIVATE atm_api CLI11::CLI11 atm_options)

add_executable(attome cli/main.cpp)
target_link_libraries(attome PRIVATE atm_api CLI11::CLI11 atm_options)

option(ATM_BUILD_EDITOR "Build the editor (SDL3 + Dear ImGui)" ON)
if(ATM_BUILD_EDITOR)
  find_package(SDL3 CONFIG REQUIRED)
  find_package(imgui CONFIG REQUIRED)
  file(GLOB editor_sources CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/app/editor/*.cpp)
  add_executable(attome-editor WIN32 ${editor_sources})
  target_link_libraries(attome-editor PRIVATE atm_api SDL3::SDL3 imgui::imgui atm_options)
endif()

if(ATM_BUILD_TESTS)
  find_package(Catch2 3 CONFIG REQUIRED)
  file(GLOB test_sources CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/core/*/tests/*.cpp)
  add_executable(atm_tests ${test_sources})
  target_link_libraries(atm_tests PRIVATE atm_api Catch2::Catch2WithMain atm_options)
  enable_testing()
  add_test(NAME atm_tests COMMAND atm_tests)
endif()

if(ATM_BUILD_BENCH)
  add_executable(atm_bench tests/bench/bench_main.cpp)
  target_link_libraries(atm_bench PRIVATE atm_api atm_options)
endif()
