#include "meshtastic_proto.h"

#include <RadioLib.h>
#include <esp_system.h>
#include <mbedtls/aes.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <string.h>

#include "lage_db.h"
#include "lora_radio.h"
#include "mesh_security.h"
#include "meshtastic/mesh.pb.h"
#include "station_config.h"
#include "ui_model.h"
#include "wall_clock.h"

// Every constant below is copied from meshtastic/firmware's own source
// (read directly, not from memory) during the Issue #36 investigation on
// 2026-09-18 -- see src/sensecap/README.md section 5 for the file-by-file
// citations (RadioInterface.cpp/.h, RadioLibInterface.h, SX126xInterface.cpp,
// Channels.cpp/.h, MeshRadio.h, CryptoEngine.cpp/.h, Router.cpp, MeshTypes.h).

namespace {

// RadioInterface.h: PacketHeader. "This structure has to exactly match the
// wire layout when sent over the radio link." to/from/id are 4 bytes each
// on this platform (NodeNum/PacketId are uint32_t), then 4 single bytes --
// 16 bytes total, no padding on a plain sequence of these field widths.
#pragma pack(push, 1)
struct PacketHeaderRaw {
  uint32_t to;
  uint32_t from;
  uint32_t id;
  uint8_t flags;
  uint8_t channel;
  uint8_t next_hop;
  uint8_t relay_node;
};
#pragma pack(pop)
static_assert(sizeof(PacketHeaderRaw) == 16, "must match MESHTASTIC_HEADER_LENGTH");

constexpr uint32_t kNodenumBroadcast = 0xFFFFFFFFu;    // MeshTypes.h NODENUM_BROADCAST
constexpr uint8_t kHopLimitDefault = 3;                // MeshTypes.h HOP_RELIABLE
constexpr uint8_t kFlagsHopLimitMask = 0x07;
constexpr uint8_t kFlagsWantAckMask = 0x08;  // RadioInterface.h PACKET_FLAGS_WANT_ACK_MASK
constexpr uint8_t kFlagsHopStartShift = 5;
constexpr uint8_t kFlagsHopStartMask = 0xE0;
constexpr size_t kMaxPayload = 233; // meshtastic_Data_payload_t = PB_BYTES_ARRAY_T(233)

// Channels.h: "16 bytes of random PSK for our _public_ default channel that
// all devices power up on (AES128)". Public, well-known by design -- this is
// not a secret, every stock Meshtastic device ships with it.
const uint8_t kDefaultPsk[16] = {0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
                                  0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01};

// Two channels, both reachable on the same frequency (only the PSK/hash
// differ -- see defaultChannelFrequencyMHz()'s comment: region+preset fixes
// the frequency regardless of channel name/key, exactly how real Meshtastic
// lets one radio serve several channels at once).
//
// - Public: the default (unnamed) primary channel every stock Meshtastic
//   device powers up on. Name for hashing purposes is the modem preset's
//   display name (Channels::getName() falls back to this when the stored
//   name is empty) -- "LongFast" for LONG_FAST. Used for NodeInfo exchange,
//   generic ACK replies, and the `mesh send` test command -- keeps this
//   board discoverable/testable without needing kayna-funkt-specific config.
// - Private ("kayna-funkt"): a short-PSK channel per the user's explicit
//   2026-09-18 decision (real device testing found "failed to deliver to
//   mesh" on the public channel's broadcasts -- Meshtastic never ACKs
//   broadcasts by design; emergency reports need a direct message on a
//   dedicated channel instead). Short PSK now for easy onboarding
//   ("kurzer Schluessel"); a real random per-deployment key is a planned
//   follow-up, not implemented yet.
struct MeshChannel {
  const char *name;
  uint8_t psk[16];
  uint8_t hash; // computed once in meshtastic_proto_begin()
};

// Channels::getKey(): a 1-byte PSK is a "short" preset index into
// defaultpsk, bumping only the last byte (index 1 = defaultpsk unchanged).
// This is NOT a real secret -- same category as the public PSK, just a
// different, less commonly used index. Fine for now per the user's explicit
// "short key for easier login, real secret channel later" decision.
constexpr uint8_t kPrivateChannelPskIndex = 5;

MeshChannel g_publicChannel = {"LongFast", {0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59, 0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e,
                                            0x69, 0x01},
                               0};
MeshChannel g_privateChannel = {"kayna-funkt", {0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59, 0xf0, 0xbc, 0xff, 0xab, 0xcf,
                                                0x4e, 0x69,
                                                (uint8_t)(0x01 + (kPrivateChannelPskIndex - 1))},
                                0};

// EU_868 region (RadioInterface.cpp regions[] table).
constexpr float kRegionFreqStartMHz = 869.4f;
constexpr float kRegionFreqEndMHz = 869.65f;

// LONG_FAST preset, non-wide-LoRa (MeshRadio.h modemPresetToParams default case).
constexpr float kBandwidthKHz = 250.0f;
constexpr uint8_t kSpreadingFactor = 11;
constexpr uint8_t kCodingRate = 5;
constexpr uint8_t kSyncWord = 0x2b;      // RadioLibInterface.h
constexpr uint16_t kPreambleLength = 16; // RadioInterface.h preambleLengthDefault
constexpr int8_t kTxPowerDbm = 20;       // safely under region limit (27) and chip max (+22)

uint8_t xorHash(const uint8_t *p, size_t len) {
  uint8_t code = 0;
  for (size_t i = 0; i < len; i++) code ^= p[i];
  return code;
}

// RadioInterface.cpp applyModemConfig(): numChannels = floor((freqEnd -
// freqStart) / (spacing + bw/1000)). For EU_868 + LONG_FAST that's
// floor(0.25 / 0.25) = 1 -- exactly one possible frequency slot, so
// channel_num = hash(name) % 1 = 0 always, no hopping, no ambiguity.
// freq = freqStart + bw/2000 + channel_num*(bw/1000) = 869.4 + 0.125 + 0.
float defaultChannelFrequencyMHz() { return kRegionFreqStartMHz + (kBandwidthKHz / 2000.0f); }

// Channels::generateHash(): xorHash(channel name) ^ xorHash(psk bytes).
uint8_t channelHash(const MeshChannel &ch) {
  uint8_t h = xorHash(reinterpret_cast<const uint8_t *>(ch.name), strlen(ch.name));
  h ^= xorHash(ch.psk, sizeof(ch.psk));
  return h;
}

// Which configured channel (if any) a received packet's channel-hash byte
// belongs to -- nullptr if it matches neither (Channels::decryptForHash():
// unrecognized hash means "not for us", not an error).
const MeshChannel *findChannelByHash(uint8_t hash) {
  if (hash == g_publicChannel.hash) return &g_publicChannel;
  if (hash == g_privateChannel.hash) return &g_privateChannel;
  return nullptr;
}

uint32_t g_myNodeNum = 0;
bool g_started = false;

// Tracks the most recent emergency send awaiting a real delivery ACK from
// the dispatch node (see meshtastic_send_emergency()). 0 = none pending.
uint32_t g_pendingEmergencyPacketId = 0;
bool g_emergencyAckReceived = false;

// CryptoEngine::encryptAESCtr/initNonce: nonce = 8 bytes packet ID (LE) + 4
// bytes sending node number (LE) + 4 byte block counter starting at 0. CTR
// mode is symmetric (CryptoEngine::decrypt()'s own comment: "For CTR, the
// implementation is the same"), so one function does both directions.
// mbedtls_aes_crypt_ctr increments the whole 16-byte nonce_counter as a
// single big-endian counter, but for any one packet here (well under 240
// bytes = 15 AES blocks) that's indistinguishable from "only the last 4
// bytes are the counter" -- it never carries into byte 11.
void aesCtrCrypt(const uint8_t psk[16], uint32_t fromNode, uint32_t packetId, uint8_t *data, size_t len) {
  uint8_t nonce[16] = {0};
  uint64_t packetId64 = packetId; // upper 32 bits zero: our packet IDs are 32-bit
  memcpy(nonce, &packetId64, sizeof(uint64_t));
  memcpy(nonce + sizeof(uint64_t), &fromNode, sizeof(uint32_t));

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes, psk, 128); // CTR always uses the encrypt key schedule
  size_t ncOff = 0;
  uint8_t streamBlock[16] = {0};
  mbedtls_aes_crypt_ctr(&aes, len, &ncOff, nonce, streamBlock, data, data);
  mbedtls_aes_free(&aes);
}

