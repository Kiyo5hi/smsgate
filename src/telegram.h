#pragma once

#include <Arduino.h>
#include <vector>

#include "ibot_client.h"

// Initialize the TLS client and open the first connection to
// api.telegram.org. Returns true on success. Separate from the
// RealBotClient constructor because it does network I/O at startup
// that we want to happen inside setup(), not at static-init time.
bool setupTelegramClient();

// Production IBotClient implementation. Thin shim: the TLS client and
// HTTP formatting live as file-static state inside telegram.cpp (their
// lifetime is the whole program, so there's no point storing them on
// the instance).
class RealBotClient : public IBotClient
{
public:
    bool sendMessage(const String &text) override;
    int32_t sendMessageReturningId(const String &text) override;
    bool pollUpdates(int32_t sinceUpdateId, int32_t timeoutSec,
                     std::vector<TelegramUpdate> &out) override;

private:
    // Shared HTTP-POST helper used by both sendMessage variants.
    // On HTTP+API success, returns the parsed `result.message_id`
    // (always > 0). On any failure (transport, HTTP non-2xx, JSON
    // parse, ok!=true) returns 0. Tests don't see this — only the
    // public overrides above.
    int32_t doSendMessage(const String &text);
};
