#!/usr/bin/env python3
"""Exercise authenticated updates against independently verified HTTPS S3."""

from pathlib import Path
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True

from s3_fixture import (ACCESS_KEY, SECRET_KEY, SESSION_TOKEN, FIXTURES,
	PREFIX, S3Fixture)


separator = sys.argv.index("--")
probe = sys.argv[1:separator]
command = sys.argv[separator + 1:]
environment = {name: value for name, value in os.environ.items()
	if name not in ("AWS_ACCESS_KEY_ID", "AWS_SECRET_ACCESS_KEY",
		"AWS_SESSION_TOKEN")}
credentials = {"AWS_ACCESS_KEY_ID": ACCESS_KEY,
	"AWS_SECRET_ACCESS_KEY": SECRET_KEY, "AWS_SESSION_TOKEN": SESSION_TOKEN}
credentials.update({"LAUNCHER_FILE_1_" + key: value
	for key, value in list(credentials.items())})


def invoke(arguments, label, timeout, env):
	print(f"S3 {label}: starting", flush=True)
	started = time.monotonic()
	result = subprocess.run(arguments, text=True, encoding="utf-8",
		capture_output=True, check=False, timeout=timeout, env=env)
	print(f"S3 {label}: completed in {time.monotonic() - started:.2f}s",
		flush=True)
	return result


def run(*args, auth=None):
	result = invoke([*command, *args], args[0], 45,
		{**environment, **(auth or {})})
	for secret in (ACCESS_KEY, SECRET_KEY, SESSION_TOKEN):
		assert secret not in result.stdout + result.stderr, result
	return result


def configure(install, server, region="auto", trusted=True):
	args = ["configure", "--install", str(install), "--repo", server.url,
		"--s3-region", region, "--file-repo", server.url,
		"--file-s3-region", region, "--game", "game.exe"]
	if trusted:
		args += ["--ca-bundle", str(FIXTURES / "localhost-cert.pem")]
	result = run(*args)
	assert result.returncode == 0, result.stderr


def named_revision(root, manifest, canonical_name):
	fingerprints = list(manifest["loose"])
	for group in manifest["groups"].values():
		fingerprints.extend(group["files"])
	for fingerprint in fingerprints:
		record = json.loads((root / "revisions" /
			(fingerprint + ".json")).read_text())
		if record["name"] == canonical_name:
			return record
	raise AssertionError("Missing canonical name: " + canonical_name)


def assert_private_state(install):
	for file in Path(str(install) + ".launcher").rglob("*"):
		if file.is_file():
			contents = file.read_bytes()
			for secret in (ACCESS_KEY, SECRET_KEY, SESSION_TOKEN):
				assert secret.encode() not in contents, str(file)


def assert_no_parts(install):
	sidecar = Path(str(install) + ".launcher")
	assert not list(sidecar.rglob("*.part")), list(sidecar.rglob("*.part"))
	assert not (sidecar / "journal.json").exists()


