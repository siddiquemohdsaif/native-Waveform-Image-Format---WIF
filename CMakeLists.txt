cmake_minimum_required(VERSION 3.16)

project(wif C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(wif-preprocess
    src/cli/preprocess.c
    src/wif/utils/preprocessor.c
)

target_include_directories(wif-preprocess PRIVATE src)

add_executable(wif-evaluate-plane
    src/cli/evaluate_plane.c
    src/wif/evaluators/eye_entropy_evaluator.c
)

target_include_directories(wif-evaluate-plane PRIVATE src)

add_executable(wif-rans-route-map
    src/cli/rans_route_map.c
    src/wif/utils/rans_route_map.c
)

target_include_directories(wif-rans-route-map PRIVATE src)

add_executable(wif-decode-plane
    src/cli/decode_plane.c
    src/wif/utils/decode_plane.c
    src/wif/utils/rans_route_map.c
)

target_include_directories(wif-decode-plane PRIVATE src)

add_executable(wif-ycbcr-to-png
    src/cli/ycbcr_to_png.c
    src/wif/utils/ycbcr_to_png.c
)

target_include_directories(wif-ycbcr-to-png PRIVATE src)

if(WIN32)
    target_link_libraries(wif-preprocess PRIVATE ole32 windowscodecs)
    target_link_libraries(wif-evaluate-plane PRIVATE ole32 windowscodecs)
    target_link_libraries(wif-decode-plane PRIVATE ole32 windowscodecs)
    target_link_libraries(wif-ycbcr-to-png PRIVATE ole32 windowscodecs)
else()
    message(FATAL_ERROR "WIF debug PNG tools currently use Windows Imaging Component and must be built on Windows.")
endif()
