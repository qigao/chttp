cmake_minimum_required(VERSION 3.25)
project(HTTPServices VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
if(MSVC)
  add_compile_options(/experimental:c11atomics)
endif()

option(BUILD_TESTS "Build test suite" ON)
option(BUILD_EXAMPLES "Build example programs" ON)
option(BUILD_BENCHMARKS "Build benchmark executables" OFF)
include(CTest)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CmakeUtils)
include(Sanitizers)
include(FindSalts)

add_subdirectory(vendor/turbo_crypto)
add_subdirectory(chttp)
add_subdirectory(s3)
add_subdirectory(crpc)

include(CMakePackageConfigHelpers)
configure_package_config_file(
  cmake/HTTPServicesConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/HTTPServicesConfig.cmake"
  INSTALL_DESTINATION lib/cmake/HTTPServices)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/HTTPServicesConfigVersion.cmake"
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/HTTPServicesConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/HTTPServicesConfigVersion.cmake"
  cmake/FindSalts.cmake
  DESTINATION lib/cmake/HTTPServices)
install(EXPORT HTTPServicesTargets NAMESPACE Salts::
  DESTINATION lib/cmake/HTTPServices)

add_custom_target(verify_installed_package
  COMMAND "${CMAKE_COMMAND}"
    "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
    "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
    "-DBUILD_CONFIG=$<CONFIG>"
    "-DBUILD_GENERATOR=${CMAKE_GENERATOR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyInstalledPackage.cmake"
  DEPENDS salts_chttp salts_s3 salts_crpc VERBATIM)
