#!/usr/bin/env python3
# Mock OIDC IdP used by tests/e2e_oauth.sh.
#
# Stdlib-only Python HTTP server + openssl shell-out for RS256
# signing. Deliberately minimal — implements just enough of the OIDC
# Authorization Code + PKCE flow to exercise hull/web/middleware/oauth.
#
# Endpoints:
#   GET  /authorize    - redirect back to redirect_uri with code+state
#   POST /token        - exchange code+verifier for id_token (RS256)
#   GET  /.well-known/jwks.json - serve JWKS with x5c entry
#   GET  /health       - liveness probe for the orchestrator
#
# This is TEST CODE. Do not deploy. The committed key.pem is a
# test-only key with no real-world security value.

import base64
import hashlib
import http.server
import json
import os
import secrets
import socketserver
import subprocess
import sys
import threading
import time
import urllib.parse

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
KEY_PATH = os.path.join(BASE_DIR, "key.pem")
CERT_B64_PATH = os.path.join(BASE_DIR, "cert.b64")
KID_PATH = os.path.join(BASE_DIR, "kid.txt")

with open(CERT_B64_PATH) as f:
    CERT_B64 = f.read().strip()
with open(KID_PATH) as f:
    KID = f.read().strip()

# State carried between /authorize and /token. Keyed by the code we
# issued in /authorize. Cleared on /token redemption (single-use).
PENDING = {}  # code -> { state, nonce, code_challenge, redirect_uri, sub }

# Sample claims the IdP will issue. Tests can override via env vars.
SUB = os.environ.get("MOCK_IDP_SUB", "user-123")
EMAIL = os.environ.get("MOCK_IDP_EMAIL", "alice@example.test")
NAME = os.environ.get("MOCK_IDP_NAME", "Alice Example")
CLIENT_ID = os.environ.get("MOCK_IDP_CLIENT_ID", "test-client")
ISSUER = os.environ.get("MOCK_IDP_ISSUER")  # set after bind by parent


def b64url(b):
    return base64.urlsafe_b64encode(b).rstrip(b"=").decode("ascii")


def sign_rs256(message_bytes):
    """RS256 signature using openssl dgst, returning raw signature bytes."""
    p = subprocess.run(
        ["openssl", "dgst", "-sha256", "-sign", KEY_PATH],
        input=message_bytes, capture_output=True, check=True)
    return p.stdout


def make_id_token(sub, aud, nonce, extra=None):
    """Build + sign an RS256 ID token. exp = now + 1h, iat = now."""
    header = {"alg": "RS256", "kid": KID, "typ": "JWT"}
    now = int(time.time())
    claims = {
        "iss": ISSUER, "sub": sub, "aud": aud,
        "iat": now, "exp": now + 3600,
        "nonce": nonce, "email": EMAIL, "name": NAME,
    }
    if extra:
        claims.update(extra)
    header_b64 = b64url(json.dumps(header, separators=(",", ":")).encode())
    claims_b64 = b64url(json.dumps(claims, separators=(",", ":")).encode())
    signing_input = (header_b64 + "." + claims_b64).encode("ascii")
    sig = sign_rs256(signing_input)
    return header_b64 + "." + claims_b64 + "." + b64url(sig)


