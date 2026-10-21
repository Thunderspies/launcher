set(launcher_aws_crt_platform_options)
if(MINGW)
	# Match aws-c-common's disabled SIMD across all upstream subprojects.
	set(launcher_aws_crt_platform_options "USE_CPU_EXTENSIONS OFF")
endif()
CPMAddPackage(
	NAME LauncherAwsCrt
	VERSION 0.43.9
	GITHUB_REPOSITORY awslabs/aws-crt-cpp
	GIT_TAG f4b8dfd95b593a92de469151544bccc4d39dc58c
	GIT_SUBMODULES_RECURSE YES
	EXCLUDE_FROM_ALL YES
	OPTIONS
		"BUILD_DEPS ON"
		"BUILD_TESTING OFF"
		"BUILD_SHARED_LIBS OFF"
		"USE_OPENSSL OFF"
		"BYO_CRYPTO OFF"
		"CMAKE_POSITION_INDEPENDENT_CODE ON"
		"CMAKE_C_EXTENSIONS ON"
		${launcher_aws_crt_platform_options}
)
