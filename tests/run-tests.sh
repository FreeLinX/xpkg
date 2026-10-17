#!/bin/sh
# xpkg end-to-end tests: builds packages with xpkg-create, serves a signed
# repo over HTTP on localhost and drives xpkg against a scratch root.
#
#   tests/run-tests.sh <xpkg> <xpkg-create> [runner]
#
# runner: optional command prefix to execute the (musl) binaries on the
# build host, e.g. the stack's musl-run wrapper.
set -u
XPKG_BIN="$1"; CREATE_BIN="$2"; RUN="${3:-}"
T="$(mktemp -d "${TMPDIR:-/tmp}/xpkg-test.XXXXXX")"
PORT=$((20000 + $$ % 20000))
pass=0; fail=0

export XPKG_ROOT="$T/root"
export NO_COLOR=1
mkdir -p "$XPKG_ROOT/etc/xpkg/keys" "$T/repo" "$T/st"
xpkg() { $RUN "$XPKG_BIN" "$@"; }
create() { $RUN "$CREATE_BIN" "$@"; }

ok()   { pass=$((pass + 1)); echo "ok   $1"; }
bad()  { fail=$((fail + 1)); echo "FAIL $1"; }
check() { # desc, command...
    d="$1"; shift
    if "$@" >"$T/out" 2>&1; then ok "$d"; else bad "$d"; sed 's/^/     | /' "$T/out"; fi
}
check_fail() {
    d="$1"; shift
    if "$@" >"$T/out" 2>&1; then bad "$d (unexpected success)"; sed 's/^/     | /' "$T/out"; else ok "$d"; fi
}

# --- packages ------------------------------------------------------------
LONG="files-with-a-really-long-directory-name-that-goes-on/and-on-and-on-past-the-ustar-limit/for-sure"
mk() { rm -rf "$T/st/$1"; mkdir -p "$T/st/$1"; }

mk libfoo1
mkdir -p "$T/st/libfoo1/usr/lib" "$T/st/libfoo1/etc"
echo "libfoo 1" > "$T/st/libfoo1/usr/lib/libfoo.so.1"
echo "setting=1" > "$T/st/libfoo1/etc/foo.conf"
create create --name libfoo --version 1.0 --description 'Foo "library"' \
    --stage "$T/st/libfoo1" --output "$T/repo/libfoo-1.0.xpkg" >/dev/null

mk app1
mkdir -p "$T/st/app1/usr/bin" "$T/st/app1/usr/share/$LONG"
printf '#!/bin/sh\necho app 1\n' > "$T/st/app1/usr/bin/app"; chmod 4755 "$T/st/app1/usr/bin/app"
echo old > "$T/st/app1/usr/share/app-old-file"
echo deep > "$T/st/app1/usr/share/$LONG/deep.txt"
ln -s "/usr/share/$LONG/deep.txt" "$T/st/app1/usr/bin/app-link"
printf 'touch "${XPKG_ROOT:-}/tmp/post-ran-$XPKG_ACTION" 2>/dev/null || touch /tmp/.xpkg-post-$XPKG_ACTION\n' > "$T/post"
create create --name app --version 1.0 --description 'An app' --depends libfoo \
    --stage "$T/st/app1" --output "$T/repo/app-1.0.xpkg" >/dev/null

mk clash
mkdir -p "$T/st/clash/usr/bin"; echo x > "$T/st/clash/usr/bin/app"
create create --name clash --version 1 --stage "$T/st/clash" --output "$T/repo/clash-1.xpkg" >/dev/null

mk hl
mkdir -p "$T/st/hl/usr/lib/dri"
head -c 100000 /dev/urandom > "$T/st/hl/usr/lib/dri/a_dri.so"
ln "$T/st/hl/usr/lib/dri/a_dri.so" "$T/st/hl/usr/lib/dri/b_dri.so"
ln "$T/st/hl/usr/lib/dri/a_dri.so" "$T/st/hl/usr/lib/dri/c_dri.so"
create create --name hl --version 1 --stage "$T/st/hl" --output "$T/repo/hl-1.xpkg" >/dev/null

create keygen --out "$T/test" >/dev/null
cp "$T/test.pub" "$XPKG_ROOT/etc/xpkg/keys/test.pub"
index() { create index --dir "$T/repo" --output "$T/repo/index.json" >/dev/null && create sign --key "$T/test.key" "$T/repo/index.json" >/dev/null; }
index

