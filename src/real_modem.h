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
    // `stream` is the raw serial port (SerialAT) that TinyGSM wraps.
    // Needed for sendPduSms, which writes the PDU hex + Ctrl-Z directly
    // after AT+CMGS prompts '>'.
    RealModem(TinyGsm &modem, Stream &stream)
        : modem_(modem), stream_(stream) {}

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

    bool callHangup() override
    {
        return modem_.callHangup();
    }

    bool sendSMS(const String &number, const String &text) override
    {
        return modem_.sendSMS(number, text);
    }

    int sendPduSms(const String &pduHex, int tpduLen) override
    {
        // AT+CMGS=<tpduLen> — modem must already be in PDU mode.
        modem_.sendAT(String("+CMGS=") + String(tpduLen));
        // Wait for the '>' prompt (up to 10s).
        if (modem_.waitResponse(10000UL, GF(">")) != 1)
            return -1;
        // Write the PDU hex followed by Ctrl-Z.
        stream_.print(pduHex);
        stream_.write(static_cast<char>(0x1A));
        stream_.flush();
        // Wait for OK / +CMGS: (up to 60s for network round-trip).
        // Capture the response text so we can parse the +CMGS: <mr> line.
        String resp;
        int8_t rc = modem_.waitResponse(60000UL, resp);
        if (rc != 1)
            return -1;
        // Parse "+CMGS: <mr>" from the response.
        int cmgsIdx = resp.indexOf("+CMGS:");
        if (cmgsIdx >= 0)
        {
            String mrStr = resp.substring(cmgsIdx + 6);
            mrStr.trim();  // Arduino String::trim() returns void — can't chain
            int mr = mrStr.toInt();
            return mr; // 0-255
        }
        // OK but no +CMGS: line — some modem firmware variants omit it.
        // Return -1 (failure sentinel) so SmsSender does NOT store a
        // phantom MR=0 entry in the DeliveryReportMap. A real MR=0
        // would only be returned when the +CMGS: line IS present.
        return -1;
    }

    // RFC-0103: USSD relay. Sends AT+CUSD=1,<code>,15 and waits for the
    // +CUSD: URC carrying the carrier's response text. Returns the quoted
    // text field from "+CUSD: 0,\"<text>\",15", or empty string on failure.
    String ussdQuery(const String &code, uint32_t timeoutMs) override
    {
        modem_.sendAT(String("+CUSD=1,") + code + String(",15"));
        // Wait for either +CUSD: (success) or ERROR. The A76xx returns
        // +CUSD: synchronously within the AT command response window on
        // most firmware versions.
        String resp;
        int8_t rc = modem_.waitResponse(timeoutMs, resp);
        if (rc != 1 && rc != 2)
            return String(); // timeout
        // Parse "+CUSD: <n>,\"<text>\"" from the response.
        int cusdIdx = resp.indexOf("+CUSD:");
        if (cusdIdx < 0)
            return String(); // ERROR response or unexpected format
        // Skip past "+CUSD: <n>," to find the opening quote.
        int quoteOpen = resp.indexOf('"', cusdIdx);
        if (quoteOpen < 0)
            return String();
        int quoteClose = resp.indexOf('"', quoteOpen + 1);
        if (quoteClose < 0)
            return String();
        return resp.substring(quoteOpen + 1, quoteClose);
    }

private:
    TinyGsm &modem_;
    Stream &stream_;
};
