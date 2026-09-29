#!/usr/bin/env bash
# Make a self-signed TLS certificate for running asr_server.py as wss://.
#
# Usage:   ./make_cert.sh <laptop-ip>        e.g.  ./make_cert.sh 192.168.1.50
#
# Creates (paths are relative to this script, so it works from any folder):
#   tools/certs/server_cert.pem   certificate (EC prime256v1, valid 10 years)
#   tools/certs/server_key.pem    private key (keep it on the laptop)
#   main/certs/server_cert.pem    copy of the certificate that the firmware embeds
#
# The certificate lists the IP as a Subject Alternative Name (plus 127.0.0.1 for local
# tests). Uses only plain openssl commands, so it also works with macOS LibreSSL.
set -euo pipefail

if [ $# -ne 1 ]; then
    echo "Usage: $0 <laptop-ip>   (the IP the ESP32 will connect to, e.g. 192.168.1.50)" >&2
    exit 1
fi
IP="$1"
if ! [[ "$IP" =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ ]]; then
    echo "Error: '$IP' does not look like an IPv4 address." >&2
    exit 1
fi
command -v openssl >/dev/null || { echo "Error: openssl not found." >&2; exit 1; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CERT_DIR="$SCRIPT_DIR/certs"
FW_CERT_DIR="$SCRIPT_DIR/../main/certs"
CERT="$CERT_DIR/server_cert.pem"
KEY="$CERT_DIR/server_key.pem"
mkdir -p "$CERT_DIR" "$FW_CERT_DIR"

# SAN list: the given IP, plus 127.0.0.1 so fake_device.py works on the laptop too.
SAN="IP:$IP"
if [ "$IP" != "127.0.0.1" ]; then
    SAN="$SAN,IP:127.0.0.1"
fi

# Small openssl config so we do not depend on -addext (missing in older LibreSSL).
CONF="$(mktemp)"
trap 'rm -f "$CONF"' EXIT
cat > "$CONF" <<EOF
[req]
distinguished_name = dn
x509_extensions = v3
prompt = no
[dn]
CN = $IP
[v3]
basicConstraints = critical, CA:TRUE
subjectKeyIdentifier = hash
subjectAltName = $SAN
EOF

# 1. EC private key on the prime256v1 (P-256) curve
openssl ecparam -name prime256v1 -genkey -noout -out "$KEY"
chmod 600 "$KEY"
# 2. self-signed certificate for that key
openssl req -new -x509 -sha256 -days 3650 -key "$KEY" -out "$CERT" -config "$CONF"
# 3. copy for the firmware build
cp "$CERT" "$FW_CERT_DIR/server_cert.pem"

echo
echo "Created:"
echo "  $CERT"
echo "  $KEY"
echo "  $(cd "$FW_CERT_DIR" && pwd)/server_cert.pem  (embedded by the firmware)"
echo "  SAN: $SAN"
echo
echo "Next steps:"
echo "  1. idf.py menuconfig -> SIH26172 wake-word subsystem -> Streaming server (WebSocket):"
echo "       WebSocket URI                       = wss://$IP:8765/stream"
echo "       Server certificate check for wss:// = Pinned certificate"
echo "  2. Rebuild and flash:  idf.py build flash monitor"
echo "  3. Run the server:"
echo "       python3 $SCRIPT_DIR/asr_server.py --certfile $CERT --keyfile $KEY"
echo "  4. Optional laptop-only test:"
echo "       python3 $SCRIPT_DIR/fake_device.py --uri wss://127.0.0.1:8765/stream --cafile $CERT"
echo
echo "If the laptop IP changes, run this script again and reflash."
