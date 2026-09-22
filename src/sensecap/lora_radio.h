#pragma once
#include <Arduino.h>
#include <RadioLib.h>

// Onboard SX1262 LoRa radio (Weg 1 — direct, not via an external
// Meshtastic node). This module owns hardware bring-up only: chip wake/
// reset timing (see meshtastic_proto.h for why that mattered, Issue #36)
// and the RadioLibHal that redirects NSS/RST/BUSY/DIO1 through the
// IO-expander. Meshtastic-protocol framing (packet header, encryption,
// channel hash, and the actual on-air radio parameters that need to match
// stock firmware) lives in meshtastic_proto.h/.cpp, which uses
// lora_radio_instance() to configure and drive this same radio object.

// Initializes the SX1262 and returns whether it responded. Call once,
// before meshtastic_proto_begin().
bool lora_radio_begin();

// True once lora_radio_begin() has succeeded.
bool lora_radio_ready();

// The underlying RadioLib SX1262 instance, for meshtastic_proto.cpp to
// configure (frequency/BW/SF/CR/sync word/preamble) and drive directly
// (transmit/startReceive/readData/getIrqStatus). Only valid after
// lora_radio_begin() returned true.
SX1262 &lora_radio_instance();

// Sends `text` as a single raw, non-Meshtastic-compatible LoRa packet.
// Kept only as a low-level hardware smoke test (see Issue #36's history) —
// real sending should go through meshtastic_proto_send_text() instead.
bool lora_radio_send_test(const char *text);
