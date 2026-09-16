cmake_minimum_required(VERSION 3.25)
project(Chttp VERSION 1.0.0 LANGUAGES C CXX)

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

include(ProjectOptions.cmake)
include(CTest)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Sanitizers)

if(NOT DEFINED ENV{SALTS_ROOT} OR "$ENV{SALTS_ROOT}" STREQUAL "")
  message(FATAL_ERROR "SALTS_ROOT must name the installed Salts profile")
endif()
if(NOT DEFINED ENV{SALTS_UTILS_ROOT} OR "$ENV{SALTS_UTILS_ROOT}" STREQUAL "")
  message(FATAL_ERROR "SALTS_UTILS_ROOT must name the installed SaltsUtils profile")
endif()
file(TO_CMAKE_PATH "$ENV{SALTS_ROOT}" SALTS_ROOT_PATH)
file(TO_CMAKE_PATH "$ENV{SALTS_UTILS_ROOT}" SALTS_UTILS_ROOT_PATH)
if(NOT IS_DIRECTORY "${SALTS_ROOT_PATH}")
  message(FATAL_ERROR "SALTS_ROOT is not a directory: $ENV{SALTS_ROOT}")
endif()
if(NOT IS_DIRECTORY "${SALTS_UTILS_ROOT_PATH}")
  message(FATAL_ERROR "SALTS_UTILS_ROOT is not a directory: $ENV{SALTS_UTILS_ROOT}")
endif()

unset(Salts_DIR CACHE)
unset(Salts_DIR)
find_package(Salts CONFIG REQUIRED PATHS "${SALTS_ROOT_PATH}"
             NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
unset(SaltsUtils_DIR CACHE)
unset(SaltsUtils_DIR)
find_package(SaltsUtils CONFIG REQUIRED PATHS "${SALTS_UTILS_ROOT_PATH}"
             NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)

foreach(required_salts_utils_target IN ITEMS
        Salts::Crypto
        Salts::JsonParser
        Salts::JsonCSerdeAdapter
        Salts::DateTimeParser)
  if(NOT TARGET ${required_salts_utils_target})
    message(FATAL_ERROR
            "SALTS_UTILS_ROOT package is missing required target ${required_salts_utils_target}")
  endif()
endforeach()

find_package(llhttp CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

add_subdirectory(vendor/cjwt)
add_subdirectory(http_common)
add_subdirectory(http_client)
add_subdirectory(http_server)
add_subdirectory(s3)
if(BUILD_OPENAPI)
  add_subdirectory(openapi)
endif()
if(BUILD_TESTS)
  add_subdirectory(http_common/tests)
  add_subdirectory(http_common/tests/integration)
  add_subdirectory(http_client/tests)
  add_subdirectory(s3/tests)
  add_subdirectory(http_server/tests)
endif()

include(CMakePackageConfigHelpers)
configure_package_config_file(
  cmake/ChttpConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/ChttpConfig.cmake"
  INSTALL_DESTINATION lib/cmake/Chttp
  NO_SET_AND_CHECK_MACRO NO_CHECK_REQUIRED_COMPONENTS_MACRO)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/ChttpConfigVersion.cmake"
  VERSION ${PROJECT_VERSION} COMPATIBILITY ExactVersion)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/ChttpConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/ChttpConfigVersion.cmake"
  DESTINATION lib/cmake/Chttp)
install(EXPORT ChttpTargets NAMESPACE CHttp::
  DESTINATION lib/cmake/Chttp)
install(FILES "${llhttp_DIR}/copyright"
  DESTINATION share/licenses/Chttp/llhttp)
