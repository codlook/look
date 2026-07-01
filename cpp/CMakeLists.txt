cmake_minimum_required(VERSION 3.20)
project(looklang VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
    src/installer.cpp
    src/miniz/miniz.c
    src/test_runner.cpp
    src/repl.cpp
    src/linenoise/linenoise.c
)

add_executable(look
    src/main.cpp
    ${LOOK_CORE}
)

add_executable(look-cgi
    src/cgi_main.cpp
    ${LOOK_CORE}
)

add_executable(look-fcgi
    src/fcgi_main.cpp
    src/http_main.cpp
    src/http_server.cpp
    src/event_loop.cpp
    src/compiler.cpp
    src/vm.cpp
    ${LOOK_CORE}
)

# ── Platform bağımsız linkleme ────────────────────────────────────────────────
if(WIN32)
    target_link_libraries(look      ws2_32 bcrypt)
    target_link_libraries(look-cgi  ws2_32 bcrypt)
    target_link_libraries(look-fcgi ws2_32 bcrypt)
    set_target_properties(look      PROPERTIES OUTPUT_NAME "lk")
    set_target_properties(look-cgi  PROPERTIES OUTPUT_NAME "lk-cgi")
    set_target_properties(look-fcgi PROPERTIES OUTPUT_NAME "lk-fcgi")
else()
    # Linux: pthread (std::filesystem, socket), dl (sqlite3 dlopen), ssl+crypto (http:: TLS)
    find_package(Threads REQUIRED)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(look      Threads::Threads dl OpenSSL::SSL OpenSSL::Crypto)
    target_link_libraries(look-cgi  Threads::Threads dl OpenSSL::SSL OpenSSL::Crypto)
    target_link_libraries(look-fcgi Threads::Threads dl OpenSSL::SSL OpenSSL::Crypto)

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
