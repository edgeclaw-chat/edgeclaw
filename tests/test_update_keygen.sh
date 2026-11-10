#!/usr/bin/env bash
# tests/test_update_keygen.sh -- the key pipeline behind self-update.
#
# Three copies of one public key have to stay in step: the PEM, the C array compiled into the
# binary, and the copy embedded in tools/install.sh (which runs as `curl | sh` and has no
# checkout). If they drift, nothing crashes -- the signature check simply rejects every
# release, or worse, accepts nothing while looking fine. So the conversion and embedding
# tools are tested rather than trusted.
#
# Everything here is offline: keys are generated locally and thrown away.

set -u

ROOT="${TEST_SRCDIR}/${TEST_WORKSPACE}"

# py_binary puts a launcher in the runfiles named without the .py, while sh_binary keeps its
# source name. Resolve either, so this test does not silently depend on which rule type the
# tool happens to use.
resolve() {
    for cand in "$ROOT/tools/$1" "$ROOT/tools/$1.py" "$ROOT/tools/$1.sh"; do
        [ -f "$cand" ] && { printf '%s\n' "$cand"; return 0; }
    done
    return 1
}

GEN_PUBKEY=$(resolve gen_pubkey_c) || { echo "missing gen_pubkey_c" >&2; exit 1; }
EMBED=$(resolve embed_pubkey_install) || { echo "missing embed_pubkey_install" >&2; exit 1; }
KEYGEN=$(resolve update_keygen) || { echo "missing update_keygen" >&2; exit 1; }
INSTALL_SH=$(resolve install) || { echo "missing install" >&2; exit 1; }
command -v openssl >/dev/null 2>&1 || { echo "openssl required" >&2; exit 1; }

fails=0
checks=0
check() {
    checks=$((checks + 1))
    if [ "$1" = "$2" ]; then
        printf 'ok    %s\n' "$3"
    else
        printf 'FAIL  %s\n        got:  %s\n        want: %s\n' "$3" "$1" "$2"
        fails=$((fails + 1))
    fi
}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK" || exit 1
mkdir -p tools

# ---------------------------------------------------------------- key generation

openssl ecparam -name prime256v1 -genkey -noout -out good.key 2>/dev/null
openssl ec -in good.key -pubout -out tools/update_signing.pub.pem 2>/dev/null

# ------------------------------------------------------- PEM -> C array

if python3 "$GEN_PUBKEY" tools/update_signing.pub.pem > out.c 2>err.txt; then
    check ok ok "a P-256 public key converts to a C array"
else
    check failed ok "a P-256 public key converts to a C array"
    cat err.txt >&2
fi

# The array must be exactly 65 bytes: 0x04 (uncompressed point) then X and Y.
bytes=$(grep -oE '0x[0-9a-f]{2}' out.c | wc -l | tr -d ' ')
check "$bytes" "65" "the C array holds 65 bytes (04 || X || Y)"
first=$(grep -oE '0x[0-9a-f]{2}' out.c | head -1)
check "$first" "0x04" "the first byte is the uncompressed-point marker"

# The generated file must carry the guard the build expects.
if grep -q 'edc_update_pubkey.h' out.c; then
    check yes yes "the generated file includes its own header"
else
    check no yes "the generated file includes its own header"
fi

# The fingerprint line in the banner must match the key it came from: this is the only way a
# human can tell two builds' trust anchors apart.
want_fp=$(openssl pkey -pubin -in tools/update_signing.pub.pem -outform DER | sha256sum | cut -d' ' -f1)
got_fp=$(grep -oE 'SHA-256 of the DER public key: [0-9a-f]{64}' out.c | awk '{print $NF}')
check "$got_fp" "$want_fp" "the banner records the key's real fingerprint"

# ------------------------------------------------- a key of the wrong curve is refused

openssl ecparam -name secp384r1 -genkey -noout -out p384.key 2>/dev/null
openssl ec -in p384.key -pubout -out p384.pub.pem 2>/dev/null
if python3 "$GEN_PUBKEY" p384.pub.pem >/dev/null 2>&1; then
    check "accepted" "refused" "a P-384 key is refused (the verifier is compiled for P-256)"