with tempfile.TemporaryDirectory(prefix="launcher-s3-") as temporary:
	base = Path(temporary)
	public = base / "repository"
	shutil.copytree(FIXTURES / "public", public)
	shutil.copytree(FIXTURES / "file-public", public, dirs_exist_ok=True)
	for region, token in (("us-west-2", None), ("auto", SESSION_TOKEN)):
		with S3Fixture(public, region, token) as server:
			install = base / region
			auth = dict(credentials)
			if token is None:
				del auth["AWS_SESSION_TOKEN"]
				del auth["LAUNCHER_FILE_1_AWS_SESSION_TOKEN"]
			configure(install, server, region)
			for incomplete in ({}, {"AWS_ACCESS_KEY_ID": ACCESS_KEY},
				{"AWS_SECRET_ACCESS_KEY": SECRET_KEY},
				{"AWS_SESSION_TOKEN": SESSION_TOKEN},
				{"AWS_ACCESS_KEY_ID": "", "AWS_SECRET_ACCESS_KEY": SECRET_KEY}):
				result = run("update", "--install", str(install), auth=incomplete)
				assert result.returncode != 0, "Missing credentials accepted"
				assert "invalid" in result.stderr.lower(), result.stderr
				assert not server.requests, server.requests
			for offline in ("status", "configure"):
				result = run(offline, "--install", str(install),
					auth={"AWS_SESSION_TOKEN": "incomplete"})
				assert result.returncode == 0, result.stderr
			result = run("update", "--install", str(install), auth=auth)
			assert result.returncode == 0, (result.stderr, server.failures)
			assert (install / "game.exe").read_bytes() == b"game-one\n"
			paths = [path[len(PREFIX):] for path, error in server.requests]
			assert "channels/live.json" in paths, paths
			assert "releases/r1.json" in paths, paths
			assert any(path.endswith("/BLOB") for path in paths), paths
			assert not any(path.startswith("catalog/") for path in paths), paths
			assert not any(path.endswith("/META") for path in paths), paths
			assert not server.failures, server.failures
			assert all(path.startswith(PREFIX) for path, _ in server.requests)
			count = len(server.requests)
			for offline in ("status", "verify", "repair"):
				result = run(offline, "--install", str(install))
				assert result.returncode == 0, result.stderr
			assert len(server.requests) == count, server.requests[count:]
			assert_private_state(install)
			# Reopening does not recover credentials from saved state.
			result = run("update", "--install", str(install))
			assert result.returncode != 0, "Credentials survived reopening"
			assert len(server.requests) == count
			shutil.rmtree(Path(str(install) + ".launcher") / "objects")
			(install / "game.exe").write_bytes(b"damaged")
			result = run("repair", "--install", str(install), auth=auth)
			assert result.returncode == 0, result.stderr
			assert (install / "game.exe").read_bytes() == b"game-one\n"
			# Wrong credentials fail before any installation changes.
			for wrong in ({**auth, "AWS_SECRET_ACCESS_KEY": "wrong"},
				{**auth, "AWS_ACCESS_KEY_ID": "wrong"},
				{**auth, "AWS_SESSION_TOKEN": "wrong"}):
				result = run("update", "--install", str(install), auth=wrong)
				assert result.returncode != 0, "Bad credentials accepted"
				assert "network" in result.stderr.lower(), result.stderr
				assert server.requests[-1][1] is not None
				assert (install / "game.exe").read_bytes() == b"game-one\n"
			assert_no_parts(install)
			assert_private_state(install)

	with S3Fixture(public) as server:
		install = base / "failures"
		configure(install, server, trusted=False)
		result = run("update", "--install", str(install), auth=credentials)
		assert result.returncode != 0, "Untrusted TLS accepted"
		assert not server.requests
		configure(install, server)
		result = run("configure", "--install", str(install), "--repo",
			server.url.replace("localhost", server.server.server_address[0]))
		assert result.returncode == 0, result.stderr
		result = run("update", "--install", str(install), auth=credentials)
		assert result.returncode != 0, "TLS hostname mismatch accepted"
		assert not server.requests
		configure(install, server)
		metadata = (public / "channels/live.json").read_bytes()
		for response in ((302, b"", {"Location": server.url + "redirected"}),
			(206, metadata), (404, b"missing"),
			(200, metadata, {"Content-Encoding": "gzip"}),
			(200, metadata[:-1], {}, len(metadata))):
			server.responses["channels/live.json"] = response
			count = len(server.requests)
			result = run("update", "--install", str(install), auth=credentials)
			assert result.returncode != 0, response
			assert "network" in result.stderr.lower(), result.stderr
			assert len(server.requests) == count + 1, "Redirect followed"
			assert not (install / "game.exe").exists()
			assert_no_parts(install)
		server.responses.clear()
		# The inclusive metadata limit and progress cancellation use native
		# callbacks, including an assertion that callbacks stay on the caller.
		for mode in ("cancel", "metadata-limit", "metadata-excess"):
			if mode.startswith("metadata-"):
				body = metadata.ljust(16 * 1024 * 1024, b" ")
				if mode == "metadata-excess":
					body += b" "
				server.responses["channels/live.json"] = (200, body)
			result = invoke([*probe, mode, str(base / mode),
				server.url, str(FIXTURES / "localhost-cert.pem")],
				mode, 30, environment)
			assert result.returncode == 0, result.stderr
			assert_no_parts(base / mode)
		server.responses.clear()
		for mode in ("rotate-plan", "retain-channel", "clear-region",
			"clear-endpoint", "failed-configure", "clear-credentials",
			"failed-credentials", "file-clear-endpoint", "file-clear-region",
			"file-clear-credentials", "file-rotate-plan"):
			count = len(server.requests)
			result = invoke([*probe, mode, str(base / mode),
				server.url, str(FIXTURES / "localhost-cert.pem")],
				mode, 30, environment)
			assert result.returncode == 0, result.stderr
			if mode.startswith("clear-"):
				assert len(server.requests) == count, server.requests[count:]
			assert_private_state(base / mode)
		assert not server.failures, server.failures
		result = run("update", "--install", str(install), auth=credentials)
		assert result.returncode == 0, result.stderr
		manifest = json.loads((public / "releases/r1.json").read_text())
		record = named_revision(public, manifest, "game.exe")
		digest = record["sha256"]
		blob_path = record["kind"] + "/" + digest + "/BLOB"
		# Corrupt, short, excessive and HTTP-failed object bodies preserve
		# the installed release and remove partial cache objects.
		for response in ((200, b"x" * 9), (200, b"short"),
			(200, b"game-one\nexcess"), (404, b"missing"),
			(200, b"short", {}, 9), (206, b"game-one\n"),
			(200, b"game-one\n", {"Content-Encoding": "gzip"})):
			server.responses[blob_path] = response
			cache = Path(str(install) + ".launcher") / "objects" / digest
			cache.unlink(missing_ok=True)
			(install / "game.exe").write_bytes(b"damaged!!")
			result = run("repair", "--install", str(install), auth=credentials)
			assert result.returncode != 0, response
			assert (install / "game.exe").read_bytes() == b"damaged!!"
			assert_no_parts(install)
		server.responses.clear()
		result = run("repair", "--install", str(install), auth=credentials)
		assert result.returncode == 0, result.stderr
		# Public mode ignores malformed AWS variables and continues using cURL.
		result = run("configure", "--install", str(install), "--s3-region", "")
		assert result.returncode == 0, result.stderr
		result = run("repair", "--install", str(install),
			auth={"AWS_SESSION_TOKEN": "incomplete"})
		assert result.returncode == 0, result.stderr
		assert_private_state(install)

