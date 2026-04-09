#pragma once

#include <Arduino.h>

#include "imodem.h"

// Outbound SMS sender (RFC-0003 §3). Wraps IModem::sendSMS with:
//   - ASCII gate: bails on bodies with any byte > 0x7F. The first
//     cut intentionally does NOT support Unicode SMS — that requires
//     either RFC-0002's full PDU encoder or TinyGSM's UTF16 path
//     plus UTF-8 -> UTF-16BE byte conversion. The orchestrator
//     turns the failure into a Telegram error reply.
//   - PDU mode restoration: TinyGSM's sendSMS internally flips the
//     modem to text mode (`+CMGF=1`), which silently breaks the
//     receive path (which expects PDU mode). After every send
//     attempt — successful or not — we send `+CMGF=0` so the next
//     incoming SMS still parses correctly.
//
// `lastError` returns a human-readable string for the last failed
// send so the poller can include it in the error reply to the user.
class ISmsSender
{
public:
    virtual ~ISmsSender() = default;

    // Send `body` to phone number `number`. Returns true on success.
    virtual bool send(const String &number, const String &body) = 0;

    // Reason for the most recent failure, or "" if none / last call
    // succeeded. Stable until the next send() call.
    virtual const String &lastError() const = 0;
};

class SmsSender : public ISmsSender
{
public:
    explicit SmsSender(IModem &modem);

    bool send(const String &number, const String &body) override;
    const String &lastError() const override { return lastError_; }

private:
    static bool isAscii(const String &s);
    void restorePduMode();

    IModem &modem_;
    String lastError_;
};
