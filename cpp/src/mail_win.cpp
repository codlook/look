// Windows: deliver_maildir — OpenSSL'siz, taşınabilir (std::ofstream + filesystem).
// XAMPP geliştirme ortamında mail:: modülü deliver_maildir'i çağırabilir; SMTP/IMAP
// SUNUCULARI Windows'ta yoktur (POSIX-only, OpenSSL'e bağlı). Bu dosya yalnız o tek
// fonksiyonu Windows için sağlar; Linux'ta smtp_server.cpp'deki gerçek impl kullanılır.
#include "look/smtp_server.h"

#include <fstream>
#include <filesystem>
#include <chrono>
#include <random>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace look {

std::string deliver_maildir(const std::string& base_dir,
                            const std::string& mailbox,
                            const SmtpMessage& msg) {
    // GÜVENLİK: mailbox doğrulaması — smtp_server.cpp'deki Linux ikizi ile AYNI.
    // userland mail::deliver_maildir üzerinden kullanıcı girdisi gelirse
    // "../.." / mutlak yol → hedef DIŞINA yazma (arbitrary file write) engellenir.
    if (mailbox.empty() || mailbox.find("..") != std::string::npos)
        throw std::runtime_error("deliver_maildir: invalid mailbox (traversal)");
    for (unsigned char c : mailbox)
        if (c == '/' || c == '\\' || c == '\0' || c < 0x20)
            throw std::runtime_error("deliver_maildir: invalid mailbox (forbidden character)");
    fs::path mbox = fs::path(base_dir) / mailbox;
    std::error_code ec;
    fs::create_directories(mbox / "new", ec);
    fs::create_directories(mbox / "cur", ec);
    fs::create_directories(mbox / "tmp", ec);

    // Benzersiz Maildir dosya adı: <epoch>.<hex-random>
    auto now = std::chrono::system_clock::now();
    long long ts = std::chrono::duration_cast<std::chrono::seconds>(
                       now.time_since_epoch()).count();
    std::random_device rd;
    std::mt19937_64 rng(rd());
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream oss;
    oss << ts << "." << std::hex << dist(rng);
    std::string unique = oss.str();

    fs::path tmp_path = mbox / "tmp" / unique;
    fs::path new_path = mbox / "new" / unique;

    {
        std::ofstream f(tmp_path, std::ios::binary);
        if (!f) throw std::runtime_error("maildir: cannot create " + tmp_path.string());
        f << "Return-Path: <" << msg.mail_from << ">\r\n";
        f.write(msg.data.data(), (std::streamsize)msg.data.size());
        if (!f) { fs::remove(tmp_path, ec); throw std::runtime_error("maildir: write error"); }
    }
    fs::rename(tmp_path, new_path, ec);
    if (ec) { fs::remove(tmp_path, ec); throw std::runtime_error("maildir: rename failed"); }
    return unique;
}

} // namespace look
