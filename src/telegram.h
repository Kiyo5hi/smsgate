#pragma once

#include <Arduino.h>
#include <Client.h>
#include <vector>

#include "ibot_client.h"

// Production IBotClient implementation. Owns a pointer to the active
// transport (either the WiFi TLS client or the modem TLS client). The
// transport is injected at runtime via setTransport() so both paths share
// a single HTTP layer. The concrete client objects live as file-static state
// inside telegram.cpp (process lifetime).
class RealBotClient : public IBotClient
{
public:
    // Set the active transport. Must be called before any send/poll operation.
    // Calling this mid-operation (i.e. while a request is in flight) is NOT
    // safe; only call it from setup() or from the loop() WiFi-drop handler
    // after the current tick completes.
    void setTransport(Client &c) { transport_ = &c; }

    // Set the outbound destination for sendMessage / sendMessageReturningId.
    // Must be called in setup() after parsing the allow list. With the
    // multi-user allow list (RFC-0014) this is always allowedIds[0] (the
    // admin). Replaces the old file-static `chatID` in telegram.cpp.
    void setAdminChatId(int64_t id) { adminChatId_ = id; }

    // Returns the active transport, or nullptr if none has been set.
    Client *getTransport() const { return transport_; }

    bool sendMessage(const String &text) override;
    int32_t sendMessageReturningId(const String &text) override;
    bool sendMessageTo(int64_t chatId, const String &text) override;
    int32_t sendMessageToReturningId(int64_t chatId, const String &text) override;
    bool pollUpdates(int32_t sinceUpdateId, int32_t timeoutSec,
                     std::vector<TelegramUpdate> &out) override;

private:
    // Active transport. Points to either telegramWifiClient (WiFiClientSecure)
    // or telegramModemClient (GsmClientSecureA76xxSSL), both file-static in
    // telegram.cpp.
    Client *transport_ = nullptr;

    // Outbound destination for sendMessage / sendMessageReturningId.
    // Set via setAdminChatId() in setup() after parsing the allow list.
    // Replaces the old file-static `chatID` compile-time string in telegram.cpp.
    int64_t adminChatId_ = 0;

    // Shared HTTP-POST helper used by all sendMessage variants.
    // On HTTP+API success, returns the parsed `result.message_id`
    // (always > 0). On any failure (transport, HTTP non-2xx, JSON
    // parse, ok!=true) returns -1 or 0 (see body for guard).
    // Tests don't see this — only the public overrides above.
    int32_t doSendMessage(const String &text, int64_t chatId);
};

// Initialize the WiFi TLS client and open the first connection to
// api.telegram.org. Returns true on success. Also calls bot.setTransport()
// to inject the WiFi client so subsequent bot calls use this path.
bool setupTelegramClient(RealBotClient &bot);

// Initialize the cellular TLS client and open the first connection to
// api.telegram.org. Must be called after modem.gprsConnect().
// Uses authmode=0 (no server certificate verification) on the modem TLS
// socket — cert verification is NOT available on the modem path (no
// setCACertBundle equivalent; the modem's AT+CSSLCFG requires a cert file
// uploaded to modem flash). A prominent [CELLULAR TLS] warning is printed
// at boot. Also calls bot.setTransport() to inject the modem client.
// Returns true on success.
bool setupCellularClient(RealBotClient &bot);

// Register bot commands with the Telegram Bot API via setMyCommands.
// Call once after setupTelegramClient() or setupCellularClient(). Best-effort
// — failure is logged but non-fatal (the commands still work, they just won't
// appear in the Telegram UI autocomplete). Uses bot.getTransport() to reach
// the already-active transport.
bool registerBotCommands(RealBotClient &bot);
