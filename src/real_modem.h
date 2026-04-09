#pragma once

#include <Arduino.h>
#include <TinyGsmClient.h>

#include "imodem.h"

// Production adapter: forwards every IModem call to the real TinyGsm
// instance. Header-only because it's trivial and only used in one
// translation unit (main.cpp). NOT compiled in the native test env —
// it has a hard dependency on TinyGSM and real Arduino Serial.
class RealModem : public IModem
{
public:
    explicit RealModem(TinyGsm &modem) : modem_(modem) {}

    void sendAT(const String &cmd) override
    {
        modem_.sendAT(cmd);
    }

    int8_t waitResponse(uint32_t timeoutMs, String &out) override
    {
        return modem_.waitResponse(timeoutMs, out);
    }

    int8_t waitResponseOk(uint32_t timeoutMs) override
    {
        return modem_.waitResponse(timeoutMs);
    }

private:
    TinyGsm &modem_;
};
