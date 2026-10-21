add_executable(fixture_steps fixtures/steps.cpp)
target_link_libraries(fixture_steps PRIVATE cucumber-cpp GTest::gtest)

add_executable(fixture_server fixtures/server.cpp)
add_executable(fixture_unit_empty ../unit/main.cpp)
target_link_libraries(fixture_unit_empty PRIVATE GTest::gtest)
add_executable(fixture_unit_cases ../unit/main.cpp fixtures/unit.cpp)
target_link_libraries(fixture_unit_cases PRIVATE GTest::gtest)

add_test(NAME runner_regressions
	COMMAND "${RUBY_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run.rb"
		$<TARGET_FILE:fixture_steps> $<TARGET_FILE:fixture_server>
		$<TARGET_FILE:fixture_unit_empty> $<TARGET_FILE:fixture_unit_cases>
		"${CMAKE_COMMAND}" "${CMAKE_CTEST_COMMAND}" "${CMAKE_GENERATOR}"
)
set_tests_properties(runner_regressions PROPERTIES
	LABELS infrastructure
	TIMEOUT 300
	WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
)
