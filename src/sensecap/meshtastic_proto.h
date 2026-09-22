#pragma once
#include <Arduino.h>

// Real Meshtastic-protocol-compatible packet layer on top of the onboard
// SX1262 (see lora_radio.h/.cpp for the radio itself). This is Weg 1 grown
// up: not just raw LoRa anymore, but the actual over-the-air packet format
// stock Meshtastic firmware speaks -- 16-byte plaintext header, AES-CTR
// encrypted protobuf payload, same nonce/key/channel-hash scheme -- so a
// factory-default Meshtastic app or node on the same region+preset can
// receive and decode what we send, and we can decode what they send.
//
// Two channels (see meshtastic_proto.cpp for the exact PSKs/hashes):
// - The public default primary channel ("LongFast" name, default PSK) --
//   used for NodeInfo exchange, generic ACK replies, and the `mesh send`
//   test command. Keeps this board discoverable by any stock Meshtastic
//   device without extra config.
// - A private "kayna-funkt" channel (short-key preset for now, see
//   meshtastic_send_emergency()) -- emergency reports go out here as a
//   direct message to a configured dispatch node, not a broadcast.
//
// Region is fixed to EU_868 per org policy (see CLAUDE org instructions),
// which for the LONG_FAST preset has exactly one possible frequency slot
// (869.525 MHz -- verified against meshtastic/firmware's own
// RadioInterface.cpp formula, see meshtastic_proto.cpp's comments). No
// PKI/DM encryption, no routing/rebroadcast -- we originate and receive
// like a simple leaf node, just on two channels instead of one.
//
// See lib/meshtastic_proto/README.md for where the protobuf definitions
// (vendored, not hand-written) come from.

// Call once, after lora_radio_begin() has already succeeded.
bool meshtastic_proto_begin();

// Call every loop() iteration -- drives the receive path (polls the radio's
// IRQ status over SPI; DIO1 is behind the IO-expander so we can't use a real
// hardware interrupt for it, see lora_radio.cpp).
void meshtastic_proto_loop();

// Broadcasts a TEXT_MESSAGE_APP packet on the default primary channel.
// Returns whether the radio accepted it for transmission (RadioLib's return
// code) -- NOT a delivery/ack confirmation. Nothing here tracks real
// end-to-end acks (same honesty caveat as the rest of this project).
bool meshtastic_proto_send_text(const char *text);

// Formats and sends an emergency report using the same LAGE: wire format
// src/xiao/main.cpp parses (see main README "Lagemeldungen (kayna-funkt)").
// Sent as a direct message (with a real ACK request) to
// station_config().dispatchNodeNum on the private channel -- returns false
// immediately, without transmitting anything, if that's not configured
// (0). The return value is only whether the radio locally accepted the
// send; call meshtastic_proto_emergency_ack_received() afterwards (poll it
// from ui_model_tick, it can take a few seconds) to find out whether the
// dispatch node actually confirmed receipt.
bool meshtastic_send_emergency(const char *category, const char *type, const char *label);

// True once a real ROUTING_APP delivery ACK came back from the dispatch
// node for the most recent meshtastic_send_emergency() call. Resets to
// false at the start of each new meshtastic_send_emergency() call.
bool meshtastic_proto_emergency_ack_received();

// Broadcasts a periodic status ("STATUS:<station>;uptime=...;zustand=...;
// akku=n/v;sabotage=n/v") on the private channel, so a Leitstelle watching
// several stations can notice one going silent (Issue #13). Battery and
// sabotage fields are honestly "n/v" (not available) -- no sensors for
// either exist on this board yet (Issues #5, #7). Call periodically from
// loop() (main.cpp), not on a tight timer -- once every few minutes is
// plenty for outage detection and keeps airtime/duty-cycle usage low.
bool meshtastic_send_heartbeat();

// Our own node number (derived from the ESP32's factory MAC, like real
// Meshtastic firmware does) -- logged at startup, useful for identifying
// this device's packets in a real Meshtastic app while testing.
uint32_t meshtastic_proto_my_node_num();