// Not Meshtastic's own node-number algorithm (never found the exact formula
// needed -- doesn't matter: the wire format doesn't care how a node number
// was chosen, only that it's stable and non-zero/non-broadcast).
uint32_t deriveNodeNum() {
  uint64_t mac = ESP.getEfuseMac();
  uint32_t num = (uint32_t)(mac & 0xFFFFFFFFu);
  if (num == 0 || num == kNodenumBroadcast) num = 0x2A2A2A2A;
  return num;
}

// Builds the 16-byte header + encrypted protobuf payload and transmits it.
// hopLimit=0 is what real firmware uses for direct ACK replies
// (RoutingModule::sendAckNak's own default, see MeshModule::allocAckNak) --
// zero hops means "don't relay this, it's only meant for whoever's in
// direct radio range", appropriate for an ACK responding to a packet we
// just received directly. Normal originated traffic uses kHopLimitDefault.
// wantAck is force-cleared for broadcasts, matching Router.cpp's own
// "Never set the want_ack flag on broadcast packets". outPacketId (if
// non-null) receives the randomly generated packet ID, so callers that
// need to later match an incoming ACK's request_id against this send can.
bool sendData(uint32_t toNode, const meshtastic_Data &data, const MeshChannel &channel, uint8_t hopLimit = kHopLimitDefault,
              bool wantAck = false, uint32_t *outPacketId = nullptr) {
  if (!g_started) {
    Serial.println("[MESH] sendData: Protokoll nicht gestartet");
    return false;
  }
  if (toNode == kNodenumBroadcast) wantAck = false;

  uint8_t plain[kMaxPayload];
  pb_ostream_t ostream = pb_ostream_from_buffer(plain, sizeof(plain));
  if (!pb_encode(&ostream, meshtastic_Data_fields, &data)) {
    Serial.println("[MESH] sendData: pb_encode fehlgeschlagen");
    return false;
  }
  size_t plainLen = ostream.bytes_written;

  uint32_t packetId = esp_random();
  if (packetId == 0) packetId = 1;
  if (outPacketId) *outPacketId = packetId;

  aesCtrCrypt(channel.psk, g_myNodeNum, packetId, plain, plainLen);

  PacketHeaderRaw header;
  header.to = toNode;
  header.from = g_myNodeNum;
  header.id = packetId;
  header.flags = hopLimit & kFlagsHopLimitMask;
  header.flags |= wantAck ? kFlagsWantAckMask : 0;
  header.flags |= (hopLimit << kFlagsHopStartShift) & kFlagsHopStartMask;
  header.channel = channel.hash;
  header.next_hop = 0;
  header.relay_node = 0;

  uint8_t wire[sizeof(PacketHeaderRaw) + kMaxPayload];
  memcpy(wire, &header, sizeof(header));
  memcpy(wire + sizeof(header), plain, plainLen);
  size_t wireLen = sizeof(header) + plainLen;

  SX1262 &radio = lora_radio_instance();
  int16_t state = radio.transmit(wire, wireLen);
  Serial.printf("[MESH] sendData portnum=%d an !%08x (id=0x%08x, %u Bytes) -> transmit() = %d (%s)\n", (int)data.portnum,
                (unsigned)toNode, (unsigned)packetId, (unsigned)wireLen, state, state == RADIOLIB_ERR_NONE ? "OK" : "FEHLER");
  radio.startReceive(); // resume listening (transmit() leaves the radio in standby)
  return state == RADIOLIB_ERR_NONE;
}

