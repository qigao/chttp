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
find_package(SaltsUtils CONFIG REQUIRED COMPONENTS Crypto PATHS "${SALTS_UTILS_ROOT_PATH}"
             NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(NOT TARGET Salts::DataBind)
  message(FATAL_ERROR "SaltsUtils must provide the Salts::DataBind component")
endif()
find_package(llhttp CONFIG REQUIRED)

add_subdirectory(vendor/cjwt)
add_subdirectory(http_common)
add_subdirectory(http_client)
add_subdirectory(http_server)
include(cmake/ChttpApplication.cmake)
add_subdirectory(app)
add_subdirectory(rpc_service)
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
install(FILES cmake/ChttpApplication.cmake DESTINATION lib/cmake/Chttp)
install(DIRECTORY cmake/application DESTINATION lib/cmake/Chttp)
install(FILES LICENSE NOTICE THIRD_PARTY_NOTICES.md
        DESTINATION share/licenses/Chttp)
install(
  EXPORT ChttpTargets
  NAMESPACE CHttp::
  DESTINATION lib/cmake/Chttp)
