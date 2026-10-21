# GoogleTest's discovery include runs first; an empty list may be undefined.
if(NOT unit_tests_TESTS)
	add_test(unit_empty "${unit_runner}")
	set_tests_properties(unit_empty PROPERTIES LABELS unit)
endif()
