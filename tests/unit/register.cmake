include(GoogleTest)
gtest_discover_tests(unit_tests PROPERTIES LABELS unit)

# CTest includes do not inherit CMake's configure-time executable variables.
file(GENERATE
	OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/unit_runner-$<CONFIG>.cmake"
	CONTENT "set(unit_runner [==[$<TARGET_FILE:unit_tests>]==])\n"
)
get_property(unit_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(unit_multi_config)
	set(unit_configuration "\${CTEST_CONFIGURATION_TYPE}")
else()
	set(unit_configuration "${CMAKE_BUILD_TYPE}")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/require_cases.cmake"
	"include(\"${CMAKE_CURRENT_BINARY_DIR}/unit_runner-"
	"${unit_configuration}.cmake\")\n"
	"include(\"${CMAKE_CURRENT_LIST_DIR}/require_cases.cmake\")\n"
)
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
	"${CMAKE_CURRENT_BINARY_DIR}/require_cases.cmake"
)