# Separate origins and key sets verify that credentials never spill between
# manifest, public file repositories, and private file repositories.
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import threading


class PublicHandler(SimpleHTTPRequestHandler):
	def log_message(self, *_args):
		pass

	def do_GET(self):
		self.server.seen.append(self.path)
		assert not any(name.lower() == "authorization" or
			name.lower().startswith("x-amz-") for name in self.headers)
		super().do_GET()


with tempfile.TemporaryDirectory(prefix="launcher-split-s3-") as temporary:
	base = Path(temporary)
	shutil.copytree(FIXTURES / "public", base / "public")
	shutil.copytree(FIXTURES / "file-public", base / "files")
	(base / "missing").mkdir()
	empty_files = base / "public/empty-files"
	empty_files.mkdir()
	(empty_files / "repository.json").write_text(
		'{"format":2,"kind":"files"}\n')
	public = ThreadingHTTPServer(("127.0.0.1", 0),
		partial(PublicHandler, directory=str(base)))
	public.seen = []
	worker = threading.Thread(target=public.serve_forever, daemon=True)
	worker.start()
	try:
		with S3Fixture(base / "public") as manifest, \
			S3Fixture(base / "files", "us-west-2", "file-token",
				"file-key", "file-secret") as files:
			url = f"http://127.0.0.1:{public.server_port}/"
			install = base / "game"
			result = run("configure", "--install", str(install),
				"--repo", manifest.url, "--s3-region", "auto",
				"--file-repo", url + "missing/",
				"--file-repo", manifest.url + "empty-files/",
				"--file-s3-region", "auto",
				"--file-repo", files.url, "--file-s3-region", "us-west-2", "--ca-bundle",
				str(FIXTURES / "localhost-cert.pem"))
			assert result.returncode == 0, result.stderr
			manifest_keys = {name: value for name, value in credentials.items()
				if name.startswith("AWS_")}
			result = run("update", "--install", str(install), auth=manifest_keys)
			assert result.returncode != 0, "Implicit file credentials accepted"
			assert not files.requests, files.requests
			keys = {**manifest_keys,
				**{"LAUNCHER_FILE_2_" + name: value
					for name, value in manifest_keys.items()},
				"LAUNCHER_FILE_3_AWS_ACCESS_KEY_ID": "file-key",
				"LAUNCHER_FILE_3_AWS_SECRET_ACCESS_KEY": "file-secret",
				"LAUNCHER_FILE_3_AWS_SESSION_TOKEN": "file-token"}
			result = run("update", "--install", str(install), auth=keys)
			assert result.returncode == 0, (result.stderr, files.failures)
			paths = [path[len(PREFIX):] for path, _ in files.requests]
			assert not any(path.startswith("catalog/") for path in paths), paths
			assert any(path.startswith("revisions/") for path in paths), paths
			assert len([path for path in paths if path.endswith("/BLOB")]) == 2, paths
			assert "/missing/repository.json" in public.seen, public.seen
			assert not any("/revisions/" in path for path in public.seen
				if path.startswith("/missing/")), public.seen
			assert not manifest.failures and not files.failures
			# Reconfigure the manifest to public, retaining private file access.
			result = run("configure", "--install", str(install),
				"--repo", url + "public/", "--s3-region", "")
			assert result.returncode == 0, result.stderr
			result = run("update", "--install", str(install), auth={**keys,
				"AWS_ACCESS_KEY_ID": "ignored\npublic",
				"LAUNCHER_FILE_1_AWS_SESSION_TOKEN": "ignored\npublic"})
			assert result.returncode == 0, result.stderr
			# Fresh-source lookup after reopening must stop on private auth error.
			shutil.rmtree(base / "game.launcher/objects")
			(install / "game.exe").write_bytes(b"damaged!!")
			result = run("repair", "--install", str(install), auth={**keys,
				"LAUNCHER_FILE_3_AWS_SECRET_ACCESS_KEY": "wrong"})
			assert result.returncode != 0, "Bad private file key accepted"
			assert files.requests[-1][1] is not None
			result = run("repair", "--install", str(install), auth=keys)
			assert result.returncode == 0, result.stderr
			assert (install / "game.exe").read_bytes() == b"game-one\n"
			for path in (base / "game.launcher").rglob("*.json"):
				assert "file-secret" not in path.read_text(), path
	finally:
		public.shutdown()
		public.server_close()
		worker.join()