(cd "$T/repo" && exec python3 -m http.server "$PORT" --bind 127.0.0.1 >/dev/null 2>&1) &
SRV=$!
trap 'kill $SRV 2>/dev/null; rm -rf "$T"' EXIT
i=0; until python3 -c "import urllib.request;urllib.request.urlopen('http://127.0.0.1:$PORT/index.json')" 2>/dev/null || [ $i -ge 50 ]; do i=$((i+1)); python3 -c 'import time;time.sleep(0.1)'; done
echo "http://127.0.0.1:$PORT" > "$XPKG_ROOT/etc/xpkg/repos.conf"

# --- tests -----------------------------------------------------------------
check "update (signed index)" xpkg update
check "search finds by description" sh -c "$RUN $XPKG_BIN search library | grep -q libfoo"
check "show" sh -c "$RUN $XPKG_BIN show app | grep -q 'Depends on  : libfoo'"
check "install pulls dependency" xpkg install app
check "dependency installed" test -f "$XPKG_ROOT/usr/lib/libfoo.so.1"
check "setuid bit kept" sh -c "[ \$(stat -c %a '$XPKG_ROOT/usr/bin/app') = 4755 ]"
check "long path (pax) extracted" test -f "$XPKG_ROOT/usr/share/$LONG/deep.txt"
check "long symlink target" sh -c "[ \"\$(readlink '$XPKG_ROOT/usr/bin/app-link')\" = '/usr/share/$LONG/deep.txt' ]"
check "hardlinked package installs" xpkg install hl
check "hardlinks share one inode" sh -c "[ \$(stat -c %i '$XPKG_ROOT/usr/lib/dri/a_dri.so') = \$(stat -c %i '$XPKG_ROOT/usr/lib/dri/c_dri.so') ] && cmp -s '$T/st/hl/usr/lib/dri/b_dri.so' '$XPKG_ROOT/usr/lib/dri/b_dri.so'"
check "hardlinked archive is small" sh -c "[ \$(stat -c %s '$T/repo/hl-1.xpkg') -lt 150000 ]"
check "verify hardlinked files" xpkg verify hl
check "owns" sh -c "$RUN $XPKG_BIN owns /usr/bin/app | grep -q 'owned by app'"
check "info shows dependency reason" sh -c "$RUN $XPKG_BIN info libfoo | grep -q 'as a dependency'"
check "list -e hides dependencies" sh -c "$RUN $XPKG_BIN list -e | grep -q '^app ' && ! $RUN $XPKG_BIN list -e | grep -q libfoo"
check "verify all intact" xpkg verify
check_fail "conflicting package refused" xpkg install clash
check "conflict left app untouched" grep -q "app 1" "$XPKG_ROOT/usr/bin/app"
check_fail "remove of a needed package refused" xpkg remove libfoo

# user edits a config file, then libfoo 2 ships a new one
echo "setting=mine" > "$XPKG_ROOT/etc/foo.conf"
check "edited config reported by verify" sh -c "$RUN $XPKG_BIN verify libfoo | grep -q EDITED"
mk libfoo2; mkdir -p "$T/st/libfoo2/usr/lib" "$T/st/libfoo2/etc"
echo "libfoo 2" > "$T/st/libfoo2/usr/lib/libfoo.so.1"
echo "setting=2" > "$T/st/libfoo2/etc/foo.conf"
create create --name libfoo --version 1.10 --stage "$T/st/libfoo2" --output "$T/repo/libfoo-1.10.xpkg" >/dev/null
mk app2; mkdir -p "$T/st/app2/usr/bin"
printf '#!/bin/sh\necho app 2\n' > "$T/st/app2/usr/bin/app"
create create --name app --version 2.0 --depends libfoo --post-install "$T/post" \
    --stage "$T/st/app2" --output "$T/repo/app-2.0.xpkg" >/dev/null
index
check "outdated lists both" sh -c "$RUN $XPKG_BIN update >/dev/null && $RUN $XPKG_BIN outdated | grep -c ' -> ' | grep -q 2"
check "upgrade" xpkg upgrade
check "new version installed (1.10 > 1.9 numeric)" grep -q "libfoo 2" "$XPKG_ROOT/usr/lib/libfoo.so.1"
check "edited config kept" grep -q "setting=mine" "$XPKG_ROOT/etc/foo.conf"
check "new config saved as .xpkgnew" grep -q "setting=2" "$XPKG_ROOT/etc/foo.conf.xpkgnew"
check "stale file removed on upgrade" test ! -e "$XPKG_ROOT/usr/share/app-old-file"
check "stale long dir pruned" test ! -e "$XPKG_ROOT/usr/share/files-with-a-really-long-directory-name-that-goes-on"
check "reinstall" xpkg reinstall app
check "dry-run changes nothing" sh -c "$RUN $XPKG_BIN -n remove app >/dev/null && test -f '$XPKG_ROOT/usr/bin/app'"

