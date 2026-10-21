#!/usr/bin/env python3
"""Interrupt real forward-update syscalls and verify frozen offline recovery."""

from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading


class QuietHandler(SimpleHTTPRequestHandler):
	def log_message(self, *_args):
		pass


def run(install, command, *args, environment=None):
	return subprocess.run([sys.argv[1], command, "--install", str(install),
		*args], text=True, capture_output=True, check=False,
		env=environment, timeout=30)


def expect_success(result):
	assert result.returncode == 0, (result.returncode, result.stderr)


def check_active(install, release="r2"):
	if release == "r2":
		assert (install / "game.exe").read_bytes() == b"game-two\n"
		assert not (install / "data.bin").exists()
		assert (install / "docs/info.txt").read_bytes() == b"new-data\n"
	else:
		assert (install / "game.exe").read_bytes() == b"game-one\n"
		assert (install / "data.bin").read_bytes() == b"abc"
		assert not (install / "docs/info.txt").exists()
	assert (install / "saves/personal").read_bytes() == b"personal"


def add_large_asset(public, release, payload):
	# Raw 128 KiB replacements leave enough obsolete payload to cross the
	# automatic 20 percent reclamation threshold without fixture-wide changes.
	digest = hashlib.sha256(payload).hexdigest()
	metadata = {"format": 1, "name": "assets/large.bin", "kind": "hogg",
		"size": len(payload), "stored_size": len(payload), "encoding": "raw",
		"sha256": digest, "logical_md5": hashlib.md5(payload).hexdigest()}
	encoded = (json.dumps(metadata, sort_keys=True, indent=2) + "\n").encode()
	fingerprint = hashlib.sha256(encoded).hexdigest()
	blob = public / "hogg" / digest / "BLOB"
	blob.parent.mkdir(parents=True, exist_ok=True)
	blob.write_bytes(payload)
	(public / "revisions" / f"{fingerprint}.json").write_bytes(encoded)
	path = public / "releases" / f"{release}.json"
	manifest = json.loads(path.read_bytes())
	manifest["groups"]["assets"]["files"].append(fingerprint)
	manifest["groups"]["assets"]["files"].sort()
	path.write_text(json.dumps(manifest, sort_keys=True, indent=2) + "\n")


def selected_metadata(public, release):
	manifest = json.loads((public / "releases" / f"{release}.json").read_bytes())
	fingerprints = list(manifest["loose"])
	for group in manifest["groups"].values():
		fingerprints.extend(group["files"])
	return {fingerprint: json.loads((public / "revisions" /
		f"{fingerprint}.json").read_bytes()) for fingerprint in fingerprints}


def check_prefetched(public, private):
	before = selected_metadata(public, "r1")
	target = selected_metadata(public, "r2")
	for fingerprint, metadata in target.items():
		if fingerprint in before:
			continue
		cached = private / "objects" / metadata["sha256"]
		blob = public / metadata["kind"] / metadata["sha256"] / "BLOB"
		assert cached.read_bytes() == blob.read_bytes(), fingerprint


