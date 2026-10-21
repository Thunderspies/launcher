require 'fiddle/import'

module WindowsJob
	extend Fiddle::Importer
	dlload 'kernel32.dll'
	extern 'void* CreateJobObjectW(void*, void*)', :stdcall
	extern 'void* GetCurrentProcess()', :stdcall
	extern 'int SetInformationJobObject(void*, int, void*, long)', :stdcall
	extern 'int AssignProcessToJobObject(void*, void*)', :stdcall
end

job = WindowsJob.CreateJobObjectW(nil, nil)
abort 'Could not create the child process job.' if job.null?

# JOBOBJECT_EXTENDED_LIMIT_INFORMATION has a DWORD LimitFlags at offset 16.
size = Fiddle::SIZEOF_VOIDP == 8 ? 144 : 112
limits = "\0" * size
limits[16, 4] = [0x2000].pack('L<') # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
if WindowsJob.SetInformationJobObject(job, 9, limits, size).zero?
	abort 'Could not set child process job limits.'
end
if WindowsJob.AssignProcessToJobObject(job,
	WindowsJob.GetCurrentProcess()).zero?
	abort 'Could not join the child process job.'
end

# Descendants inherit the job before they can spawn their own children.
# Keep its non-inheritable handle open until this wrapper exits; closing
# it explicitly would kill the wrapper before it can return the exit status.
# CTRL_BREAK also reaches the child, so let it handle the graceful stop.
Signal.trap('INT') {}
pid = Process.spawn(*ARGV)
_, status = Process.waitpid2(pid)
exit(status.exitstatus || 128 + status.termsig)
