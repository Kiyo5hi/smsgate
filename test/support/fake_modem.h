#pragma once

#include <Arduino.h>
#include <vector>

#include "imodem.h"

// Scripted fake modem for SmsHandler tests. Queues pre-canned
// responses for `waitResponse` calls and records every AT command the
// handler sends so tests can assert the exact protocol sequence
// (CMGR, CMGD, CMGL, etc.).
class FakeModem : public IModem
{
public:
    struct Response
    {
        int8_t code;  // 1 OK, 2 ERROR, -1 timeout; matches TinyGSM convention
        String data;
    };

    // Append an AT response to the FIFO the next waitResponse() will drain.
    void queueResponse(int8_t code, const String &data)
    {
        Response r;
        r.code = code;
        r.data = data;
        responses_.push_back(r);
    }

    // Convenience: queue an OK response carrying `data`.
    void queueOk(const String &data) { queueResponse(1, data); }

    // Convenience: queue a bare OK with empty payload (for CMGD).
    void queueOkEmpty() { queueResponse(1, String()); }

    // ---- IModem ----

    void sendAT(const String &cmd) override
    {
        sent_.push_back(cmd);
    }

    int8_t waitResponse(uint32_t /*timeoutMs*/, String &out) override
    {
        if (responses_.empty())
        {
            out = String();
            return -1;
        }
        Response r = responses_.front();
        responses_.erase(responses_.begin());
        out = r.data;
        return r.code;
    }

    int8_t waitResponseOk(uint32_t /*timeoutMs*/) override
    {
        if (responses_.empty())
            return -1;
        int8_t code = responses_.front().code;
        responses_.erase(responses_.begin());
        return code;
    }

    bool callHangup() override
    {
        callHangupCalls_++;
        if (!callHangupResults_.empty())
        {
            bool r = callHangupResults_.front();
            callHangupResults_.erase(callHangupResults_.begin());
            return r;
        }
        return callHangupDefault_;
    }

    // Queue the return value for the next callHangup() call. Falls back
    // to `callHangupDefault_` (default true) if the queue is empty.
    void queueCallHangupResult(bool ok) { callHangupResults_.push_back(ok); }

    // Set the fallback return value for callHangup() when the queue is drained.
    void setCallHangupDefault(bool ok) { callHangupDefault_ = ok; }

    // ---- IModem::sendSMS (RFC-0003) ----

    struct SmsSendCall
    {
        String number;
        String text;
    };

    bool sendSMS(const String &number, const String &text) override
    {
        SmsSendCall c;
        c.number = number;
        c.text = text;
        smsSendCalls_.push_back(c);
        if (!smsSendResults_.empty())
        {
            bool r = smsSendResults_.front();
            smsSendResults_.erase(smsSendResults_.begin());
            return r;
        }
        return smsSendDefault_;
    }

    void queueSmsSendResult(bool ok) { smsSendResults_.push_back(ok); }
    void setSmsSendDefault(bool ok) { smsSendDefault_ = ok; }
    const std::vector<SmsSendCall> &smsSendCalls() const { return smsSendCalls_; }

    // ---- IModem::sendPduSms (Unicode TX) ----

    struct PduSendCall
    {
        String pduHex;
        int tpduLen;
    };

    // Returns the queued MR (>= 0 on success, -1 on failure).
    // Queue with queuePduSendResult(mr) where mr >= 0 means success,
    // mr == -1 means failure. Default is 0 (success, MR=0).
    int sendPduSms(const String &pduHex, int tpduLen) override
    {
        PduSendCall c;
        c.pduHex = pduHex;
        c.tpduLen = tpduLen;
        pduSendCalls_.push_back(c);
        if (!pduSendResults_.empty())
        {
            int mr = pduSendResults_.front();
            pduSendResults_.erase(pduSendResults_.begin());
            return mr;
        }
        return pduSendDefault_;
    }

    // Queue an MR result: pass >= 0 for success (MR value), -1 for failure.
    void queuePduSendResult(int mr) { pduSendResults_.push_back(mr); }
    // Set the default MR for calls with no queued result. Default is 0 (success).
    void setPduSendDefault(int mr) { pduSendDefault_ = mr; }
    const std::vector<PduSendCall> &pduSendCalls() const { return pduSendCalls_; }

    // ---- Inspection helpers for tests ----

    const std::vector<String> &sentCommands() const { return sent_; }

    size_t responsesRemaining() const { return responses_.size(); }

    int callHangupCalls() const { return callHangupCalls_; }

private:
    std::vector<Response> responses_;
    std::vector<String> sent_;
    std::vector<bool> callHangupResults_;
    bool callHangupDefault_ = true;
    int callHangupCalls_ = 0;
    std::vector<SmsSendCall> smsSendCalls_;
    std::vector<bool> smsSendResults_;
    bool smsSendDefault_ = true;
    std::vector<PduSendCall> pduSendCalls_;
    std::vector<int> pduSendResults_;
    int pduSendDefault_ = 0; // default: success, MR=0
};
