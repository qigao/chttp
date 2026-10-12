cmake_minimum_required(VERSION 3.25)
project(chttp_rpc_service_consumer LANGUAGES C CXX)

find_package(Chttp CONFIG REQUIRED)

if(NOT TARGET CHttp::RpcService)
  message(FATAL_ERROR "Chttp package did not export CHttp::RpcService")
endif()

add_executable(chttp_rpc_service_consumer main.c)
target_link_libraries(chttp_rpc_service_consumer PRIVATE CHttp::RpcService)
target_compile_features(chttp_rpc_service_consumer PRIVATE c_std_11)

# New native Server/Client consumers must not require the RpcService target.
if(NOT TARGET CHttp::Server OR NOT TARGET CHttp::Client)
  message(FATAL_ERROR "Missing exported CHttp Server/Client targets")
endif()
add_executable(chttp_rpc_native_consumer native.c)
target_link_libraries(chttp_rpc_native_consumer PRIVATE
  CHttp::Server CHttp::Client)
target_compile_features(chttp_rpc_native_consumer PRIVATE c_std_11)
