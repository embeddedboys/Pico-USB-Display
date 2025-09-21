add_library(jpegdec)

target_sources(jpegdec PRIVATE
    JPEGDEC.c
)

target_include_directories(jpegdec PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_compile_definitions(jpegdec PRIVATE
        PICO_PLATFORM
)
