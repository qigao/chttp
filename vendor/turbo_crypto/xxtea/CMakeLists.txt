
set(LIBXXTEA_SRC xxtea.c)
add_library(xxtea SHARED ${LIBXXTEA_SRC})
add_library(xxtea_static STATIC ${LIBXXTEA_SRC})

# On Windows, avoid .lib name conflict between shared and static
if(WIN32)
    set_target_properties(xxtea_static PROPERTIES OUTPUT_NAME "xxtea_static")
else()
    set_target_properties(xxtea_static PROPERTIES OUTPUT_NAME "xxtea")
endif()

set_target_properties(xxtea PROPERTIES FOLDER vendor/xxtea)
set_target_properties(xxtea_static PROPERTIES FOLDER vendor/xxtea)
target_include_directories(xxtea PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(xxtea_static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})


add_subdirectory(example)

