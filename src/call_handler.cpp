#include "call_handler.h"
#include "sms_codec.h"

CallHandler::CallHandler(IModem &modem, IBotClient &bot, CallHandler::ClockFn clock)
    : modem_(modem), bot_(bot), clock_(std::move(clock))
{
}

void CallHandler::onUrcLine(const String &line)
{
    // First, let time advance — the caller may not call tick() between
    // every URC line, and we want the cooldown-to-idle transition to
    // happen before we inspect state.
    uint32_t now = clock_ ? clock_() : 0;
    maybeExitCooldown(now);

    if (state_ == State::Cooldown)
    {
        // Suppress everything — the call is being torn down.
        return;
    }

    // RING URC: bare line, no payload. On A76xx this fires every ~3s
    // until hangup.
    if (line.startsWith("RING"))
    {
        // Guard against false positives like "RINGING" — real modems
        // emit exactly "RING\r\n". The length check rejects prefix
        // garbage without being too strict about trailing whitespace
        // (main.cpp already trims).
        if (line.length() != 4)
        {
            return;
        }

        if (state_ == State::Idle)
        {
            state_ = State::Ringing;
            ringingStartedMs_ = now;
        }
        sawRing_ = true;

        // If +CLIP already landed earlier in the same line batch, we
        // have everything we need — commit now and skip the deadline.
        if (sawClip_)
        {
            commitRinging();
        }
        return;
    }

    // +CLIP URC: carries the caller number. Interleaved with RING.
    if (line.startsWith("+CLIP:"))
    {
        String number;
        if (!sms_codec::parseClipLine(line, number))
        {
            return; // malformed, ignore
        }

        if (state_ == State::Idle)
        {
            state_ = State::Ringing;
            ringingStartedMs_ = now;
        }
        sawClip_ = true;
        number_ = number;

        // If RING already arrived, we have both halves — commit now.
        if (sawRing_)
        {
            commitRinging();
        }
        return;
    }

    // Other URCs are not our concern.
}

void CallHandler::tick()
{
    uint32_t now = clock_ ? clock_() : 0;

    if (state_ == State::Cooldown)
    {
        maybeExitCooldown(now);
        return;
    }

    if (state_ == State::Ringing)
    {
        // We're waiting for the second half of the {RING, +CLIP} pair.
        // If we've been waiting past the deadline, commit what we have
        // — either the number we already captured from +CLIP, or
        // "Unknown" if only RING arrived.
        if (now - ringingStartedMs_ >= kUnknownNumberDeadlineMs)
        {
            commitRinging();
        }
    }
}

void CallHandler::commitRinging()
{
    // Build the Telegram notification. humanReadablePhoneNumber lives
    // in sms_codec and handles the "+86 xxx-xxxx-xxxx" formatting —
    // reused to keep the look-and-feel consistent with SMS forwards.
    String display;
    if (number_.length() == 0)
    {
        display = "Unknown";
    }
    else
    {
        display = sms_codec::humanReadablePhoneNumber(number_);
    }

    String msg = String("\xF0\x9F\x93\x9E Incoming call from ") + display + " (auto-rejected)";

    Serial.print("Incoming call from ");
    Serial.print(display);
    Serial.println(", posting and hanging up.");

    callsReceived_++;  // RFC-0043: track lifetime call count

    // Best-effort notify. We don't share SmsHandler's reboot budget —
    // a flaky Telegram connection during a call shouldn't nuke the
    // device, especially because SmsHandler's own counter already
    // tracks that health signal.
    bot_.sendMessage(msg);

    // Auto-hangup. Try the TinyGSM path first; fall back to raw
    // AT+CHUP as a belt-and-braces measure if TinyGSM returns false.
    if (!modem_.callHangup())
    {
        Serial.println("callHangup() failed, falling back to AT+CHUP.");
        modem_.sendAT("+CHUP");
        modem_.waitResponseOk(1000UL);
    }

    // Enter cooldown regardless of hangup success — we've done all we
    // can, and the dedupe window keeps us from double-notifying on
    // continued RINGs if the hangup didn't take.
    uint32_t now = clock_ ? clock_() : 0;
    cooldownUntilMs_ = now + kDedupeWindowMs;
    state_ = State::Cooldown;

    // Clear per-event scratch.
    sawRing_ = false;
    sawClip_ = false;
    number_ = String();
    ringingStartedMs_ = 0;
}

void CallHandler::maybeExitCooldown(uint32_t now)
{
    if (state_ != State::Cooldown)
        return;
    if (now >= cooldownUntilMs_)
    {
        state_ = State::Idle;
        cooldownUntilMs_ = 0;
    }
}
