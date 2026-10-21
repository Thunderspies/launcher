# Record the launcher's child group leaders without changing spawn behavior.
# This lets the harness detect leaks and clean up even a broken launcher.
module RunnerChildTracking
	def spawn(*arguments, **options)
		pid = super
		path = File.join(ENV.fetch('RUNNER_FIXTURE_DIR'), "group-#{pid}.pid")
		File.write(path, pid.to_s)
		pid
	end
end

Process.singleton_class.prepend(RunnerChildTracking)
