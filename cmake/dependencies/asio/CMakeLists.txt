CPMAddPackage(
	NAME Asio
	VERSION 1.30.2
	GITHUB_REPOSITORY chriskohlhoff/asio
	GIT_TAG asio-1-30-2
	EXCLUDE_FROM_ALL YES
	PATCHES "${CMAKE_CURRENT_SOURCE_DIR}/cmake.patch"
)

# Cucumber-Cpp consumes the legacy include variable from find_package(Asio).
file(WRITE "${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/asio-extra.cmake"
	"set(ASIO_INCLUDE_DIR \"${Asio_SOURCE_DIR}/asio/include\")\n"
)
