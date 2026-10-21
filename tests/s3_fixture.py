#!/usr/bin/env python3
"""Local HTTPS S3 fixture with an independent AWS Signature V4 verifier."""

from datetime import datetime, timezone
from hashlib import sha256
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import quote, unquote, urlsplit
import hmac
import ipaddress
import re
import socket
import ssl
import sys
import threading


ACCESS_KEY = "fixture-access-key"
SECRET_KEY = "fixture-secret-key"
SESSION_TOKEN = "fixture-session-token"
PREFIX = "/bucket/prefix space/"
FIXTURES = Path(__file__).resolve().parent / "fixtures"


def signature_error(request, region, token, access_key=ACCESS_KEY,
	secret_key=SECRET_KEY):
	"""Return why authentication fails, without using AWS implementation code."""
	authorization = request.headers.get("Authorization", "")
	match = re.fullmatch(r"AWS4-HMAC-SHA256 Credential=([^,]+),\s*"
		r"SignedHeaders=([^,]+),\s*Signature=([a-f0-9]{64})", authorization)
	if not match:
		return "Missing or malformed authorization"
	credential, signed_headers, signature = match.groups()
	scope = credential.split("/")
	if len(scope) != 5:
		return "Invalid credential scope"
	access, date, signing_region, service, terminator = scope
	if access != access_key or service != "s3" or terminator != "aws4_request":
		return "Invalid credential scope"
	if region and signing_region != region:
		return "Wrong signing region"
	stamp = request.headers.get("x-amz-date", "")
	try:
		instant = datetime.strptime(stamp, "%Y%m%dT%H%M%SZ").replace(
			tzinfo=timezone.utc)
	except ValueError:
		return "Invalid signing date"
	if date != stamp[:8] or abs((datetime.now(timezone.utc) - instant)
		.total_seconds()) > 300:
		return "Expired signing date"
	if request.headers.get("x-amz-security-token") != token:
		return "Wrong session token"
	names = signed_headers.split(";")
	if names != sorted(set(names)) or not {"host", "x-amz-date"} <= set(names):
		return "Invalid signed headers"
	if token and "x-amz-security-token" not in names:
		return "Unsigned session token"
	canonical_headers = ""
	for name in names:
		if name.lower() != name or name not in request.headers:
			return "Missing signed header"
		value = " ".join(request.headers[name].strip().split())
		canonical_headers += f"{name}:{value}\n"
	target = urlsplit(request.path)
	if target.query:
		return "Unexpected query"
	payload = request.headers.get("x-amz-content-sha256", sha256(b"").hexdigest())
	if payload not in (sha256(b"").hexdigest(), "UNSIGNED-PAYLOAD"):
		return "Invalid empty GET payload digest"
	# S3 retains encoded separators and does not normalize its object paths.
	canonical_uri = quote(target.path, safe="/%-_.~")
	canonical = "\n".join(("GET", canonical_uri, "", canonical_headers,
		signed_headers, payload))
	credential_scope = "/".join(scope[1:])
	to_sign = "\n".join(("AWS4-HMAC-SHA256", stamp, credential_scope,
		sha256(canonical.encode()).hexdigest()))
	key = ("AWS4" + secret_key).encode()
	for value in (date, signing_region, service, terminator):
		key = hmac.new(key, value.encode(), sha256).digest()
	expected = hmac.new(key, to_sign.encode(), sha256).hexdigest()
	if not hmac.compare_digest(signature, expected):
		return "Signature mismatch"
	return None


class S3Handler(BaseHTTPRequestHandler):
	def log_message(self, *_args):
		pass

	def do_GET(self):
		fixture = self.server.fixture
		error = signature_error(self, fixture.region, fixture.token,
			fixture.access_key, fixture.secret_key)
		path = unquote(urlsplit(self.path).path)
		fixture.requests.append((path, error))
		if error:
			fixture.failures.append(error)
			self.reply(403, b"Access denied")
			return
		if not path.startswith(PREFIX):
			self.reply(404, b"Unknown bucket or prefix")
			return
		relative = path[len(PREFIX):]
		if ".." in relative.split("/"):
			self.reply(400, b"Invalid path")
			return
		if relative in fixture.responses:
			self.reply(*fixture.responses[relative])
			return
		file = fixture.root / relative
		if not file.is_file():
			self.reply(404, b"Missing object")
			return
		self.reply(200, file.read_bytes())

	def reply(self, status, body, headers=None, declared_size=None):
		self.send_response(status)
		self.send_header("Content-Length", str(len(body) if declared_size is None
			else declared_size))
		self.send_header("Connection", "close")
		for name, value in (headers or {}).items():
			self.send_header(name, value)
		self.end_headers()
		try:
			self.wfile.write(body)
		except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
			pass


class S3Fixture:
	def __init__(self, root, region="auto", token=SESSION_TOKEN,
		access_key=ACCESS_KEY, secret_key=SECRET_KEY):
		self.root = Path(root)
		self.access_key = access_key
		self.secret_key = secret_key
		self.region = region
		self.token = token
		self.requests = []
		self.failures = []
		self.responses = {}
		# CRT uses OS DNS; localhost may map to another loopback address.
		addresses = socket.getaddrinfo("localhost", 0, socket.AF_INET,
			socket.SOCK_STREAM)
		host = next(address[4][0] for address in addresses
			if ipaddress.ip_address(address[4][0]).is_loopback)
		self.server = ThreadingHTTPServer((host, 0), S3Handler)
		self.server.fixture = self
		context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
		context.minimum_version = ssl.TLSVersion.TLSv1_2
		context.load_cert_chain(FIXTURES / "localhost-cert.pem",
			FIXTURES / "localhost-key.pem")
		self.server.socket = context.wrap_socket(self.server.socket,
			server_side=True)
		self.worker = None

	@property
	def url(self):
		return f"https://localhost:{self.server.server_port}" + quote(PREFIX)

	def __enter__(self):
		self.worker = threading.Thread(target=self.server.serve_forever,
			daemon=True)
		self.worker.start()
		return self

	def __exit__(self, *_args):
		self.server.shutdown()
		self.server.server_close()
		self.worker.join()


if __name__ == "__main__":
	with S3Fixture(sys.argv[1]) as fixture:
		host, port = fixture.server.server_address
		print(f"Listening on {host}:{port}", flush=True)
		fixture.worker.join()
