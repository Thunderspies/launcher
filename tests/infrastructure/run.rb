require 'fileutils'
require 'json'
require 'rbconfig'
require 'socket'
require 'tmpdir'

$stdout.sync = true

ROOT = File.expand_path('../..', __dir__)
ENV['BUNDLE_GEMFILE'] = File.join(ROOT, 'Gemfile')
ENV['BUNDLE_FROZEN'] = 'true'
require 'bundler/setup'

FIXTURES = File.join(__dir__, 'fixtures')
BUNDLE_DIRECTORY = Bundler.settings[:path] &&
	File.expand_path(Bundler.settings[:path].to_s, ROOT)
WINDOWS = RbConfig::CONFIG['host_os'].match?(/mswin|mingw/)
STEPS, SERVER, UNIT_EMPTY, UNIT_CASES, CMAKE, CTEST, GENERATOR = ARGV
abort 'Expected fixture executables, CMake, CTest, and generator.' unless GENERATOR

def stage_repository(directory)
	repository = File.join(directory, 'repository')
	%w[Gemfile Gemfile.lock tests/acceptance/run.rb
		tests/acceptance/windows_child.rb features/support/env.rb
		features/step_definitions/cucumber.wire].each do |relative|
		target = File.join(repository, relative)
		FileUtils.mkdir_p(File.dirname(target))
		FileUtils.cp(File.join(ROOT, relative), target)
	end
	File.join(repository, 'tests', 'acceptance', 'run.rb')
end

def monotonic_time
	Process.clock_gettime(Process::CLOCK_MONOTONIC)
end

def check(condition, message)
	raise message unless condition
end

def wait_until(message, timeout: 20)
	deadline = monotonic_time + timeout
	until yield
		raise message if monotonic_time >= deadline
		sleep 0.02
	end
end

def alive?(pid)
	Process.kill(0, pid)
	stat = "/proc/#{pid}/stat"
	return false if !WINDOWS && File.file?(stat) &&
		File.read(stat).match?(/\)\s+Z\s/)
	true
rescue Errno::ESRCH, Errno::ENOENT
	false
end

def fixture_pids(directory)
	Dir.glob(File.join(directory, '*.pid')).map do |path|
		Integer(File.read(path))
	end.uniq
end

def cleanup(directory)
	Dir.glob(File.join(directory, 'group-*.pid')).each do |path|
		pid = Integer(File.read(path))
		Process.kill('KILL', WINDOWS ? pid : -pid)
	rescue Errno::ESRCH
	end
	fixture_pids(directory).each do |pid|
		Process.kill('KILL', pid) if alive?(pid)
	rescue Errno::ESRCH
	end
end

def verify_cleanup(directory, output)
	fixture_pids(directory).each do |pid|
		wait_until("Child #{pid} survived runner cleanup", timeout: 5) do
			!alive?(pid)
		end
	end
	output.scan(/Listening on 127\.0\.0\.1:(\d+)/).flatten.uniq.each do |port|
		listener = TCPServer.new('127.0.0.1', Integer(port))
		listener.close
	end
end

def spawn_command(directory, command, environment = {})
	log = File.join(directory, 'output.log')
	group = WINDOWS ? { new_pgroup: true } : { pgroup: true }
	pid = Process.spawn(Bundler.original_env.merge(environment), *command,
		**group, unsetenv_others: true,
		out: log, err: [:child, :out], chdir: ROOT)
	File.write(File.join(directory, 'launcher.pid'), pid.to_s)
	pid
end

def finish_command(directory, pid)
	status = nil
	wait_until('Subprocess timed out', timeout: 30) do
		result = Process.waitpid2(pid, Process::WNOHANG)
		status = result[1] if result
		status
	end
	output = File.read(File.join(directory, 'output.log'))
	verify_cleanup(directory, output)
	[status, output]
rescue StandardError
	log = File.join(directory, 'output.log')
	warn File.read(log) if File.file?(log)
	raise
ensure
	if status.nil?
		begin
			Process.kill('KILL', WINDOWS ? pid : -pid)
			Process.waitpid(pid)
		rescue Errno::ESRCH, Errno::ECHILD
		end
	end
end

def run_command(directory, command, environment = {})
	finish_command(directory, spawn_command(directory, command, environment))
end

def launch(directory, mode, *arguments, server: STEPS, environment: {},
	interrupt: false)
	settings = {
		'BUNDLE_PATH' => BUNDLE_DIRECTORY,
		'RUNNER_FIXTURE_DIR' => directory,
		'RUNNER_FIXTURE_MODE' => mode,
		'RUNNER_FIXTURE_RUBY' => RbConfig.ruby,
		'RUNNER_FIXTURE_SCRIPT' => File.join(FIXTURES, 'server.rb')
	}.merge(environment)
	command = [RbConfig.ruby, '-r', File.join(FIXTURES, 'track_children.rb')]
	if interrupt && WINDOWS
		command += ['-r', File.join(FIXTURES, 'interrupt.rb')]
	end
	command += [stage_repository(directory), server, '--require',
		File.join(FIXTURES, 'support.rb'), *arguments]
	pid = spawn_command(directory, command, settings)
	if interrupt && !WINDOWS
		wait_until('Cucumber did not start before interrupt') do
			File.file?(File.join(directory, 'consumer.ready'))
		end
		Process.kill('INT', pid)
	end
	finish_command(directory, pid)
