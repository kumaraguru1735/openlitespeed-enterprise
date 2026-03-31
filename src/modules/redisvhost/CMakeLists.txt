cmake_minimum_required(VERSION 3.0)

set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -Wall")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -Wall")

execute_process( COMMAND getconf LFS_CFLAGS
            OUTPUT_VARIABLE  LFS_CFLAGS )

add_definitions( ${LFS_CFLAGS} )

find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h
    PATHS /usr/include /usr/local/include)

find_library(HIREDIS_LIBRARY hiredis
    PATHS /usr/lib /usr/local/lib /usr/lib64 /usr/lib/x86_64-linux-gnu)

if (NOT HIREDIS_INCLUDE_DIR OR NOT HIREDIS_LIBRARY)
    message(WARNING "hiredis not found - mod_redisvhost will not be built. "
                    "Install libhiredis-dev to enable.")
    return()
endif()

add_library(mod_redisvhost SHARED
    mod_redisvhost.cpp
)

target_include_directories(mod_redisvhost PRIVATE ${HIREDIS_INCLUDE_DIR})
target_link_libraries(mod_redisvhost ${HIREDIS_LIBRARY} pthread)
set_target_properties(mod_redisvhost PROPERTIES PREFIX "")
