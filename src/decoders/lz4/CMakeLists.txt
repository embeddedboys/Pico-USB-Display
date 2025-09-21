set(LIBRARY_NAME lz4)

add_library(${LIBRARY_NAME})

target_sources(${LIBRARY_NAME} PRIVATE
	${LZ4_PATH}/lib/lz4.c
)

target_include_directories(${LIBRARY_NAME} PUBLIC
	${CMAKE_CURRENT_LIST_DIR}
	${LZ4_PATH}/lib
)
