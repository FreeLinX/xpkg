/* hash.c - SHA256 file hashing, via the already-ported OpenSSL (libcrypto).
 *
 * Used by `xpkg install` (recording each installed file's hash) and
 * `xpkg verify` (re-hashing and comparing against what was recorded).
 */
#include <stdio.h>
#include <string.h>
#include <openssl/evp.h>
#include "xpkg.h"

#define HASH_READ_CHUNK 65536

/* Writes a 65-byte (64 hex chars + NUL) lowercase hex digest into out. */
xpkg_status_t xpkg_sha256_file(const char *path, char out[65]) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return XPKG_ERR_IO;
    }

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx || EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
        if (ctx) EVP_MD_CTX_free(ctx);
        fclose(f);
        return XPKG_ERR_IO;
    }

    unsigned char buf[HASH_READ_CHUNK];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        EVP_DigestUpdate(ctx, buf, n);
    }
    fclose(f);

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len;
    EVP_DigestFinal_ex(ctx, digest, &digest_len);
    EVP_MD_CTX_free(ctx);

    for (unsigned int i = 0; i < digest_len; i++) {
        snprintf(out + (i * 2), 3, "%02x", digest[i]);
    }
    out[digest_len * 2] = '\0';

    return XPKG_OK;
}
