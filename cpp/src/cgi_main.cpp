#include "look/lexer.h"
#include "look/parser.h"
#include "look/runtime_host.h"
#include "look/logger.h"
#include "look/web.h"
#include "look/runtime_init.h"   // runtime_init (CA probe + SQLite init, süreç başı — 3 giriş noktası ortak)
#include "look/session_files.h"  // yanıttan sonra oturum temizliği (kısa ömürlü süreç)
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <cstring>
#ifdef _WIN32
#  include <io.h>
#  include <fcntl.h>
#endif

namespace fs = std::filesystem;

// Paylaşılan web motoru — http_main.cpp (lk-fcgi ile aynı)
bool look_http_engine_ensure(const std::string& script_path, std::string& err);
void look_app_dispatch(look::WebContext& web, std::ostringstream& output, const std::string& prof_path);
void look_http_engine_set_quiet(bool quiet);   // açılış bilgi satırları yok — her istek ayrı süreç

// ── Error response ────────────────────────────────────────────────────────────

static bool is_production() {
    const char* env = std::getenv("APP_ENV");
    const char* dbg = std::getenv("APP_DEBUG");
    bool debug = dbg && std::string(dbg) == "true";
    bool dev   = env && std::string(env) == "development";
    return !dev && !debug;
}

// development: detayli hata | production: temiz sayfa
static void send_error(int status, const std::string& title, const std::string& msg) {
    // Her zaman JSON — bu bir API runtime'i
    std::cout << "Status: " << status << " " << title << "\r\n"
              << "Content-Type: application/json; charset=utf-8\r\n"
              << "Access-Control-Allow-Origin: *\r\n\r\n";

    if (is_production()) {
        // Production: kullaniciya genel mesaj, detay sadece log'a gider
        // "error"/"code" = İngilizce anahtarlar; "hata"/"kod" 1.x boyunca yanında kalır.
        std::cout << "{\"ok\":false,\"error\":\"Server error. Please try again.\",\"code\":" << status
                  << ",\"hata\":\"Server error. Please try again.\","
                  << "\"kod\":" << status << "}\n";
    } else {
        // Development: hata detayi JSON icinde
        // msg icindeki ozel karakterleri escape et
        std::string safe;
        for (char c : msg) {
            if      (c == '"')  safe += "\\\"";
            else if (c == '\\') safe += "\\\\";
            else if (c == '\n') safe += "\\n";
            else if (c == '\r') safe += "\\r";
            else                safe += c;
        }
        std::cout << "{\"ok\":false,\"error\":\"" << safe << "\",\"code\":" << status
                  << ",\"hata\":\"" << safe << "\","
                  << "\"kod\":" << status << "}\n";
    }
}

// ── Resolve script path ───────────────────────────────────────────────────────

#include "look/path_guard.h"   // kanonik symlink/traversal kapisi (string kontrolu yetmez)

