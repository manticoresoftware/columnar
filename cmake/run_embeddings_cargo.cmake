# Launcher used only when the ClangCL toolset is active but ring's C must be
# built by native cl.exe (see build_embeddings.cmake). The ClangCL MSBuild
# toolset prepends Clang's builtin-header directory to INCLUDE; cl.exe then
# picks up Clang's <stddef.h> and dies with C1012. cc-rs trusts the ambient
# INCLUDE inside an active devenv, so we can't simply unset it (that leaves
# cl.exe with no CRT/SDK headers at all -> C1083). Instead drop just the LLVM
# entries from INCLUDE/EXTERNAL_INCLUDE at build time, then run the forwarded
# command.

# Collect everything after the "--" separator as the command to run.
set(_cmd)
set(_seen_sep OFF)
math(EXPR _last "${CMAKE_ARGC} - 1")
foreach(_i RANGE 0 ${_last})
	set(_a "${CMAKE_ARGV${_i}}")
	if(_seen_sep)
		list(APPEND _cmd "${_a}")
	elseif(_a STREQUAL "--")
		set(_seen_sep ON)
	endif()
endforeach()

if(NOT _cmd)
	message(FATAL_ERROR "run_embeddings_cargo: no command given after '--'")
endif()

# Strip any LLVM/Clang directory from the include search paths so native cl.exe
# uses only the MSVC + UCRT + Windows SDK headers.
foreach(_var INCLUDE EXTERNAL_INCLUDE)
	if(DEFINED ENV{${_var}})
		set(_dirs "$ENV{${_var}}")   # ';'-separated -> a CMake list
		set(_kept)
		foreach(_d IN LISTS _dirs)
			string(TOLOWER "${_d}" _dl)
			if(NOT _dl MATCHES "[/\\]llvm[/\\]")
				list(APPEND _kept "${_d}")
			endif()
		endforeach()
		set(ENV{${_var}} "${_kept}")   # list stringifies back to ';'-separated
	endif()
endforeach()

execute_process(COMMAND ${_cmd} RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
	message(FATAL_ERROR "run_embeddings_cargo: command failed with exit code ${_rc}")
endif()
