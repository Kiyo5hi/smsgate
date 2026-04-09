#pragma once

#include <Arduino.h>
#include <functional>

#include "imodem.h"
#include "ibot_client.h"

// Reboot callback injected from the composition root. Production code
// passes a lambda that calls `ESP.restart()`; tests pass a lambda that
// bumps a counter so the reboot path is actually exercised.
using RebootFn = std::function<void()>;

// Stateful SMS pipeline. Owns the consecutive-failure counter and the
// reboot threshold; depends only on the injected IModem, IBotClient,
// and RebootFn. No globals, no Arduino hardware access, so the whole
// class is reachable from the native test env.
class SmsHandler
{
public:
    // After this many consecutive Telegram send failures, the handler
    // calls the injected RebootFn to escape stuck TLS / WiFi / DNS /
    // TinyGSM states. Public so tests can reference it by name.
    static constexpr int MAX_CONSECUTIVE_FAILURES = 8;

    SmsHandler(IModem &modem, IBotClient &bot, RebootFn reboot);

    // Read the SMS at SIM index <idx>, forward it to the bot, and
    // delete it from the SIM on success. Leaves the SMS in place on
    // failure so a later retry can pick it up. After
    // MAX_CONSECUTIVE_FAILURES the reboot callback fires.
    void handleSmsIndex(int idx);

    // Drain every SMS currently on the SIM via AT+CMGL. Used at startup
    // to catch up on messages that arrived while the bridge was offline.
    void sweepExistingSms();

    // Test-only accessor — no runtime caller.
    int consecutiveFailures() const { return consecutiveFailures_; }

private:
    IModem &modem_;
    IBotClient &bot_;
    RebootFn reboot_;
    int consecutiveFailures_ = 0;
};
