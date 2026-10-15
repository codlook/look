#include "look/sqlite_client.h"
#include "look/format.h"   // look_format_double (dilin tek double formati)
#include "sqlite3/sqlite-amalgamation-3530400/sqlite3.h"
#include <stdexcept>
#include <cstdio>
#include <string>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <set>
#include <map>
#include <memory>
#include <cctype>

namespace look {

// ── Yazma kapısı: aynı dosyaya yazan bağlantılar bu süreçte SIRAYLA yazar ─────
// SQLite tek yazıcıya izin verir; ötekiler busy_timeout içinde aralıklı uyuyup yeniden dener.
// Bu deneme ADİL DEĞİL: 20 eşzamanlı yazıcıda bazıları sırasını sürekli kaçırıp süreyi aşıyor
// ve "database is locked" (kod 5) ile 500 dönüyordu (ölçüldü: 2 sn'de 60 sn'de 27 hata, 5 sn'de
// de 3 — en uzun bekleme 5,5 sn). Bu kapı süreç içindeki yazıcıları bilet sırasıyla kuyruğa
// sokar: kimse sırasını kaybetmez. Okumalar (WAL) kapıdan geçmez. Başka programların aynı
// dosyaya yazmaları için SQLite'ın kendi beklemesi (busy_timeout) yerinde kalır.
// Açık işlem (BEGIN ... COMMIT/ROLLBACK) kapıyı işlem bitene dek tutar.
struct SqliteWriteGate {
    std::mutex              m;
    std::condition_variable cv;
    uint64_t                next = 0, serving = 0;
    std::set<uint64_t>      abandoned;   // süresi dolup vazgeçen biletler — sıra atlanır
    bool acquire(std::chrono::milliseconds limit) {
        std::unique_lock<std::mutex> lk(m);
        uint64_t t = next++;
        if (cv.wait_for(lk, limit, [&] { return serving == t; })) return true;
        abandoned.insert(t);
        return false;
    }
    void release() {
        std::lock_guard<std::mutex> lk(m);
        ++serving;
        while (abandoned.erase(serving)) ++serving;
        cv.notify_all();
    }
};
static std::shared_ptr<SqliteWriteGate> sqlite_gate_for(const std::string& path) {
    static std::mutex mu;
    static std::map<std::string, std::shared_ptr<SqliteWriteGate>> gates;
    std::lock_guard<std::mutex> lk(mu);
    auto& g = gates[path];
    if (!g) g = std::make_shared<SqliteWriteGate>();
    return g;
}
static bool sqlite_starts_begin(const std::string& sql) {
    size_t i = 0;
    while (i < sql.size() && std::isspace((unsigned char)sql[i])) ++i;
    static const char kw[] = "BEGIN";
    for (int k = 0; k < 5; ++k, ++i)
        if (i >= sql.size() || std::toupper((unsigned char)sql[i]) != kw[k]) return false;
    return true;
}
static constexpr std::chrono::milliseconds SQLITE_GATE_LIMIT{10000};

void SqliteClient::gate_before(sqlite3_stmt* stmt, const std::string& sql) {
    if (!gate_ || gate_held_) return;
    if (sqlite3_stmt_readonly(stmt) && !sqlite_starts_begin(sql)) return;
    if (!gate_->acquire(SQLITE_GATE_LIMIT))
        throw std::runtime_error("sqlite: step error: database is locked: waited 10 s for the other "
                                 "writers of this database in this process");
    gate_held_ = true;
}
void SqliteClient::gate_after() {
    // Açık işlem sürüyorsa (autocommit kapalı) kapı tutulur; işlem bitince bırakılır.
    if (gate_held_ && (!db_ || sqlite3_get_autocommit(db_))) {
        gate_held_ = false;
        gate_->release();
    }
}

// double formatı → look/format.h look_format_double (dilin tek formatı, NaN/Inf dahil)

// SQLite'in TÜM lazy bir-kez global init'lerini SÜREÇ BAŞINDA, thread'lerden ÖNCE seri
// tetikle (bkz. sqlite_client.h). sqlite3_initialize() isInit'i; ":memory:" aç + randomblob
// PRNG seed'ini (unixRandomness) + pcache/VFS'i seri kurar. Aksi halde concurrent-open, bu
// benign double-checked-locking fast-path'lerinde TSan data-race verir (t4 enforced guard CI'da
// yakaladı: önce sqlite3_initialize, sonra unixRandomness). Seri warm-up → global yazımlar tüm
// worker okumalarından happens-before olur → yarış yok. Idempotent.
void sqlite_global_init() {
    sqlite3_initialize();
    sqlite3* db = nullptr;
    if (sqlite3_open(":memory:", &db) == SQLITE_OK && db) {
        sqlite3_exec(db, "SELECT randomblob(32)", nullptr, nullptr, nullptr);  // PRNG seed
        sqlite3_close(db);
    }
}

SqliteClient::SqliteClient() = default;

SqliteClient::~SqliteClient() {
    close();
}

void SqliteClient::open(const std::string& path) {
    // Fail-loud: SQLITE_THREADSAFE derleme bayrağı sessizce =0'a dönerse, çok thread'li
    // worker havuzu SQLite global statiklerini bozar (bkz. CMakeLists =2). Ucuz kontrol.
    if (sqlite3_threadsafe() == 0)
        throw std::runtime_error(
            "sqlite: library built with SQLITE_THREADSAFE=0 — UNSAFE with the multi-threaded worker "
            "pool. CMakeLists must set SQLITE_THREADSAFE=2.");
    if (db_) close();
    int rc = sqlite3_open(path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::string err = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("sqlite: cannot open database '" + path + "': " + err);
    }
    gate_ = (path == ":memory:") ? nullptr : sqlite_gate_for(path);   // ":memory:" her açılışta ayrı veritabanı
    // D-01: busy_timeout İLK İŞ — eşzamanlı yazıcı BUSY'de anında hata almak yerine bekler.
    // WAL tek yazıcıya izin verir; busy_timeout olmadan pool'daki ikinci bağlantı "database
    // is locked" ile hemen döner. Bu süreçteki yazıcılar zaten yazma kapısında sıraya girer
    // (yukarıda); busy_timeout artık yalnız BAŞKA bir programın aynı dosyaya yazması için
    // beklenir — 5000 ms, jobs:: ile aynı. İmplicit-transaction INSERT'ler bununla kapanır;
    // explicit BEGIN için begin_stmt() "BEGIN IMMEDIATE" verir — SQLITE_BUSY_SNAPSHOT
    // busy_timeout ile retry EDİLEMEZ.
    sqlite3_busy_timeout(db_, 5000);
    // WAL modu — okuma/yazma çakışmasını azaltır
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    // NOT: synchronous varsayılan (FULL) BIRAKILDI. synchronous=NORMAL dayanıklılığı
    // (ACID-D) sessizce düşürür — güç kesintisi/OS çökmesinde son commit'ler kaybolabilir
    // (bozulma değil, KAYIP). Bir web framework'ünün varsayılanı bunu kullanıcıya sormadan
    // almamalı. Perf isteyen ileride bilinçli+belgeli DSN ?synchronous=NORMAL ile açar.
    // Foreign key kontrolünü etkinleştir
    sqlite3_exec(db_, "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);
}

void SqliteClient::close() {
    if (gate_held_) { gate_held_ = false; gate_->release(); }   // açık işlem kapanışta geri alınır
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

std::vector<DbRow> SqliteClient::query(const std::string& sql) {
    if (!db_) throw std::runtime_error("sqlite: no open database");

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error(std::string("sqlite: ") + sqlite3_errmsg(db_) + " — SQL: " + sql);
    }

    try { gate_before(stmt, sql); } catch (...) { sqlite3_finalize(stmt); throw; }
    struct GateRelease { SqliteClient* c; ~GateRelease() { c->gate_after(); } } gate_release{this};

    std::vector<DbRow> rows;
    int col_count = sqlite3_column_count(stmt);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        DbRow row;
        for (int i = 0; i < col_count; i++) {
            std::string col_name = sqlite3_column_name(stmt, i);
            int col_type = sqlite3_column_type(stmt, i);

            DbValue dv;
            switch (col_type) {
                case SQLITE_INTEGER:
                    dv.str  = std::to_string(sqlite3_column_int64(stmt, i));
                    dv.type = sqlite_type::INTEGER;
                    break;
                case SQLITE_FLOAT:
                    dv.str  = look_format_double(sqlite3_column_double(stmt, i));
                    dv.type = sqlite_type::FLOAT;
                    break;
                case SQLITE_NULL:
                    dv.str     = "";
                    dv.type    = sqlite_type::NUL;
                    dv.is_null = true;
                    break;
                case SQLITE_BLOB:
                    // BLOB'u hex string olarak döndür
                    {
                        const uint8_t* data = (const uint8_t*)sqlite3_column_blob(stmt, i);
                        int bytes = sqlite3_column_bytes(stmt, i);
                        std::string hex;
                        hex.reserve(bytes * 2);
                        static const char digits[] = "0123456789abcdef";
                        for (int b = 0; b < bytes; b++) {
                            hex += digits[data[b] >> 4];
                            hex += digits[data[b] & 0xF];
                        }
                        dv.str  = hex;
                        dv.type = sqlite_type::BLOB;
                    }
                    break;
                default: // SQLITE_TEXT
                    {
                        const char* txt = (const char*)sqlite3_column_text(stmt, i);
                        int tb = sqlite3_column_bytes(stmt, i);
                        dv.str  = txt ? std::string(txt, (size_t)tb) : "";  // NUL-safe (length-aware)
                        dv.type = sqlite_type::TEXT;
                    }
                    break;
            }
            row.push_back({col_name, dv});
        }
        rows.push_back(std::move(row));
    }

    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        throw std::runtime_error(std::string("sqlite: step error: ") + sqlite3_errmsg(db_) + " (code " + std::to_string(sqlite3_extended_errcode(db_)) + ")");
    }