end

def expect_failure(status, output, diagnostic)
	check(!status.success?, "Expected failure, got success:\n#{output}")
	check(output.include?(diagnostic),
		"Missing diagnostic #{diagnostic.inspect}:\n#{output}")
end

def regression(name)
	Dir.mktmpdir('native-runner-') do |directory|
		begin
			yield directory
			puts "PASS #{name}"
		ensure
			cleanup(directory)
		end
	end
end

wire = File.join(FIXTURES, 'wire.feature')
empty = File.join(FIXTURES, 'empty.feature')
no_steps = File.join(FIXTURES, 'no_steps.feature')

regression('native wire execution and custom formatter') do |directory|
	report = File.join(directory, 'custom report.json')
	status, output = launch(directory, 'wire', wire,
		'--tags', '@passing', '--format', 'json', '--out', report)
	check(status.success?, output)
	check(File.file?(File.join(directory, 'native_server.pid')),
		'The native step was not executed')
	features = JSON.parse(File.read(report))
	steps = features.flat_map { |feature| feature.fetch('elements') }
		.flat_map { |scenario| scenario.fetch('steps') }
	check(steps.any? { |step| step.dig('result', 'status') == 'passed' },
		'Custom formatter did not report the passing native step')
end

{'@failing' => 'Intentional native assertion failure',
	'@pending' => 'pending', '@undefined' => 'undefined'}.each do |tag, message|
	regression("strict native failure #{tag}") do |directory|
		status, output = launch(directory, 'wire', wire,
			'--tags', tag, '--no-strict')
		expect_failure(status, output, message)
	end
end

[[], [empty], [wire, '--tags', '@absent'], ["#{wire}:999"]].each_with_index do
	|arguments, index|
	regression("empty acceptance selection #{index + 1}") do |directory|
		# No arguments exercises the launcher's default feature selection.
		command = [RbConfig.ruby, '-r', File.join(FIXTURES, 'track_children.rb'),
			stage_repository(directory), STEPS, *arguments]
		status, output = run_command(directory, command,
			'BUNDLE_PATH' => BUNDLE_DIRECTORY,
			'RUNNER_FIXTURE_DIR' => directory)
		expect_failure(status, output, 'Acceptance suite selected no scenarios')
	end
end

regression('focused feature line') do |directory|
	status, output = launch(directory, 'wire', "#{wire}:4")
	check(status.success?, output)
	check(File.file?(File.join(directory, 'native_server.pid')), output)
end

regression('empty selections finish Cucumber cleanup hooks') do |directory|
	status, output = launch(directory, 'wire', empty)
	expect_failure(status, output, 'Acceptance suite selected no scenarios')
	check(File.file?(File.join(directory, 'consumer.finished')),
		'Empty-suite guard interrupted AfterAll cleanup')
end

regression('missing native executable') do |directory|
	status, output = launch(directory, 'wire', wire,
		server: File.join(directory, 'missing'))
	expect_failure(status, output, 'Native runner unavailable')
end

regression('missing installed gems') do |directory|
	status, output = launch(directory, 'wire', wire, environment: {
		'BUNDLE_IGNORE_CONFIG' => '1',
		'BUNDLE_PATH' => File.join(directory, 'missing-gems')
	})
	expect_failure(status, output, 'Acceptance gems unavailable')
end

{'early_exit' => 'exited before reporting readiness',
	'timeout' => 'startup timed out',
	'oversized_output' => 'readiness output is too long',
	'after_ready_failure' => 'Wire server failed',
	'after_success_failure' => 'Wire server failed',
	'consumer_failure' => 'Intentional consumer failure'}.each do |mode, message|
	regression("server and consumer failure #{mode}") do |directory|
		status, output = launch(directory, mode, no_steps, server: SERVER)
		expect_failure(status, output, message)
		check(fixture_pids(directory).length >= 4,
			'Process-tree fixture did not start')
	end
end

regression('forced cleanup after success') do |directory|
	status, output = launch(directory, 'cleanup_success', no_steps, server: SERVER)
	check(status.success?, output)
	%w[server-child server-grandchild consumer-child consumer-grandchild].each do
		|role|
		check(File.file?(File.join(directory, "#{role}.pid")),
			"The #{role} fixture did not start")
	end
end

regression('interrupt cleanup') do |directory|
	status, output = launch(directory, 'interrupt', no_steps,
		server: SERVER, interrupt: true)
	expect_failure(status, output, 'Acceptance run interrupted by SIGINT')
	check(status.exitstatus == 130, "Unexpected interrupt status: #{status}")