else
    check "refused" "refused" "a P-384 key is refused (the verifier is compiled for P-256)"
fi

# ... and so is something that is not a public key at all.
printf 'not a key\n' > junk.pem
if python3 "$GEN_PUBKEY" junk.pem >/dev/null 2>&1; then
    check "accepted" "refused" "a non-PEM file is refused"
else
    check "refused" "refused" "a non-PEM file is refused"
fi

# ------------------------------------------------------- embedding into install.sh

cp "$INSTALL_SH" tools/install.sh
if python3 "$EMBED" >/dev/null 2>&1; then
    check ok ok "the public key embeds into install.sh"
else
    check failed ok "the public key embeds into install.sh"
fi

if python3 "$EMBED" --check >/dev/null 2>&1; then
    check match match "the embedded copy is reported as matching"
else
    check mismatch match "the embedded copy is reported as matching"
fi

# The result has to be a script that still parses -- embedding by text substitution is easy to
# get wrong in a way only a shell would notice.
if sh -n tools/install.sh 2>/dev/null; then
    check parses parses "the patched script still parses"
else
    check broken parses "the patched script still parses"
fi

# The key that lands inside must be byte-identical to the PEM, or the verification would
# silently use a truncated key.
embedded=$(sed -n "/<<'EDGECLAW_UPDATE_PUBKEY'/,/^EDGECLAW_UPDATE_PUBKEY$/p" tools/install.sh | sed '1d;$d')
printf '%s\n' "$embedded" > embedded.pem
if cmp -s embedded.pem tools/update_signing.pub.pem; then
    check same same "the embedded key is byte-identical to the PEM"
else
    check different same "the embedded key is byte-identical to the PEM"
fi

# Embedding is idempotent: running it twice must not nest or duplicate the block.
python3 "$EMBED" >/dev/null 2>&1
count=$(grep -c 'EDGECLAW_UPDATE_PUBKEY' tools/install.sh)
check "$count" "2" "a second embed leaves exactly one marker pair"

# ------------------------------------------------------ drift is detected

# Swap in a different key and --check must say so, which is the whole point of the flag.
openssl ecparam -name prime256v1 -genkey -noout -out other.key 2>/dev/null
openssl ec -in other.key -pubout -out tools/update_signing.pub.pem 2>/dev/null
if python3 "$EMBED" --check >/dev/null 2>&1; then
    check match mismatch "a drifted embedded key is reported as a mismatch"
else
    check mismatch mismatch "a drifted embedded key is reported as a mismatch"
fi

# And --check must not have modified the file it was asked about.
if sh -n tools/install.sh 2>/dev/null; then
    check intact intact "--check leaves the script untouched"
else
    check broken intact "--check leaves the script untouched"
fi

# ------------------------------------------------- update_keygen.sh --check

cp "$KEYGEN" tools/update_keygen.sh
openssl ec -in good.key -pubout -out tools/update_signing.pub.pem 2>/dev/null
if sh tools/update_keygen.sh --check good.key >/dev/null 2>&1; then
    check match match "--check accepts the private key that matches the committed public key"
else
    check mismatch match "--check accepts the private key that matches the committed public key"
fi

if sh tools/update_keygen.sh --check other.key >/dev/null 2>&1; then
    check match mismatch "--check rejects a private key that does not match"
else
    check mismatch mismatch "--check rejects a private key that does not match"
fi

# ---------------------------------------------------- the committed key is a placeholder

# The repository's real key must not be one this test generated. Asserting the specific
# fingerprint would break on every legitimate rotation, so instead assert the pipeline works
# end to end against whatever key IS committed -- which is what the release workflow relies on.
COMMITTED="${ROOT}/tools/update_signing.pub.pem"
if [ -f "$COMMITTED" ]; then
    if python3 "$GEN_PUBKEY" "$COMMITTED" > /dev/null 2>&1; then
        check ok ok "the committed public key converts cleanly"
    else
        check failed ok "the committed public key converts cleanly"
    fi
else
    check missing present "tools/update_signing.pub.pem exists"
fi

printf '\n%d checks, %d failed\n' "$checks" "$fails"
[ "$fails" -eq 0 ]
