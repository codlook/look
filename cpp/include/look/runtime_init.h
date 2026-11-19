#pragma once
namespace look {

// Process startup init — ALL entry points (lk / lk-fcgi / lk-cgi) call this.
// A new startup job is added here → ALL THREE binaries pick it up automatically (no writing in three separate places).
//
// WHY: the same "main✅ fcgi✅ cgi❌" pattern bit TWICE (configure_system_ca_bundle first,
// sqlite_global_init later) — startup jobs were added to the three entry points SEPARATELY and every
// time one of them (cgi) was missed. This consolidation applies "one structure per concept" and
// reduces code: the init list is in one place, a third divergence is impossible.
void runtime_init();

} // namespace look
