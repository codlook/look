cmake_minimum_required(VERSION 3.20)
project(looklang VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Sanitizer build — güvenlik/fuzzing turu ──────────────────────────────────
# -DLOOK_SANITIZE=address           → AddressSanitizer (overflow, use-after-free)
# -DLOOK_SANITIZE=undefined         → UBSan (integer overflow, null deref)
# -DLOOK_SANITIZE=address,undefined → ikisi birlikte
# Kullanım: cmake -B build-asan -DLOOK_SANITIZE=address,undefined
if(LOOK_SANITIZE)
    message(STATUS "Sanitizer aktif: ${LOOK_SANITIZE}")
    add_compile_options(-fsanitize=${LOOK_SANITIZE} -fno-omit-frame-pointer -g)
    # Sanitizer runtime'ını statik linkle — binary self-contained olur, toolset
    # kurulu olmayan container/VPS'te de çalışır (libasan.so bağımlılığı olmaz).
    # TSan ve ASan farklı statik-runtime bayrakları ister (karıştırılamaz).
    if(LOOK_SANITIZE MATCHES "thread")
        add_link_options(-fsanitize=${LOOK_SANITIZE} -static-libtsan)
    else()
        add_link_options(-fsanitize=${LOOK_SANITIZE} -static-libasan -static-libubsan)
    endif()
else()
    # ── Aşama A1: Link-Time Optimization ─────────────────────────────────────
    # Sanitizer YOKken LTO aç — modüller arası inline (VM ↔ Value ↔ builtin).
    # march=native BİLİNÇLİ olarak yok: binary farklı CPU'lara dağıtılıyor.
    # Sadece Release/RelWithDebInfo'da; Debug'da kapalı (derleme hızı).
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        include(CheckIPOSupported)
        check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_msg)
        if(_ipo_ok)
            set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
            message(STATUS "LTO (IPO) aktif")
        else()
            message(STATUS "LTO desteklenmiyor: ${_ipo_msg}")
        endif()
    endif()
endif()

include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/src)

# sqlite3.c saf C kodudur — C++ kurallarıyla derlenmez
set_source_files_properties(
    src/sqlite3/sqlite-amalgamation-3470200/sqlite3.c
    PROPERTIES LANGUAGE C
)

# SQLite: thread-safe modu kapat (single-thread warm start)
# -DSQLITE_THREADSAFE=0 → küçük binary, mutex overhead yok
add_compile_definitions(SQLITE_THREADSAFE=0)

# Platform-specific fiber backend
if(WIN32)
    set(LOOK_FIBER src/fiber_win.cpp)
    # Proje-geneli (hedeflerden ÖNCE): NOMINMAX (windows.h min/max makroları
    # std::min/max'i bozmasın), WIN32_LEAN_AND_MEAN, CRT uyarılarını sustur.
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
else()
    set(LOOK_FIBER src/fiber_posix.cpp)
endif()

set(LOOK_CORE
    src/token.cpp
    src/lexer.cpp
    src/parser.cpp
    src/interpreter.cpp
    src/stdlib.cpp
    src/web.cpp
    src/web_stdlib.cpp
    src/mysql_client.cpp
    src/sqlite_client.cpp
    src/postgres_client.cpp
    src/sqlite3/sqlite-amalgamation-3470200/sqlite3.c
    src/logger.cpp
    src/extra_stdlib.cpp
    src/file_stdlib.cpp
    src/date_stdlib.cpp
    src/websocket.cpp
    src/sse.cpp
    src/timer_stdlib.cpp
    src/template_stdlib.cpp
    src/http_client.cpp
    src/http_stdlib.cpp
    src/cache_stdlib.cpp
    src/queue_stdlib.cpp
    src/jobs_stdlib.cpp
    src/mail_stdlib.cpp
    src/parallel_runtime.cpp
    src/db_async_pool.cpp
    src/installer.cpp
    src/resp_client.cpp
    src/miniz/miniz.c
    src/test_runner.cpp
    src/repl.cpp
    src/linenoise/linenoise.c
    ${LOOK_FIBER}
)

# OpenSSL'e bağlı sunucu özellikleri (SMTP/IMAP/DKIM/DNS) yalnız POSIX'te.
# Windows = geliştirme ortamı (XAMPP): mail SUNUCUSU yok, http:: Schannel kullanır,
# sıfır dış bağımlılık (OpenSSL DLL yok). deliver_maildir Windows'ta mail_win.cpp'de.
if(WIN32)
    list(APPEND LOOK_CORE src/mail_win.cpp)
else()
    list(APPEND LOOK_CORE src/smtp_server.cpp src/imap_server.cpp src/dkim.cpp src/dns.cpp)
endif()

