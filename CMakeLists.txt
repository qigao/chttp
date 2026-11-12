cmake_minimum_required(VERSION 3.25)
include(CMakeOptions.cmake)

project(
  Chttp
  VERSION 2.1.0
  LANGUAGES C CXX)

# CMake's library version is numeric; the manifest retains the release suffix.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg.json" chttp_manifest)
string(JSON CHTTP_RELEASE_VERSION GET "${chttp_manifest}" version-semver)
string(REGEX REPLACE "[-+].*$" "" chttp_release_base "${CHTTP_RELEASE_VERSION}")
if(NOT chttp_release_base STREQUAL PROJECT_VERSION)
  message(FATAL_ERROR "CMake VERSION and vcpkg release version disagree")
endif()

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

if(BUILD_TESTS OR BUILD_BENCHMARK)
  enable_testing()
endif()
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Sanitizers)

# Consume the selected preinstalled vcpkg profile, as in SaltsBase/SaltsUtils.
list(PREPEND CMAKE_PREFIX_PATH
  "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/$ENV{VCPKG_CONFIGURATION_SUBDIR}"
  "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
find_package(SaltsBase "$ENV{SALTS_BASE_VERSION}" EXACT CONFIG REQUIRED
  PATHS "$ENV{SALTS_BASE_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_package(CMeta "$ENV{CMETA_VERSION}" EXACT CONFIG REQUIRED
  PATHS "$ENV{CMETA_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_package(SaltsCore "$ENV{SALTS_CORE_VERSION}" EXACT CONFIG REQUIRED
  PATHS "$ENV{SALTS_CORE_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_package(SaltsUtils "$ENV{SALTS_UTILS_VERSION}" EXACT CONFIG REQUIRED
  PATHS "$ENV{SALTS_UTILS_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_package(llhttp CONFIG REQUIRED)
find_package(GmSSL CONFIG REQUIRED)

add_subdirectory(vendor/cjwt)
add_subdirectory(http_common)
add_subdirectory(http_client)
add_subdirectory(http_server)
add_subdirectory(app)
add_subdirectory(s3)
add_subdirectory(openapi)

# Static SDK objects carry explicit exports, disabling MinGW's implicit exports.
# Keep the shared libraries' WINDOWS_EXPORT_ALL_SYMBOLS contract on this linker.
if(MINGW)
  foreach(library IN ITEMS chttp_client chttp_server chttp_app chttp_s3)
    target_link_options(${library} PRIVATE -Wl,--export-all-symbols)
  endforeach()
endif()

include(CMakePackageConfigHelpers)
configure_package_config_file(
  cmake/ChttpConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/ChttpConfig.cmake"
  INSTALL_DESTINATION lib/cmake/Chttp
  NO_SET_AND_CHECK_MACRO NO_CHECK_REQUIRED_COMPONENTS_MACRO)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/ChttpConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY ExactVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/ChttpConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/ChttpConfigVersion.cmake"
        DESTINATION lib/cmake/Chttp)
install(FILES LICENSE NOTICE THIRD_PARTY_NOTICES.md
        DESTINATION share/licenses/Chttp)
install(
  EXPORT ChttpTargets
  NAMESPACE CHttp::
  DESTINATION lib/cmake/Chttp)
