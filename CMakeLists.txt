# SPDX-License-Identifier: LGPL-2.1-or-later
#
# WebGTK build.
#
#   libwebgtk.so   -> GTK3 view + backends
#   libwebgtk4.so  -> GTK4 view + backends
#   gtk3-demo      -> GTK3 embedder demo
#   gtk4-demo      -> GTK4 embedder demo
#
# The Blink/V8 engine is optional. Download the prebuilt engine
# (scripts/download_engine_prebuilt.sh) to link the real backend; otherwise the
# mock backend is used and everything still builds and tests. Pointing
# WEBGTK_ENGINE_DIR at a Chromium source tree adds a compile-verification target
# for the backend instead of linking it.

cmake_minimum_required(VERSION 3.16)
project(webgtk VERSION 0.1.0 LANGUAGES C CXX)

# GNUInstallDirs gives the canonical lib/include/share names and, on Debian and
# Ubuntu, the multiarch library dir. The .pc files and the packages rely on it.
include(GNUInstallDirs)

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
option(ENABLE_INTROSPECTION
       "Generate GObject Introspection bindings when the tooling is present" ON)
option(WEBGTK_ENABLE_SANITIZERS "Enable ASan/UBSan in Debug builds" ON)
set(WEBGTK_ENGINE_DIR "" CACHE PATH
    "Chromium source tree to compile the engine backend against")
option(WEBGTK_USE_PREBUILT_ENGINE
       "Link the prebuilt engine in engine/prebuilt (see scripts/download_engine_prebuilt.sh)"
       OFF)