add_executable(look
    src/main.cpp
    src/event_loop.cpp
    src/compiler.cpp   # C9: CLI-VM (opt-in LOOK_CLI_VM) — bytecode derleyici
    src/vm.cpp         # C9: CLI-VM — bytecode yürütücü
    src/builtins.cpp   # C9: builtin_names/builtin_index (VM builtin tablosu)
    ${LOOK_CORE}
)

add_executable(look-cgi
    src/cgi_main.cpp
    src/event_loop.cpp
    ${LOOK_CORE}
)

add_executable(fiber-test
    src/fiber_test.cpp
    src/logger.cpp
    ${LOOK_FIBER}
)

add_executable(imap-m1-test
    src/imap_m1_test.cpp
    src/imap_server.cpp
    src/logger.cpp
)

add_executable(look-fcgi
    src/fcgi_main.cpp
    src/http_main.cpp
    src/http_server.cpp
    src/event_loop.cpp
    src/compiler.cpp
    src/builtins.cpp
    src/vm.cpp
    ${LOOK_CORE}
)

# ── Platform bağımsız linkleme ────────────────────────────────────────────────
if(WIN32)
    target_link_libraries(look      ws2_32 bcrypt)
    target_link_libraries(look-cgi  ws2_32 bcrypt)
    target_link_libraries(look-fcgi ws2_32 bcrypt)
    # Ana thread + std::thread worker'ları 8 MB stack (Linux varsayılanıyla aynı).
    # Windows default 1 MB, tree-walk interpreter'ın MAX_CALL_DEPTH=500 recursion
    # guard'ı ateşlemeden native stack'i taşırıp çökertiyordu (STATUS_STACK_OVERFLOW).
    # 8 MB'da guard 500'de temiz ateşler → derin/sonsuz recursion yakalanabilir hata.
    if(MSVC)
        target_link_options(look      PRIVATE /STACK:8388608)
        target_link_options(look-cgi  PRIVATE /STACK:8388608)
        target_link_options(look-fcgi PRIVATE /STACK:8388608)
    endif()
    set_target_properties(look      PROPERTIES OUTPUT_NAME "lk")
    set_target_properties(look-cgi  PROPERTIES OUTPUT_NAME "lk-cgi")
    set_target_properties(look-fcgi PROPERTIES OUTPUT_NAME "lk-fcgi")
else()
    # Linux: pthread (std::filesystem, socket), dl (sqlite3 dlopen), ssl+crypto (http:: TLS)
    # resolv: dns_txt_lookup() via res_query() — DKIM + SPF
    find_package(Threads REQUIRED)
    # Portatif tek binary: OpenSSL'i STATİK linkle (varsayılan AÇIK).
    # AlmaLinux 8'de (OpenSSL 1.1) derlenen binary, Ubuntu 22.04+ (OpenSSL 3.0)
    # üzerinde libssl.so.1.1 bulunamadığı için çalışmıyordu. Statik link ile
    # tek binary tüm dağıtımlarda çalışır ("sıfır bağımlılık" — glibc zaten
    # 2.28'de derlendiği için ileri-uyumlu). Kapatmak: -DLOOK_STATIC_SSL=OFF
    option(LOOK_STATIC_SSL "OpenSSL'i statik linkle (portatif binary)" ON)
    if(LOOK_STATIC_SSL)
        set(OPENSSL_USE_STATIC_LIBS TRUE)
    endif()
    find_package(OpenSSL REQUIRED)
    # Statik libcrypto.a dlopen + zlib gerektirebilir → dl, z ekli
    target_link_libraries(look      Threads::Threads dl OpenSSL::SSL OpenSSL::Crypto resolv)
    target_link_libraries(look-cgi  Threads::Threads dl OpenSSL::SSL OpenSSL::Crypto resolv)
    target_link_libraries(look-fcgi Threads::Threads dl OpenSSL::SSL OpenSSL::Crypto resolv)
    target_link_libraries(imap-m1-test Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
    if(LOOK_STATIC_SSL)
        # Statik OpenSSL 1.1 libcrypto zlib sembollerine bağımlı olabilir
        find_library(ZLIB_A NAMES libz.a z)
        if(ZLIB_A)
            target_link_libraries(look ${ZLIB_A})
            target_link_libraries(look-cgi ${ZLIB_A})
            target_link_libraries(look-fcgi ${ZLIB_A})
        endif()
    endif()

    # GCC 8 ve altında filesystem ayrı kütüphane
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
        target_link_libraries(look      stdc++fs)
        target_link_libraries(look-cgi  stdc++fs)
        target_link_libraries(look-fcgi stdc++fs)
    endif()

    # Linux'ta binary adlarında .exe uzantısı olmasın
    set_target_properties(look      PROPERTIES OUTPUT_NAME "lk")
    set_target_properties(look-cgi  PROPERTIES OUTPUT_NAME "lk-cgi")
    set_target_properties(look-fcgi PROPERTIES OUTPUT_NAME "lk-fcgi")
endif()
