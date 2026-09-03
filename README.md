## Commands

```
xpkg install <file.xpkg>   Install a package from a local file
xpkg install <name>        Fetch <name> from a configured repo and install
xpkg remove <name>         Remove an installed package
xpkg list                  List installed packages
xpkg info <name>           Show details for an installed package
xpkg files <name>          List files owned by an installed package
xpkg verify <name>         Re-hash installed files, report changes
xpkg repo add <url>        Add a repo (http:// or https://) to repos.conf
xpkg repo remove <url>     Remove a repo
xpkg repo list             List configured repos
```

- Repos: `/etc/xpkg/repos.conf`, one URL per line.
- Each repo is expected to serve an `index.json` (name/version/filename/
  sha256) plus the `.xpkg` files themselves.
- Install-by-name looks up `index.json`, builds the fetch URL, downloads,
  verifies the sha256, extracts, and installs. Packages are always
  installed from the local `cache` copy after checksum verification.

## Network / HTTPS notes

- HTTP and HTTPS (TLS 1.2/1.3) via OpenSSL (`-lssl -lcrypto`).
- Follows redirects, absolute *and* relative `Location` (HF returns
  relative `Location: /api/...` on its 307s).
- Supports both `Content-Length` and chunked transfer-encoding bodies.
- Prefers IPv4 when a host exposes both A and AAAA records (matters on
  networks with no IPv6 route).
- Verified live against `huggingface.co` over HTTPS (307 redirect followed
  to a 200 JSON body), and end-to-end against a local HTTP repo.

## Build dependencies

Built against the FreeLinX toolchain (clang + LLD + musl, `-static`,
`-fuse-ld=lld -rtlib=compiler-rt -unwindlib=none`), linking:

- zlib (`-lz`)          — gzip (`.xpkg` is gzip'd ustar)
- openssl (`-lssl -lcrypto`) — HTTPS fetch
- sqlite (`-lsqlite3`)  — package DB at `/var/lib/xpkg/xpkg.db`

These are ports built into `build/deps/{zlib,openssl,sqlite}/`; pull them
in with `FREELINIX_PORTS_DEPS ?= ../ports-actual/build/deps` (see the
Makefile). `base/sqlite` is a new FreeLinX port (autoconf amalgamation,
static `libsqlite3.a`) created specifically so xpkg can link `-lsqlite3`.

`tools/xpkg-create.c` is a separate small tool that packages a staging
tree into `.xpkg` and generates `index.json` (sha256 via OpenSSL).

Override `FREELINIX_CC`/`FREELINIX_SYSROOT`/`FREELINIX_PORTS_DEPS` if your
layout differs from the defaults (see Makefile).

## Hosting the public repo

The intended public binary repo is a Hugging Face **Dataset** — free, no
large-file cap, stable `https://huggingface.co/datasets/<owner>/<repo>/resolve/main/<file>`
URLs, HTTPS-only (no plaintext HTTP). `repos.conf`/`index.json` are
host-agnostic, so any static HTTPS host (incl. Cloudflare R2) works.

Publish flow: build each package into a staging tree → run `xpkg-create`
to produce `.xpkg` + add entries to `index.json` → upload both to the HF
dataset → every user does `xpkg repo add <dataset URL>` once.

## Status (2026-09-03)

- Local-file and repo install all **built and verified** (static x86-64
  ELF, zero warnings): install/remove/list/info/files/verify, plus
  repo add/remove/list and `install <name>` fetch-by-name over
  HTTP/HTTPS with sha256 verification (end-to-end against a local HTTP
  repo, and the HTTPS/TLS/redirect path verified live against
  huggingface.co).
- Install clears its scratch dir each time, so each package owns exactly
  its own files (no cross-package leakage when packages are installed
  back-to-back).
- **Later** — `xpkg update` (refresh cached repo indexes), a real
  dependency resolver, upgrading in place.

## Related repositories

- `toolchain` — the Clang/LLD/musl toolchain xpkg is built with.
- `ports` — where sqlite (`base/sqlite`, a new port)/zlib/openssl
  (xpkg's own build dependencies) and every future FreeLinX package's
  *source* get built. xpkg is the tool that installs the *output* of
  that pipeline once packaged.
- `src` — root filesystem; xpkg itself gets staged into
  `src/rootfs/usr/bin/xpkg` the same way runit/pfetch were.