# Engine resolution, in priority order:
#   1. WEBGTK_ENGINE_DIR      - compile-check the backend against a source tree
#   2. WEBGTK_USE_PREBUILT_ENGINE=ON - link engine/prebuilt
#   3. engine/prebuilt exists  - use it automatically
#   4. none                    - mock backend only
#
# A Chromium source tree alone cannot be linked from: the pruned archive has no
# build outputs, and compiling src/backend_chromium.cc needs the exact flags gn
# emits (which CMake cannot reproduce). So WEBGTK_ENGINE_DIR selects a *compile
# verification* build: the backend is checked against the real headers by the
# `webgtk-verify-engine` target, and the mock backend still backs the demos and
# tests. Linking the real backend happens through the prebuilt artifact.
set(WEBGTK_ENGINE_PREBUILT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/engine/prebuilt")

if(WEBGTK_ENGINE_DIR)
  set(WEBGTK_ENGINE_MODE "source")
elseif(WEBGTK_USE_PREBUILT_ENGINE)
  set(WEBGTK_ENGINE_MODE "prebuilt")
elseif(EXISTS "${WEBGTK_ENGINE_PREBUILT_DIR}/manifest.env")
  set(WEBGTK_ENGINE_MODE "prebuilt")
else()
  set(WEBGTK_ENGINE_MODE "mock")
endif()

# Only a prebuilt artifact is linkable, so only then is the Chromium backend
# compiled into the library. Source mode compile-checks it out of band.
if(WEBGTK_ENGINE_MODE STREQUAL "prebuilt")
  set(WEBGTK_HAVE_ENGINE 1)
else()
  set(WEBGTK_HAVE_ENGINE 0)
endif()

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

set(WEBGTK_SOURCES src/backend_mock.cc src/webgtk_view.cc
                   src/webgtk_bridge_script.cc)
if(WEBGTK_HAVE_ENGINE)
  list(APPEND WEBGTK_SOURCES src/backend_chromium.cc)
endif()

# Compilation happens in the OBJECT library, so the engine headers go there.
# A prebuilt build compiles against the staged headers and generated headers in
# the artifact. Source mode does not add headers here: the backend is checked
# separately, with the flags gn emits (see webgtk-verify-engine below).
function(webgtk_engine_include_dirs target)
  if(WEBGTK_ENGINE_MODE STREQUAL "prebuilt")
    target_include_directories(${target} PRIVATE
      ${WEBGTK_ENGINE_PREBUILT_DIR}/include
      ${WEBGTK_ENGINE_PREBUILT_DIR}/gen)
  endif()
endfunction()

function(webgtk_link_engine target)
  if(WEBGTK_ENGINE_MODE STREQUAL "prebuilt")
    # The backend is compiled into this library, so its references to Chromium
    # symbols must be resolved here. Chromium is built as components, so link
    # every library in the artifact: an umbrella that only depends on them does
    # not resolve the symbols for its consumers.
    file(GLOB WEBGTK_ENGINE_LIBS "${WEBGTK_ENGINE_PREBUILT_DIR}/lib/*.so")
    if(NOT WEBGTK_ENGINE_LIBS)
      message(FATAL_ERROR
        "No shared libraries in ${WEBGTK_ENGINE_PREBUILT_DIR}/lib. "
        "Run scripts/download_engine_prebuilt.sh first.")
    endif()
    target_link_libraries(${target} PRIVATE ${WEBGTK_ENGINE_LIBS})
    # Keep a runpath so the demos and tests find the engine libraries in place.
    set_target_properties(${target} PROPERTIES
      BUILD_RPATH "${WEBGTK_ENGINE_PREBUILT_DIR}/lib"
      INSTALL_RPATH "${WEBGTK_ENGINE_PREBUILT_DIR}/lib")
  endif()
endfunction()

# One object library per GTK major version: webgtk_view.cc is compiled twice
# with different WEBGTK_GTK4 settings, so the two shared libraries can never
# share object files.
function(webgtk_add_objects target gtk_target)
  add_library(${target} OBJECT ${WEBGTK_SOURCES})
  target_include_directories(${target}
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(${target} PUBLIC ${gtk_target} epoxy)
  if(WEBGTK_HAVE_ENGINE)
    target_compile_definitions(${target} PUBLIC WEBGTK_HAVE_ENGINE=1)
    webgtk_engine_include_dirs(${target})
  endif()
endfunction()

function(webgtk_add_shared target objects)
  add_library(${target} SHARED)
  # PUBLIC so consumers inherit the include dirs and GTK dependency.
  target_link_libraries(${target} PUBLIC ${objects})
  # A stable soname so packaged binaries keep working across patch releases.
  set_target_properties(${target} PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})
  if(WEBGTK_HAVE_ENGINE)
    webgtk_link_engine(${target})
  endif()
endfunction()

webgtk_add_objects(webgtk_gtk3_objs PkgConfig::GTK3)
webgtk_add_objects(webgtk_gtk4_objs PkgConfig::GTK4)
target_compile_definitions(webgtk_gtk4_objs PRIVATE WEBGTK_GTK4=1)

webgtk_add_shared(webgtk webgtk_gtk3_objs)
webgtk_add_shared(webgtk4 webgtk_gtk4_objs)

# ---- Engine compile verification -----------------------------------------
# Source mode cannot link Chromium, but it can prove src/backend_chromium.cc
# still matches the engine's headers. scripts/verify_engine.sh extracts the
# exact flags gn emits for a //content source file and compiles the backend
# with them, generating the headers it needs along the way. Exposed as a target
# so `cmake --build build --target webgtk-verify-engine` runs the check.
if(WEBGTK_ENGINE_MODE STREQUAL "source")
  add_custom_target(webgtk-verify-engine
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify_engine.sh"
            --dir "${WEBGTK_ENGINE_DIR}"
    COMMENT "Compile-verifying src/backend_chromium.cc against the engine source"
    USES_TERMINAL VERBATIM)
endif()

if(WEBGTK_ENGINE_MODE STREQUAL "prebuilt")
  message(STATUS "WebGTK: linking prebuilt engine from ${WEBGTK_ENGINE_PREBUILT_DIR}")
elseif(WEBGTK_ENGINE_MODE STREQUAL "source")
  message(STATUS "WebGTK: engine source at ${WEBGTK_ENGINE_DIR} "
                 "(build target webgtk-verify-engine to compile-check the backend)")
else()
  message(STATUS "WebGTK: no engine configured, mock backend only "
                 "(run scripts/download_engine_prebuilt.sh to enable it)")
endif()

# ---- GObject Introspection -----------------------------------------------
# A GIR + typelib per GTK major version, so the same shared libraries are
# usable from PyGObject, JavaScript, Rust, Vala and friends. The two typelibs
# must not share a name, so the GTK4 one is versioned "4.0"; a binding asks for
# it with gi.require_version('WebGTK', '4.0').
if(ENABLE_INTROSPECTION)
  find_program(WEBGTK_GIR_SCANNER g-ir-scanner)
  find_program(WEBGTK_GIR_COMPILER g-ir-compiler)
  if(NOT WEBGTK_GIR_SCANNER OR NOT WEBGTK_GIR_COMPILER)
    message(WARNING "g-ir-scanner/g-ir-compiler not found: "
                    "introspection disabled (install gobject-introspection)")
  endif()
endif()

function(webgtk_add_introspection target gtk_target nsversion gtk_major)
  if(NOT ENABLE_INTROSPECTION OR NOT WEBGTK_GIR_SCANNER
     OR NOT WEBGTK_GIR_COMPILER)
    return()
  endif()

  set(gir "${CMAKE_CURRENT_BINARY_DIR}/WebGTK-${nsversion}.gir")
  set(typelib "${CMAKE_CURRENT_BINARY_DIR}/WebGTK-${nsversion}.typelib")
  set(header "${CMAKE_CURRENT_SOURCE_DIR}/include/webgtk/webgtk-view.h")

  # The scanner links and runs a small program against the freshly built
  # library, so it has to find it and, in Debug builds, the sanitizer runtime.
  set(scanner_env
      "LD_LIBRARY_PATH=${CMAKE_BINARY_DIR}:$ENV{LD_LIBRARY_PATH}"
      "ASAN_OPTIONS=detect_leaks=0:verify_asan_link_order=0")
  if(NOT WEBGTK_SANITIZE_FLAGS)
    # Nothing to relax when the library is not instrumented.
    list(REMOVE_AT scanner_env 1)
  endif()

  # The header selects the constructor with WEBGTK_GTK4, so the scanner must be
  # told which flavour it is scanning or it would always see the GTK3 branch.
  set(scan_cflags "")
  if(gtk_major STREQUAL "gtk4")
    set(scan_cflags --cflags-begin -DWEBGTK_GTK4=1 --cflags-end)
  endif()

  add_custom_command(
    OUTPUT "${gir}"
    COMMAND ${CMAKE_COMMAND} -E env ${scanner_env}
            "${WEBGTK_GIR_SCANNER}"
            --namespace=WebGTK --nsversion=${nsversion}
            --identifier-prefix=WebGtk --symbol-prefix=webgtk
            --include=GObject-2.0 --include=${gtk_target}
            --library=${target} --library-path=$<TARGET_FILE_DIR:${target}>
            --c-include=webgtk/webgtk-view.h
            --add-include-path=${CMAKE_CURRENT_SOURCE_DIR}/include
            -I${CMAKE_CURRENT_SOURCE_DIR}/include
            -I${CMAKE_CURRENT_SOURCE_DIR}/src
            ${scan_cflags}
            --warn-all
            --output=${gir}
            ${header}
    DEPENDS ${target} ${header}
    COMMENT "Scanning ${target} for WebGTK-${nsversion}.gir"
    VERBATIM)

  add_custom_command(
    OUTPUT "${typelib}"
    COMMAND "${WEBGTK_GIR_COMPILER}" --output=${typelib} ${gir}
    DEPENDS "${gir}"
    COMMENT "Compiling WebGTK-${nsversion}.typelib"
    VERBATIM)

  add_custom_target(webgtk-gir-${gtk_major} ALL DEPENDS "${gir}" "${typelib}")
  add_dependencies(webgtk-gir-${gtk_major} ${target})

  # The typelib goes to lib/girepository-1.0, a version-independent libdir with
  # no multiarch suffix. The .gir is a development file, so it ships in the
  # devel component alongside the headers.
  install(FILES "${typelib}"
          DESTINATION ${CMAKE_INSTALL_LIBDIR}/girepository-1.0
          COMPONENT runtime)
  install(FILES "${gir}"
          DESTINATION ${CMAKE_INSTALL_DATADIR}/gir-1.0
          COMPONENT devel)
endfunction()

webgtk_add_introspection(webgtk Gtk-3.0 1.0 gtk3)
webgtk_add_introspection(webgtk4 Gtk-4.0 4.0 gtk4)

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

  # The Python demo needs an interpreter with PyGObject. The one CMake finds may
  # not have it (a virtualenv without system site-packages, say), so probe.
  find_package(Python3 COMPONENTS Interpreter QUIET)
  set(WEBGTK_PYTHON "")
  foreach(candidate ${Python3_EXECUTABLE} /usr/bin/python3)
    if(NOT WEBGTK_PYTHON AND candidate)
      execute_process(COMMAND ${candidate} -c "import gi"
                      RESULT_VARIABLE _gi_status OUTPUT_QUIET ERROR_QUIET)
      if(_gi_status EQUAL 0)
        set(WEBGTK_PYTHON ${candidate})
      endif()
    endif()
  endforeach()

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

  # The same checks, but through PyGObject: proves the typelib is usable, not
  # merely well formed. Needs introspection to have produced the typelibs.
  if(WEBGTK_PYTHON AND WEBGTK_GIR_SCANNER AND WEBGTK_GIR_COMPILER)
    set(WEBGTK_PYTHON_ENV "${WEBGTK_TEST_ENV};GI_TYPELIB_PATH=${CMAKE_BINARY_DIR};LD_LIBRARY_PATH=${CMAKE_BINARY_DIR};ASAN_OPTIONS=detect_leaks=0:verify_asan_link_order=0")
    add_test(NAME webgtk-python-gtk3-selftest
             COMMAND ${WEBGTK_TEST_LAUNCHER} ${WEBGTK_PYTHON}
                     ${CMAKE_CURRENT_SOURCE_DIR}/examples/demo.py --selftest
                     --gtk3)
    add_test(NAME webgtk-python-gtk4-selftest
             COMMAND ${WEBGTK_TEST_LAUNCHER} ${WEBGTK_PYTHON}
                     ${CMAKE_CURRENT_SOURCE_DIR}/examples/demo.py --selftest)
    set_tests_properties(webgtk-python-gtk3-selftest webgtk-python-gtk4-selftest
                         PROPERTIES ENVIRONMENT "${WEBGTK_PYTHON_ENV}")
  else()
    message(STATUS "WebGTK: python demo tests skipped "
                   "(need PyGObject and the introspection tools)")
  endif()
endif()

# ---- Installation --------------------------------------------------------
#
# Every install rule names a component, so one install tree can be split into
# separate packages. CPack turns the components into:
#
#   runtime -> libwebgtk0      the shared libraries and the typelibs
#   devel   -> libwebgtk-dev   headers, pkg-config files, .gir, unversioned .so
#   demo    -> webgtk-demos    the GTK3/GTK4 demo binaries
set(WEBGTK_RUNTIME_PACKAGE "libwebgtk0")
set(WEBGTK_DEVEL_PACKAGE "libwebgtk-dev")
set(WEBGTK_DEMO_PACKAGE "webgtk-demos")

# NAMELINK_COMPONENT sends the bare libwebgtk.so symlink (the linker input) to
# the devel package and keeps the versioned real file in runtime.
install(TARGETS webgtk webgtk4
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
                COMPONENT runtime
                NAMELINK_COMPONENT devel)

install(FILES
          "${CMAKE_CURRENT_SOURCE_DIR}/include/webgtk/webgtk.h"
          "${CMAKE_CURRENT_SOURCE_DIR}/include/webgtk/webgtk-view.h"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/webgtk
        COMPONENT devel)

if(WEBGTK_BUILD_DEMOS)
  install(TARGETS gtk3-demo gtk4-demo
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT demo)
endif()

# pkg-config files. The prefix is written relative to the .pc location, so the
# installed tree is relocatable: `pkg-config --variable=includedir webgtk-1.0`
# resolves correctly wherever the tree is unpacked.
function(webgtk_add_pkgconfig module_name gtk_module libname description)
  set(pcdir "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
  # ${pcfiledir} is <prefix>/<pcdir>; walk back up to <prefix>.
  file(RELATIVE_PATH pc_prefix_from_pcdir "${CMAKE_INSTALL_FULL_LIBDIR}/pkgconfig"
       "${CMAKE_INSTALL_PREFIX}")
  set(WEBGTK_PC_PREFIX_FROM_PCDIR "${pc_prefix_from_pcdir}")
  set(WEBGTK_PC_NAME "${module_name}")
  set(WEBGTK_PC_DESCRIPTION "${description}")
  set(WEBGTK_PC_LIBNAME "${libname}")
  set(WEBGTK_PC_GTK_MODULE "${gtk_module}")
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/webgtk.pc.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/${module_name}.pc" @ONLY)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${module_name}.pc"
          DESTINATION ${pcdir} COMPONENT devel)
endfunction()

webgtk_add_pkgconfig(webgtk-1.0 gtk+-3.0 webgtk
  "WebGTK GTK3 view - lightweight webview on a pruned Blink/V8 core")
webgtk_add_pkgconfig(webgtk-4.0 gtk4 webgtk4
  "WebGTK GTK4 view - lightweight webview on a pruned Blink/V8 core")

# ---- Packaging (CPack) ---------------------------------------------------
#
# Two portable archives and up to three .deb packages:
#   webgtk-<ver>-linux-<arch>.tar.gz    the whole install tree, relocatable
#   libwebgtk0_<ver>-1_<arch>.deb       runtime
#   libwebgtk-dev_<ver>-1_<arch>.deb    development files
#   webgtk-demos_<ver>-1_<arch>.deb     demos
set(WEBGTK_DEB_ARCH "")
execute_process(COMMAND dpkg --print-architecture
                OUTPUT_VARIABLE WEBGTK_DEB_ARCH
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
if(NOT WEBGTK_DEB_ARCH)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    set(WEBGTK_DEB_ARCH "amd64")
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(WEBGTK_DEB_ARCH "arm64")
  else()
    set(WEBGTK_DEB_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
  endif()
endif()

set(CPACK_PACKAGE_NAME "webgtk")
set(CPACK_PACKAGE_VENDOR "M5Devs")
set(CPACK_PACKAGE_CONTACT "M5Devs <webgtk-maintainers@users.noreply.github.com>")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Lightweight GTK webview built on a pruned Blink/V8 core")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/M5Devs/webgtk")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_FILE_NAME "webgtk-${PROJECT_VERSION}-linux-${WEBGTK_DEB_ARCH}")
set(CPACK_GENERATOR "TGZ;DEB")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  set(CPACK_STRIP_FILES TRUE)
endif()

# Debian control. The library dependencies are declared explicitly rather than
# left to dpkg-shlibdeps: the pruned engine and the GTK stack are the contract
# we want to state, and the t64 alternatives cover the time_t transition in
# Debian 13 and Ubuntu 24.04 without pinning either spelling.
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_GROUPING IGNORE)
set(CPACK_DEBIAN_ENABLE_COMPONENT_DEPENDS ON)
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "${WEBGTK_DEB_ARCH}")
set(CPACK_DEBIAN_PACKAGE_RELEASE "1")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(WEBGTK_DEB_VERSION "${PROJECT_VERSION}-${CPACK_DEBIAN_PACKAGE_RELEASE}")

