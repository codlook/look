cmake_minimum_required(VERSION 3.20)
project(looklang VERSION 1.1.5 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Sanitizer build — security/fuzzing round ─────────────────────────────────
# -DLOOK_SANITIZE=address           → AddressSanitizer (overflow, use-after-free)
# -DLOOK_SANITIZE=undefined         → UBSan (integer overflow, null deref)
# -DLOOK_SANITIZE=address,undefined → both together
# Usage: cmake -B build-asan -DLOOK_SANITIZE=address,undefined
if(LOOK_SANITIZE)
    message(STATUS "Sanitizer enabled: ${LOOK_SANITIZE}")
    if(MSVC)
        # MSVC AddressSanitizer (VS 2019 16.9+): /fsanitize=address. Only 'address'
        # is supported (no UBSan/TSan on MSVC). It CONFLICTS with LTCG(/GL) → IPO is
        # already not enabled in this branch (it is in the else branch). The ASan runtime DLL must be on PATH (MSVC bin).
        add_compile_options(/fsanitize=address)
        # Remove LTCG from the Release flags (CMake's Release default may add /GL).
        string(REPLACE "/GL" "" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
        string(REPLACE "/GL" "" CMAKE_C_FLAGS_RELEASE   "${CMAKE_C_FLAGS_RELEASE}")
    else()
        add_compile_options(-fsanitize=${LOOK_SANITIZE} -fno-omit-frame-pointer -g)
        # Link the sanitizer runtime statically — the binary is self-contained and also
        # runs in a container/VPS without the toolset installed (no libasan.so dependency).
        # TSan and ASan need different static-runtime flags (they cannot be mixed).
        if(LOOK_SANITIZE MATCHES "thread")
            add_link_options(-fsanitize=${LOOK_SANITIZE} -static-libtsan)
        else()
            add_link_options(-fsanitize=${LOOK_SANITIZE} -static-libasan -static-libubsan)
        endif()
    endif()
else()
    # ── Stage A1: Link-Time Optimization ────────────────────────────────────
    # Enable LTO when there is NO sanitizer — cross-module inlining (VM ↔ Value ↔ builtin).
    # march=native is deliberately NOT used: the binary is distributed to different CPUs.
    # Release/RelWithDebInfo only; off in Debug (build speed).
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        include(CheckIPOSupported)
        check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_msg)
        if(_ipo_ok)
            set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
            message(STATUS "LTO (IPO) enabled")
        else()
            message(STATUS "LTO desteklenmiyor: ${_ipo_msg}")
        endif()
    endif()
endif()

include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/src)

# sqlite3.c is pure C — it is not built with the C++ rules
set_source_files_properties(
    src/sqlite3/sqlite-amalgamation-3530400/sqlite3.c
    PROPERTIES LANGUAGE C
)

# compiler.cpp: every implicit narrowing is a build error. Bytecode operands are 8/16 bit;
# a value silently truncated on its way into one (emit(op, base + k, ...)) makes the VM run
# wrong code with no error. Explicit narrowing goes through u8()/u16() in compiler.h, which
# range-check. GCC/Clang only: the MSVC build has no equivalent wired in yet.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(src/compiler.cpp PROPERTIES COMPILE_OPTIONS "-Werror=conversion")
endif()

# SQLite: multi-thread mode (2) — required because the worker pool is multi-threaded.
# =0 (single-thread) also removes the mutexes of SQLite's GLOBAL statics (the sqlite3StatusUp counters,
# the sqlite3_randomness PRNG state, sqlite3_initialize, the page cache). Separate connections
# separate the per-connection state but NOT the global state;
# when two threads call open/query at the same time these global statics race (TSan: 241 reports,
# t4_sqlite_connect regression test). =2 "multi-thread" = the statics stay protected, there is
# NO per-connection mutex → performance-neutral, since the run-time pattern (each connection in one thread) is kept.
add_compile_definitions(SQLITE_THREADSAFE=2)