// NextHopRouter.cpp: "if (!perhapsRebroadcast(p) && isToUs(p) && p->want_ack)
// sendAckNak(meshtastic_Routing_Error_NONE, getFrom(p), p->id, p->channel, 0)"
// -- a Routing-portnum Data message, error_reason=NONE, request_id=the
// packet we're acking, hop_limit=0, want_ack=false on the ack itself (no
// ack-for-the-ack). We never rebroadcast (we're a leaf node), so the
// "!perhapsRebroadcast" condition is always true for us here.
void sendRoutingAck(uint32_t toNode, uint32_t requestId, const MeshChannel &channel) {
  meshtastic_Routing routing = meshtastic_Routing_init_zero;
  routing.which_variant = meshtastic_Routing_error_reason_tag;
  routing.error_reason = meshtastic_Routing_Error_NONE;

  uint8_t routingBytes[16];
  pb_ostream_t rstream = pb_ostream_from_buffer(routingBytes, sizeof(routingBytes));
  if (!pb_encode(&rstream, meshtastic_Routing_fields, &routing)) {
    Serial.println("[MESH] sendRoutingAck: pb_encode(Routing) fehlgeschlagen");
    return;
  }

  meshtastic_Data data = meshtastic_Data_init_zero;
  data.portnum = meshtastic_PortNum_ROUTING_APP;
  memcpy(data.payload.bytes, routingBytes, rstream.bytes_written);
  data.payload.size = rstream.bytes_written;
  data.request_id = requestId;

  Serial.printf("[MESH] Sende ACK an !%08x fuer Paket 0x%08x (Kanal \"%s\")\n", (unsigned)toNode, (unsigned)requestId,
                channel.name);
  sendData(toNode, data, channel, /*hopLimit=*/0);
}

