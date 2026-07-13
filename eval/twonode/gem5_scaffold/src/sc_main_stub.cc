// SPDX-License-Identifier: Apache-2.0
// Minimal sc_main stub. gem5's SystemC kernel requires this symbol to
// initialize the SC elaboration phase (which triggers
// before_end_of_elaboration -> sendRangeChange on Gem5ToTlmBridge).
// The real facade has one too, but we can't link it due to ODR issues
// with topology_tlm.cpp symbols.
//
// We intentionally do NOT call sc_start() here — the TLM drain_synchronous
// path in NICTopologySC doesn't need a running SC kernel. The elaboration
// phase alone (which gem5 runs automatically before sc_main returns) is
// sufficient for the bridge to announce its address range.

extern "C" int sc_main(int /*argc*/, char** /*argv*/) {
    return 0;
}