set(CPACK_DEBIAN_RUNTIME_PACKAGE_NAME "${WEBGTK_RUNTIME_PACKAGE}")
set(CPACK_DEBIAN_RUNTIME_PACKAGE_SECTION "libs")
set(CPACK_DEBIAN_RUNTIME_PACKAGE_DEPENDS
    "libc6, libgcc-s1, libstdc++6, \
     libglib2.0-0t64 | libglib2.0-0, \
     libgtk-3-0t64 | libgtk-3-0, libgtk-4-1, libepoxy0")
set(CPACK_DEBIAN_RUNTIME_DESCRIPTION
    "Runtime libraries for WebGTK.\n Ships libwebgtk.so (GTK3), libwebgtk4.so (GTK4) and the\n GObject Introspection typelibs.")

set(CPACK_DEBIAN_DEVEL_PACKAGE_NAME "${WEBGTK_DEVEL_PACKAGE}")
set(CPACK_DEBIAN_DEVEL_PACKAGE_SECTION "libdevel")
set(CPACK_DEBIAN_DEVEL_PACKAGE_DEPENDS
    "${WEBGTK_RUNTIME_PACKAGE} (= ${WEBGTK_DEB_VERSION}), \
     libglib2.0-dev, libgtk-3-dev, libgtk-4-dev")
set(CPACK_DEBIAN_DEVEL_DESCRIPTION
    "Development files for WebGTK.\n The C ABI and GObject headers, the webgtk-1.0 and webgtk-4.0\n pkg-config files and the .gir data.")

set(CPACK_DEBIAN_DEMO_PACKAGE_NAME "${WEBGTK_DEMO_PACKAGE}")
set(CPACK_DEBIAN_DEMO_PACKAGE_SECTION "misc")
set(CPACK_DEBIAN_DEMO_PACKAGE_DEPENDS
    "${WEBGTK_RUNTIME_PACKAGE} (= ${WEBGTK_DEB_VERSION})")
set(CPACK_DEBIAN_DEMO_DESCRIPTION
    "GTK3 and GTK4 demo programs for WebGTK.")

include(CPack)
