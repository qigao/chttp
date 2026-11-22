find_package(
        unofficial-tree-sitter CONFIG REQUIRED PATHS "${CHTTP_DEPENDENCY_PREFIX}" NO_DEFAULT_PATH
        NO_CMAKE_FIND_ROOT_PATH
)
find_library(
        OPENAPI_C_GRAMMAR_LIBRARY
        NAMES tree-sitter-c REQUIRED
        PATHS "${CHTTP_DEPENDENCY_PREFIX}/$ENV{VCPKG_CONFIGURATION_SUBDIR}lib"
        NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH
)

add_library(
        openapi_generator STATIC src/plugin.c src/extract.c src/document.c src/comment.c
                                 src/databind_http.c
)
add_library(OpenAPI::Generator ALIAS openapi_generator)
target_include_directories(
        openapi_generator
        PUBLIC include
        PRIVATE src
)
target_link_libraries(
        openapi_generator
        PUBLIC unofficial::tree-sitter::tree-sitter CMeta::CMeta CMeta::CFlow Salts::JsonParser
               Salts::CYamlJsonAdapter Salts::DataBind
        PRIVATE ${CMAKE_DL_LIBS}
)

add_library(openapi_ui_model STATIC src/ui_model.c)
add_library(OpenAPI::UIModel ALIAS openapi_ui_model)
target_include_directories(
        openapi_ui_model
        PUBLIC include
        PRIVATE src
)
target_link_libraries(openapi_ui_model PUBLIC OpenAPI::Generator CHttp::App)
add_executable(openapi-gen src/main.c)
target_link_libraries(openapi-gen PRIVATE openapi_generator)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             queries/extract/c.scm queries/extract/c-name.scm)
set(OPENAPI_PLUGIN_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/c.c")
include(plugins/GenerateLanguage.cmake)
add_library(openapi_c MODULE "${CMAKE_CURRENT_BINARY_DIR}/generated/c.c")
target_include_directories(openapi_c PRIVATE include)
target_link_libraries(
        openapi_c PRIVATE "${OPENAPI_C_GRAMMAR_LIBRARY}" unofficial::tree-sitter::tree-sitter
)
set_target_properties(openapi_c PROPERTIES PREFIX "")
if(BUILD_EXAMPLES OR BUILD_TESTS)
        add_subdirectory(examples)
endif()
if(BUILD_TESTS)
        add_subdirectory(tests)
endif()