// Real apps need to know who we are (long/short name, hardware model)
// before they'll properly treat us as a DM-capable node -- otherwise a
// direct message to us shows as an unresolved/failed send even once our
// ROUTING_APP ack above is working (found live: "brauche erst austausch
// der user info, dann kann ich dm schreiben"). meshtastic_HardwareModel
// has a real SENSECAP_INDICATOR value (70) -- accurate, since this really
// is that hardware, just running our own firmware instead of stock
// Meshtastic. Broadcast once at startup, and echoed directly back to
// anyone who sends us their own NodeInfo first (a real handshake, not
// just a one-shot announcement).
void sendNodeInfo(uint32_t toNode) {
  meshtastic_User user = meshtastic_User_init_zero;
  snprintf(user.id, sizeof(user.id), "!%08x", (unsigned)g_myNodeNum);
  snprintf(user.long_name, sizeof(user.long_name), "kayna-funkt SenseCAP");
  snprintf(user.short_name, sizeof(user.short_name), "KF");
  user.hw_model = meshtastic_HardwareModel_SENSECAP_INDICATOR;
  user.is_licensed = false;
  user.role = meshtastic_Config_DeviceConfig_Role_CLIENT;

  uint8_t userBytes[meshtastic_User_size];
  pb_ostream_t ustream = pb_ostream_from_buffer(userBytes, sizeof(userBytes));
  if (!pb_encode(&ustream, meshtastic_User_fields, &user)) {
    Serial.println("[MESH] sendNodeInfo: pb_encode(User) fehlgeschlagen");
    return;
  }

  meshtastic_Data data = meshtastic_Data_init_zero;
  data.portnum = meshtastic_PortNum_NODEINFO_APP;
  memcpy(data.payload.bytes, userBytes, ustream.bytes_written);
  data.payload.size = ustream.bytes_written;

  Serial.printf("[MESH] Sende NodeInfo an !%08x\n", (unsigned)toNode);
  sendData(toNode, data, g_publicChannel); // discovery always happens on the public channel
}

// Application-level acknowledgment (Issue B5, Testprotokoll 2026-09-18):
// the ROUTING_APP ack in handleReceivedPacket() only proves the packet
// reached *some* node's radio layer -- it fires before meshSecurityCheck()
// even runs, so it can never prove the emergency report was actually
// accepted and stored. This is our own, separate confirmation, sent back
// as a normal PRIVATE_APP direct message ONLY after the report passed
// meshSecurityCheck() and was written to the DB. Any counterpart (dispatch
// station) that wants to give a real "wir haben es" guarantee instead of
// just a transport-layer echo should reply the same way. Format deliberately
// plain text like the rest of our wire protocol: "LAGE:ACK:<hex packetId>".
void sendApplicationAck(uint32_t toNode, uint32_t requestPacketId, const MeshChannel &channel) {
  char buf[32];
  snprintf(buf, sizeof(buf), "LAGE:ACK:%08x", (unsigned)requestPacketId);

  meshtastic_Data data = meshtastic_Data_init_zero;
  data.portnum = meshtastic_PortNum_PRIVATE_APP;
  size_t len = strlen(buf);
  memcpy(data.payload.bytes, buf, len);
  data.payload.size = len;

  Serial.printf("[MESH] Sende Anwendungs-ACK \"%s\" an !%08x (Kanal \"%s\")\n", buf, (unsigned)toNode, channel.name);
  sendData(toNode, data, channel, /*hopLimit=*/0);
}

// Same LAGE: wire format src/xiao/main.cpp parses (see main README
// "Lagemeldungen (kayna-funkt)"). State short-codes (LGE/NOR/WTG/SAB/SAUS)
// aren't handled here -- this board's UI doesn't have that state concept.
void handleTextMessage(uint32_t from, uint32_t packetId, const MeshChannel &channel, const char *text) {
  ui_model_notify_rx();
  Serial.printf("[MESH] Text von !%08x: \"%s\"\n", (unsigned)from, text);

  String msg(text);

  // Reply to our own emergency send, not a new report -- handle first,
  // separately from the "LAGE:" report parsing below (see B5 comment on
  // sendApplicationAck()). Only counts if it names the packet we're
  // actually waiting on AND comes from the configured dispatch node --
  // otherwise any node echoing a guessed/replayed id could fake success.
  if (msg.startsWith("LAGE:ACK:")) {
    uint32_t ackedId = strtoul(msg.c_str() + strlen("LAGE:ACK:"), nullptr, 16);
    if (from == station_config().dispatchNodeNum && ackedId != 0 && ackedId == g_pendingEmergencyPacketId) {
      g_emergencyAckReceived = true;
      Serial.println("[MESH] Anwendungs-ACK der Leitstelle erhalten -- Notmeldung wirklich angenommen.");
    }
    return;
  }

  if (!msg.startsWith("LAGE:")) return;

  String payload = msg.substring(5);
  int p1 = payload.indexOf(';');
  int p2 = payload.indexOf(';', p1 + 1);
  int p3 = payload.indexOf(';', p2 + 1);
  if (p1 < 0 || p2 < 0 || p3 < 0) {
    Serial.println("[MESH] Ungueltiges LAGE-Format");
    return;
  }

  String idPart = payload.substring(0, p1); idPart.trim();
  String kategorie = payload.substring(p1 + 1, p2); kategorie.trim();
  String status = payload.substring(p2 + 1, p3); status.trim();
  String content = payload.substring(p3 + 1); content.trim();

  bool isNew = idPart.equalsIgnoreCase("NEU");
  if (!meshSecurityCheck(from, isNew)) return; // Issues #1 (Allowlist) / #3 (Ratenlimit)

  char fromBuf[12];
  snprintf(fromBuf, sizeof(fromBuf), "!%08x", (unsigned)from);
  String fromNodeStr(fromBuf);

  bool stored;
  if (isNew) {
    int id = lageDbCreate(kategorie, status, content, fromNodeStr);
    Serial.printf("[MESH] Neue Lagemeldung angelegt, ID %d\n", id);
    stored = id > 0;
  } else {
    bool ok = lageDbUpdate(idPart.toInt(), kategorie, status, content, fromNodeStr);
    Serial.printf("[MESH] Lagemeldung %d %s\n", idPart.toInt(), ok ? "aktualisiert" : "nicht gefunden");
    stored = ok;
  }

  // Real acceptance confirmation -- see sendApplicationAck() comment.
  if (stored) sendApplicationAck(from, packetId, channel);
}

