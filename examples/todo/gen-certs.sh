#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CERT_DIR="$SCRIPT_DIR/certs"
CERT="$CERT_DIR/cert.pem"
KEY="$CERT_DIR/key.pem"

if [ -f "$CERT" ] && [ -f "$KEY" ]; then
    echo "Certs already exist at $CERT_DIR — skipping."
    exit 0
fi

mkdir -p "$CERT_DIR"

openssl ecparam -genkey -name prime256v1 -noout -out "$KEY"
openssl req -new -x509 -key "$KEY" -out "$CERT" -days 365 \
    -subj "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"

echo "Generated self-signed TLS cert:"
echo "  cert: $CERT"
echo "  key:  $KEY"
