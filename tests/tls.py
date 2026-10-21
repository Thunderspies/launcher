#!/usr/bin/env python3
"""Verify real cURL TLS trust and hostname checking against a local host."""

from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import os
import ssl
import subprocess
import sys
import tempfile
import threading


class QuietHandler(SimpleHTTPRequestHandler):
	def log_message(self, *_args):
		pass


fixture = Path(__file__).resolve().parent / "fixtures"
handler = partial(QuietHandler, directory=str(fixture))
server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.minimum_version = ssl.TLSVersion.TLSv1_2
context.load_cert_chain(fixture / "localhost-cert.pem",
	fixture / "localhost-key.pem")
server.socket = context.wrap_socket(server.socket, server_side=True)
worker = threading.Thread(target=server.serve_forever, daemon=True)
worker.start()


def run(*args):
	# Unsigned network operations ignore even malformed AWS credentials.
	return subprocess.run([*sys.argv[1:], *args], text=True,
		capture_output=True, check=False, timeout=30, env={**os.environ,
			"AWS_ACCESS_KEY_ID": "invalid\nkey",
			"AWS_SECRET_ACCESS_KEY": "", "AWS_SESSION_TOKEN": "ignored"})


try:
	with tempfile.TemporaryDirectory(prefix="launcher-tls-") as temporary:
		install = str(Path(temporary) / "game")
		url = f"https://localhost:{server.server_port}/public/"
		result = run("configure", "--install", install, "--repo", url,
			"--file-repo", f"https://localhost:{server.server_port}/file-public/")
		assert result.returncode == 0, result.stderr
		result = run("update", "--install", install)
		assert result.returncode != 0, "Untrusted TLS certificate accepted"
		result = run("configure", "--install", install, "--ca-bundle",
			str(fixture / "localhost-cert.pem"))
		assert result.returncode == 0, result.stderr
		result = run("update", "--install", install)
		assert result.returncode == 0, result.stderr
		result = run("verify", "--install", install)
		assert result.returncode == 0, result.stderr
		url = f"https://127.0.0.1:{server.server_port}/"
		result = run("configure", "--install", install, "--repo", url)
		assert result.returncode == 0, result.stderr
		result = run("update", "--install", install)
		assert result.returncode != 0, "TLS hostname mismatch accepted"
finally:
	server.shutdown()
	server.server_close()
	worker.join()
