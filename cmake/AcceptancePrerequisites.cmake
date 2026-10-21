find_program(RUBY_EXECUTABLE NAMES ruby)
if(NOT RUBY_EXECUTABLE)
	message(FATAL_ERROR
		"Acceptance tests require Ruby. Install Ruby and Bundler, then "
		"run 'bundle install' in the repository root. "
		"Use -DBUILD_ACCEPTANCE_TESTS=OFF for unit tests only."
	)
endif()

find_program(BUNDLER_EXECUTABLE NAMES bundle bundler)
if(NOT BUNDLER_EXECUTABLE)
	message(FATAL_ERROR
		"Acceptance tests require Bundler. Run 'gem install bundler', "
		"then 'bundle install' in the repository root."
	)
endif()

# RubyGems installs an extensionless Ruby script that Windows cannot execute
# directly. Use the selected interpreter instead of relying on its shebang.
execute_process(
	COMMAND ${CMAKE_COMMAND} -E env
		"BUNDLE_GEMFILE=${PROJECT_SOURCE_DIR}/Gemfile"
		"BUNDLE_FROZEN=true"
		"${RUBY_EXECUTABLE}" "${BUNDLER_EXECUTABLE}" check
	WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
	RESULT_VARIABLE bundle_result
	OUTPUT_VARIABLE bundle_output
	ERROR_VARIABLE bundle_error
)
if(NOT bundle_result EQUAL 0)
	message(FATAL_ERROR
		"Acceptance test gems are unavailable. Run 'bundle install' "
		"in the repository root using the selected Ruby and Bundler.\n"
		"${bundle_output}${bundle_error}"
	)
endif()
