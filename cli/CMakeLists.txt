add_executable(launcher_cli main.cpp)
set_target_properties(launcher_cli PROPERTIES OUTPUT_NAME launcher)
target_link_libraries(launcher_cli PRIVATE launcher::launcher)
if(MINGW)
	target_link_options(launcher_cli PRIVATE -municode)
endif()
if(WIN32 AND LAUNCHER_BUILD_SHARED)
	add_custom_command(TARGET launcher_cli POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			$<TARGET_FILE:launcher> $<TARGET_FILE_DIR:launcher_cli>)
elseif(UNIX AND NOT APPLE)
	set_target_properties(launcher_cli PROPERTIES
		INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()
if(MSVC)
	target_compile_options(launcher_cli PRIVATE /W4 /utf-8)
else()
	target_compile_options(launcher_cli PRIVATE -Wall -Wextra -Wpedantic)
endif()
