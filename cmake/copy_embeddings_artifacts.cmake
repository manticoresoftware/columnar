if (NOT EXISTS "${EMBEDDINGS_LIB_SRC_PATH}")
	message ( FATAL_ERROR "Expected library file not found: ${EMBEDDINGS_LIB_SRC_PATH}" )
endif ()

execute_process (
		COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${EMBEDDINGS_LIB_SRC_PATH}" "${EMBEDDINGS_LIB_DST_PATH}"
		RESULT_VARIABLE COPY_RESULT
)
if (NOT COPY_RESULT EQUAL 0)
	message ( FATAL_ERROR "Failed to copy embeddings library: ${COPY_RESULT}" )
endif ()

if (EMBEDDINGS_PDB_SRC_PATH AND EMBEDDINGS_PDB_DST_PATH AND EXISTS "${EMBEDDINGS_PDB_SRC_PATH}")
	execute_process (
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${EMBEDDINGS_PDB_SRC_PATH}" "${EMBEDDINGS_PDB_DST_PATH}"
			RESULT_VARIABLE COPY_PDB_RESULT
	)
	if (NOT COPY_PDB_RESULT EQUAL 0)
		message ( FATAL_ERROR "Failed to copy embeddings PDB: ${COPY_PDB_RESULT}" )
	endif ()
endif ()
