cmake_minimum_required(VERSION 3.18)
project(lenovo_elden_sensors VERSION 0.4.0 LANGUAGES C)
include(GNUInstallDirs)
include(CTest)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SSC REQUIRED IMPORTED_TARGET libssc)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET gio-unix-2.0)
pkg_check_modules(PROTOBUF REQUIRED IMPORTED_TARGET libprotobuf-c)
# protoc-c runs on the build host, including during cross-compilation.
find_program(PROTOC_C protoc-c REQUIRED NO_CMAKE_FIND_ROOT_PATH)
foreach(proto hall virtual)
  add_custom_command(OUTPUT ${proto}.pb-c.c ${proto}.pb-c.h
    COMMAND ${PROTOC_C} --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/src
      --c_out=${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src/${proto}.proto
    DEPENDS src/${proto}.proto VERBATIM)
endforeach()
add_library(virtual-decode STATIC src/virtual-decode.c ${CMAKE_CURRENT_BINARY_DIR}/virtual.pb-c.c)
target_include_directories(virtual-decode PUBLIC src ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(virtual-decode PUBLIC PkgConfig::GLIB PkgConfig::PROTOBUF m)
add_executable(ssc-input src/ssc-input.c ${CMAKE_CURRENT_BINARY_DIR}/hall.pb-c.c)
target_include_directories(ssc-input PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(ssc-input PRIVATE PkgConfig::SSC PkgConfig::GLIB PkgConfig::PROTOBUF m)
add_executable(ssc-virtual src/ssc-virtual.c)
target_link_libraries(ssc-virtual PRIVATE virtual-decode PkgConfig::SSC)
foreach(target ssc-input ssc-virtual virtual-decode)
  target_compile_options(${target} PRIVATE -Wall -Wextra)
endforeach()
if(BUILD_TESTING)
  add_executable(test-virtual tests/test-virtual.c)
  target_link_libraries(test-virtual PRIVATE virtual-decode)
  add_test(NAME virtual-decoder COMMAND test-virtual)
endif()
install(TARGETS ssc-input ssc-virtual RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
option(INSTALL_CLI_WRAPPER "Install the virtual sensor command wrapper" ON)
set(WRAPPER_INSTALL_DIR "${CMAKE_INSTALL_BINDIR}" CACHE STRING
    "Wrapper installation directory (absolute or relative to CMAKE_INSTALL_PREFIX)")
if(INSTALL_CLI_WRAPPER)
  if(WRAPPER_INSTALL_DIR STREQUAL "")
    message(FATAL_ERROR "WRAPPER_INSTALL_DIR must not be empty")
  endif()
  # Use target installation paths, never the build sysroot or DESTDIR.
  # Quote literal paths for /bin/sh, including embedded single quotes.
  string(REPLACE "'" "'\"'\"'" WRAPPER_LIBDIR_SHELL "${CMAKE_INSTALL_FULL_LIBDIR}")
  string(REPLACE "'" "'\"'\"'" WRAPPER_BINDIR_SHELL "${CMAKE_INSTALL_FULL_BINDIR}")
  configure_file(scripts/lenovo-elden-sensors-virtual.in
                 lenovo-elden-sensors-virtual @ONLY)
  install(PROGRAMS "${CMAKE_CURRENT_BINARY_DIR}/lenovo-elden-sensors-virtual"
          DESTINATION "${WRAPPER_INSTALL_DIR}")
endif()
option(INSTALL_RUNTIME_DATA "Install vendor runtime and fallback registry" OFF)
set(DSP_INSTALL_DIR "runtime/dsp" CACHE STRING
    "DSP installation directory (absolute or relative to CMAKE_INSTALL_PREFIX)")
if(INSTALL_RUNTIME_DATA)
  if(DSP_INSTALL_DIR STREQUAL "")
    message(FATAL_ERROR "DSP_INSTALL_DIR must not be empty")
  endif()
  install(DIRECTORY vendor/runtime/sensors/ DESTINATION runtime/sensors)
  # DSP ELF files run on Hexagon, not the Linux CPU. Distributions may install
  # them under /lib/firmware to exclude them from Linux RPATH sanitization.
  install(DIRECTORY vendor/runtime/dsp/ DESTINATION "${DSP_INSTALL_DIR}")
  install(DIRECTORY vendor/fallback-registry/ DESTINATION fallback-registry)
endif()