end

regression('concurrent isolated native runs') do |directory|
	workers = 2.times.map do |index|
		Thread.new do
			Thread.current.report_on_exception = false
			path = File.join(directory, index.to_s)
			FileUtils.mkdir_p(path)
			begin
				status, output = launch(path, 'wire', wire, '--tags', '@passing')
				check(status.success?, output)
				output[/Listening on 127\.0\.0\.1:(\d+)/, 1]
			ensure
				cleanup(path)
			end
		end
	end
	# Join every worker before propagating errors so its cleanup always finishes.
	workers.each do |worker|
		worker.join
	rescue StandardError
	end
	ports = workers.map(&:value)
	check(ports.compact.uniq.length == 2, 'Concurrent runs shared a port')
end

regression('unit runner rejects empty cases and permits discovery') do |directory|
	status, output = run_command(directory, [UNIT_EMPTY])
	expect_failure(status, output, 'Unit suite contains no cases')
	status, output = run_command(directory, [UNIT_EMPTY, '--gtest_list_tests'])
	check(status.success?, output)
	status, output = run_command(directory, [UNIT_CASES])
	check(status.success?, output)
	check(output.include?('RunnerFixture.ExecutesCase'), output)
end

regression('CTest distinguishes empty and populated unit discovery') do |directory|
	# Exercise the project's discovery include in an isolated CMake fixture.
	# No fixture cases are ever registered in the product build directories.
	File.write(File.join(directory, 'CMakeLists.txt'), <<~CMAKE_FILE)
		cmake_minimum_required(VERSION 3.24)
		project(unit_discovery_fixture LANGUAGES NONE)
		enable_testing()
		add_executable(unit_tests IMPORTED)
		set_target_properties(unit_tests PROPERTIES
			IMPORTED_LOCATION "${FIXTURE_EXECUTABLE}")
		set(CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE PRE_TEST)
		include("#{File.join(ROOT, 'tests/unit/register.cmake')}")
	CMAKE_FILE
	presets = {'version' => 5, 'configurePresets' => []}
	{'empty' => UNIT_EMPTY, 'populated' => UNIT_CASES}.each do |name, executable|
		presets['configurePresets'] << {
			'name' => name, 'generator' => GENERATOR,
			'binaryDir' => "${sourceDir}/#{name}",
			'cacheVariables' => {'FIXTURE_EXECUTABLE' => executable}
		}
	end
	File.write(File.join(directory, 'CMakePresets.json'),
		JSON.pretty_generate(presets))
	%w[empty populated].each do |name|
		status, output = run_command(directory,
			[CMAKE, '--preset', name, '-S', directory])
		check(status.success?, output)
		status, output = run_command(directory,
			[CTEST, '--test-dir', File.join(directory, name),
				'-C', 'Debug', '--output-on-failure', '-L', 'unit',
				'--no-tests=error'])
		if name == 'empty'
			expect_failure(status, output, 'Unit suite contains no cases')
		else
			check(status.success?, output)
			check(output.include?('RunnerFixture.ExecutesCase'), output)
			check(!output.include?('unit_empty'), output)
		end
	end
end

regression('CMake checks Bundler through the selected Ruby') do |directory|
	# An extensionless script without execute permission also reproduces the
	# Windows requirement for an explicit interpreter on POSIX hosts.
	bundler = File.join(directory, 'bundle script')
	File.write(bundler, <<~RUBY_SCRIPT)
		abort 'Wrong arguments' unless ARGV == ['check']
		abort 'Wrong working directory' unless Dir.pwd == #{directory.dump}
		abort 'Wrong Gemfile' unless
			ENV['BUNDLE_GEMFILE'] == #{File.join(directory, 'Gemfile').dump}
		abort 'Bundle is not frozen' unless ENV['BUNDLE_FROZEN'] == 'true'
		abort 'Fixture gems are unavailable' if File.exist?('missing-gems')
		puts 'Fixture bundle checked'
	RUBY_SCRIPT
	File.write(File.join(directory, 'check.cmake'), <<~CMAKE_FILE)
		set(PROJECT_SOURCE_DIR "#{directory}")
		set(RUBY_EXECUTABLE "#{RbConfig.ruby.tr('\\', '/')}")
		set(BUNDLER_EXECUTABLE "#{bundler}")
		include("#{File.join(ROOT, 'cmake/AcceptancePrerequisites.cmake')}")
		message(STATUS "${bundle_output}")
	CMAKE_FILE
	command = [CMAKE, '-P', File.join(directory, 'check.cmake')]
	status, output = run_command(directory, command)
	check(status.success?, output)
	check(output.include?('Fixture bundle checked'), output)
	File.write(File.join(directory, 'missing-gems'), '')
	status, output = run_command(directory, command)
	expect_failure(status, output, 'Acceptance test gems are unavailable')
	check(output.include?('Fixture gems are unavailable'), output)
end

puts 'All runner infrastructure regressions passed.'
