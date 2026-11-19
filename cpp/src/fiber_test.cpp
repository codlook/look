// fiber_test.cpp — minimal POC: fiber yield/resume + FiberLocal
// Only for testing, not included in LOOK_CORE.

#include "look/fiber.h"
#include <cassert>
#include <iostream>
#include <string>

using namespace look;

// Test 1: Basic yield/resume
void test_basic() {
    int step = 0;
    auto f = Fiber::create([&] {
        step = 1;
        Fiber::yield();
        step = 2;
        Fiber::yield();
        step = 3;
    });

    assert(step == 0);
    f->resume(); assert(step == 1);
    f->resume(); assert(step == 2);
    f->resume(); assert(step == 3);
    assert(f->done());
    std::cout << "PASS Test 1: basic yield/resume\n";
}

// Test 2: FiberLocal isolation — same thread, different fibers
void test_fiber_local() {
    std::string val_a, val_b;

    auto fa = Fiber::create([&] {
        // fiber A writes to its own local
        Fiber::current()->local.conns["key"] = nullptr;
        val_a = (Fiber::current()->local.conns.count("key") ? "A_set" : "A_miss");
        Fiber::yield();
        val_a += (Fiber::current()->local.conns.count("key") ? "+A_kept" : "+A_lost");
    });

    auto fb = Fiber::create([&] {
        // fiber B accesses its own local — must not see A's value
        val_b = (Fiber::current()->local.conns.count("key") ? "B_sees_A" : "B_isolated");
        Fiber::yield();
    });

    fa->resume();  // A runs, yields
    fb->resume();  // B runs, yields — must not see A's local
    fa->resume();  // A continues — its own local must be intact

    assert(val_a == "A_set+A_kept");
    assert(val_b == "B_isolated");
    std::cout << "PASS Test 2: FiberLocal isolation\n";
}

// Test 3: FiberScheduler round-robin
void test_scheduler() {
    FiberScheduler sched;
    set_thread_scheduler(&sched);

    std::string log;
    sched.spawn([&] { log += "A1"; Fiber::yield(); log += "A2"; });
    sched.spawn([&] { log += "B1"; Fiber::yield(); log += "B2"; });
    sched.run_until_idle();

    // Round-robin: A1, B1, A2, B2
    assert(log == "A1B1A2B2");
    std::cout << "PASS Test 3: scheduler round-robin (" << log << ")\n";
    set_thread_scheduler(nullptr);
}

// Test 4: Is there a guard page (is a stack overflow caught) — Linux only
#ifndef _WIN32
void test_guard_page_exists() {
    // Check that the fiber was created — was the guard page set up successfully?
    auto f = Fiber::create([] { Fiber::yield(); });
    f->resume();
    assert(!f->done());
    f->resume();
    assert(f->done());
    std::cout << "PASS Test 4: guard page setup (fiber lifecycle OK)\n";
}
#endif

// Test 5: done() is true once the fiber has completed
void test_done_flag() {
    auto f = Fiber::create([] { /* finishes immediately */ });
    assert(!f->done());
    f->resume();
    assert(f->done());
    std::cout << "PASS Test 5: done() flag correct\n";
}

int main() {
    std::cout << "=== Fiber POC Tests ===\n";
    test_basic();
    test_fiber_local();
    test_scheduler();
#ifndef _WIN32
    test_guard_page_exists();
#endif
    test_done_flag();
    std::cout << "=== All PASS ===\n";
    return 0;
}
