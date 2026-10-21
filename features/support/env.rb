require 'cucumber/wire'

InstallPlugin do |configuration, _registry|
	scenarios = 0
	configuration.on_event(:test_case_started) { scenarios += 1 }
	configuration.on_event(:test_run_finished) do
		if scenarios.zero?
			# Defer failure so Cucumber can finish its AfterAll hooks.
			at_exit do
				warn 'Acceptance suite selected no scenarios. ' \
					'Add scenarios or adjust the feature paths and filters.'
				exit 1 if !$! || ($!.is_a?(SystemExit) && $!.success?)
			end
		end
	end
end
