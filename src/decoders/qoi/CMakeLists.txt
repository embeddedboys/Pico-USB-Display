set(LIBRARY_NAME qoi)

add_library(${LIBRARY_NAME})

target_sources(${LIBRARY_NAME} PRIVATE
	${CMAKE_CURRENT_LIST_DIR}/rgb565_qoi.c
)

target_include_directories(${LIBRARY_NAME} PUBLIC
	${CMAKE_CURRENT_LIST_DIR}
)
