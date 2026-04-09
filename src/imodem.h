#pragma once

#include <Arduino.h>
#include <stdint.h>

// Narrow interface over the TinyGSM modem. The SMS pipeline only uses
// `sendAT` + `waitResponse`; that's all this exposes. A real
// implementation wraps the global `TinyGsm modem` instance (see
// `real_modem.h`); a fake lives under `test/support/fake_modem.h`.
//
// All methods take a single String command so the class is trivially
// fakeable. TinyGSM's variadic sendAT() can still be used in main.cpp
// for the modem-configuration sequence that happens before the SMS
// pipeline starts — this interface is only for the pipeline itself.
class IModem
{
public:
    virtual ~IModem() = default;

    // Send `AT<cmd>\r\n` to the modem. `cmd` must NOT include the
    // leading "AT" or the trailing CRLF — the adapter adds them.
    virtual void sendAT(const String &cmd) = 0;

    // Wait up to `timeoutMs` for an OK/ERROR terminator, appending the
    // full modem response (including the terminator) into `out`.
    // Returns the same int8_t the real TinyGSM returns:
    //    1  -> matched OK
    //    2  -> matched ERROR
    //   -1  -> timeout
    virtual int8_t waitResponse(uint32_t timeoutMs, String &out) = 0;

    // Thin convenience: wait for OK with no response captured. Used on
    // fire-and-forget commands like AT+CMGD.
    virtual int8_t waitResponseOk(uint32_t timeoutMs) = 0;
};
