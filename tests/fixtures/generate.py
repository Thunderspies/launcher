#!/usr/bin/env python3
"""Generate launcher interoperability fixtures with the publisher API."""

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zlib


def main():
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--publisher", type=Path,
		default=Path(__file__).resolve().parents[3] / "publisher")
	args = parser.parse_args()
	sys.path.insert(0, str(args.publisher.resolve()))
	from publisher import Repository, FileRepository, FileGroup, Release

	output = Path(__file__).resolve().parent
	with tempfile.TemporaryDirectory(prefix="launcher-fixtures-") as work:
		work = Path(work)
		source = work / "source"
		source.mkdir()
		files = FileRepository.initialize(work / "file-repository")
		repo = Repository.initialize(work / "repository")

		def capture(name, data, kind="hogg", **kwargs):
			path = source / name
			path.parent.mkdir(parents=True, exist_ok=True)
			path.write_bytes(data)
			return files.capture(path, canonical_name=name, kind=kind,
				**kwargs)

		def release(name, hogg, loose):
			groups = {"assets": FileGroup(name="assets", kind="hogg",
				hogg_path="piggs/assets.hogg", fingerprints=tuple(hogg))}
			if loose:
				groups["loose"] = FileGroup(name="loose", kind="loose",
					fingerprints=tuple(loose))
			selected = Release(release=name, groups=groups)
			repo.publish(selected, metadata=[files.record(fp)
				for fp in [*hogg, *loose]])
			repo.pin_channel(name if name not in ("r1", "r2") else
				("live" if name == "r1" else "next"), release=name)

		a = capture("assets/data.bin", b"abc")
		d = capture("data.bin", b"abc", "loose")
		g = capture("game.exe", b"game-one\n", "loose")
		release("r1", [a], [d, g])
		repo.pin_channel("rollback", release="r1")
		a2 = capture("assets/data.bin", b"new-data\n")
		g2 = capture("game.exe", b"game-two\n", "loose")
		info = capture("docs/info.txt", b"new-data\n", "loose")
		release("r2", [a2], [info, g2])
		release("empty", [], [])
		binary = bytes(range(256)) + b"\x00\xff\r\n"
		release("binary", [capture("zero.bin", b""),
			capture("binary/payload.bin", binary)],
			[capture("loose-zero.bin", b"", "loose"),
			capture("loose-binary.bin", binary, "loose")])
		compressed = b"a" * 4096
		encoded = zlib.compress(compressed, 6)
		hogg = []
		for name, data in (("a.bin", compressed), ("b.bin", compressed),
			("encoded.bin", encoded), ("empty.bin", b""),
			("bytes.bin", bytes(range(256)))):
			hogg.append(capture(name, data))
		loose = [capture("stored.bin", encoded, "loose")]
		release("compressed", hogg, loose)
		for release_name, storage in (("headers", "inline"),
			("headerobjects", "object")):
			with_headers = list(hogg)
			for i, (name, header) in enumerate((("a.bin", b"\0\xffhead\0"),
				("b.bin", b"other"))):
				with_headers[i] = capture(name, compressed,
					cached_header=header, header_storage=storage)
			release(release_name, with_headers, loose)
		release("unicode", [capture("Ä/cafÉ.bin", b"unicode data")], [])
		for name, directory in (("public", repo.root),
			("file-public", files.root)):
			target = output / name
			if target.exists():
				shutil.rmtree(target)
			shutil.copytree(directory, target)
	producer = subprocess.check_output(
		["git", "-C", str(args.publisher), "rev-parse", "HEAD"],
		text=True).strip()
	(output / "producer.json").write_text(json.dumps({
		"repository": "../publisher", "revision": producer,
		"generator": "python3 tests/fixtures/generate.py",
	}, indent=2) + "\n")


if __name__ == "__main__":
	main()
