// Streaming SHA-256. See sha256.hpp.
#include "common/sha256.hpp"

#include "common/errors.hpp"

#include <openssl/evp.h>

#include <cstdio>

namespace obd::common {

Sha256::Sha256() {
    ctx_ = EVP_MD_CTX_new();
    if (!ctx_ || EVP_DigestInit_ex(static_cast<EVP_MD_CTX*>(ctx_),
                                   EVP_sha256(), nullptr) != 1) {
        if (ctx_) {
            EVP_MD_CTX_free(static_cast<EVP_MD_CTX*>(ctx_));
            ctx_ = nullptr;
        }
        throw error(EIO, "cannot initialize SHA-256 context");
    }
}

Sha256::~Sha256() {
    if (ctx_) EVP_MD_CTX_free(static_cast<EVP_MD_CTX*>(ctx_));
}

void Sha256::update(const void* data, size_t size) {
    if (EVP_DigestUpdate(static_cast<EVP_MD_CTX*>(ctx_), data, size) != 1) {
        throw error(EIO, "SHA-256 update failed");
    }
}

std::string Sha256::final_hex() {
    uint8_t digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (EVP_DigestFinal_ex(static_cast<EVP_MD_CTX*>(ctx_), digest, &len) !=
        1) {
        throw error(EIO, "SHA-256 final failed");
    }
    static const char* hexd = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (unsigned int i = 0; i < len; i++) {
        out.push_back(hexd[digest[i] >> 4]);
        out.push_back(hexd[digest[i] & 0xf]);
    }
    return out;
}

std::string Sha256::hex(const void* data, size_t size) {
    Sha256 h;
    h.update(data, size);
    return h.final_hex();
}

}  // namespace obd::common
