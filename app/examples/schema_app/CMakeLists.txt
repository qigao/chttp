cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(chttp_schema_example LANGUAGES C)
  find_package(Chttp CONFIG REQUIRED PATHS "$ENV{CHTTP_ROOT}" NO_DEFAULT_PATH)
endif()

chttp_app_target(TARGET chttp_schema_example
  IDL "${CMAKE_CURRENT_SOURCE_DIR}/app.schema"
  SOURCES services.c)
