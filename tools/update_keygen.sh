#!/bin/sh
# Generate the release signing key pair, or rotate it.
#
#   sh tools/update_keygen.sh            # make a new pair in ./signing-out/
#   sh tools/update_keygen.sh --check    # does the committed pubkey match a given key?
#
# Why this is a script and not a paragraph of instructions
# -------------------------------------------------------
# The two halves of this key pair live in two places that must agree: the private key in
# the CI secret UPDATE_SIGNING_KEY, the public key committed as tools/update_signing.pub.pem
# and compiled into the binary. Getting them out of step does not fail loudly at release
# time -- the workflow verifies before publishing, so it does fail there -- but the failure
# reads as "signature does not verify", which sends people looking at the wrong thing.
# Better to produce both from one command and print exactly where each goes.
#
# What is NOT in this repository: the private key. Never commit it, and never paste it into
# an issue. If it is lost, rotate: generate a new pair, update the secret and the committed
# public key, and ship a release. Nothing signed with the old key can be trusted again.
#
# Algorithm: ECDSA P-256 (prime256v1) with SHA-256. Chosen over Ed25519 because the build
# already enables MBEDTLS_ECDSA_C and MBEDTLS_PK_C, while Ed25519 would need
# MBEDTLS_PSA_CRYPTO_C -- a much larger configuration change for the same job.

set -eu

OUT=signing-out

case "${1:-}" in
    -h|--help)
        sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    --check)
        [ $# -ge 2 ] || { echo "usage: $0 --check <private-key.pem>" >&2; exit 2; }
        key=$2
        [ -f "$key" ] || { echo "no such file: $key" >&2; exit 1; }
        tmp=$(mktemp -d)
        trap 'rm -rf "$tmp"' EXIT
        openssl ec -in "$key" -pubout -out "$tmp/pub.pem" 2>/dev/null
        have=$(openssl pkey -pubin -in "$tmp/pub.pem" -outform DER | sha256sum | cut -d' ' -f1)
        want=$(openssl pkey -pubin -in tools/update_signing.pub.pem -outform DER | sha256sum | cut -d' ' -f1)
        if [ "$have" = "$want" ]; then
            echo "MATCH: this private key belongs to the committed public key"
        else
            echo "MISMATCH: the committed tools/update_signing.pub.pem is a different key"
            echo "  committed: $want"
            echo "  this key : $have"
            echo "Update the committed public key, or use the matching private key."
            exit 1
        fi
        ;;
    "")
        command -v openssl >/dev/null 2>&1 || { echo "openssl is required" >&2; exit 1; }
        mkdir -p "$OUT"
        chmod 700 "$OUT"
        openssl ecparam -name prime256v1 -genkey -noout -out "$OUT/edgeclaw-signing.key"
        chmod 600 "$OUT/edgeclaw-signing.key"
        openssl ec -in "$OUT/edgeclaw-signing.key" -pubout -out "$OUT/update_signing.pub.pem" 2>/dev/null
        cat <<EOF

Generated:
  $OUT/edgeclaw-signing.key   <- PRIVATE. Goes into the CI secret, nowhere else.
  $OUT/update_signing.pub.pem <- PUBLIC. Commit it over tools/update_signing.pub.pem.

Next steps, in this order:

  1. Commit the public key:
       cp $OUT/update_signing.pub.pem tools/update_signing.pub.pem
       git add tools/update_signing.pub.pem

  2. Re-embed it in the binary (the client verifies against this copy, not the file):
       python3 tools/gen_pubkey_c.py tools/update_signing.pub.pem \\
         > src/common/edc_update_pubkey.c

  3. Re-embed it in tools/install.sh, which runs standalone via `curl | sh` and therefore
     cannot read the file (this replaces the block between the EDGECLAW_UPDATE_PUBKEY
     markers):
       python3 tools/embed_pubkey_install.py

  4. Store the private key as the repository secret UPDATE_SIGNING_KEY:
       gh secret set UPDATE_SIGNING_KEY < $OUT/edgeclaw-signing.key

  5. Sanity check that all three copies agree:
       sh tools/update_keygen.sh --check $OUT/edgeclaw-signing.key

  6. Delete $OUT when done. Everything signed before this point is untrusted under the new
     key, so publish a new release rather than re-signing an old tag.

EOF
        ;;
    *)
        echo "unknown argument: $1 (try --help)" >&2
        exit 2
        ;;
esac
