add_executable(example_full_use_case full_use_case.c)
target_link_libraries(example_full_use_case PRIVATE cjwt)
set_target_properties(example_full_use_case PROPERTIES FOLDER
                                                       vendor/cjwt/examples)

# Basic examples
add_executable(example_hs_basic basic/hs_example.c)
target_link_libraries(example_hs_basic PRIVATE cjwt)
set_target_properties(example_hs_basic PROPERTIES FOLDER vendor/cjwt/examples)

add_executable(example_rs_basic basic/rs_example.c)
target_link_libraries(example_rs_basic PRIVATE cjwt)
set_target_properties(example_rs_basic PROPERTIES FOLDER vendor/cjwt/examples)

add_executable(example_es_basic basic/es_example.c)
target_link_libraries(example_es_basic PRIVATE cjwt)
set_target_properties(example_es_basic PROPERTIES FOLDER vendor/cjwt/examples)

add_executable(example_ps_basic basic/ps_example.c)
target_link_libraries(example_ps_basic PRIVATE cjwt)
set_target_properties(example_ps_basic PROPERTIES FOLDER vendor/cjwt/examples)
