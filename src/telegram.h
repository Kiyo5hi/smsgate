#pragma once

#include <Arduino.h>

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
};
