# SPDX-License-Identifier: LGPL-2.1-or-later
#
# WebGTK build.
#
#   libwebgtk.so   -> GTK3 view + backends
#   libwebgtk4.so  -> GTK4 view + backends
#   gtk3-demo      -> GTK3 embedder demo
#   gtk4-demo      -> GTK4 embedder demo
#
# The Blink/V8 engine archive is optional. Point WEBGTK_ENGINE_DIR at a tree
# containing include/ and lib/ to link the real backend; otherwise the mock
# backend is used and everything still builds and tests.

cmake_minimum_required(VERSION 3.16)
project(webgtk VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(WEBGTK_BUILD_TESTS "Build the WebGTK test suite" ON)
option(WEBGTK_BUILD_DEMOS "Build the GTK3/GTK4 demos" ON)
option(WEBGTK_ENABLE_SANITIZERS "Enable ASan/UBSan in Debug builds" ON)
set(WEBGTK_ENGINE_DIR "" CACHE PATH
    "Directory containing the pruned Blink/V8 engine (include/ and lib/)")

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)

set(WEBGTK_SANITIZE_FLAGS "")
if(WEBGTK_ENABLE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug"
   AND NOT MSVC)
  set(WEBGTK_SANITIZE_FLAGS -fsanitize=address,undefined
                           -fno-omit-frame-pointer)
  # Applied globally: executables must link the runtime too, otherwise the
  # shared library's ASan cannot initialise.
  add_compile_options(${WEBGTK_SANITIZE_FLAGS})
  add_link_options(${WEBGTK_SANITIZE_FLAGS})
endif()

set(WEBGTK_SOURCES src/backend_mock.cc src/webgtk_view.cc)
if(WEBGTK_ENGINE_DIR)
  list(APPEND WEBGTK_SOURCES src/backend_chromium.cc)
endif()

# One object library per GTK major version: webgtk_view.cc is compiled twice
# with different WEBGTK_GTK4 settings, so the two shared libraries can never
# share object files.
function(webgtk_add_objects target gtk_target)
  add_library(${target} OBJECT ${WEBGTK_SOURCES})
  target_include_directories(${target}
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(${target} PUBLIC ${gtk_target} epoxy)
  if(WEBGTK_ENGINE_DIR)
    target_compile_definitions(${target} PUBLIC WEBGTK_HAVE_ENGINE=1)
    # A Chromium embedder compiles against the checkout, not a staged SDK:
    # out/<name>/gen holds generated headers, the root holds the rest.
    target_include_directories(${target} PRIVATE
      ${WEBGTK_ENGINE_DIR}/chromium
      ${WEBGTK_ENGINE_DIR}/chromium/out/webgtk/gen)
  endif()
endfunction()

function(webgtk_add_shared target objects)
  add_library(${target} SHARED)
  # PUBLIC so consumers inherit the include dirs and GTK dependency.
  target_link_libraries(${target} PUBLIC ${objects})
  if(WEBGTK_ENGINE_DIR)
    target_link_directories(${target} PRIVATE ${WEBGTK_ENGINE_DIR}/lib)
    target_link_libraries(${target} PRIVATE webgtk_engine)
  endif()
endfunction()

webgtk_add_objects(webgtk_gtk3_objs PkgConfig::GTK3)
webgtk_add_objects(webgtk_gtk4_objs PkgConfig::GTK4)
target_compile_definitions(webgtk_gtk4_objs PRIVATE WEBGTK_GTK4=1)

webgtk_add_shared(webgtk webgtk_gtk3_objs)
webgtk_add_shared(webgtk4 webgtk_gtk4_objs)

if(WEBGTK_ENGINE_DIR)
  message(STATUS "WebGTK: linking engine from ${WEBGTK_ENGINE_DIR}")
else()
  message(STATUS "WebGTK: no engine configured, mock backend only")
endif()

# ---- Demos ---------------------------------------------------------------
if(WEBGTK_BUILD_DEMOS)
  add_executable(gtk3-demo examples/webgtk_demo.cc)
  target_link_libraries(gtk3-demo PRIVATE webgtk PkgConfig::GTK3)

  add_executable(gtk4-demo examples/webgtk_demo.cc)
  target_compile_definitions(gtk4-demo PRIVATE WEBGTK_GTK4=1)
  target_link_libraries(gtk4-demo PRIVATE webgtk4 PkgConfig::GTK4)
endif()

# ---- Tests ---------------------------------------------------------------
if(WEBGTK_BUILD_TESTS)
  enable_testing()
  find_program(WEBGTK_XVFB xvfb-run)

  # test_webgtk.c is plain C: it proves the ABI needs no C++ or GObject headers.
  add_executable(test-webgtk tests/test_webgtk.c)
  target_link_libraries(test-webgtk PRIVATE webgtk PkgConfig::GTK3)

  add_executable(test-webgtk4 tests/test_webgtk.c)
  target_compile_definitions(test-webgtk4 PRIVATE WEBGTK_GTK4=1)
  target_link_libraries(test-webgtk4 PRIVATE webgtk4 PkgConfig::GTK4)

  # A GtkGLArea needs a display; xvfb-run supplies one headlessly.
  if(WEBGTK_XVFB)
    set(WEBGTK_TEST_LAUNCHER ${WEBGTK_XVFB} -a)
  else()
    message(WARNING "xvfb-run not found: GUI tests run against the real display")
    set(WEBGTK_TEST_LAUNCHER "")
  endif()
  # Software GL plus the Mesa/dbus leak suppressions: a GtkGLArea keeps
  # process-lifetime allocations in the system GL stack that LSan flags.
  set(WEBGTK_TEST_ENV
      "GSK_RENDERER=cairo;LIBGL_ALWAYS_SOFTWARE=1;GALLIUM_DRIVER=llvmpipe;LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/lsan.supp:print_suppressions=0")

  add_test(NAME webgtk-abi-gtk3
           COMMAND ${WEBGTK_TEST_LAUNCHER} $<TARGET_FILE:test-webgtk>)
  add_test(NAME webgtk-abi-gtk4
           COMMAND ${WEBGTK_TEST_LAUNCHER} $<TARGET_FILE:test-webgtk4>)
  set_tests_properties(webgtk-abi-gtk3 webgtk-abi-gtk4
                       PROPERTIES ENVIRONMENT "${WEBGTK_TEST_ENV}")

  if(WEBGTK_BUILD_DEMOS)
    add_test(NAME webgtk-demo-gtk3-selftest
             COMMAND ${WEBGTK_TEST_LAUNCHER} $<TARGET_FILE:gtk3-demo>
                     --selftest)
    add_test(NAME webgtk-demo-gtk4-selftest
             COMMAND ${WEBGTK_TEST_LAUNCHER} $<TARGET_FILE:gtk4-demo>
                     --selftest)
    set_tests_properties(webgtk-demo-gtk3-selftest webgtk-demo-gtk4-selftest
                         PROPERTIES ENVIRONMENT "${WEBGTK_TEST_ENV}")
  endif()
endif()
