add_executable(chttp_application_example application.c)
target_link_libraries(chttp_application_example PRIVATE CHttp::App)
set_target_properties(chttp_application_example PROPERTIES FOLDER "app/examples")

add_executable(chttp_application_cpp_example application.cpp)
target_link_libraries(chttp_application_cpp_example PRIVATE CHttp::App)
set_target_properties(chttp_application_cpp_example PROPERTIES FOLDER "app/examples")
