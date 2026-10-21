require 'rbconfig'

def fixture_wait(path)
	deadline = Process.clock_gettime(Process::CLOCK_MONOTONIC) + 20
	until File.file?(path)
		raise "Fixture did not create #{path}" if
			Process.clock_gettime(Process::CLOCK_MONOTONIC) >= deadline
		sleep 0.02
	end
end

InstallPlugin do |_configuration, _registry|
	directory = ENV.fetch('RUNNER_FIXTURE_DIR')
	File.write(File.join(directory, 'consumer.pid'), Process.pid.to_s)
	mode = ENV.fetch('RUNNER_FIXTURE_MODE')
	if mode != 'wire'
		Process.spawn(RbConfig.ruby, File.join(__dir__, 'server.rb'),
			'consumer-child', out: File::NULL, err: File::NULL)
		fixture_wait(File.join(directory, 'consumer-child.ready'))
	end
	File.write(File.join(directory, 'consumer.ready'), 'ready')
	if %w[interrupt after_ready_failure].include?(mode)
		loop { sleep 1 }
	end
	raise 'Intentional consumer failure' if mode == 'consumer_failure'
end

AfterAll do
	directory = ENV.fetch('RUNNER_FIXTURE_DIR')
	File.write(File.join(directory, 'consumer.finished'), 'finished')
	if ENV['RUNNER_FIXTURE_MODE'] == 'after_success_failure'
		File.write(File.join(directory, 'consumer.done'), 'done')
		fixture_wait(File.join(directory, 'server.exiting'))
	end
end
