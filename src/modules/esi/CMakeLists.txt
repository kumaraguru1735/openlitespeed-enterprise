cmake_minimum_required(VERSION 3.0)

SET(mod_esi_STAT_SRCS
    mod_esi.cpp
    esiparser.cpp
)

add_library(mod_esi STATIC ${mod_esi_STAT_SRCS})

target_include_directories(mod_esi PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
)