void handleReceivedPacket(const uint8_t *buf, size_t len) {
  if (len < sizeof(PacketHeaderRaw)) return;
  PacketHeaderRaw header;
  memcpy(&header, buf, sizeof(header));

  const MeshChannel *channel = findChannelByHash(header.channel);
  if (!channel) {
    Serial.printf("[MESH] Paket auf fremdem Kanal ignoriert (hash 0x%02x, kennen nur 0x%02x \"%s\" und 0x%02x \"%s\")\n",
                  header.channel, g_publicChannel.hash, g_publicChannel.name, g_privateChannel.hash, g_privateChannel.name);
    return;
  }

  // Our own transmissions can come back to us via RF self-coupling (seen
  // live: broadcasted NodeInfo, heard our own echo, replied to "ourselves"
  // as if it were a stranger -> sent another NodeInfo -> heard THAT too ->
  // infinite reply loop spamming the channel). Real firmware guards every
  // reply path with isFromUs(); this is the equivalent single guard point.
  if (header.from == g_myNodeNum) return;

  size_t cipherLen = len - sizeof(PacketHeaderRaw);
  if (cipherLen == 0 || cipherLen > kMaxPayload) return;

  static uint8_t plain[kMaxPayload];
  memcpy(plain, buf + sizeof(PacketHeaderRaw), cipherLen);
  aesCtrCrypt(channel->psk, header.from, header.id, plain, cipherLen);

  meshtastic_Data data = meshtastic_Data_init_zero;
  pb_istream_t stream = pb_istream_from_buffer(plain, cipherLen);
  if (!pb_decode(&stream, meshtastic_Data_fields, &data)) {
    Serial.println("[MESH] Payload-Decode fehlgeschlagen (falscher Kanal-Key oder korrupt)");
    return;
  }

  float rssi = lora_radio_instance().getRSSI();
  float snr = lora_radio_instance().getSNR();
  Serial.printf("[MESH] Paket von !%08x an !%08x, Kanal \"%s\", portnum=%d, %u Bytes, RSSI=%.1fdBm SNR=%.1fdB\n",
                (unsigned)header.from, (unsigned)header.to, channel->name, (int)data.portnum, (unsigned)data.payload.size, rssi,
                snr);

  // Explicitly requested (2026-09-18): a durable, queryable record of when
  // the configured Leitstelle was last actually heard from -- any packet
  // type counts (ACK, application ACK, NodeInfo, ...), since Meshtastic/
  // LoRa has no separate "connection" step to log instead (see the B2
  // rework's comment on why the old fake "Verbindung"-row was removed).
  // Meant as the data source a future Krisenstab/Systeminfo page can
  // finally show something real from. Throttled so a quick back-and-forth
  // (e.g. our send immediately followed by its ACK) collapses into one
  // entry instead of flooding the log.
  uint32_t dispatchNode = station_config().dispatchNodeNum;
  if (dispatchNode != 0 && header.from == dispatchNode) {
    static unsigned long lastLeitstelleContactLoggedAt = 0;
    unsigned long nowMs = millis();
    if (nowMs - lastLeitstelleContactLoggedAt >= 2000) {
      lastLeitstelleContactLoggedAt = nowMs;
      char msg[64];
      snprintf(msg, sizeof(msg), "Kontakt von Leitstelle !%08x (portnum=%d)", (unsigned)header.from, (int)data.portnum);
      eventLog("system", msg);
    }
  }

  // NextHopRouter.cpp: "if (!perhapsRebroadcast(p) && isToUs(p) && p->want_ack)
  // sendAckNak(...)" -- only for packets addressed directly to us (not
  // broadcast), and only if the sender actually asked for one.
  bool addressedToUs = (header.to == g_myNodeNum);
  bool wantAck = (header.flags & kFlagsWantAckMask) != 0;
  if (addressedToUs && wantAck) {
    sendRoutingAck(header.from, header.id, *channel);
  }

  if (data.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP || data.portnum == meshtastic_PortNum_PRIVATE_APP) {
    // PRIVATE_APP is what meshtastic_send_emergency() actually sends as a
    // direct message (see its comment for why: modern firmware's Router.cpp
    // rejects non-PKI direct TEXT_MESSAGE_APP as a "legacy DM" security
    // policy, found live against a real device -- PRIVATE_APP isn't subject
    // to that check and is the officially reserved portnum >= 256 for
    // exactly this kind of custom application traffic).
    char text[kMaxPayload + 1];
    size_t n = data.payload.size < sizeof(text) - 1 ? data.payload.size : sizeof(text) - 1;
    memcpy(text, data.payload.bytes, n);
    text[n] = '\0';
    handleTextMessage(header.from, header.id, *channel, text);
  } else if (data.portnum == meshtastic_PortNum_ROUTING_APP) {
    meshtastic_Routing routing = meshtastic_Routing_init_zero;
    pb_istream_t rstream = pb_istream_from_buffer(data.payload.bytes, data.payload.size);
    if (pb_decode(&rstream, meshtastic_Routing_fields, &routing) &&
        routing.which_variant == meshtastic_Routing_error_reason_tag) {
      // Issue B5 (Testprotokoll 2026-09-18): this is a TRANSPORT ack --
      // "a node claiming to be !header.from got our packet", sent by
      // handleReceivedPacket()'s own addressedToUs+wantAck branch above
      // *before* the receiving side's meshSecurityCheck() even runs. It
      // does NOT prove the emergency report was accepted, only that it
      // arrived somewhere. Logged for diagnostics only -- it must never
      // set g_emergencyAckReceived; that's reserved for the real
      // application-level "LAGE:ACK:" reply (see sendApplicationAck() /
      // handleTextMessage()).
      Serial.printf("[MESH] Transport-ACK von !%08x fuer unser Paket 0x%08x, error_reason=%d%s (KEINE Anwendungsbestaetigung)\n",
                    (unsigned)header.from, (unsigned)data.request_id, (int)routing.error_reason,
                    routing.error_reason == meshtastic_Routing_Error_NONE ? " (transportiert)" : "");
    }
  } else if (data.portnum == meshtastic_PortNum_NODEINFO_APP) {
    meshtastic_User user = meshtastic_User_init_zero;
    pb_istream_t ustream = pb_istream_from_buffer(data.payload.bytes, data.payload.size);
    if (pb_decode(&ustream, meshtastic_User_fields, &user)) {
      Serial.printf("[MESH] NodeInfo von !%08x: \"%s\" (%s)\n", (unsigned)header.from, user.long_name, user.short_name);
    }
    // Reply in kind so their app gets our identity right away, instead of
    // waiting for our next periodic broadcast.
    sendNodeInfo(header.from);
  } else if (data.portnum == meshtastic_PortNum_POSITION_APP) {
    // Real wall-clock time source (Testprotokoll A1/A4/F2, see wall_clock.h
    // comment): Position.time is "usually not sent over the mesh, but sent
    // from the phone so devices without GPS/RTC can set their clock" --
    // exactly our situation. Prefer it over `timestamp` (GPS solution time,
    // only meaningful if the position itself is valid) since `time` is the
    // field meant for this.
    meshtastic_Position position = meshtastic_Position_init_zero;
    pb_istream_t pstream = pb_istream_from_buffer(data.payload.bytes, data.payload.size);
    if (pb_decode(&pstream, meshtastic_Position_fields, &position)) {
      if (position.time != 0) {
        wallClockSetFromEpoch(position.time, "Mesh-Position");
      } else if (position.timestamp != 0) {
        wallClockSetFromEpoch(position.timestamp, "Mesh-Position (GPS)");
      }
    }
  }
}

} // namespace

