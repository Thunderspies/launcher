CPMAddPackage(
	NAME TCLAP
	VERSION 1.2.5
	GITHUB_REPOSITORY mirror/tclap
	GIT_TAG v1.2.5
	EXCLUDE_FROM_ALL YES
	PATCHES "${CMAKE_CURRENT_SOURCE_DIR}/cmake.patch"
)

# Cucumber-Cpp consumes the legacy include variable from find_package(TCLAP).
file(WRITE "${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/tclap-extra.cmake"
	"set(TCLAP_INCLUDE_DIR \"${TCLAP_SOURCE_DIR}/include\")\n"
)
