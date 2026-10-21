# Windows CI may have no console for delivering CTRL_BREAK_EVENT. Raise the
# same exception as the launcher's signal handler after both children start.
Thread.new do
	path = File.join(ENV.fetch('RUNNER_FIXTURE_DIR'), 'consumer.ready')
	deadline = Process.clock_gettime(Process::CLOCK_MONOTONIC) + 20
	until File.file?(path)
		if Process.clock_gettime(Process::CLOCK_MONOTONIC) >= deadline
			Thread.main.raise('Interrupt fixture startup timed out')
			break
		end
		sleep 0.02
	end
	Thread.main.raise(SignalException.new('INT')) if File.file?(path)
end
