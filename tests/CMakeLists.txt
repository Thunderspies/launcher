set(CMAKE_CXX_STANDARD 17)

if(BUILD_UNIT_TESTS OR BUILD_ACCEPTANCE_TESTS)
	find_package(Python3 COMPONENTS Interpreter REQUIRED)
	add_executable(process_child process_child.cpp)
endif()

if(BUILD_UNIT_TESTS)
	find_package(Threads REQUIRED)
	add_test(NAME launcher_cli
		COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cli.py"
			${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:launcher_cli>)
	set_tests_properties(launcher_cli PROPERTIES LABELS unit)
	add_test(NAME launcher_tls
		COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tls.py"
			${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:launcher_cli>)
	set_tests_properties(launcher_tls PROPERTIES LABELS unit TIMEOUT 120)
	add_executable(s3_probe s3_probe.cpp)
	target_link_libraries(s3_probe PRIVATE launcher::launcher Threads::Threads)
	add_test(NAME launcher_s3
		COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/s3.py"
			${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:s3_probe> --
			${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:launcher_cli>)
	set_tests_properties(launcher_s3 PROPERTIES LABELS unit TIMEOUT 180)
	if(WIN32)
		set_tests_properties(launcher_s3 PROPERTIES TIMEOUT 600)
	endif()
	if(WIN32 AND LAUNCHER_BUILD_SHARED)
		add_custom_command(TARGET s3_probe POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
				$<TARGET_FILE:launcher> $<TARGET_FILE_DIR:s3_probe>)
	endif()
	add_subdirectory(consumers)
	add_subdirectory(unit)
	if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING)
		add_subdirectory(faults)
	endif()
endif()

if(BUILD_ACCEPTANCE_TESTS)
	add_subdirectory(acceptance)
	add_subdirectory(infrastructure)
endif()
