cmake_minimum_required(VERSION 3.25)
project(chttp_service_consumer LANGUAGES C)

find_package(Chttp CONFIG REQUIRED)

add_executable(chttp_service_consumer main.c)
target_link_libraries(chttp_service_consumer PRIVATE CHttp::Service)
target_compile_features(chttp_service_consumer PRIVATE c_std_11)
