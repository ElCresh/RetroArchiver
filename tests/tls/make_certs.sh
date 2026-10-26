#!/bin/sh
# Generates the certificates of the fake HTTPS server (tests/fake_https.c).
# Run once, from the repository root, with OpenSSL 3; the outputs are
# committed, so the tests never need OpenSSL. Rerun only to change them.
#
#   ca.pem          the test "CA bundle": only the test root
#   good.pem        localhost, 2020-2120, with the intermediate (like archive.org)
#   expired.pem     localhost, 2001-2002: the clock error without touching the clock
#   future.pem      localhost, 2100-2120: not yet valid
#   othername.pem   other.test: wrong name
#   unknown.pem     localhost, signed by a root that is not in ca.pem
#   selfsigned.pem  nas.test, self-signed: trusted only by its fingerprint
#   leaf.key        the private key of all the server certificates
#   *.sha256        SHA-256 of the server certificate (DER), lowercase hex
set -e
out=tests/tls
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/ca.cnf" <<'EOF'
[ca]
default_ca = test_ca
[test_ca]
dir           = .
database      = $dir/index.txt
new_certs_dir = $dir/new
serial        = $dir/serial
default_md    = sha256
policy        = any
unique_subject = no
copy_extensions = copy
[any]
commonName = supplied
[ca_ext]
basicConstraints = critical, CA:TRUE
keyUsage = critical, keyCertSign, cRLSign
subjectKeyIdentifier = hash
[leaf_ext]
basicConstraints = critical, CA:FALSE
keyUsage = critical, digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
EOF

cd "$tmp"
mkdir new
: > index.txt
echo 1000 > serial
key() { openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$1" 2>/dev/null; }

# sign SUBJECT_CN SAN ISSUER_CERT ISSUER_KEY EXT START END KEY OUT
sign() {
    if [ -n "$2" ]; then san="-addext subjectAltName=DNS:$2"; else san=""; fi
    openssl req -new -key "$8" -subj "/CN=$1" $san -out req.csr
    openssl ca -batch -config ca.cnf -cert "$3" -keyfile "$4" -extensions "$5" \
        -startdate "$6" -enddate "$7" -in req.csr -out "$9" -notext 2>/dev/null
}
selfsign() {
    openssl req -new -key "$4" -subj "/CN=$1" -out req.csr
    openssl ca -batch -selfsign -config ca.cnf -keyfile "$4" -extensions "$5" \
        -startdate 20200101000000Z -enddate 21200101000000Z -in req.csr -out "$6" -notext 2>/dev/null
}

key root.key; key inter.key; key other.key; key leaf.key
selfsign "RetroArchiver Test Root CA" "" "" root.key ca_ext root.pem
sign "RetroArchiver Test Intermediate CA" "" root.pem root.key ca_ext \
    20200101000000Z 21200101000000Z inter.key inter.pem
selfsign "Unknown Test Root CA" "" "" other.key ca_ext other.pem

leaf() { sign "$2" "$2" inter.pem inter.key leaf_ext "$3" "$4" leaf.key "$1.crt"; cat "$1.crt" inter.pem > "$1.pem"; }
leaf good      localhost  20200101000000Z 21200101000000Z
leaf expired   localhost  20010101000000Z 20020101000000Z
leaf future    localhost  21000101000000Z 21200101000000Z
leaf othername other.test 20200101000000Z 21200101000000Z
sign localhost localhost other.pem other.key leaf_ext 20200101000000Z 21200101000000Z leaf.key unknown.crt
cp unknown.crt unknown.pem
openssl req -new -x509 -key leaf.key -subj "/CN=nas.test" -addext subjectAltName=DNS:nas.test \
    -days 36500 -out selfsigned.pem

fp() { openssl x509 -in "$1" -outform DER | openssl dgst -sha256 -r | cut -c1-64; }
cd - >/dev/null
cp "$tmp/root.pem" "$out/ca.pem"
for n in good expired future othername unknown selfsigned; do cp "$tmp/$n.pem" "$out/$n.pem"; done
cp "$tmp/leaf.key" "$out/leaf.key"
(cd "$tmp" && fp good.crt) > "$out/good.sha256"
(cd "$tmp" && fp selfsigned.pem) > "$out/selfsigned.sha256"
echo "  certificates written in $out"
