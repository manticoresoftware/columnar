set(CTEST_SOURCE_DIRECTORY "$ENV{CI_PROJECT_DIR}")
set(CTEST_BINARY_DIRECTORY "build")

file ( GLOB TESTXML "build/Testing/2*/*.xml" )
if ( NOT TESTXML )
    message(FATAL_ERROR "Nothing to upload.")
endif()

find_program ( PHP NAMES php )
find_program ( XSLTPROC NAMES xsltproc )

foreach (RESULT ${TESTXML})
	get_filename_component(TSTNAME ${RESULT} PATH)
	get_filename_component(TSTNAME ${TSTNAME} NAME_WE)
	execute_process(
			COMMAND ${PHP} ${CTEST_SOURCE_DIRECTORY}/misc/junit/filter.php ${RESULT}
			COMMAND ${XSLTPROC} --stringparam pass "${TSTNAME}_" -o ${CTEST_BINARY_DIRECTORY}/junit_${TSTNAME}.xml ${CTEST_SOURCE_DIRECTORY}/misc/junit/ctest2junit.xsl -
	)
endforeach ()


