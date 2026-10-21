add_library(transaction_fault SHARED transaction_fault.c)
target_link_libraries(transaction_fault PRIVATE ${CMAKE_DL_LIBS})
target_compile_options(transaction_fault PRIVATE -Wall -Wextra -Wpedantic)
add_executable(borrow_lifetime borrow_lifetime.cpp)
target_link_libraries(borrow_lifetime PRIVATE launcher::launcher)
add_test(NAME launcher_faults
	COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run.py"
		$<TARGET_FILE:launcher_cli> $<TARGET_FILE:transaction_fault>
		$<TARGET_FILE:borrow_lifetime>)
set_tests_properties(launcher_faults PROPERTIES LABELS unit TIMEOUT 120)
