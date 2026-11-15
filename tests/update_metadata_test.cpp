#include "update/metadata.h"
#include "platform/linux/files.h"
#include <cassert>
#include <iostream>
#include <memory>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
namespace j = ct::json;
namespace u = ct::update;
using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
template <class Function> void rejects(Function function) {
    bool rejected = false;
    try {
        function();
    } catch (const std::exception &) {
        rejected = true;
    }
    assert(rejected);
}
Key new_key() {
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
        EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr), EVP_PKEY_CTX_free);
    EVP_PKEY *key = nullptr;
    assert(context && EVP_PKEY_keygen_init(context.get()) == 1 &&
           EVP_PKEY_keygen(context.get(), &key) == 1);
    return Key(key, EVP_PKEY_free);
}
void public_key(const std::filesystem::path &path, EVP_PKEY *key) {
    std::unique_ptr<BIO, decltype(&BIO_free)> buffer(BIO_new(BIO_s_mem()), BIO_free);
    assert(buffer && PEM_write_bio_PUBKEY(buffer.get(), key) == 1);
    char *contents = nullptr;
    const auto size = BIO_get_mem_data(buffer.get(), &contents);
    assert(size > 0);
    ct::atomic_write(path, std::string(contents, size_t(size)));
}
void sign(const std::filesystem::path &path, const std::filesystem::path &signature, EVP_PKEY *key,
          json_object *value) {
    const auto contents = j::encode(value, u::MaxMetadata);
    ct::atomic_write(path, contents);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),
                                                                    EVP_MD_CTX_free);
    assert(context && EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key) == 1);
    std::string bytes(64, '\0');
    size_t size = bytes.size();
    assert(EVP_DigestSign(context.get(), reinterpret_cast<unsigned char *>(bytes.data()), &size,
                          reinterpret_cast<const unsigned char *>(contents.data()),
                          contents.size()) == 1);
    assert(size == 64);
    ct::atomic_write(signature, bytes);
}
j::Value metadata(ct::TrackingBackend backend) {
    auto value = u::Release{"0.1.1", 2}.to_json();
    json_object_object_add(value.get(), "schema", json_object_new_int(5));
    json_object_object_add(value.get(), "expires", json_object_new_int64(2000));
    json_object_object_add(value.get(), "size", json_object_new_int64(100));
    json_object_object_add(value.get(), "artifact",
                           json_object_new_string("controller-utils-0.1.1-2-linuxarm64.tar.gz"));
    json_object_object_add(value.get(), "sha256",
                           json_object_new_string(std::string(64, 'a').c_str()));
    json_object_object_add(value.get(), "notes", json_object_new_string("Fixture release"));
    auto required = json_object_new_object();
    json_object_object_add(required, "backend",
                           json_object_new_string(u::backend_name(backend).c_str()));

    for (const auto &[field, names] :
         std::vector<std::pair<const char *, std::vector<std::string>>>{
             {"driver", {u::DriverFiles.begin(), u::DriverFiles.end()}},
             {"helper", u::helper_files(backend)},
             {"runtime", {u::RuntimeFiles.begin(), u::RuntimeFiles.end()}}}) {
        auto fingerprints = json_object_new_object();
        for (const auto &name : names)
            json_object_object_add(fingerprints, name.c_str(),
                                   json_object_new_string(std::string(64, 'b').c_str()));
        json_object_object_add(required, field, fingerprints);
    }
    json_object_object_add(value.get(), "requirements", required);
    return value;
}
} // namespace
int main() {
    const std::filesystem::path reference = "tests/fixtures/update-metadata";
    auto known = u::verify_metadata(reference / "latest.json", reference / "latest.sig",
                                    reference / "public.pem", 2, 1000);
    assert(known.release == (u::Release{"0.1.1", 2}));
    assert(known.notes == "Fixture release signed by OpenSSL CLI");
    const auto signed_bytes = ct::read_file(reference / "latest.json", u::MaxMetadata);
    const auto signed_signature = ct::read_file(reference / "latest.sig", 64);
    const auto signed_key = ct::read_file(reference / "public.pem", 16384);
    u::verify_signed_bytes(signed_bytes, signed_signature, signed_key);
    rejects([&] { u::verify_signed_bytes(signed_bytes + '\0', signed_signature, signed_key); });
    rejects([&] { u::verify_signed_bytes(signed_bytes, signed_signature.substr(1), signed_key); });
    const auto root = std::filesystem::temp_directory_path() /
                      ("controller-utils-update-metadata-" + std::to_string(getpid()));
    assert(std::filesystem::create_directory(root));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::filesystem::remove_all(path);
        }
    } cleanup{root};
    auto key = new_key();
    const auto manifest = root / "latest.json", signature = root / "latest.sig",
               public_path = root / "assets/update-public.pem";
    public_key(public_path, key.get());
    assert(!u::channel(root));
    for (const auto good : {"https://updates.example.test/stable/", "HTTPS://example.test/",
                            "https://[::1]:443/stable/"})
        assert(u::fixed_https_directory(good) == good);
    for (const auto bad : {"http://example.test/", "https://example.test", "https:///",
                           "https://u:p@example.test/", "https://example.test/?query/",
                           "https://example.test/#fragment/", "https://example.test/\n/"})
        rejects([&] { u::fixed_https_directory(bad); });
    rejects(
        [] { u::fixed_https_directory(std::string("https://example.test/") + '\0' + "other/"); });
    ct::atomic_write(root / "assets/update-channel.json",
                     R"({"url":"https://example.test/stable/"})");
    assert(u::channel(root) == "https://example.test/stable/");
    std::filesystem::remove(public_path);
    rejects([&] { u::channel(root); });
    public_key(public_path, key.get());
    auto installed = u::Release{"0.1.0", 1}.to_json();
    ct::atomic_write(root / "release.json", j::encode(installed.get()));
    assert(u::release(root) == (u::Release{"0.1.0", 1}));
    for (const auto invalid :
         {R"({"version":"0.1.0","sequence":0,"platform":"linuxarm64"})",
          R"({"version":"0.1.0","sequence":18446744073709551615,"platform":"linuxarm64"})",
          R"({"version":"0.1.0","sequence":true,"platform":"linuxarm64"})",
          R"({"version":"0.1.0","sequence":1,"platform":"linux64"})",
          R"({"version":"0000001.2.0","sequence":1,"platform":"linuxarm64"})",
          R"({"version":"0.2","sequence":1,"platform":"linuxarm64"})"}) {
        auto value = j::parse(invalid);
        rejects([&] { u::Release::from_json(value.get()); });
    }
    for (const auto backend : {ct::TrackingBackend::Tracker}) {
        auto value = metadata(backend);
        sign(manifest, signature, key.get(), value.get());
        auto verified = u::verify_metadata(manifest, signature, public_path, 2, 1000);
        assert(verified.release == (u::Release{"0.1.1", 2}));
        assert(j::string(j::field(verified.requirements.get(), "backend")) ==
               u::backend_name(backend));
        rejects([&] { u::verify_metadata(manifest, signature, public_path, 3, 1000); });
        rejects([&] { u::verify_metadata(manifest, signature, public_path, 0, 2000); });
        for (const auto name : {"driver", "helper", "runtime"}) {
            auto invalid = j::parse(j::encode(value.get()));
            auto fingerprints = j::field(j::field(invalid.get(), "requirements"), name);
            json_object_object_add(fingerprints, "unexpected",
                                   json_object_new_string(std::string(64, 'a').c_str()));
            sign(manifest, signature, key.get(), invalid.get());
            rejects([&] { u::verify_metadata(manifest, signature, public_path, 0, 1000); });
        }
    }
    {
        auto value = metadata(ct::TrackingBackend::Tracker);
        json_object_object_add(j::field(value.get(), "requirements"), "backend",
                               json_object_new_string("native-service"));
        sign(manifest, signature, key.get(), value.get());
        rejects([&] { u::verify_metadata(manifest, signature, public_path, 0, 1000); });
    }
    for (const auto &[name, replacement] : std::vector<std::pair<const char *, std::string>>{
             {"schema", "2"},
             {"schema", "true"},
             {"expires", "1"},
             {"sequence", "0"},
             {"platform", "\"linux64\""},
             {"artifact", "\"../controller-utils-test-linuxarm64.tar.gz\""},
             {"sha256", "\"WRONG\""},
             {"size", std::to_string(u::MaxArchive + 1)},
             {"size", "0"},
             {"size", "true"},
             {"notes", "\"" + std::string(241, 'x') + "\""}}) {
        auto value = metadata(ct::TrackingBackend::Tracker);
        auto field = j::parse(replacement + " ");
        json_object_object_add(value.get(), name, field.release());
        sign(manifest, signature, key.get(), value.get());
        rejects([&] { u::verify_metadata(manifest, signature, public_path, 0, 1000); });
    }
    auto value = metadata(ct::TrackingBackend::Tracker);
    sign(manifest, signature, key.get(), value.get());
    ct::atomic_write(manifest, ct::read_file(manifest, u::MaxMetadata) + " ");
    rejects([&] { u::verify_metadata(manifest, signature, public_path, 0, 1000); });
    sign(manifest, signature, key.get(), value.get());
    auto wrong_key = new_key();
    public_key(public_path, wrong_key.get());
    rejects([&] { u::verify_metadata(manifest, signature, public_path, 0, 1000); });
    public_key(public_path, key.get());
    for (const auto bytes : {size_t(0), size_t(63), size_t(65)}) {
        ct::atomic_write(signature, std::string(bytes, 'x'));
        rejects([&] { u::verify_metadata(manifest, signature, public_path, 0, 1000); });
    }
    std::string unicode;
    for (unsigned i = 0; i < 240; ++i)
        unicode += "ä";
    json_object_object_add(value.get(), "notes", json_object_new_string(unicode.c_str()));
    for (unsigned cycle = 0; cycle < 100; ++cycle) {
        sign(manifest, signature, key.get(), value.get());
        assert(u::verify_metadata(manifest, signature, public_path, 0, 1000).notes == unicode);
    }
    ct::atomic_write(manifest, std::string(u::MaxMetadata + 1, ' '));
    rejects([&] { u::verify_metadata(manifest, signature, public_path, 0, 1000); });
    std::cout << "update metadata: Ed25519, 100 signatures, native schema, selected backend, "
                 "compatibility and URL boundaries passed\n";
}