bool meshtastic_proto_begin() {
  if (!lora_radio_ready()) {
    Serial.println("[MESH] meshtastic_proto_begin: Radio nicht bereit");
    return false;
  }
  g_myNodeNum = deriveNodeNum();
  g_publicChannel.hash = channelHash(g_publicChannel);
  g_privateChannel.hash = channelHash(g_privateChannel);

  SX1262 &radio = lora_radio_instance();
  float freq = defaultChannelFrequencyMHz();
  int16_t state = radio.setFrequency(freq);
  if (state == RADIOLIB_ERR_NONE) state = radio.setBandwidth(kBandwidthKHz);
  if (state == RADIOLIB_ERR_NONE) state = radio.setSpreadingFactor(kSpreadingFactor);
  if (state == RADIOLIB_ERR_NONE) state = radio.setCodingRate(kCodingRate);
  if (state == RADIOLIB_ERR_NONE) state = radio.setSyncWord(kSyncWord);
  if (state == RADIOLIB_ERR_NONE) state = radio.setPreambleLength(kPreambleLength);
  if (state == RADIOLIB_ERR_NONE) state = radio.setCRC(true);
  if (state == RADIOLIB_ERR_NONE) state = radio.setOutputPower(kTxPowerDbm);
  // Real firmware makes this a user setting (config.lora.sx126x_rx_boosted_gain,
  // SX126xInterface.cpp); doesn't affect wire-format compatibility, only local
  // receive sensitivity -- enabled here to give a real over-the-air test
  // against another device the best chance of actually being heard.
  if (state == RADIOLIB_ERR_NONE) state = radio.setRxBoostedGainMode(true);

  Serial.printf(
      "[MESH] Meshtastic-Protokoll: Node !%08x, %.3f MHz, BW%.0f SF%d CR4/%d, Sync 0x%02x\n"
      "  Kanal \"%s\" Hash 0x%02x (oeffentlich, Discovery/Test)\n"
      "  Kanal \"%s\" Hash 0x%02x (privat, Notmeldungen)\n",
      (unsigned)g_myNodeNum, freq, kBandwidthKHz, kSpreadingFactor, kCodingRate, kSyncWord, g_publicChannel.name,
      g_publicChannel.hash, g_privateChannel.name, g_privateChannel.hash);

  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[MESH] Radio-Konfiguration fehlgeschlagen: %d\n", state);
    return false;
  }

  state = radio.startReceive();
  g_started = (state == RADIOLIB_ERR_NONE);
  Serial.printf("[MESH] startReceive() -> %d (%s)\n", state, g_started ? "OK, lausche auf dem Default-Kanal" : "FEHLER");

  // Announce ourselves so real nodes/apps that hear this build their node
  // list entry (long/short name, hardware model) right away instead of
  // only after their next unrelated interaction with us -- see the
  // sendNodeInfo() comment for why this matters for DMs specifically.
  if (g_started) sendNodeInfo(kNodenumBroadcast);
  return g_started;
}

