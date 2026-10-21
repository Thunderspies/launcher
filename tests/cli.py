#!/usr/bin/env python3
"""Exercise command parsing and saved configuration through the real CLI."""

from pathlib import Path
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import os
import signal
import threading
import time
import json
import subprocess
import sys
import tempfile


def run(*args):
	return subprocess.run([*sys.argv[1:], *args], text=True,
		encoding="utf-8", capture_output=True, check=False)


help_result = run("--help")
assert help_result.returncode == 0, help_result.stderr
assert "configure" in help_result.stdout
assert "repair" in help_result.stdout
assert "--progress" in help_result.stdout
assert run("status", "--install", "/tmp/unused-launcher-progress",
	"--progress", "invalid").returncode == 2
assert "--s3-region" in help_result.stdout
assert run("unknown").returncode != 0
assert run("configure", "--install").returncode != 0

with tempfile.TemporaryDirectory(prefix="launcher-cli-") as temporary:
	install = str(Path(temporary) / "игра-é-ゲーム")
	game_args = ["café", "Игровой мир", "設定"]
	result = run("configure", "--install", install,
		"--repo", "http://127.0.0.1:1/", "--game", "game.exe",
		"--runner", "wine", "--runner-arg", "--example",
		"--game-arg", game_args[0], "--game-arg", game_args[1],
		"--game-arg", game_args[2])
	assert result.returncode == 0, result.stderr
	settings = json.loads(Path(install + ".launcher/config.json").read_text(
		encoding="utf-8"))
	assert settings["game_args"] == game_args, settings
	result = run("status", "--install", install)
	assert result.returncode == 0, result.stderr
	assert "http://127.0.0.1:1/" in result.stdout
	assert "live" in result.stdout
	assert "game.exe" in result.stdout
	assert "none" in result.stdout.lower()
	result = run("configure", "--install", install, "--channel", "next")
	assert result.returncode == 0, result.stderr
	result = run("status", "--install", install)
	assert result.returncode == 0, result.stderr
	assert "next" in result.stdout
	assert "http://127.0.0.1:1/" in result.stdout
	assert run("repair", "--install", install).returncode != 0
	assert run("launch", "--install", install).returncode != 0
	result = run("configure", "--install", install,
		"--repo", "https://example.invalid/bucket/prefix/",
		"--s3-region", "auto")
	assert result.returncode == 0, result.stderr
	settings = json.loads(Path(install + ".launcher/config.json").read_text(
		encoding="utf-8"))
	assert settings["format"] == 3, settings
	assert settings["s3_region"] == "auto", settings
	result = run("configure", "--install", install, "--channel", "live")
	assert result.returncode == 0, result.stderr
	settings = json.loads(Path(install + ".launcher/config.json").read_text(
		encoding="utf-8"))
	assert settings["s3_region"] == "auto", settings
	result = run("configure", "--install", install, "--s3-region", "")
	assert result.returncode == 0, result.stderr
	settings = json.loads(Path(install + ".launcher/config.json").read_text(
		encoding="utf-8"))
	assert settings["format"] == 3, settings
	assert settings["s3_region"] == "", settings

	result = run("configure", "--install", install,
		"--file-repo", "http://example.invalid/public",
		"--file-repo", "https://example.invalid/private",
		"--file-s3-region", "auto")
	assert result.returncode == 0, result.stderr
	settings = json.loads(Path(install + ".launcher/config.json").read_text())
	assert settings["file_repositories"] == [
		{"base_url": "http://example.invalid/public/", "s3_region": ""},
		{"base_url": "https://example.invalid/private/", "s3_region": "auto"}]
	result = run("status", "--install", install)
	assert result.returncode == 0, result.stderr
	assert result.stdout.index("public/") < result.stdout.index("private/")
	assert "File S3 region 2: auto" in result.stdout
	result = run("configure", "--install", install, "--channel", "next")
	assert result.returncode == 0, result.stderr
	assert json.loads(Path(install + ".launcher/config.json").read_text())[ \
		"file_repositories"] == settings["file_repositories"]
	assert run("configure", "--install", install,
		"--file-s3-region", "auto").returncode != 0
	result = run("configure", "--install", install, "--clear-file-repos")
	assert result.returncode == 0, result.stderr
	assert json.loads(Path(install + ".launcher/config.json").read_text())[ \
		"file_repositories"] == []


class ProgressHandler(SimpleHTTPRequestHandler):
	def log_message(self, *_args):
		pass

	def do_GET(self):
		if self.path.endswith("/BLOB") and self.server.delay:
			self.server.started.set()
			time.sleep(2)
		try:
			super().do_GET()
		except (BrokenPipeError, ConnectionResetError):
			pass


fixtures = Path(__file__).resolve().parent / "fixtures"
handler = partial(ProgressHandler, directory=str(fixtures))
server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
server.delay = False
server.started = threading.Event()
worker = threading.Thread(target=server.serve_forever, daemon=True)
worker.start()
try:
	with tempfile.TemporaryDirectory(prefix="launcher-progress-") as temporary:
		base = Path(temporary)
		url = f"http://127.0.0.1:{server.server_port}/"
		for mode in ("auto", "always", "never"):
			install = str(base / mode)
			result = run("configure", "--install", install,
				"--repo", url + "public/", "--file-repo", url + "file-public/")
			assert result.returncode == 0, result.stderr
			result = run("update", "--install", install,
				"--progress", mode)
			assert result.returncode == 0, result.stderr
			assert "update completed" in result.stdout
			if mode == "always":
				assert "Resolving" in result.stderr, result.stderr
				assert "Downloading" in result.stderr, result.stderr
				assert "Activating" in result.stderr, result.stderr
				assert "\r" not in result.stderr
				assert len(result.stderr.splitlines()) < 30, result.stderr
			else:
				assert not result.stderr, result.stderr
		if os.name == "posix":
			install = str(base / "interrupted")
			result = run("configure", "--install", install,
				"--repo", url + "public/", "--file-repo", url + "file-public/")
			assert result.returncode == 0, result.stderr
			server.delay = True
			process = subprocess.Popen([*sys.argv[1:], "update", "--install",
				install, "--progress", "never"], text=True,
				stdout=subprocess.PIPE, stderr=subprocess.PIPE)
			try:
				assert server.started.wait(10), "No download started"
				process.send_signal(signal.SIGINT)
				stdout, stderr = process.communicate(timeout=15)
				assert process.returncode == 130, (stdout, stderr)
				assert "cancel" in stderr.lower(), stderr
				assert not Path(install, "game.exe").exists()
				assert not Path(install + ".launcher/installed-state.json").exists()
			finally:
				server.delay = False
				if process.poll() is None:
					process.kill()
					process.wait(timeout=5)
			result = run("update", "--install", install)
			assert result.returncode == 0, result.stderr
finally:
	server.shutdown()
	server.server_close()
	worker.join()
