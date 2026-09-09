add_executable(chttp_websocket_test chttp_websocket_test.c)
target_link_libraries(chttp_websocket_test PRIVATE CHttp::Client CHttp::Server Salts::TinyTest)
target_include_directories(chttp_websocket_test PRIVATE ${PROJECT_SOURCE_DIR}/http_common/tests/support ${PROJECT_SOURCE_DIR}/http_common/http ${PROJECT_SOURCE_DIR}/http_common/rpc)
add_test(NAME chttp_websocket_test COMMAND chttp_websocket_test)
set_target_properties(chttp_websocket_test PROPERTIES FOLDER "http_common/tests/integration")
set_target_properties(chttp_websocket_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
