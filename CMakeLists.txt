cmake_minimum_required(VERSION 3.25)
include(CMakeOptions.cmake)

# Tag-bound canonical ServiceHost tests do not publish a CHttp SDK.
# Production/default HTTP routing remains untouched until an explicit release.
if(CHTTP_ENABLE_SERVICE_HOST)
  set(CMAKE_SKIP_INSTALL_RULES TRUE)
endif()

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

include(CTest)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Sanitizers)

if(NOT DEFINED ENV{SALTS_ROOT} OR NOT IS_DIRECTORY "$ENV{SALTS_ROOT}")
  message(FATAL_ERROR "SALTS_ROOT must name the installed Salts SDK")
endif()
if(NOT DEFINED ENV{SALTS_UTILS_ROOT} OR NOT IS_DIRECTORY "$ENV{SALTS_UTILS_ROOT}")
  message(FATAL_ERROR "SALTS_UTILS_ROOT must name the installed SaltsUtils SDK")
endif()
file(TO_CMAKE_PATH "$ENV{SALTS_ROOT}" SALTS_ROOT_PATH)
file(TO_CMAKE_PATH "$ENV{SALTS_UTILS_ROOT}" SALTS_UTILS_ROOT_PATH)
unset(Salts_DIR CACHE)
unset(Salts_DIR)
unset(SaltsUtils_DIR CACHE)
unset(SaltsUtils_DIR)
find_package(Salts CONFIG REQUIRED PATHS "${SALTS_ROOT_PATH}"
             NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
# SaltsUtils.Native 4.3 exports Salts::Crypto with GmSSL::GmSSL in
# INTERFACE_LINK_LIBRARIES. Resolve its declared vendor target explicitly
# before CMake generates the imported SDK link closure.
find_package(GmSSL CONFIG REQUIRED)
find_package(SaltsUtils CONFIG REQUIRED PATHS "${SALTS_UTILS_ROOT_PATH}"
             NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(NOT TARGET Salts::DataBind)
  message(FATAL_ERROR "SaltsUtils must provide the Salts::DataBind component")
endif()
find_package(llhttp CONFIG REQUIRED)

add_subdirectory(vendor/cjwt)
add_subdirectory(http_common)
add_subdirectory(http_client)
add_subdirectory(http_server)
# Only the exact imported tagged Salts runtime may own Configurator and
# ServiceHost. Do not import an old Plugin/Component Owner or source branch.
if(CHTTP_ENABLE_SERVICE_HOST)
  if(NOT BUILD_TESTS OR NOT BUILD_TESTING)
    message(FATAL_ERROR "Tagged ServiceHost HTTP qualification requires CTest")
  endif()
  if(DEFINED CHTTP_SALTS_CANDIDATE_SOURCE_DIR AND
     NOT "${CHTTP_SALTS_CANDIDATE_SOURCE_DIR}" STREQUAL "")
    message(FATAL_ERROR "Source-copy ServiceHost is forbidden")
  endif()
  if(NOT DEFINED ENV{SALTS_CANONICAL_TAG} OR
     NOT "$ENV{SALTS_CANONICAL_TAG}" MATCHES "^v2\\.3\\.0(-rc\\.[1-9][0-9]*)?$")
    message(FATAL_ERROR "Explicit published SALTS_CANONICAL_TAG required")
  endif()
  if("$ENV{SALTS_CANONICAL_TAG}" MATCHES "^v2\\.3\\.0-rc\\.([0-9]+)$")
    if(CMAKE_MATCH_1 LESS_EQUAL 17)
      message(FATAL_ERROR "Salts rc.17 and earlier lack canonical ServiceHost")
    endif()
  endif()
  if(NOT DEFINED ENV{SALTS_UTILS_CANONICAL_TAG} OR
     NOT "$ENV{SALTS_UTILS_CANONICAL_TAG}" MATCHES "^v4\\.3\\.0(-rc\\.[1-9][0-9]*)?$")
    message(FATAL_ERROR "Explicit published SALTS_UTILS_CANONICAL_TAG required")
  endif()
  string(REGEX REPLACE "^v" "" _salts_version "$ENV{SALTS_CANONICAL_TAG}")
  string(REGEX REPLACE "^v" "" _utils_version "$ENV{SALTS_UTILS_CANONICAL_TAG}")
  foreach(_target IN ITEMS
      Salts::Configurator Salts::ServiceHost Salts::ComponentProviderABI
      Salts::Unicode)
    if(NOT TARGET ${_target})
      message(FATAL_ERROR "Tagged canonical Salts is missing ${_target}")
    endif()
    get_target_property(_imported ${_target} IMPORTED)
    if(NOT _imported)
      message(FATAL_ERROR "${_target} must be imported from the Salts SDK")
    endif()
  endforeach()
  if(TARGET Salts::ComponentPlugin OR TARGET Salts::ComponentPluginABI OR
     TARGET Salts::Component OR TARGET Salts::ServiceHostCandidate OR
     TARGET Salts::ConfiguratorCandidate)
    message(FATAL_ERROR "Removed Component runtime or copied Candidate is forbidden")
  endif()
  foreach(_file IN ITEMS
      include/salts/service_host.h
      include/salts/configurator.h
      include/salts/component_runtime_abi.h
      include/salts/component_provider_abi.h
      salts-sdk-manifest.txt)
    if(NOT EXISTS "${SALTS_ROOT_PATH}/${_file}")
      message(FATAL_ERROR "Canonical tagged Salts missing ${_file}")
    endif()
  endforeach()
  foreach(_old IN ITEMS component_plugin.h component_plugin_abi.h)
    if(EXISTS "${SALTS_ROOT_PATH}/include/salts/${_old}")
      message(FATAL_ERROR "Legacy ComponentPlugin header must not ship")
    endif()
  endforeach()
  file(STRINGS "${SALTS_ROOT_PATH}/salts-sdk-manifest.txt"
    _salts_manifest_version REGEX "^version=")
  if(NOT _salts_manifest_version STREQUAL "version=${_salts_version}")
    message(FATAL_ERROR "Salts SDK manifest does not match the published tag")
  endif()
  if(NOT EXISTS "${SALTS_UTILS_ROOT_PATH}/salts-utils-sdk-manifest.txt")
    message(FATAL_ERROR "Tagged SaltsUtils SDK manifest is absent")
  endif()
  file(STRINGS "${SALTS_UTILS_ROOT_PATH}/salts-utils-sdk-manifest.txt"
    _utils_manifest_version REGEX "^version=")
  if(NOT _utils_manifest_version STREQUAL "version=${_utils_version}")
    message(FATAL_ERROR "SaltsUtils SDK manifest is not the selected tag")
  endif()
  file(STRINGS "${SALTS_UTILS_ROOT_PATH}/salts-utils-sdk-manifest.txt"
    _utils_manifest_salts REGEX "^salts_version=")
  if(NOT _utils_manifest_salts STREQUAL "salts_version=${_salts_version}")
    message(FATAL_ERROR "SaltsUtils does not depend on the exact tagged Salts")
  endif()
  file(READ "${SALTS_ROOT_PATH}/include/salts/component_runtime_abi.h"
    _strict_host_abi)
  if(NOT _strict_host_abi MATCHES
     "SALTS_COMPONENT_RUNTIME_ABI_VERSION 0x00010000u")
    message(FATAL_ERROR "Expected installed native ServiceHost ABI epoch 1")
  endif()
  set(CHTTP_SERVICE_HOST_LINK_TARGET Salts::ServiceHost)
endif()
add_subdirectory(app)
add_subdirectory(s3)
add_subdirectory(openapi)

if(BUILD_TESTS)
  add_subdirectory(http_common/tests)
  add_subdirectory(http_common/tests/integration)
  add_subdirectory(http_client/tests)
  add_subdirectory(s3/tests)
  add_subdirectory(http_server/tests)
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
