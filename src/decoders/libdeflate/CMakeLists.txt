set(LIBRARY_NAME libdeflate)

# Decompressor half of libdeflate v1.26 (ebiggers/libdeflate), the alternative
# inflate for the QOI+deflate codec (DECODER_TYPE 5, PUD_INFLATE=libdeflate).
# The seven upstream files are byte-identical to that tag; nothing of ours is
# in this directory apart from this file and .clang-format.
add_library(${LIBRARY_NAME} STATIC
	${CMAKE_CURRENT_LIST_DIR}/lib/deflate_decompress.c
	${CMAKE_CURRENT_LIST_DIR}/lib/utils.c
)

target_include_directories(${LIBRARY_NAME} PUBLIC
	${CMAKE_CURRENT_LIST_DIR}
)

if(PUD_CODEC_IN_RAM)
	# libdeflate has no placement hook: its decode loop is a static function
	# expanded from a template, so no attribute can be put on it from outside.
	# Instead the decompressor is compiled into a single .text section, and the
	# archive member is renamed to .time_critical.libdeflate before the link --
	# the SDK linker script copies .time_critical* to SRAM.  The source stays
	# untouched.  (GNU as has no --rename-section for ELF, hence objcopy.)
	#
	# Only deflate_decompress.c is moved: utils.c holds the allocator defaults,
	# nothing hot.  objcopy runs on the archive and renames the section only
	# where it exists by that name, i.e. in that one member.
	set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/lib/deflate_decompress.c
		PROPERTIES COMPILE_OPTIONS -fno-function-sections)
	add_custom_command(TARGET ${LIBRARY_NAME} POST_BUILD
		COMMAND ${CMAKE_OBJCOPY}
			--rename-section .text=.time_critical.libdeflate,alloc,load,readonly,code,contents
			$<TARGET_FILE:${LIBRARY_NAME}>
		VERBATIM)
endif()
