cmake_minimum_required(VERSION 3.25)
include(CMakeOptions.cmake)

# Opt-in ServiceHostCandidate contains private unshipped ABI. The original CHttp
# release/install/export pipeline must never package an opted-in test graph.
if(CHTTP_ENABLE_SERVICE_HOST_CANDIDATE)
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
# Strictly private CHttp HTTP MethodPlan qualification. The installed path
# never imports the unpublished Candidate archives or manufactures an alias.
if(CHTTP_ENABLE_SERVICE_HOST_CANDIDATE)
  if(NOT BUILD_TESTS OR NOT BUILD_TESTING)
    message(FATAL_ERROR "ServiceHost probe requires active CTest")
  endif()
  if(CHTTP_USE_INSTALLED_SERVICE_HOST_ABI_PROBE)
    if(DEFINED CHTTP_SALTS_CANDIDATE_SOURCE_DIR AND
       NOT "${CHTTP_SALTS_CANDIDATE_SOURCE_DIR}" STREQUAL "")
      message(FATAL_ERROR
        "Installed ServiceHost probe must not accept a private candidate source tree")
    endif()
    if(NOT TARGET Salts::ServiceHost OR NOT TARGET Salts::Configurator OR
       TARGET Salts::ServiceHostCandidate OR TARGET Salts::ConfiguratorCandidate)
      message(FATAL_ERROR
        "Installed probe requires only the exact Salts::ServiceHost and Salts::Configurator SDK exports")
    endif()
    set(_chttp_host_abi_header
        "${SALTS_ROOT_PATH}/include/salts/component_runtime_abi.h")
    if(NOT EXISTS "${_chttp_host_abi_header}")
      message(FATAL_ERROR "Installed Salts SDK lacks exact ServiceHost ABI header")
    endif()
    file(READ "${_chttp_host_abi_header}" _chttp_host_abi_text)
    if(NOT _chttp_host_abi_text MATCHES
       "SALTS_COMPONENT_RUNTIME_ABI_VERSION 0x00010000u")
      message(FATAL_ERROR "Installed ServiceHost experimental epoch is not v1.0")
    endif()
    set(CHTTP_SERVICE_HOST_LINK_TARGET Salts::ServiceHost)
  else()
    if(NOT DEFINED CHTTP_SALTS_CANDIDATE_SOURCE_DIR OR
       NOT EXISTS "${CHTTP_SALTS_CANDIDATE_SOURCE_DIR}/component-runtime/CMakeLists.txt")
      message(FATAL_ERROR
        "Private candidate requires pinned CHTTP_SALTS_CANDIDATE_SOURCE_DIR")
    endif()
    set(_chttp_saved_build_tests "${BUILD_TESTS}")
    set(BUILD_TESTS OFF)
    add_subdirectory(
      "${CHTTP_SALTS_CANDIDATE_SOURCE_DIR}/component-runtime"
      "${CMAKE_CURRENT_BINARY_DIR}/salts-service-host-candidate")
    set(BUILD_TESTS "${_chttp_saved_build_tests}")
    set(CHTTP_SERVICE_HOST_LINK_TARGET Salts::ServiceHostCandidate)
  endif()
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
