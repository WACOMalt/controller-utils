#include "metadata.h"
#include "platform/linux/files.h"
#include <memory>
#include <openssl/evp.h>
#include <openssl/pem.h>

namespace ct::update {
namespace {
using PublicKey = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
PublicKey parse_public_key(const std::string &pem) {
    std::unique_ptr<BIO, decltype(&BIO_free)> source(BIO_new_mem_buf(pem.data(), int(pem.size())),
                                                     BIO_free);
    PublicKey key(source ? PEM_read_bio_PUBKEY(source.get(), nullptr, nullptr, nullptr) : nullptr,
                  EVP_PKEY_free);
    if (!key || EVP_PKEY_is_a(key.get(), "ED25519") != 1)
        throw std::invalid_argument("Update signing key is invalid");
    return key;
}
PublicKey read_public_key(const std::filesystem::path &path) {
    return parse_public_key(read_file(path, 16384));
}
} // namespace
void verify_signed_bytes(const std::string &contents, const std::string &sig,
                         const std::string &public_key) {
    if (sig.size() != 64)
        throw std::invalid_argument("Invalid update signature or metadata size");
    auto key = parse_public_key(public_key);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),
                                                                    EVP_MD_CTX_free);
    if (!context ||
        EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, key.get()) != 1 ||
        EVP_DigestVerify(context.get(), reinterpret_cast<const unsigned char *>(sig.data()),
                         sig.size(), reinterpret_cast<const unsigned char *>(contents.data()),
                         contents.size()) != 1)
        throw std::invalid_argument("Update signature verification failed");
}
void validate_public_key(const std::filesystem::path &path) {
    read_public_key(path);
}
} // namespace ct::update