def exercise(mode):
	fixture = Path(__file__).resolve().parents[1] / "fixtures/public"
	with tempfile.TemporaryDirectory(prefix="launcher-fault-") as temporary:
		base = Path(temporary)
		public = base / "public"
		shutil.copytree(fixture, public)
		shutil.copytree(fixture.parent / "file-public", public,
			dirs_exist_ok=True)
		add_large_asset(public, "r1", bytes(range(256)) * 512)
		add_large_asset(public, "r2", bytes(reversed(range(256))) * 512)
		server = ThreadingHTTPServer(("127.0.0.1", 0),
			partial(QuietHandler, directory=str(public)))
		worker = threading.Thread(target=server.serve_forever, daemon=True)
		worker.start()
		install = base / "game"
		private = base / "game.launcher"
		archive = install / "piggs/assets.hogg"
		try:
			endpoint = f"http://127.0.0.1:{server.server_port}/"
			expect_success(run(install, "configure", "--repo", endpoint,
				"--file-repo", endpoint))
			expect_success(run(install, "update"))
			original_state = (private / "installed-state.json").read_bytes()
			original_archive = archive.read_bytes()
			(install / "saves").mkdir()
			(install / "saves/personal").write_bytes(b"personal")
			(public / "channels/live.json").write_text(
				'{"format":1,"release":"r2"}\n')
			log = base / "fault.log"
			environment = os.environ.copy()
			environment.update({
				"LD_PRELOAD": str(Path(sys.argv[2]).resolve()),
				"LAUNCHER_FAULT_MODE": mode,
				"LAUNCHER_FAULT_LOG": str(log),
				"LAUNCHER_FAULT_PRIVATE_ROOT": str(private),
				"LAUNCHER_FAULT_ARCHIVE": str(archive),
			})
			if mode == "missing-payload":
				metadata = next(value for value in selected_metadata(
					public, "r2").values() if value["name"] ==
					"assets/large.bin")
				(public / "hogg" / metadata["sha256"] / "BLOB").unlink()
				result = run(install, "update")
				assert result.returncode == 1, result
				assert archive.read_bytes() == original_archive
				assert (private / "installed-state.json").read_bytes() == (
					original_state)
				assert not (private / "journal.json").exists()
				check_active(install, "r1")
				print("missing-payload: no installed bytes changed")
				return
			if mode == "low-space":
				result = subprocess.run([sys.argv[3], str(install)],
					text=True, capture_output=True, check=False,
					env=environment, timeout=30)
				expect_success(result)
			else:
				result = run(install, "update", environment=environment)
			trace = log.read_text()
			assert "journal-published\n" in trace, trace
			check_prefetched(public, private)
			assert not (private / "transaction/backups").exists()
			if mode in ("journal-kill", "entry-kill", "cleanup-kill",
				"compact-kill"):
				assert result.returncode == 86, result
			elif mode == "read-state-once":
				expect_success(result)
				assert "failed-state-read\n" not in trace, trace
			elif mode == "fsync-state":
				assert result.returncode == 3, result
				assert "committed" in result.stderr, result.stderr
				assert "failed-private-fsync\n" in trace, trace
				assert (private / "journal.json").is_file()
				environment["LAUNCHER_FAULT_ARMED"] = "1"
				blocked = run(install, "verify", environment=environment)
				assert blocked.returncode == 1, blocked
				assert "recovery-required" in blocked.stderr, blocked.stderr
				assert (private / "journal.json").is_file(), (
					"Undurable selected state lost its recovery journal")
			elif mode == "compact-flush":
				expect_success(result)
				assert "warning" in result.stderr, result.stderr
				assert "failed-compaction-flush\n" in trace, trace
			elif mode == "compact-corrupt":
				assert result.returncode == 1, result
				assert "integrity" in result.stderr, result.stderr
				assert "corrupted-compacted-canonical\n" in trace, trace
			if mode == "journal-kill":
				assert archive.read_bytes() == original_archive
				check_active(install, "r1")
			if mode == "entry-kill":
				assert "flushed-archive-entry\n" in trace, trace
			if mode == "low-space":
				assert "failed-archive-write\n" in trace, trace
			if mode == "cleanup-kill":
				assert "cleanup-removed-transaction\n" in trace, trace
			if mode.startswith("compact-"):
				assert "compacted-archive-renamed\n" in trace, trace
			# The channel deliberately moves away from the pending target.
			(public / "channels/live.json").write_text(
				'{"format":1,"release":"r1"}\n')
		finally:
			server.shutdown()
			server.server_close()
			worker.join()
		# Recovery uses the journal's exact descriptors and prefetched objects.
		shutil.rmtree(private / "files", ignore_errors=True)
		if mode == "compact-corrupt":
			result = run(install, "verify")
			assert result.returncode == 1, result
			assert "integrity" in result.stderr, result.stderr
			expect_success(run(install, "repair"))
		expect_success(run(install, "verify"))
		check_active(install)
		manifest = json.loads(json.loads((private /
			"installed-state.json").read_bytes())["manifest"])
		assert manifest["release"] == "r2"
		assert not (private / "journal.json").exists()
		assert not (private / "transaction").exists()
		assert not (private / "compact.hogg").exists()
		print(f"{mode}: verified frozen r2 offline")


modes = ("missing-payload", "journal-kill", "entry-kill", "low-space",
	"read-state-once", "fsync-state", "cleanup-kill", "compact-flush",
	"compact-corrupt", "compact-kill")
for selected_mode in sys.argv[4:] or modes:
	exercise(selected_mode)