void meshtastic_proto_loop() {
  if (!g_started) return;
  SX1262 &radio = lora_radio_instance();
  uint16_t irq = radio.getIrqStatus();
  if (!(irq & RADIOLIB_SX126X_IRQ_RX_DONE)) return;

  // readData() (and startReceive(), called again below) clear IRQ status
  // internally -- clearIrqStatus() itself is protected in RadioLib's public
  // API, not meant to be called directly here.
  if (irq & RADIOLIB_SX126X_IRQ_CRC_ERR) {
    Serial.println("[MESH] Paket mit CRC-Fehler verworfen");
  } else {
    size_t len = radio.getPacketLength();
    if (len >= sizeof(PacketHeaderRaw) && len <= 255) {
      static uint8_t buf[255];
      int16_t state = radio.readData(buf, len);
      if (state == RADIOLIB_ERR_NONE) {
        handleReceivedPacket(buf, len);
      } else {
        Serial.printf("[MESH] readData() -> %d\n", state);
      }
    }
  }
  radio.startReceive(); // resume listening
}

bool meshtastic_proto_send_text(const char *text) {
  meshtastic_Data data = meshtastic_Data_init_zero;
  data.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
  size_t textLen = strlen(text);
  if (textLen > sizeof(data.payload.bytes)) textLen = sizeof(data.payload.bytes);
  memcpy(data.payload.bytes, text, textLen);
  data.payload.size = textLen;

  Serial.printf("[MESH] Sende Text \"%s\" (oeffentlicher Kanal, Broadcast)\n", text);
  // Broadcast on the public channel, not addressed to a specific node --
  // real firmware "Never set[s] the want_ack flag on broadcast packets"
  // (Router.cpp), so this can never get a real delivery ACK back, only a
  // local "radio accepted it for transmission" confirmation. This is the
  // generic test path (`mesh send` serial command); emergency reports use
  // meshtastic_send_emergency() instead, which sends a direct message on
  // the private channel and DOES get a real ACK.
  return sendData(kNodenumBroadcast, data, g_publicChannel);
}

