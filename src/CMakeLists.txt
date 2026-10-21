if(LAUNCHER_BUILD_SHARED)
	add_library(launcher SHARED)
	target_compile_definitions(launcher PUBLIC LAUNCHER_SHARED)
else()
	add_library(launcher STATIC)
endif()
add_library(launcher::launcher ALIAS launcher)
target_sources(launcher PRIVATE core.cpp transaction.cpp format.cpp
	transport.cpp archive.cpp platform.cpp)
target_include_directories(launcher PUBLIC
	$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
	$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_definitions(launcher PRIVATE LAUNCHER_BUILD)
if(UNIX)
	target_compile_definitions(launcher PRIVATE _FILE_OFFSET_BITS=64)
endif()
target_link_libraries(launcher PRIVATE piggle::piggle CURL::libcurl
	yyjson mbedcrypto aws-c-auth aws-c-http)
set_target_properties(launcher PROPERTIES VERSION ${PROJECT_VERSION}
	SOVERSION 0)
if(MSVC)
	target_compile_options(launcher PRIVATE /W4 /utf-8)
	target_compile_definitions(launcher PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
	target_compile_options(launcher PRIVATE -Wall -Wextra -Wpedantic)
endif()