    // INSERT/UPDATE/DELETE için istatistikleri güncelle
    if (rows.empty()) {
        affected_rows_  = sqlite3_changes(db_);
        last_insert_id_ = sqlite3_last_insert_rowid(db_);
    }

    return rows;
}

std::vector<DbRow> SqliteClient::execute(const std::string& sql, const std::vector<DbParam>& params) {
    if (!db_) throw std::runtime_error("sqlite: no open database");

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
        throw std::runtime_error(std::string("sqlite: ") + sqlite3_errmsg(db_));

    // Placeholder SAYIM kontrolü (fail-loud, Go db.Query gibi) — SQLite bind edilmeyen '?'yi
    // sessizce NULL sayar; eksik/fazla parametre gizli bug yapardı. SQLite'ın KENDİ sayımı
    // (backtick-ident, string, yorum içindeki '?' hariç) → eski lehçe-parser'ın kör-noktası yok.
    int want = sqlite3_bind_parameter_count(stmt);
    if (want != (int)params.size()) {
        sqlite3_finalize(stmt);
        throw std::runtime_error("db: parameter count mismatch — SQL " + std::to_string(want) +
            " placeholder ('?') iceriyor, " + std::to_string(params.size()) + " parameters given.");
    }

    // Parametre binding — gerçek prepared statement, string escape yok
    for (int i = 0; i < (int)params.size(); i++) {
        const auto& p = params[i];
        switch (p.kind) {
            case DbParam::NULL_VAL:  sqlite3_bind_null(stmt, i+1); break;
            case DbParam::INT_VAL:   sqlite3_bind_int64(stmt, i+1, p.i); break;
            case DbParam::FLOAT_VAL: sqlite3_bind_double(stmt, i+1, p.d); break;
            case DbParam::BOOL_VAL:  sqlite3_bind_int(stmt, i+1, p.b ? 1 : 0); break;
            case DbParam::TEXT_VAL:  sqlite3_bind_text(stmt, i+1, p.s.c_str(), (int)p.s.size(), SQLITE_TRANSIENT); break;
        }
    }

    try { gate_before(stmt, sql); } catch (...) { sqlite3_finalize(stmt); throw; }
    struct GateRelease { SqliteClient* c; ~GateRelease() { c->gate_after(); } } gate_release{this};

    std::vector<DbRow> rows;
    int col_count = sqlite3_column_count(stmt);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        DbRow row;
        for (int i = 0; i < col_count; i++) {
            std::string col_name = sqlite3_column_name(stmt, i);
            int col_type = sqlite3_column_type(stmt, i);
            DbValue dv;
            switch (col_type) {
                case SQLITE_INTEGER:
                    dv.str  = std::to_string(sqlite3_column_int64(stmt, i));
                    dv.type = sqlite_type::INTEGER; break;
                case SQLITE_FLOAT:
                    dv.str  = look_format_double(sqlite3_column_double(stmt, i));
                    dv.type = sqlite_type::FLOAT; break;
                case SQLITE_NULL:
                    dv.is_null = true; dv.type = sqlite_type::NUL; break;
                default: {
                    const char* txt = (const char*)sqlite3_column_text(stmt, i);
                        int tb = sqlite3_column_bytes(stmt, i);
                        dv.str  = txt ? std::string(txt, (size_t)tb) : "";  // NUL-safe (length-aware)
                    dv.type = sqlite_type::TEXT; break;
                }
            }
            row.push_back({col_name, dv});
        }
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);

    // rc-check — query()'deki (satir 139) ile SIMETRIK. Eskiden YOKTU: sqlite3_step
    // constraint ihlalinde SQLITE_CONSTRAINT dondurur (ROW degil) → dongu cikar, satir
    // reddedilir ama exception ATILMAZDI → db::exec sessizce basarili donerdi (UNIQUE/
    // NOT NULL/FK/CHECK sessizce yutuluyordu, veri-butunlugu tuzagi). Artik query() ile
    // ayni sozlesme: fail-loud.
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        throw std::runtime_error(std::string("sqlite: step error: ") + sqlite3_errmsg(db_) + " (code " + std::to_string(sqlite3_extended_errcode(db_)) + ")");
    }

    if (rows.empty()) {
        affected_rows_  = sqlite3_changes(db_);
        last_insert_id_ = sqlite3_last_insert_rowid(db_);
    }
    return rows;
}

} // namespace look
