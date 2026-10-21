cmake_minimum_required(VERSION 3.24)
project(launcher VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
option(BUILD_UNIT_TESTS "Build GoogleTest unit tests" ON)
option(BUILD_ACCEPTANCE_TESTS "Build Cucumber acceptance tests" OFF)
option(LAUNCHER_BUILD_SHARED "Build the launcher shared library" ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
add_subdirectory(cmake/dependencies EXCLUDE_FROM_ALL)

add_subdirectory(src)
add_subdirectory(cli)

if(BUILD_TESTING AND (BUILD_UNIT_TESTS OR BUILD_ACCEPTANCE_TESTS))
	if(BUILD_ACCEPTANCE_TESTS)
		include(cmake/AcceptancePrerequisites.cmake)
	endif()
	add_subdirectory(tests)
endif()

install(TARGETS launcher launcher_cli EXPORT launcherTargets
	ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
	LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(LAUNCHER_BUILD_SHARED)
	set(launcher_package_dir "${CMAKE_INSTALL_LIBDIR}/cmake/launcher")
	configure_package_config_file(cmake/launcherConfig.cmake.in
		"${CMAKE_CURRENT_BINARY_DIR}/launcherConfig.cmake"
		INSTALL_DESTINATION "${launcher_package_dir}")
	write_basic_package_version_file(
		"${CMAKE_CURRENT_BINARY_DIR}/launcherConfigVersion.cmake"
		VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
	install(EXPORT launcherTargets NAMESPACE launcher::
		DESTINATION "${launcher_package_dir}")
	install(FILES "${CMAKE_CURRENT_BINARY_DIR}/launcherConfig.cmake"
		"${CMAKE_CURRENT_BINARY_DIR}/launcherConfigVersion.cmake"
		DESTINATION "${launcher_package_dir}")
endif()
