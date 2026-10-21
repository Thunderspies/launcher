require 'rbconfig'
require 'socket'
require 'json'

directory = ENV.fetch('RUNNER_FIXTURE_DIR')
mode = ENV.fetch('RUNNER_FIXTURE_MODE')
role = ARGV.first || 'server'
File.write(File.join(directory, "#{role}.pid"), Process.pid.to_s)

def wait_for_file(path)
	deadline = Process.clock_gettime(Process::CLOCK_MONOTONIC) + 20
	until File.file?(path) && File.size(path).positive?
		raise "Fixture did not create #{path}" if
			Process.clock_gettime(Process::CLOCK_MONOTONIC) >= deadline
		sleep 0.02
	end
end

def spawn_relative(directory, role)
	Process.spawn(RbConfig.ruby, __FILE__, role,
		out: File::NULL, err: File::NULL)
	wait_for_file(File.join(directory, "#{role}.pid"))
end

if role != 'server'
	Signal.trap('INT', 'IGNORE')
	Signal.trap('TERM', 'IGNORE')
	if role.end_with?('-child')
		spawn_relative(directory, role.sub(/-child\z/, '-grandchild'))
	end
	File.write(File.join(directory, "#{role}.ready"), 'ready')
	loop { sleep 1 }
end

spawn_relative(directory, 'server-child')
wait_for_file(File.join(directory, 'server-child.ready'))
exit 7 if mode == 'early_exit'

# These fixtures deliberately require the launcher's forced cleanup.
Signal.trap('INT', 'IGNORE')
Signal.trap('TERM', 'IGNORE')
loop { sleep 1 } if mode == 'timeout'
if mode == 'oversized_output'
	STDOUT.write('x' * 70_000)
	STDOUT.flush
	loop { sleep 1 }
end

server = TCPServer.new('127.0.0.1', 0)
puts "Listening on 127.0.0.1:#{server.addr[1]}"
STDOUT.flush
File.write(File.join(directory, 'server.ready'), 'ready')

# The no-step scenario still invokes Cucumber's wire before/after hooks.
Thread.new do
	loop do
		client = server.accept
		begin
			while (line = client.gets)
				request = JSON.parse(line)
				unless %w[begin_scenario end_scenario].include?(request[0])
					raise "Unexpected fixture wire request: #{request.inspect}"
				end
				client.puts(JSON.generate(['success']))
			end
		ensure
			client.close
		end
	end
end

if mode == 'after_ready_failure'
	wait_for_file(File.join(directory, 'consumer.ready'))
	exit 7
elsif mode == 'after_success_failure'
	wait_for_file(File.join(directory, 'consumer.done'))
	File.write(File.join(directory, 'server.exiting'), 'exiting')
	exit 7
end
loop { sleep 1 }
