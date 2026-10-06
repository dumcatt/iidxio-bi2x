cmake_minimum_required(VERSION 3.16)
project(iidxio-bio2video VERSION 2.0
			 LANGUAGES C
)

set(CMAKE_C_STANDARD 99)
set(CMAKE_SHARED_LIBRARY_PREFIX "")

if (DEBUG_BUILD)
	add_compile_options(-g)
else()
	add_compile_options(-O2)
endif()

add_compile_options(-Wall)

if (MINGW)
	# no libgcc / winpthread DLL dependencies
	add_link_options(-static -static-libgcc)
endif()

# Stand-alone BI2X driver: no libaio / libacc needed at build or run time.
add_library(bi2x STATIC
	src/bi2x/bi2x_crc.c
	src/bi2x/bi2x_frame.c
	src/bi2x/bi2x_link.c
	src/bi2x/bi2x_log.c
	src/bi2x/bi2x_lz.c
	src/bi2x/bi2x_module.c
	src/bi2x/bi2x_port_posix.c
	src/bi2x/bi2x_port_win32.c
	src/bi2x/bi2x_tdj.c
)
target_include_directories(bi2x PUBLIC src)

if (WIN32)
	target_link_libraries(bi2x setupapi)
else()
	target_compile_definitions(bi2x PRIVATE _DEFAULT_SOURCE)
	target_link_libraries(bi2x pthread m)
endif()

add_executable(bi2xtest src/main.c)
target_link_libraries(bi2xtest bi2x)

add_executable(bi2x-modextract tools/bi2x_modextract.c)
target_link_libraries(bi2x-modextract bi2x)

if (WIN32)
	if (NOT EXISTS ${CMAKE_SOURCE_DIR}/bemanitools/src/main/bemanitools/iidxio.h)
		message(WARNING "bemanitools submodule missing, skipping iidxio-bi2x.dll "
		                "(git submodule update --init)")
	else()
		add_library(iidxio-bi2x SHARED src/iidxio.c)
		target_include_directories(iidxio-bi2x PRIVATE bemanitools/src/main)
		target_link_libraries(iidxio-bi2x bi2x)
	endif()
else()
	# host-side tests against the simulated board (test/run_sim_test.sh)
	add_executable(test_host test/test_host.c)
	target_link_libraries(test_host bi2x)
endif()
