#!/bin/sh
# Assemble, check, sign and (optionally) upload the FreeLinX package repo.
#
#   tools/publish-repo.sh [--upload]
#
# Sources (every *.xpkg found):
#   ../ports/packages                 ports framework packages
#   ../Desktop-test/stack/work/pkgs   desktop stack (stack/package-stack.sh)
#
# Gates, in order - any failure stops the publish:
#   1. every archive unpacks and passes check-nognu.sh (no GCC/glibc code)
#   2. index.json is generated from the archives (never hand-edited)
#   3. index.json.sig is made with the repository key and verified
#      against keys/freelinx.pub, the key FreeLinX images trust
#
# Environment: XPKG_SIGNING_KEY (default ~/.config/xpkg/freelinx.key),
# XPKG_REPO_OUT (default ../repo-out), XPKG_HF_REPO (FreeLinX/packages).
set -eu

HERE="$(cd "$(dirname "$0")/.." && pwd)"
TOP="$(cd "$HERE/.." && pwd)"
OUT="${XPKG_REPO_OUT:-$TOP/repo-out}"
KEY="${XPKG_SIGNING_KEY:-$HOME/.config/xpkg/freelinx.key}"
HF_REPO="${XPKG_HF_REPO:-FreeLinX/packages}"
W="$TOP/Desktop-test/stack/work"
CHECK="$TOP/Desktop-test/check-nognu.sh"
CREATE="$W/build/xpkg-create"
RUN="$W/bin/musl-run"

[ -f "$KEY" ] || { echo "publish: no signing key at $KEY" >&2; exit 1; }
[ -x "$CREATE" ] || "$W/bin/flx-cc" -O2 -o "$CREATE" "$HERE/tools/xpkg-create.c" -lz -lcrypto

rm -rf "$OUT"
mkdir -p "$OUT"
for d in "$TOP/ports/packages" "$W/pkgs"; do
    [ -d "$d" ] || continue
    for f in "$d"/*.xpkg; do
        [ -f "$f" ] || continue
        ln -f "$f" "$OUT/" 2>/dev/null || cp "$f" "$OUT/"
    done
done
echo "publish: $(ls "$OUT"/*.xpkg | wc -l) archives"

# 1. no-GNU gate
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
bad=0
for f in "$OUT"/*.xpkg; do
    n=$(basename "$f" .xpkg)
    mkdir -p "$T/$n"
    if ! tar -xzf "$f" -C "$T/$n" 2>/dev/null; then
        echo "publish: $n does not unpack" >&2
        bad=$((bad + 1))
    elif ! "$CHECK" "$T/$n" > "$T/$n.log" 2>&1; then
        echo "publish: $n contains GCC/glibc code:" >&2
        grep '^FAIL' "$T/$n.log" | head -3 >&2
        bad=$((bad + 1))
    fi
    rm -rf "$T/$n"
done
[ "$bad" -eq 0 ] || { echo "publish: $bad package(s) failed the gate; nothing published" >&2; exit 1; }
echo "publish: every package passes check-nognu"

# 2 + 3. index and signature
"$RUN" "$CREATE" index --dir "$OUT" --output "$OUT/index.json"
"$RUN" "$CREATE" sign --key "$KEY" "$OUT/index.json"
python3 - "$OUT/index.json" "$HERE/keys/freelinx.pub" <<'EOF'
import base64, sys
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
data = open(sys.argv[1], "rb").read()
sig = base64.b64decode(open(sys.argv[1] + ".sig").read().strip())
pub = Ed25519PublicKey.from_public_bytes(base64.b64decode(open(sys.argv[2]).read().strip()))
pub.verify(sig, data)
print("publish: signature verifies against keys/freelinx.pub")
EOF

if [ "${1:-}" = "--upload" ]; then
    echo "publish: uploading to https://huggingface.co/datasets/$HF_REPO"
    # --delete: archives no longer published (replaced, or failing the
    # gate) disappear from the mirror too
    hf upload "$HF_REPO" "$OUT" . --repo-type dataset --delete '*.xpkg' \
        --commit-message "xpkg repo: $(ls "$OUT"/*.xpkg | wc -l) packages, signed index" 2>&1 | grep -v '^Hint'
fi
