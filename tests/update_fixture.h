#pragma once
#include "update/archive.h"
#include "platform/linux/files.h"
#include <archive.h>
#include <archive_entry.h>
#include <cassert>
#include <cstring>
#include <elf.h>
#include <memory>
#include <openssl/evp.h>
#include <openssl/pem.h>

namespace update_fixture {
namespace u = ct::update;
namespace j = ct::json;
struct Entry {
    std::string name, data, link;
    mode_t type = AE_IFREG;
    bool hardlink = false;
};
inline std::string program() {
    Elf64_Ehdr header{};
    std::memcpy(header.e_ident, ELFMAG, SELFMAG);
    header.e_ident[EI_CLASS] = ELFCLASS64;
    header.e_ident[EI_DATA] = ELFDATA2LSB;
    header.e_ident[EI_VERSION] = EV_CURRENT;
    header.e_type = ET_DYN;
    header.e_machine = EM_AARCH64;
    header.e_version = EV_CURRENT;
    header.e_ehsize = sizeof(header);
    header.e_phoff = sizeof(header);
    header.e_phentsize = sizeof(Elf64_Phdr);
    header.e_phnum = 1;
    std::string bytes(sizeof(header) + sizeof(Elf64_Phdr), '\0');
    std::memcpy(bytes.data(), &header, sizeof(header));
    return bytes;
}
inline std::vector<Entry> payload() {
    std::vector<Entry> result{
        {"controller-utils", program(), ""},
        {"release.json", R"({"version":"0.1.1","sequence":2,"platform":"linuxarm64"})", ""},
        {"LICENSE", "MIT test notice", ""},
        {"third_party/openvr/LICENSE", "OpenVR test notice", ""}};
    for (const auto name : u::PackageFiles)
        result.push_back(
            {name, std::string(name) == "controller-utils-launcher" ? program() : "package fixture",
             ""});
    for (const auto asset : u::Assets) {
        auto data = "fixture-" + std::string(asset);
        if (std::string(asset) == "update-channel.json")
            data = R"({"url":"https://updates.example.test/stable/"})";
        else if (std::string(asset) == "update-public.pem")
            data = ct::read_file("tests/fixtures/update-metadata/public.pem", 16384);
        result.push_back({"assets/" + std::string(asset), data, ""});
    }
    for (const auto name : u::DriverFiles)
        result.push_back({"native-driver/" + std::string(name), program(), ""});
    for (const auto &name : u::helper_files(ct::TrackingBackend::Tracker))
        result.push_back({"compatibility/" + name, program(), ""});
    return result;
}
inline j::Value requirements();
inline u::Metadata make_archive(const std::filesystem::path &path,
                                const std::vector<Entry> &entries, bool gzip = true) {
    std::unique_ptr<struct archive, decltype(&archive_write_free)> output(archive_write_new(),
                                                                          archive_write_free);
    assert(output);
    assert(archive_write_set_format_pax_restricted(output.get()) == ARCHIVE_OK);
    if (gzip)
        assert(archive_write_add_filter_gzip(output.get()) == ARCHIVE_OK);
    assert(archive_write_open_filename(output.get(), path.c_str()) == ARCHIVE_OK);
    for (const auto &entry : entries) {
        std::unique_ptr<struct archive_entry, decltype(&archive_entry_free)> header(
            archive_entry_new(), archive_entry_free);
        archive_entry_set_pathname(header.get(), entry.name.c_str());
        archive_entry_set_filetype(header.get(), entry.type);
        archive_entry_set_perm(header.get(), 07777);
        archive_entry_set_size(header.get(), entry.data.size());
        if (!entry.link.empty()) {
            if (entry.hardlink)
                archive_entry_set_hardlink(header.get(), entry.link.c_str());
            else
                archive_entry_set_symlink(header.get(), entry.link.c_str());
        }
        assert(archive_write_header(output.get(), header.get()) == ARCHIVE_OK);
        if (!entry.data.empty())
            assert(archive_write_data(output.get(), entry.data.data(), entry.data.size()) ==
                   ssize_t(entry.data.size()));
    }
    assert(archive_write_close(output.get()) == ARCHIVE_OK);
    return {{"0.1.1", 2},
            4102444800,
            std::filesystem::file_size(path),
            path.filename().string(),
            ct::sha256(path),
            "fixture",
            requirements()};
}
using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
inline j::Value requirements() {
    j::Value value(json_object_new_object());
    json_object_object_add(value.get(), "backend", json_object_new_string("native-tracker"));

    for (const auto &[field, names] :
         std::vector<std::pair<const char *, std::vector<std::string>>>{
             {"driver", {u::DriverFiles.begin(), u::DriverFiles.end()}},
             {"helper", u::helper_files(ct::TrackingBackend::Tracker)},
             {"runtime", {u::RuntimeFiles.begin(), u::RuntimeFiles.end()}}}) {
        auto files = json_object_new_object();
        for (const auto &name : names)
            json_object_object_add(
                files, name.c_str(),
                json_object_new_string((std::string(field) == "runtime"
                                            ? std::string(64, 'b')
                                            : ct::sha256_bytes(program().data(), program().size()))
                                           .c_str()));
        json_object_object_add(value.get(), field, files);
    }
    return value;
}
inline Key keypair() {
    EVP_PKEY *key = EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519");
    assert(key);
    return Key(key, EVP_PKEY_free);
}
inline std::string public_key(EVP_PKEY *key) {
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), BIO_free);
    assert(bio && PEM_write_bio_PUBKEY(bio.get(), key));
    char *data = nullptr;
    const auto size = BIO_get_mem_data(bio.get(), &data);
    assert(size > 0);
    return {data, size_t(size)};
}
inline void sign(const std::filesystem::path &destination, EVP_PKEY *key,
                 const std::string &bytes) {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),
                                                                    EVP_MD_CTX_free);
    assert(context && EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key) == 1);
    std::string signature(64, '\0');
    size_t size = signature.size();
    assert(EVP_DigestSign(context.get(), reinterpret_cast<unsigned char *>(signature.data()), &size,
                          reinterpret_cast<const unsigned char *>(bytes.data()),
                          bytes.size()) == 1);
    assert(size == 64);
    ct::atomic_write(destination, signature);
}
} // namespace update_fixture
