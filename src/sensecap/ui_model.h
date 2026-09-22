#pragma once

// Builds the LVGL screens for the pages described in ui/ui.yaml and loads
// main_menu. Call ui_model_build() once after LVGL is initialized, and
// ui_model_tick() every loop() iteration afterwards (drives the status
// bar clock/blink and the hold_confirm gesture poll).
void ui_model_build();
void ui_model_tick();

// Call these once the Meshtastic bridge exists and actually sends/receives
// something, to flash the status bar's TX/RX indicators. Unused for now —
// deliberately not simulated, so the status bar doesn't lie about traffic
// that isn't happening yet.
void ui_model_notify_tx();
void ui_model_notify_rx();

// Real link state: meshtastic_bridge.cpp calls this once its handshake
// with the external node completes, which drives both the status bar's
// ONLINE/OFFLINE dot and whether trigger_emergency can succeed. Before
// that bridge existed this was a manual "testconnect on/off"-only
// escape hatch (see main.cpp) so the success screen stayed testable
// without the UI lying about connectivity by default — that command
// still works, but now it's overriding a real signal, not standing in
// for a nonexistent one.
void ui_model_set_connected(bool connected);

// Testing-only (2026-09-18): drives the exact same path a real touchscreen
// hold-confirm does (do_trigger_emergency() -> start_transmission() ->
// the real g_transmission_pending/ACK-wait machinery in ui_model_tick()),
// unlike main.cpp's older "test emergency" serial command which only
// calls meshtastic_send_emergency() directly and therefore never touches
// VG-numbering, the stage rows, eventLog "Notmeldung..." entries, or the
// success/failure screens -- found live while trying to test the failure
// screen (B4) without physical touch access: that command's silence on
// timeout wasn't a bug, it just never was driving that machinery at all.
void ui_model_test_trigger_emergency(const char *category, const char *type, const char *label);

// Dev-tooling only, screenshot-capture branch: jumps straight to a named
// screen via nav_to(), bypassing touch so screens can be captured (see
// main.cpp's "screenshot" command) without a human tapping the panel.
// Returns false and leaves the current screen if the name isn't known.
// Unknown-name behaviour is deliberate (no silent fallback) so a typo in
// an automated capture script fails loudly instead of grabbing the wrong
// screen. NOT for merging into main -- see docs/screenshots/README.md.
bool ui_model_test_goto_screen(const char *name);
