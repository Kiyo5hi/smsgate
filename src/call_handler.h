#pragma once

#include <Arduino.h>
#include <functional>
#include <stdint.h>

#include "imodem.h"
#include "ibot_client.h"

// Stateful incoming-call pipeline. Watches RING / +CLIP URCs forwarded
// from main.cpp's serial drain, coalesces the ~3s repeating RINGs into
// a single event, posts a Telegram notification, and auto-hangs the
// call. Depends only on the injected IModem + IBotClient + ClockFn so
// the whole class is reachable from the native test env.
//
// State machine:
//
//   idle  -- RING received --> ringing (wait for +CLIP to carry number)
//   idle  -- +CLIP received --> ringing (wait for RING to confirm it's
//                                        actually a call)
//
// Entering `ringing` requires BOTH a RING and a +CLIP, OR a `tick()`
// timeout in the "RING only, no +CLIP" case — some firmware stacks can
// drop +CLIP under load. See `kUnknownNumberDeadlineMs`.
//
// Once the ringing event is committed (notification sent, hangup
// issued), we enter a `cooldown` state for `kDedupeWindowMs` during
// which further RING / +CLIP URCs from the same call are suppressed.
// After the cooldown expires (via `tick()`), we return to idle and a
// new call will re-arm the pipeline. A successful `callHangup()` also
// starts the cooldown timer.
class CallHandler
{
public:
    // How long to suppress further RING/+CLIP URCs after committing a
    // ringing event. Slightly longer than two ring periods (~3s each)
    // so one real call never overlaps itself; short enough that two
    // distinct back-to-back calls still produce two notifications.
    static constexpr uint32_t kDedupeWindowMs = 6000;

    // If we see a RING but no +CLIP ever arrives, commit the event
    // anyway after this long with "Unknown" as the caller. Keeps
    // withheld / CLI-stripped calls from being silently ignored.
    static constexpr uint32_t kUnknownNumberDeadlineMs = 1500;

    // Clock callback injected from the composition root so tests can
    // drive a virtual clock instead of waiting for wall time. Production
    // passes a lambda over `millis()`; tests pass a lambda over a local
    // counter. Nested in the class to avoid colliding with the
    // SmsHandler::ClockFn alias defined at namespace scope in sms_handler.h.
    using ClockFn = std::function<uint32_t()>;

    CallHandler(IModem &modem, IBotClient &bot, ClockFn clock);

    // Called from main.cpp's SerialAT drain for every line that isn't
    // a +CMTI: (which stays with SmsHandler). Safe to call with any
    // line — unrecognized ones are ignored.
    void onUrcLine(const String &line);

    // Called once per loop iteration from main.cpp. Drives the
    // unknown-number deadline (commit a "Unknown" event if we saw
    // RING but no +CLIP within the window) and the cooldown-to-idle
    // transition. Cheap — constant time, no AT traffic if nothing to do.
    void tick();

    // RFC-0043: Lifetime call counter (RAM only, reset on reboot).
    int callsReceived() const { return callsReceived_; }

    // RFC-0110: Reset call counter.
    void resetStats() { callsReceived_ = 0; }

    // RFC-0100 / RFC-0108: Optional callback fired when a call event is committed.
    // Receives the caller's phone number string (empty string if unknown) and the
    // Telegram message_id of the call notification (0 if the send failed or the
    // bot returned no ID). Production wires this to enqueue an auto-reply SMS
    // (RFC-0100) and to register the (messageId, number) pair in ReplyTargetMap
    // (RFC-0108) so future Telegram replies route back to the caller as SMS.
    void setOnCallFn(std::function<void(const String &number, int32_t messageId)> fn)
    {
        onCallFn_ = std::move(fn);
    }

    // RFC-0164: Toggle call Telegram notifications. When false, calls are
    // still auto-rejected but no Telegram notification is posted.
    void setCallNotifyEnabled(bool enabled) { callNotifyEnabled_ = enabled; }
    bool callNotifyEnabled() const { return callNotifyEnabled_; }

    // RFC-0165: Runtime-configurable call dedup/cooldown window.
    // Default: kDedupeWindowMs (6000ms). Range enforced by caller.
    void setDedupeWindowMs(uint32_t ms) { dedupeWindowMs_ = ms; }
    uint32_t dedupeWindowMs() const { return dedupeWindowMs_; }

    // RFC-0166: Runtime-configurable RING-without-+CLIP commit deadline.
    // Default: kUnknownNumberDeadlineMs (1500ms). Range enforced by caller.
    void setUnknownDeadlineMs(uint32_t ms) { unknownDeadlineMs_ = ms; }
    uint32_t unknownDeadlineMs() const { return unknownDeadlineMs_; }

    // RFC-0180: Last-caller info. Updated on each committed call event.
    // lastCallerNumber() returns "" if no call has been received yet.
    // lastCallTimeMs() returns the clock() value at commit time (0 = none).
    const String &lastCallerNumber() const { return lastCallerNumber_; }
    uint32_t      lastCallTimeMs()   const { return lastCallTimeMs_; }

    // Test-only accessors.
    enum class State
    {
        Idle,
        Ringing,  // have seen at least one of {RING, +CLIP}, waiting for the other or for deadline
        Cooldown, // event committed, suppressing further URCs until kDedupeWindowMs elapses
    };
    State state() const { return state_; }

private:
    void commitRinging();
    void maybeTransitionToCooldown(uint32_t now);
    void maybeExitCooldown(uint32_t now);

    IModem &modem_;
    IBotClient &bot_;
    ClockFn clock_;

    State state_ = State::Idle;

    // Per-event scratch — cleared on every transition into `Idle`.
    bool sawRing_ = false;
    bool sawClip_ = false;
    String number_;                 // populated from +CLIP; "" = withheld / not yet seen
    uint32_t ringingStartedMs_ = 0; // when we first entered Ringing — drives the unknown-number deadline

    // Cooldown bookkeeping — when the current suppression window ends.
    uint32_t cooldownUntilMs_ = 0;
    int callsReceived_ = 0;  // RFC-0043
    bool callNotifyEnabled_ = true;  // RFC-0164
    uint32_t dedupeWindowMs_ = kDedupeWindowMs;           // RFC-0165: runtime-settable
    uint32_t unknownDeadlineMs_ = kUnknownNumberDeadlineMs; // RFC-0166: runtime-settable
    std::function<void(const String &, int32_t)> onCallFn_; // RFC-0100/0108
    String   lastCallerNumber_;   // RFC-0180: phone (or "" = none, "(Unknown)" = withheld)
    uint32_t lastCallTimeMs_ = 0; // RFC-0180: clock() at last commit (0 = none)
};
