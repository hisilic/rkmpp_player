cmake_minimum_required(VERSION 2.8)

project(rkmpp_player)

set(CMAKE_CXX_STANDARD 11)

include_directories(/usr/local/include/libdrm /usr/local/include/rockchip ./rkdrm)

set(SOURCE_FILES main.c
        mppdecoder.c
        rkdrm/bo.c
        rkdrm/dev.c
        rkdrm/modeset.c
        rkdrm/rkdrm.c)

add_executable(rkmpp_player ${SOURCE_FILES})
target_link_libraries(rkmpp_player avcodec avformat avutil rockchip_mpp pthread drm)
