#pragma once

#include <Arduino.h>

// Narrow interface for "post a plain-text message to the destination
// chat/channel". The real implementation lives in `telegram.cpp`
// (`RealBotClient`) and owns a `WiFiClientSecure`. A fake lives under
// `test/support/fake_bot_client.h` and lets tests assert exactly which
// messages were sent and simulate failures.
class IBotClient
{
public:
    virtual ~IBotClient() = default;

    // Returns true iff the message was accepted end-to-end (HTTP 200
    // and the API echoed `"ok":true`). False on any transport or
    // parse failure — the caller is responsible for the retry /
    // reboot policy.
    virtual bool sendMessage(const String &text) = 0;
};
