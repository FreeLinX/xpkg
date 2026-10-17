/* sign.c - Ed25519 verification of repository indexes.
 *
 * Trusted keys live in /etc/xpkg/keys/<name>.pub: the 32-byte raw public
 * key as base64 (or hex), one line.  A repo publishes index.json.sig, the
 * signature of the exact index.json bytes, in the same encoding.  The index
 * lists every archive's sha256, so a verified index vouches for every
 * package it names.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <openssl/evp.h>
#include "xpkg.h"

/* Decodes hex or base64 text into out; returns the byte count or -1. */
static int decode_key_text(const char *text, unsigned char *out, size_t outsz) {
    char buf[512];
    size_t n = 0;
    for (const char *p = text; *p && n + 1 < sizeof(buf); p++)
        if (!isspace((unsigned char)*p)) buf[n++] = *p;
    buf[n] = '\0';
    if (n == 0) return -1;

    int is_hex = 1;
    for (size_t i = 0; i < n; i++) if (!isxdigit((unsigned char)buf[i])) { is_hex = 0; break; }
    if (is_hex && n % 2 == 0) {
        if (n / 2 > outsz) return -1;
        for (size_t i = 0; i < n / 2; i++) {
            unsigned v;
            if (sscanf(buf + 2 * i, "%2x", &v) != 1) return -1;
            out[i] = (unsigned char)v;
        }
        return (int)(n / 2);
    }
    if (n % 4 != 0) return -1;
    unsigned char tmp[512];
    int len = EVP_DecodeBlock(tmp, (const unsigned char *)buf, (int)n);
    if (len < 0) return -1;
    if (n >= 1 && buf[n - 1] == '=') len--;
    if (n >= 2 && buf[n - 2] == '=') len--;
    if ((size_t)len > outsz) return -1;
    memcpy(out, tmp, (size_t)len);
    return len;
}

static int verify_with(const unsigned char pub[32], const void *data, size_t len,
                       const unsigned char sig[64]) {
    EVP_PKEY *pk = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, pub, 32);
    if (!pk) return 0;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    int ok = ctx && EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, pk) == 1 &&
             EVP_DigestVerify(ctx, sig, 64, data, len) == 1;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pk);
    return ok;
}

int xpkg_have_keys(void) {
    DIR *d = opendir(xpkg_keys_dir());
    if (!d) return 0;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) != NULL) {
        size_t l = strlen(e->d_name);
        if (l > 4 && !strcmp(e->d_name + l - 4, ".pub")) n++;
    }
    closedir(d);
    return n;
}

xpkg_status_t xpkg_verify_signature(const void *data, size_t len, const char *sigtext,
                                    char *keyname, size_t keynamesz) {
    unsigned char sig[64];
    if (decode_key_text(sigtext, sig, sizeof(sig)) != 64) return XPKG_ERR_SIGNATURE;

    DIR *d = opendir(xpkg_keys_dir());
    if (!d) return XPKG_ERR_NOT_FOUND;
    struct dirent *e;
    int nkeys = 0;
    xpkg_status_t st = XPKG_ERR_SIGNATURE;
    while ((e = readdir(d)) != NULL) {
        size_t l = strlen(e->d_name);
        if (l <= 4 || strcmp(e->d_name + l - 4, ".pub")) continue;
        char path[XPKG_MAX_PATH];
        snprintf(path, sizeof(path), "%s/%s", xpkg_keys_dir(), e->d_name);
        char *text = xpkg_read_file(path, NULL);
        if (!text) continue;
        unsigned char pub[32];
        int kl = decode_key_text(text, pub, sizeof(pub));
        free(text);
        if (kl != 32) {
            xpkg_warn("ignoring malformed key %s", path);
            continue;
        }
        nkeys++;
        if (verify_with(pub, data, len, sig)) {
            if (keyname) snprintf(keyname, keynamesz, "%.*s", (int)(l - 4), e->d_name);
            st = XPKG_OK;
            break;
        }
    }
    closedir(d);
    if (nkeys == 0 && st != XPKG_OK) return XPKG_ERR_NOT_FOUND;
    return st;
}
