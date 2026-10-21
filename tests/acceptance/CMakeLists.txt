# The upstream cucumber-cpp library supplies the native wire-server main.
add_executable(acceptance_steps steps.cpp)
target_link_libraries(acceptance_steps PRIVATE cucumber-cpp GTest::gtest
	launcher::launcher piggle::piggle mbedcrypto yyjson)
target_compile_definitions(acceptance_steps PRIVATE
	LAUNCHER_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
	LAUNCHER_TEST_CHILD="$<TARGET_FILE:process_child>")
add_dependencies(acceptance_steps process_child)
if(WIN32 AND LAUNCHER_BUILD_SHARED)
	add_custom_command(TARGET acceptance_steps POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			$<TARGET_FILE:launcher> $<TARGET_FILE_DIR:acceptance_steps>)
endif()

add_test(NAME acceptance
	COMMAND "${RUBY_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run.rb"
		$<TARGET_FILE:acceptance_steps>
)
set_tests_properties(acceptance PROPERTIES
	LABELS acceptance
	ENVIRONMENT
		"LAUNCHER_FIXTURE_HTTP_ROOT=${PROJECT_SOURCE_DIR}/tests/fixtures/public;LAUNCHER_FIXTURE_S3_PYTHON=${Python3_EXECUTABLE}"
	WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
)
