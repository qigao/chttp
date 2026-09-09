
# cjwt tests using project's Unity
set(TEST_FILES
    test_utils.c
    test_print.c
    test_cjwt_new.c
    test_cjwt.c
    test_map.c
    test_encode.c
    test_jwk.c
    test_jwe.c
    test_jwks.c
    test_pbes2.c
)

foreach(test_file ${TEST_FILES})
    get_filename_component(test_stem ${test_file} NAME_WE)
    set(test_name chttp_cjwt_${test_stem})
    add_executable(${test_name} ${test_file})
    target_link_libraries(${test_name} PRIVATE chttp_cjwt Salts::JsonParser Salts::TinyTest turbo_crypto)
    target_include_directories(${test_name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/cjwt
        ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_compile_definitions(${test_name} PRIVATE TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    
    if(MSVC)
        target_compile_definitions(${test_name} PRIVATE 
            _CRT_SECURE_NO_WARNINGS
            "ssize_t=SSIZE_T"
            "read=_read"
            "open=_open"
            "close=_close"
            "getcwd=_getcwd"
        )
        target_compile_options(${test_name} PRIVATE /wd4005) # Macro redefinition
    endif()
    
    add_test(NAME ${test_name} COMMAND ${test_name})
    set_target_properties(${test_name} PROPERTIES FOLDER chhttp/internal/tests)
endforeach()
