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
};
