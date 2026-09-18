# Every codec library here is a single translation unit that carries both
# directions it supports (compress and decompress), and the firmware only ever
# calls one of them.  --gc-sections can only drop the other half when the
# library is compiled with the same section flags the firmware uses: without
# these, linking lz4.c costs ~25 KB of flash for compressors that can never be
# reached (measured, see notes/decoders.md).
add_compile_options(-ffunction-sections -fdata-sections)

add_subdirectory(jpegdec)
add_subdirectory(tjpgd)
add_subdirectory(lz4)
add_subdirectory(qoi)
add_subdirectory(rle)
