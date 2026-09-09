cmake_add_test(crpc_json_test
  SOURCES crpc_json_test.c
  LIBS salts_crpc Salts::TinyTest
  INCLUDES ../src
  FOLDER "crpc/tests")
set_target_properties(crpc_json_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

cmake_add_test(crpc_api_test
  SOURCES crpc_api_test.c
  LIBS salts_crpc Salts::TinyTest
  FOLDER "crpc/tests")
set_target_properties(crpc_api_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

cmake_add_test(crpc_transport_test
  SOURCES crpc_transport_test.c
  LIBS salts_crpc Salts::TinyTest
  INCLUDES ../../chttp/tests
  FOLDER "crpc/tests")
set_target_properties(crpc_transport_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

cmake_add_test(crpc_server_test
  SOURCES crpc_server_test.c
  LIBS salts_crpc Salts::TinyTest
  INCLUDES ../../chttp/tests
  FOLDER "crpc/tests")
set_target_properties(crpc_server_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

cmake_add_test(crpc_header_cpp_test
  SOURCES crpc_header_cpp_test.cpp
  LIBS salts_crpc
  FOLDER "crpc/tests")
set_target_properties(crpc_header_cpp_test PROPERTIES
  CXX_STANDARD 11
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