// 2026-09-18 decision (real-device testing found broadcasts never show as
// "delivered" in Meshtastic apps -- confirmed against source, broadcasts
// are never ACKed by design): emergency reports are now a direct message,
// on the private "kayna-funkt" channel, to the configured dispatch node
// (station_config().dispatchNodeNum), with a real ACK request. This DOES
// get a genuine ROUTING_APP ack back (already verified against a real
// device) -- meshtastic_proto_emergency_ack_received() reports whether one
// actually arrived, for ui_model.cpp to wait on instead of just trusting
// the local transmit() result.
//
// Uses PRIVATE_APP (portnum 256, officially reserved for exactly this),
// NOT TEXT_MESSAGE_APP: found live against a real device that modern
// Meshtastic firmware's Router.cpp rejects non-PKI direct messages on
// TEXT_MESSAGE_APP outright ("Rejecting legacy DM" -- a deliberate security
// policy, not a bug) even when the channel/PSK/hash are all correct.
// PRIVATE_APP isn't subject to that check. handleReceivedPacket() treats
// PRIVATE_APP the same as TEXT_MESSAGE_APP for our own LAGE: parsing.
//
// Trade-off the user explicitly accepted: this only reaches the one
// configured dispatch node, not everyone on the public channel. If wider
// broadcast reach is later wanted alongside the ACK, that would mean
// sending both (more airtime per report) -- not implemented, flagged here
// for whoever revisits this.
bool meshtastic_send_emergency(const char *category, const char *type, const char *label) {
  uint32_t dispatch = station_config().dispatchNodeNum;
  if (dispatch == 0 || dispatch == kNodenumBroadcast) {
    Serial.println("[MESH] send_emergency: keine Leitstelle konfiguriert (station_config().dispatchNodeNum) -- 'dispatch set "
                    "<hex-node-id>' ueber Serial");
    g_pendingEmergencyPacketId = 0;
    g_emergencyAckReceived = false;
    return false;
  }

  char buf[160];
  snprintf(buf, sizeof(buf), "LAGE:NEU;%s;offen;%s (Touch-Terminal, Typ: %s)", category, label, type);

  meshtastic_Data data = meshtastic_Data_init_zero;
  data.portnum = meshtastic_PortNum_PRIVATE_APP;
  size_t textLen = strlen(buf);
  if (textLen > sizeof(data.payload.bytes)) textLen = sizeof(data.payload.bytes);
  memcpy(data.payload.bytes, buf, textLen);
  data.payload.size = textLen;

  g_emergencyAckReceived = false;
  uint32_t packetId = 0;
  Serial.printf("[MESH] Sende Notmeldung \"%s\" als Direktnachricht an Leitstelle !%08x (Kanal \"%s\")\n", buf,
                (unsigned)dispatch, g_privateChannel.name);
  bool ok = sendData(dispatch, data, g_privateChannel, kHopLimitDefault, /*wantAck=*/true, &packetId);
  g_pendingEmergencyPacketId = ok ? packetId : 0;
  if (ok) ui_model_notify_tx();
  return ok;
}

bool meshtastic_proto_emergency_ack_received() { return g_emergencyAckReceived; }

uint32_t meshtastic_proto_my_node_num() { return g_myNodeNum; }

// Issue #13: periodic status so the Leitstelle can notice a station going
// silent, not just receive emergency reports. Broadcast (not a direct
// message -- no ACK expected or needed, same "presence" pattern real
// Meshtastic firmware uses for its own periodic NodeInfo broadcasts) on the
// private channel, since station health is kayna-funkt-internal operational
// info, not something the public channel needs to see.
//
// Deliberately honest about what this board can't actually measure yet:
// battery/solar state (Issue #5) and sabotage detection (Issue #7) don't
// exist in hardware, so this reports "n/v" (nicht verfuegbar) for both
// instead of inventing plausible-looking numbers. Only uptime and the
// station ID are real. No Zustandsmodell (Issue #10) on this board yet
// either, so there's no real "Betriebszustand" to report beyond "laeuft".
bool meshtastic_send_heartbeat() {
  char buf[160];
  snprintf(buf, sizeof(buf), "STATUS:%s;uptime=%lus;zustand=laeuft;akku=n/v;sabotage=n/v",
           station_config().stationId.c_str(), millis() / 1000);

  meshtastic_Data data = meshtastic_Data_init_zero;
  data.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
  size_t textLen = strlen(buf);
  if (textLen > sizeof(data.payload.bytes)) textLen = sizeof(data.payload.bytes);
  memcpy(data.payload.bytes, buf, textLen);
  data.payload.size = textLen;

  Serial.printf("[MESH] Sende Heartbeat \"%s\" (Kanal \"%s\", Broadcast)\n", buf, g_privateChannel.name);
  return sendData(kNodenumBroadcast, data, g_privateChannel);
}