class Handler(http.server.BaseHTTPRequestHandler):
    # Quiet stderr - we only care about pass/fail at the orchestrator.
    def log_message(self, *_args):
        pass

    def _send_json(self, status, payload):
        body = json.dumps(payload).encode()
        # Surface 4xx/5xx on stderr so test failures show the IdP's
        # reason for rejection (pkce_mismatch, missing_grant_type, ...)
        if status >= 400:
            sys.stderr.write("MOCK_IDP " + self.path + " -> " +
                             str(status) + " " + json.dumps(payload) + "\n")
            sys.stderr.flush()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_redirect(self, location):
        self.send_response(302)
        self.send_header("Location", location)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        params = dict(urllib.parse.parse_qsl(url.query))

        if url.path == "/health":
            self._send_json(200, {"ok": True})
            return

        if url.path == "/.well-known/jwks.json":
            self._send_json(200, {
                "keys": [{
                    "kty": "RSA",
                    "use": "sig",
                    "alg": "RS256",
                    "kid": KID,
                    "x5c": [CERT_B64],
                    # x5t and x5t#S256 are optional; some libraries
                    # require them — include x5t#S256 for completeness.
                    "x5t#S256": b64url(hashlib.sha256(
                        base64.b64decode(CERT_B64)).digest()),
                }]})
            return

        if url.path == "/authorize":
            # Required: client_id, redirect_uri, response_type=code,
            #           scope, state, nonce, code_challenge,
            #           code_challenge_method=S256
            for required in ("client_id", "redirect_uri", "response_type",
                             "state", "nonce", "code_challenge",
                             "code_challenge_method"):
                if required not in params:
                    self._send_json(400, {"error": "missing_" + required})
                    return
            if params["response_type"] != "code":
                self._send_json(400, {"error": "unsupported_response_type"})
                return
            if params["code_challenge_method"] != "S256":
                self._send_json(400, {"error": "pkce_method_must_be_s256"})
                return
            code = secrets.token_urlsafe(24)
            PENDING[code] = {
                "state": params["state"],
                "nonce": params["nonce"],
                "code_challenge": params["code_challenge"],
                "redirect_uri": params["redirect_uri"],
                "client_id": params["client_id"],
                "sub": SUB,
            }
            qs = urllib.parse.urlencode({
                "code": code, "state": params["state"]})
            sep = "&" if "?" in params["redirect_uri"] else "?"
            self._send_redirect(params["redirect_uri"] + sep + qs)
            return

        self._send_json(404, {"error": "not_found"})

    def do_POST(self):
        url = urllib.parse.urlparse(self.path)
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length).decode("utf-8") if length else ""

        if url.path == "/token":
            params = dict(urllib.parse.parse_qsl(body))
            for required in ("grant_type", "code", "redirect_uri",
                             "client_id", "code_verifier"):
                if required not in params:
                    self._send_json(400, {"error": "missing_" + required})
                    return
            if params["grant_type"] != "authorization_code":
                self._send_json(400, {"error": "unsupported_grant_type"})
                return
            entry = PENDING.pop(params["code"], None)
            if not entry:
                self._send_json(400, {"error": "invalid_code"})
                return
            if entry["redirect_uri"] != params["redirect_uri"]:
                self._send_json(400, {"error": "redirect_uri_mismatch"})
                return
            if entry["client_id"] != params["client_id"]:
                self._send_json(400, {"error": "client_id_mismatch"})
                return
            # Verify PKCE: SHA-256(verifier) base64url == code_challenge.
            expected = b64url(hashlib.sha256(
                params["code_verifier"].encode()).digest())
            if expected != entry["code_challenge"]:
                self._send_json(400, {"error": "pkce_mismatch"})
                return
            id_token = make_id_token(
                entry["sub"], entry["client_id"], entry["nonce"])
            self._send_json(200, {
                "access_token": "fake-access-token-" + secrets.token_urlsafe(8),
                "token_type": "Bearer",
                "expires_in": 3600,
                "id_token": id_token,
                "scope": "openid profile email",
            })
            return

        self._send_json(404, {"error": "not_found"})


class ReusableServer(socketserver.ThreadingMixIn,
                     http.server.HTTPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    port = int(os.environ.get("MOCK_IDP_PORT", "0"))
    host = "127.0.0.1"
    server = ReusableServer((host, port), Handler)
    actual_port = server.server_address[1]
    # Issuer must match what the client config has - parent computes it.
    global ISSUER
    ISSUER = os.environ.get("MOCK_IDP_ISSUER", "http://" + host + ":" + str(actual_port))
    # Emit port on stdout so the orchestrator can read it.
    sys.stdout.write("MOCK_IDP_PORT=" + str(actual_port) + "\n")
    sys.stdout.write("MOCK_IDP_ISSUER=" + ISSUER + "\n")
    sys.stdout.flush()
    t = threading.Thread(target=server.serve_forever, daemon=True)
    t.start()
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        server.shutdown()


if __name__ == "__main__":
    main()