# tampering
cp "$T/repo/index.json" "$T/index.good"
sed 's/"generated": /"generated": 9/' "$T/index.good" > "$T/repo/index.json"
check_fail "tampered index refused" xpkg update
cp "$T/index.good" "$T/repo/index.json"
mv "$T/repo/index.json.sig" "$T/sig.good"
check_fail "unsigned index refused when keys exist" xpkg update
mv "$T/sig.good" "$T/repo/index.json.sig"
cp "$T/repo/app-2.0.xpkg" "$T/app.good"; echo junk >> "$T/repo/app-2.0.xpkg"
check "update with good index" xpkg update
xpkg clean >/dev/null
check_fail "corrupted archive refused" xpkg reinstall app
cp "$T/app.good" "$T/repo/app-2.0.xpkg"

check "remove app" xpkg remove app
check "app files gone" test ! -e "$XPKG_ROOT/usr/bin/app"
check "autoremove drops libfoo" xpkg autoremove
check "libfoo gone" sh -c "! $RUN $XPKG_BIN list | grep -q libfoo"
check "edited config left behind" grep -q "setting=mine" "$XPKG_ROOT/etc/foo.conf"
check "local file install" xpkg install "$T/repo/libfoo-1.0.xpkg"
check "upgrade local install to 1.10" xpkg upgrade
check_fail "downgrade refused without --force" xpkg install "$T/repo/libfoo-1.0.xpkg"
check "downgrade with --force" xpkg --force install "$T/repo/libfoo-1.0.xpkg"
check "repo list shows signed" sh -c "$RUN $XPKG_BIN repo list | grep -q signed"

mv "$XPKG_ROOT/etc/xpkg/keys/test.pub" "$T/test.pub.off"
check_fail "no trusted keys: index refused" xpkg update
mv "$T/test.pub.off" "$XPKG_ROOT/etc/xpkg/keys/test.pub"

mk big
mkdir -p "$T/st/big/usr/share"; echo big > "$T/st/big/usr/share/big"
create create --name big --version 1 --stage "$T/st/big" --output "$T/repo/big-1.xpkg" >/dev/null
index
head -c 300000 /dev/zero >> "$T/repo/big-1.xpkg"
xpkg update >/dev/null 2>&1
check "oversized download stopped at the indexed size" sh -c "! $RUN $XPKG_BIN install big >'$T/big.out' 2>&1 && grep -q 'larger than expected' '$T/big.out'"

# hostile archives: a symlink planted by the package must not carry later
# entries (or hardlinks) out of the extraction directory
mkdir -p "$T/outside"
evil() { # name, python tarfile body
    python3 - "$T/repo/$1.xpkg" "$T/outside" "$2" <<'PY'
import io, sys, tarfile
out, outside, kind = sys.argv[1:]
t = tarfile.open(out, "w:gz")
def add(name, data=b"", **kw):
    i = tarfile.TarInfo(name)
    for k, v in kw.items(): setattr(i, k, v)
    i.size = len(data) if i.type == tarfile.REGTYPE else 0
    t.addfile(i, io.BytesIO(data) if i.type == tarfile.REGTYPE else None)
add("pkg-info", b"NAME=evil-" + kind.encode() + b"\nVERSION=1\nARCH=x86_64\n")
add("files/x", type=tarfile.SYMTYPE, linkname=outside)
if kind == "file": add("files/x/pwned", b"owned\n")
elif kind == "dir": add("files/x", type=tarfile.DIRTYPE, mode=0o777)
elif kind == "link":
    add("files/y", type=tarfile.LNKTYPE, linkname="files/x/secret")
t.close()
PY
}
echo secret > "$T/outside/secret"; chmod 700 "$T/outside"
evil evil-file file
check_fail "symlink-parent write refused" xpkg install "$T/repo/evil-file.xpkg"
check "nothing written outside" test ! -e "$T/outside/pwned"
evil evil-link link
check_fail "hardlink through symlink refused" xpkg install "$T/repo/evil-link.xpkg"
evil evil-dir dir
xpkg install "$T/repo/evil-dir.xpkg" >/dev/null 2>&1
check "dir entry over symlink does not chmod target" sh -c "[ \"\$(stat -c %a '$T/outside')\" = 700 ]"

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
