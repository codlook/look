#pragma once
// Remaining native (C++) stack. The callback chain (a LOOK function from within map/filter/sort… → VM
// re-entry) spends C++ stack at every level; the fixed counter (VM::MAX_NATIVE_DEPTH) is enough in a normal
// build but, if the stack is small (ulimit -s, small fiber stack, ASan's large frames), it may
// overflow before the counter. This check looks at the real remaining stack: the process never crashes, the request gets a clean
// "Stack overflow". Go grows the stack and kills the process at 1 GB; here the process stays alive.
//
// Stack range: inside a fiber it is the fiber's own stack (Fiber::stack_range), otherwise the thread's
// (pthread_getattr_np; on Windows TIB + VirtualQuery — a Windows fiber updates the TIB itself).
// The range is cached; if the current stack pointer is outside the range (the fiber changed) it is re-read.
#include <cstddef>
#include <cstdint>
#if defined(_MSC_VER)
#  include <intrin.h>   // _AddressOfReturnAddress (windows.h is NOT included here: min/max/DELETE macros)
#endif

namespace look {

// Stack that must remain before opening one more callback level: 1/8 of the stack, clamped between 64 KB and
// 256 KB. A fixed 1 MB reserve was measured and was WRONG: on a 1 MB stack (Windows thread
// default) even the first callback failed. At 256 KB on 8 MB: the 4,000-level (~6 MB)
// counter remains the real limit; the reserve is enough for value traffic at the last level (1000 nested to_string ~190 KB,
// measured). Lower bound 64 KB: on a 128 KB stack 1/8 (16 KB) was not enough for a single level of ASan's
// large frames, and the process crashed intermittently (measured, 2 out of 3).
// AddressSanitizer adds a redzone to every frame (frames ~4x): the reserve grows by the same ratio.
// Measured: under ASan a 64 KB reserve did not cover the code between two checks on a 128–256 KB stack (nested assignment compilation,
// the parser's precedence chain) → ASan stack overflow; the normal build is unaffected.
#if defined(__SANITIZE_ADDRESS__)
constexpr size_t LOOK_STACK_FRAME_SCALE = 4;
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
constexpr size_t LOOK_STACK_FRAME_SCALE = 4;
#  else
constexpr size_t LOOK_STACK_FRAME_SCALE = 1;
#  endif
#else
constexpr size_t LOOK_STACK_FRAME_SCALE = 1;
#endif
constexpr size_t LOOK_STACK_RESERVE_MIN = 64 * 1024 * LOOK_STACK_FRAME_SCALE;
constexpr size_t LOOK_STACK_RESERVE_MAX = 256 * 1024 * LOOK_STACK_FRAME_SCALE;

inline const char* look_stack_pointer() noexcept {
#if defined(_MSC_VER)
    return static_cast<const char*>(_AddressOfReturnAddress());
#else
    return static_cast<const char*>(__builtin_frame_address(0));
#endif
}

// fiber-ok: caches only the current stack's range; on every call the stack pointer is
// compared with the range, and if the fiber changed (pointer outside) it is re-read — not broken by suspension.
struct LookStackRange { const char* lo = nullptr; const char* hi = nullptr; size_t reserve = 0; };
inline LookStackRange& look_stack_range_cache() noexcept { thread_local LookStackRange r; return r; }

// The range of the current stack (the fiber's if inside a fiber, otherwise the thread's). Platform-specific:
// fiber_posix.cpp / fiber_win.cpp.
bool look_read_stack_range(const char*& lo, const char*& hi) noexcept;

// True if the remaining stack is below the reserve. If the range is unknown, false (no check, the counter remains).
inline bool look_native_stack_low() noexcept {
    const char* sp = look_stack_pointer();
    LookStackRange& r = look_stack_range_cache();
    if (sp < r.lo || sp >= r.hi) {
        const char* lo = nullptr; const char* hi = nullptr;
        if (!look_read_stack_range(lo, hi) || sp < lo || sp >= hi) {
            // Could not be read: treat the range as the whole address space, reserve 0 → no re-reading on every call (expensive);
            // the check is off on this thread, the VM::MAX_NATIVE_DEPTH counter stays in effect.
            r.lo = nullptr; r.hi = reinterpret_cast<const char*>(UINTPTR_MAX); r.reserve = 0;
            return false;
        }
        const size_t size = static_cast<size_t>(hi - lo);
        r.lo = lo; r.hi = hi;
        const size_t eighth = size / 8;
        r.reserve = eighth < LOOK_STACK_RESERVE_MIN ? LOOK_STACK_RESERVE_MIN
                  : eighth > LOOK_STACK_RESERVE_MAX ? LOOK_STACK_RESERVE_MAX : eighth;
    }
    return static_cast<size_t>(sp - r.lo) < r.reserve;   // the stack grows downward
}

// Everywhere that walks the syntax tree recursively (parser, capture / parameter / argument
// checks, undefined-call lookup) calls this on entry: if the stack is low, a parse error
// (with line/column), not a crash. The parser's counters (nesting 150, chain 1000) do not look at the real stack:
// every nested parenthesis spends ~15 C++ frames → at 128 KB 140 parentheses, at 256 KB 140
// nested blocks (capture check) crashed (measured). The body is in parser.cpp (LookParseError).
[[noreturn]] void look_throw_nested_too_deep(int line, int col);
inline void look_ast_guard(int line = 0, int col = 0) {
    if (look_native_stack_low()) look_throw_nested_too_deep(line, col);
}

} // namespace look
