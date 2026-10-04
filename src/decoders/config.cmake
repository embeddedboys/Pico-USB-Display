# One file per codec keeps decoder.c to the frame pipeline: slots, submit,
# the decode task, the dispatch and the boot logo.  Each codec owns its own
# static buffers and its own band validation, which is how they differ.
list(APPEND PUD_SOURCES
	${CMAKE_CURRENT_LIST_DIR}/decoder.c
	${CMAKE_CURRENT_LIST_DIR}/decoder_jpeg.c
	${CMAKE_CURRENT_LIST_DIR}/decoder_lz4.c
	${CMAKE_CURRENT_LIST_DIR}/decoder_qoi.c
	${CMAKE_CURRENT_LIST_DIR}/decoder_rle.c
	${CMAKE_CURRENT_LIST_DIR}/decoder_qoiz.c
	${CMAKE_CURRENT_LIST_DIR}/decoder_qoid.c
)
