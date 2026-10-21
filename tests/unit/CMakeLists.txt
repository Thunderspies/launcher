add_executable(unit_tests main.cpp format.cpp transport.cpp platform.cpp
	workflow.cpp transaction.cpp)
target_sources(unit_tests PRIVATE credentials.cpp stored.cpp)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	target_link_options(unit_tests PRIVATE
		"-Wl,--wrap=mbedtls_sha256_finish" "-Wl,--wrap=fsync")
	target_compile_definitions(unit_tests PRIVATE
		LAUNCHER_TEST_HASH_COUNTS LAUNCHER_TEST_IO_COUNTS)
endif()
set(launcher_unit_library launcher::launcher)
if(LAUNCHER_BUILD_SHARED)
	# Unit tests exercise private contracts without exporting DLL internals.
	get_target_property(launcher_unit_sources launcher SOURCES)
	list(TRANSFORM launcher_unit_sources
		PREPEND "${PROJECT_SOURCE_DIR}/src/")
	add_library(launcher_unit_library STATIC ${launcher_unit_sources})
	target_include_directories(launcher_unit_library PUBLIC
		"${PROJECT_SOURCE_DIR}/include")
	target_compile_definitions(launcher_unit_library PRIVATE LAUNCHER_BUILD)
	target_link_libraries(launcher_unit_library PRIVATE piggle::piggle
		CURL::libcurl yyjson mbedcrypto aws-c-auth aws-c-http)
	if(MSVC)
		target_compile_definitions(launcher_unit_library PRIVATE
			_CRT_SECURE_NO_WARNINGS)
		target_compile_options(launcher_unit_library PRIVATE /W4 /utf-8)
	else()
		target_compile_options(launcher_unit_library PRIVATE
			-Wall -Wextra -Wpedantic)
	endif()
	set(launcher_unit_library launcher_unit_library)
endif()
target_link_libraries(unit_tests PRIVATE GTest::gtest ${launcher_unit_library}
	piggle::piggle yyjson mbedcrypto)
target_include_directories(unit_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(unit_tests PRIVATE
	LAUNCHER_PRODUCT_UNIT_TESTS
	LAUNCHER_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
	LAUNCHER_TEST_CHILD="$<TARGET_FILE:process_child>")
add_dependencies(unit_tests process_child)
if(WIN32)
	target_link_libraries(unit_tests PRIVATE ws2_32)
endif()

include(register.cmake)
