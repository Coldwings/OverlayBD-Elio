// Streaming SHA-256 via OpenSSL EVP (OpenSSL is already linked through
// Elio's TLS support). Used for download integrity verification, matching
// overlaybd's post-download sha256 check against the config digest.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace obd::common {

class Sha256 {
public:
    /// Throws obd::error if the OpenSSL context cannot be created.
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    void update(const void* data, size_t size);

    /// Finalizes and returns the digest as 64 lowercase hex chars. The
    /// object must not be used afterwards.
    std::string final_hex();

    /// One-shot hex digest of a buffer.
    static std::string hex(const void* data, size_t size);

private:
    void* ctx_ = nullptr;  // EVP_MD_CTX*
};

}  // namespace obd::common