static std::string resolve_script(int argc, char* argv[]) {
    // Path traversal reddi — çözülen .lk client-etkili CGI env'lerinden türer ve
    // LOOK tarafından KOD olarak yürütülür; ".." doc-root dışına çıkıp arbitrary
    // .lk çalıştırma (RCE) sağlar. (fcgi_main.cpp resolve_script ile aynı guard.)
    auto no_traversal = [](const std::string& s) {
        return s.find("../") == std::string::npos &&
               s.find("..\\") == std::string::npos &&
               !(s.size() >= 2 && s.substr(s.size() - 2) == "..");
    };
    auto ends_lk = [&](const char* s) -> bool {
        if (!s || !*s) return false;
        std::string str(s);
        if (!no_traversal(str)) return false;
        return str.size() > 3 && str.substr(str.size() - 3) == ".lk";
    };

    const char* path_translated = std::getenv("PATH_TRANSLATED");
    const char* script_filename = std::getenv("SCRIPT_FILENAME");
    const char* path_info       = std::getenv("PATH_INFO");
    const char* document_root   = std::getenv("DOCUMENT_ROOT");

    if (ends_lk(path_translated)) return path_translated;
    if (ends_lk(script_filename)) return script_filename;

    // Apache Action directive: PATH_INFO = /index.lk/menu/burger-cafe
    // Extract just the .lk script part: DOCUMENT_ROOT + /index.lk
    if (path_info && *path_info && document_root && *document_root && no_traversal(path_info)) {
        std::string pi(path_info);
        size_t lk_pos = pi.find(".lk");
        // KANONIK KAPI: yol DOCUMENT_ROOT+PATH_INFO'dan LOOK'ca kurulur; string no_traversal
        // symlink'i goremez → weakly_canonical doc disina cikan (symlink/..) adayi reddeder.
        std::string cand = (lk_pos != std::string::npos)
            ? std::string(document_root) + pi.substr(0, lk_pos + 3)
            : std::string(document_root) + pi;
        if (look::path_under_root(document_root, cand)) return cand;
        return "";
    }

    if (argc >= 2) return argv[1];
    return "";
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
#ifdef _WIN32
    // CGI çıktısı byte-exact olmalı: text-mode \n→\r\n dönüşümü CGI header/body
    // yapısını bozar (kodda zaten \r\n var → \r\r\n olur, body kaybolur).
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    // Statik OpenSSL: gömülü CA yolu derleme dağıtımına (AlmaLinux) sabitlenir; başka
    // dağıtımda (Ubuntu) o yol yok → default_verify_paths CA bulamaz, GEÇERLİ sertifika
    // bile reddedilir. main.cpp/fcgi_main.cpp bunu startup'ta yapıyordu; cgi_main ATLIYORDU
    // → CGI modunda http:: + PostgreSQL TLS (verify VARSAYILAN) dağıtımlar arası kırıktı.
    // SSL_CERT_FILE'i process-global set eder; DB istemcilerinin default_verify_paths'i okur.
    look::runtime_init();  // tüm süreç-başı init (CA probe + SQLite bir-kez init) — 3 giriş noktası ortak
    // CGI'da süreç tek istek yaşar: süresi dolan oturum dosyalarının temizliği (zamanı geldiyse)
    // yanıt gönderildikten SONRA yapılır. Çıktı önce kapatılır: web sunucusu yanıtın bittiğini
    // çıktının kapanmasından anlar, ziyaretçi taramayı beklemez. Her dönüş yolunu kapsar.
    look::session_gc_short_lived(true);
    struct SessionGcAfterResponse {
        ~SessionGcAfterResponse() {
            std::cout.flush();
            std::fflush(stdout);
#ifndef _WIN32
            int nul = ::open("/dev/null", O_WRONLY);
            if (nul >= 0) { ::dup2(nul, 1); ::close(nul); }
#endif
            look::session_gc_run_pending();
        }
    } session_gc_after_response;
    std::string script_path = resolve_script(argc, argv);

    if (script_path.empty()) {
        send_error(400, "Bad Request", "No LOOK script specified.");
        return 1;
    }

    try {
        fs::path script(script_path);

        // ── .env path fix ─────────────────────────────────────────────────────
        // CGI'da working directory Apache'nin kendi klasoru olabilir.
        // .env dosyasini script'in bulundugu klasorde ara.
        fs::path script_dir = script.parent_path();
        if (!script_dir.empty()) {
            // Script dizinine git — env() ve log:: buradan calisacak
#ifdef _WIN32
            _wchdir(script_dir.wstring().c_str());
#else
            (void)chdir(script_dir.string().c_str());
#endif
        }

        // LOOK 2 (tek motor, adım 7): CGI de lk-fcgi'nin web motorunu kullanır — kurulum (üst düzey
        // VM'de bir kez), rota eşleştirme, 404, script modu, hata yanıtı. Eskiden CGI betiği her
        // istekte yorumlayıcıda baştan çalıştırıyordu: aynı program CGI'da ve FastCGI'da iki ayrı
        // motorla koşuyordu. Bir CGI süreci tek istek yaşar: "kurulum + bir istek" = eski "betiği bir
        // kez çalıştır".
        std::string setup_err;
        look_http_engine_set_quiet(true);   // stderr = Apache hata günlüğü: başarılı istekte sessiz
        if (!look_http_engine_ensure(script.string(), setup_err)) {
            look::Logger::instance().log(look::LogLevel::LOG_ERROR, "CGI",
                "Setup error: " + setup_err + " | " + script_path);
            send_error(500, "Setup Error", setup_err);
            return 1;
        }

        look::WebContext web;
        web.init_from_cgi();
        std::ostringstream output;
        look_app_dispatch(web, output, web.path);

        // ── Response ──────────────────────────────────────────────────────────
        // FastCGI ile aynı: response:: gövdesi varsa o, yoksa print çıktısı.
        std::cout << web.build_headers();
        std::cout << (web.response_body.empty() ? output.str() : web.response_body);

    } catch (const std::exception& e) {
        // Production: log'a yaz, kullaniciya temiz sayfa
        look::Logger::instance().log(look::LogLevel::LOG_ERROR, "CGI",
            std::string("Uncaught exception: ") + e.what() +
            " | Script: " + script_path);

        send_error(500, "Internal Server Error", e.what());
        return 1;
    }

    return 0;
}
