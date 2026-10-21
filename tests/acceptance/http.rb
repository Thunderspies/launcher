require 'socket'

root = File.realpath(ARGV.fetch(0))
server = TCPServer.new('127.0.0.1', 0)
$stdout.sync = true
puts "Listening on 127.0.0.1:#{server.addr[1]}"

loop do
	client = server.accept
	request = client.gets
	while (header = client.gets) && header != "\r\n"
	end
	path = request&.split(' ')&.fetch(1, '')&.delete_prefix('/')
	valid = path && path.match?(%r{\A[a-zA-Z0-9_./-]+\z}) &&
		!path.split('/').include?('..')
	file = valid && File.join(root, path)
	found = file && File.file?(file)
	body = found ? File.binread(file) : 'missing'
	status = found ? '200 OK' : '404 Not Found'
	client.write("HTTP/1.1 #{status}\r\n" \
		"Content-Length: #{body.bytesize}\r\n" \
		"Content-Encoding: identity\r\nConnection: close\r\n\r\n")
	client.write(body)
rescue Errno::EPIPE, Errno::ECONNRESET
ensure
	client&.close
end
