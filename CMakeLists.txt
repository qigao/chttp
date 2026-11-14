cmake_minimum_required(VERSION 3.25)
include(CMakeOptions.cmake)

project(
  Chttp
  VERSION 0.1.0
  LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(cmake/CmakeUtils.cmake)

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
find_package(SaltsBase CONFIG REQUIRED
  PATHS "$ENV{SALTS_BASE_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_package(CMeta CONFIG REQUIRED
  PATHS "$ENV{CMETA_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_package(SaltsCNet CONFIG REQUIRED
  PATHS "$ENV{SALTS_CNET_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_package(SaltsUtils CONFIG REQUIRED
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

set(_chttp_package_dir "${CMAKE_INSTALL_LIBDIR}/cmake/Chttp")
configure_package_config_file(
  cmake/ChttpConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/ChttpConfig.cmake"
  INSTALL_DESTINATION "${_chttp_package_dir}")
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/ChttpConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY ExactVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/ChttpConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/ChttpConfigVersion.cmake"
        DESTINATION "${_chttp_package_dir}")
install(FILES LICENSE NOTICE THIRD_PARTY_NOTICES.md
        DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/Chttp)
install(
  EXPORT ChttpTargets
  FILE ChttpTargets.cmake
  NAMESPACE CHttp::
  DESTINATION "${_chttp_package_dir}")