# Build stamp: short git SHA → `lk --version`. The product version is FIXED in project(VERSION); the stamp answers
# the "which build" question (Go's `-ldflags -X` approach: the stamp is injected EXPLICITLY at
# release time, not loaded onto every build). The release path passes `-DLOOK_BUILD=<sha>` and
# builds FRESH (portable/Docker does `rm -rf`, so the stamp is right). Dev builds read it from git (cheap).
# IMPORTANT: a release build must be FRESH (an incremental configure leaves a stale stamp) AND the artifact's
# `--version` must be extracted from the package and OBSERVED — the real guarantee is to "measure the artifact".
if(NOT DEFINED LOOK_BUILD)
    execute_process(
        COMMAND git rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE LOOK_BUILD
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()
if(NOT LOOK_BUILD)
    set(LOOK_BUILD "src")
endif()
add_compile_definitions(LOOK_BUILD="${LOOK_BUILD}")
message(STATUS "LOOK build damgasi: ${LOOK_BUILD}")

# Product version — SINGLE SOURCE: project(... VERSION) above. `lk`/`lk-fcgi` --version
# and the REPL banner read it from this compile definition (it used to be hard-coded by hand in 3 files,
# and the CMake project version had stayed at 0.1.0 = drift). A version bump is now one line: project().
add_compile_definitions(LOOK_VERSION="${PROJECT_VERSION}")

# Platform-specific fiber backend
if(WIN32)
    set(LOOK_FIBER src/fiber_win.cpp)
    # Project-wide (BEFORE the targets): NOMINMAX (so the windows.h min/max macros
    # do not break std::min/max), WIN32_LEAN_AND_MEAN, silence CRT warnings.
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
else()
    set(LOOK_FIBER src/fiber_posix.cpp)
endif()

set(LOOK_CORE
    src/token.cpp
    src/mem_budget.cpp   # memory budget: operator new/delete accounting (look/mem_budget.h)
    src/lexer.cpp
    src/parser.cpp
    src/format_src.cpp   # lk fmt — source formatter (depends on the lexer)
    src/fix_src.cpp      # lk fix — rewrites push/pop/array::pop to the new form (depends on the parser)
    src/route_group.cpp  # route::group() — setup-time prefix + middleware inheritance context
    src/value.cpp        # Value, channel
    src/runtime_host.cpp # host of the built-in modules (replaced the tree-walking interpreter)
    src/builtins.cpp   # the compiler calls is_reserved_builtin() → needed by EVERY target
                       # (look-cgi used LOOK_CORE without adding builtins separately → link failure,
                       # seen only in the 3-target portable build; ci/build-win's 2 targets missed it)
    src/runtime_init.cpp
    src/stdlib.cpp
    src/web.cpp
    src/web_stdlib.cpp
    src/mysql_client.cpp
    src/sqlite_client.cpp
    src/postgres_client.cpp
    src/sqlite3/sqlite-amalgamation-3530400/sqlite3.c
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
    src/installer.cpp
    src/resp_client.cpp
    src/miniz/miniz.c


    src/linenoise/linenoise.c
    ${LOOK_FIBER}
)

# Server features that depend on OpenSSL (SMTP/IMAP/DKIM/DNS) are POSIX-only.
# Windows = development environment (XAMPP): no mail SERVER, http:: uses Schannel,
# minimal external runtime dependencies (no OpenSSL DLL). deliver_maildir lives in mail_win.cpp on Windows.
if(WIN32)
    list(APPEND LOOK_CORE src/mail_win.cpp)
else()
    list(APPEND LOOK_CORE src/smtp_server.cpp src/imap_server.cpp src/dkim.cpp src/dns.cpp)
endif()

add_executable(look
    src/main.cpp
    src/event_loop.cpp
    src/compiler.cpp   # bytecode compiler (single engine: VM)
    src/vm.cpp         # C9: CLI VM — bytecode executor
    src/cli_runtime.cpp # lk and lk test share the same VM environment
    src/test_runner.cpp # lk test on the VM (uses cli_runtime)
    src/repl.cpp        # REPL on the VM (uses cli_runtime)
    ${LOOK_CORE}       # builtins.cpp is now in LOOK_CORE (interpreter dependency)
)

add_executable(look-cgi
    src/cgi_main.cpp
    src/http_main.cpp   # lk-cgi — the same web engine as lk-fcgi — VM
    src/http_server.cpp
    src/shutdown.cpp    # called by http_main.cpp; MSVC does not link without it (on Linux the unused call was dropped)
    src/compiler.cpp
    src/vm.cpp
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
    src/shutdown.cpp
    src/event_loop.cpp
    src/compiler.cpp
    src/vm.cpp
    ${LOOK_CORE}
)

# ── Platform-independent linking ─────────────────────────────────────────────
if(WIN32)
    target_link_libraries(look      ws2_32 bcrypt)
    target_link_libraries(look-cgi  ws2_32 bcrypt)
    target_link_libraries(look-fcgi ws2_32 bcrypt)
    # Main thread + std::thread workers get an 8 MB stack (same as the Linux default).
    # Windows default is 1 MB. The VM call stack is on the heap, but the callback chain (array::map inside
    # array::map …) re-enters the VM natively at every level; 8 MB gives the same headroom as Linux.
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
    # Portable single binary: link OpenSSL STATICALLY (ON by default).
    # A binary built on AlmaLinux 8 (OpenSSL 1.1) did not run on Ubuntu 22.04+ (OpenSSL 3.0)
    # because libssl.so.1.1 could not be found. With static linking
    # one binary runs on all distributions (minimal external runtime dependencies — glibc is already built
    # at 2.28, so it is forward-compatible). To turn it off: -DLOOK_STATIC_SSL=OFF
    option(LOOK_STATIC_SSL "OpenSSL'i statik linkle (portatif binary)" ON)
    if(LOOK_STATIC_SSL)
        set(OPENSSL_USE_STATIC_LIBS TRUE)
        # Static libcrypto.a may depend on zlib; FindOpenSSL references the
        # ZLIB::ZLIB IMPORTED target as a dependency — if find_package(ZLIB) does not create it,
        # the generate step fails with "ALIAS target missing". (Not needed in a dynamic build.)
        find_package(ZLIB QUIET)
    endif()
    find_package(OpenSSL REQUIRED)
    # Are the static libcrypto's own dependencies (libcrypto.pc Libs.private) installed? Ubuntu 26.04's
    # OpenSSL 3.5 wants -l:libjitterentropy.a -lz -lzstd; if one was missing the build only failed at the end with an unclear
    # linker error ("cannot find -l:libjitterentropy.a"). Now it stops at configure time, naming which
    # library is missing. The .pc that is checked is the file next to the libcrypto that was FOUND
    # (a portable release build supplies its own OpenSSL; the system's libcrypto.pc describes a different library).
    # If there is no .pc in that directory, no check is made.
    get_filename_component(LOOK_CRYPTO_DIR "${OPENSSL_CRYPTO_LIBRARY}" DIRECTORY)
    if(LOOK_STATIC_SSL AND OPENSSL_CRYPTO_LIBRARY MATCHES "\\.a$" AND EXISTS "${LOOK_CRYPTO_DIR}/pkgconfig/libcrypto.pc")
        find_package(PkgConfig QUIET)
        if(PKG_CONFIG_FOUND)
            execute_process(COMMAND ${CMAKE_COMMAND} -E env "PKG_CONFIG_PATH=${LOOK_CRYPTO_DIR}/pkgconfig"
                                    "PKG_CONFIG_LIBDIR=${LOOK_CRYPTO_DIR}/pkgconfig"
                                    ${PKG_CONFIG_EXECUTABLE} --libs-only-l --static libcrypto
                            OUTPUT_VARIABLE LOOK_CRYPTO_STATIC_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE
                            RESULT_VARIABLE LOOK_CRYPTO_PC_RC ERROR_QUIET)
            if(LOOK_CRYPTO_PC_RC EQUAL 0)
                separate_arguments(LOOK_CRYPTO_STATIC_LIBS UNIX_COMMAND "${LOOK_CRYPTO_STATIC_LIBS}")
                set(LOOK_CRYPTO_MISSING "")
                foreach(flag IN LISTS LOOK_CRYPTO_STATIC_LIBS)
                    if(flag MATCHES "^-l:(.+)$")
                        set(lib_file "${CMAKE_MATCH_1}")
                    elseif(flag MATCHES "^-l(.+)$")
                        set(lib_file "lib${CMAKE_MATCH_1}.a")
                    else()
                        continue()
                    endif()
                    if(lib_file MATCHES "^lib(ssl|crypto|dl|pthread|rt|m|c)\\.a$")
                        continue()   # OpenSSL itself and the libc parts
                    endif()
                    unset(LOOK_DEP_PATH CACHE)
                    find_library(LOOK_DEP_PATH NAMES "${lib_file}")
                    if(NOT LOOK_DEP_PATH)
                        list(APPEND LOOK_CRYPTO_MISSING "${lib_file}")
                    endif()
                endforeach()
                unset(LOOK_DEP_PATH CACHE)
                if(LOOK_CRYPTO_MISSING)
                    string(REPLACE ";" ", " LOOK_CRYPTO_MISSING "${LOOK_CRYPTO_MISSING}")
                    message(FATAL_ERROR
                        "The static OpenSSL on this system needs these libraries, which are not installed: ${LOOK_CRYPTO_MISSING}. "
                        "On Debian/Ubuntu install: libjitterentropy3-dev zlib1g-dev libzstd-dev "
                        "(only the ones that exist on your release). Or build with -DLOOK_STATIC_SSL=OFF "
                        "to link OpenSSL dynamically (the binary then needs this system's libssl at run time).")
                endif()
            endif()
        endif()
    endif()
    # Static libcrypto.a may need dlopen + zlib → dl, z added
    target_link_libraries(look      Threads::Threads dl OpenSSL::SSL OpenSSL::Crypto resolv)
    target_link_libraries(look-cgi  Threads::Threads dl OpenSSL::SSL OpenSSL::Crypto resolv)
    target_link_libraries(look-fcgi Threads::Threads dl OpenSSL::SSL OpenSSL::Crypto resolv)
    target_link_libraries(imap-m1-test Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
    if(LOOK_STATIC_SSL)
        # Static OpenSSL 1.1 libcrypto may depend on zlib symbols
        find_library(ZLIB_A NAMES libz.a z)
        if(ZLIB_A)
            target_link_libraries(look ${ZLIB_A})
            target_link_libraries(look-cgi ${ZLIB_A})
            target_link_libraries(look-fcgi ${ZLIB_A})
        endif()
    endif()

    # Before GCC 8, filesystem is a separate library
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
        target_link_libraries(look      stdc++fs)
        target_link_libraries(look-cgi  stdc++fs)
        target_link_libraries(look-fcgi stdc++fs)
    endif()

    # No .exe extension on binary names on Linux
    set_target_properties(look      PROPERTIES OUTPUT_NAME "lk")
    set_target_properties(look-cgi  PROPERTIES OUTPUT_NAME "lk-cgi")
    set_target_properties(look-fcgi PROPERTIES OUTPUT_NAME "lk-fcgi")
endif()
