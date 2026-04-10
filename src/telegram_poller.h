#pragma once

#include <Arduino.h>
#include <functional>
#include <stdint.h>

#include "ibot_client.h"
#include "ipersist.h"
#include "reply_target_map.h"
#include "sms_alias_store.h"
#include "sms_sender.h"

class SmsDebugLog;

// Stateful Telegram -> SMS poller (RFC-0003 §1, §2, §4, §5).
//
// Wires together:
//   - IBotClient::pollUpdates(...)        — fetch new updates
//   - ReplyTargetMap                       — map reply_to_message_id to phone
//   - ISmsSender                           — send the actual SMS
//   - IPersist                             — persist last update_id watermark
//
// State machine: dead simple. `tick()` is called from main.cpp's loop().
// At most once every kPollIntervalMs we issue a getUpdates request and
// process the response. Each update goes through:
//
//   1. Authorization gate: ignore if `from.id` (or `chat.id` fallback)
//      isn't in the allow list. Update_id still advances.
//   2. Reply-to lookup: read `reply_to_message_id` from the update,
//      look up the phone number in the ring buffer. If the slot is
//      empty or the stored msg_id doesn't match, post an error
//      reply via the bot (so the user knows their reply went into
//      the void) and advance.
//   3. Send via SmsSender. On failure, post the SmsSender error
//      message back to the user (so they know what happened) and
//      advance.
//   4. On success, post a tiny confirmation reply.
//   5. update_id advances regardless of outcome (fail-closed).
//
// Implementation note (RFC-0003 first cut): we use SHORT polling
// (kPollTimeoutSec = 0) rather than long polling, with a 3-second
// poll interval. Long polling would block the IModem URC drain
// while the request is in flight (up to 25s), which would silently
// drop +CMTI / RING / +CLIP URCs. The right fix is a non-blocking
// HTTP state machine, but that's a meaningful chunk of work; the
// short-poll approach gets us the user-visible feature now and the
// data-cap cost is negligible (~30 KB/day at 3s poll interval).
// Tracked as a follow-up RFC.
//
// Tests: drive a virtual clock + a FakeBotClient + a FakePersist
// and assert which SmsSender calls / which bot replies happen.
class TelegramPoller
{
public:
    // How long between successive getUpdates requests, in
    // milliseconds. The first poll happens immediately on the
    // first tick after construction.
    static constexpr uint32_t kPollIntervalMs = 3000;

    // Telegram-side timeout in seconds, passed as `?timeout=<n>`.
    // 0 = short polling (returns immediately if no updates).
    static constexpr int32_t kPollTimeoutSec = 0;

    // Auth callback: returns true iff the given Telegram from-id is
    // allowed to send SMS. Production passes a lambda comparing
    // against the parsed TELEGRAM_CHAT_ID; tests pass any predicate.
    // Nested in the class to avoid colliding with SmsHandler::ClockFn
    // and CallHandler::ClockFn at namespace scope.
    using ClockFn = std::function<uint32_t()>;
    using AuthFn = std::function<bool(int64_t fromId)>;

    // Status callback: returns a formatted device-health string shown
    // by the /status bot command. Production passes a lambda that
    // closes over cached modem CSQ/registration values and counters
    // from SmsHandler, TelegramPoller, ReplyTargetMap, etc. Tests
    // pass nullptr (fallback: "(status not configured)") or a simple
    // lambda returning a canned string.
    using StatusFn = std::function<String()>;

    // Called by /adduser, /removeuser, and /listusers.
    // Signature: (callerId, cmd, targetId, reason&) -> bool
    //   - cmd is "add", "remove", or "list"
    //   - targetId is the user ID to add/remove (0 for "list")
    //   - On success: returns true; for "list", reason contains the reply text
    //   - On failure: returns false; reason contains the user-facing error message
    // The lambda is responsible for the admin check (callerId in compile-time list?).
    // nullptr means user management commands are disabled.
    using ListMutatorFn = std::function<bool(int64_t callerId, const String &cmd,
                                             int64_t targetId, String &reason)>;

    // Called by /block, /unblock, and /blocklist (RFC-0021).
    // Signature: (callerId, cmd, number, reason&) -> bool
    //   - cmd is "block", "unblock", or "list"
    //   - number is the phone number string (empty for "list")
    //   - On success: returns true; for "list", reason contains the reply text
    //   - On failure: returns false; reason contains the user-facing error message
    // The lambda is responsible for the admin check (callerId in compile-time list?).
    // nullptr means SMS block list bot commands are disabled.
    using SmsBlockMutatorFn = std::function<bool(int64_t callerId, const String &cmd,
                                                  const String &number, String &reason)>;

    // NOTE: TelegramPoller takes SmsSender& (the concrete type, not ISmsSender&)
    // so it can call enqueue() and drainQueue() which are on SmsSender only —
    // not on the ISmsSender interface (RFC-0012 Option A). This permanently
    // forecloses using an ISmsSender-only test double for TelegramPoller; any
    // test double must be a concrete SmsSender constructed with a FakeModem.
    // TelegramPoller holds a reference only — it does NOT own the SmsSender,
    // so lambdas capturing `this` stored in queue entries are safe as long as
    // both objects have process lifetime (which they do in main.cpp).
    TelegramPoller(IBotClient &bot,
                   SmsSender &smsSender,
                   ReplyTargetMap &replyTargets,
                   IPersist &persist,
                   ClockFn clock,
                   AuthFn auth,
                   StatusFn status = nullptr,
                   ListMutatorFn mutator = nullptr,
                   SmsBlockMutatorFn smsBlockMutator = nullptr);

    // Load the watermark from persist. Call once at startup, after
    // ReplyTargetMap::load(). Idempotent.
    void begin();

    // Drive the poller. Cheap if it's not yet time to poll. Issues
    // one getUpdates per kPollIntervalMs.
    void tick();

    // Optional: attach a diagnostic log. When set, non-reply messages
    // containing "debug" or "/debug" dump the log instead of the
    // generic help message.
    void setDebugLog(SmsDebugLog *log) { debugLog_ = log; }

    // RFC-0055: Optional NTP sync callback. When set, /ntp calls this
    // function (blocks until sync completes) and reports success/failure.
    // Production wires this to syncTime() in main.cpp.
    void setNtpSyncFn(std::function<void()> fn) { ntpSyncFn_ = std::move(fn); }

    // RFC-0069: Optional concat-group summary callback. When set, /concat
    // calls this function and returns the result. Production wires this to
    // smsHandler.concatGroupsSummary() in main.cpp.
    void setConcatSummaryFn(std::function<String()> fn) { concatSummaryFn_ = std::move(fn); }

    // RFC-0071: Optional WiFi reconnect trigger. When set, /wifi calls this
    // function (deferred — the actual reconnect runs in the next loop()
    // iteration). Production wires this to a lambda that sets a flag in
    // main.cpp; the flag is checked at the top of loop().
    void setWifiReconnectFn(std::function<void()> fn) { wifiReconnectFn_ = std::move(fn); }

    // RFC-0072: Optional heap info callback. When set, /heap calls this
    // function and returns the result. Production wires this to a lambda
    // that formats ESP.getFreeHeap() / getMinFreeHeap() / getMaxAllocHeap().
    void setHeapFn(std::function<String()> fn) { heapFn_ = std::move(fn); }

    // RFC-0074: Firmware build timestamp string. Set once in main.cpp from
    // __DATE__ + __TIME__. Returned verbatim by the /version command.
    void setVersionStr(const String &v) { versionStr_ = v; }

    // RFC-0088: Optional phone alias store. When set, /addalias, /rmalias,
    // and /aliases commands are enabled, and @name expansion works in /send
    // and /test.
    void setAliasStore(SmsAliasStore *store) { aliasStore_ = store; }

    // RFC-0092: Optional compact signal-health callback. When set, /csq
    // calls this function and returns the result. Production wires this to
    // a lambda that formats cachedCsq / cachedRegStatus / cachedOperatorName.
    void setCsqFn(std::function<String()> fn) { csqFn_ = std::move(fn); }

    // RFC-0098: Optional alert mute/unmute callbacks. When set, /mute <minutes>
    // calls muteFn(minutes) and /unmute calls unmuteFn(). Production wires
    // these to lambdas that set/clear s_alertsMutedUntilMs in main.cpp.
    void setMuteFn(std::function<void(uint32_t)> fn)   { muteFn_   = std::move(fn); }
    void setUnmuteFn(std::function<void()> fn)          { unmuteFn_ = std::move(fn); }

    // RFC-0103: Optional USSD relay callback. When set, /ussd <code> calls
    // ussdFn(code) and replies with the result. Returns an empty string on
    // failure. Production wires this to a lambda calling realModem.ussdQuery().
    void setUssdFn(std::function<String(const String &)> fn) { ussdFn_ = std::move(fn); }

    // RFC-0105: Optional SIM info callback. When set, /sim calls simInfoFn_()
    // and replies with a compact ICCID / IMEI / operator / CSQ snapshot.
    void setSimInfoFn(std::function<String()> fn) { simInfoFn_ = std::move(fn); }

    // RFC-0107: Optional AT passthrough. Signature: (fromId, cmd) -> response.
    // The lambda is responsible for the admin check (fromId must be the first
    // user in TELEGRAM_CHAT_IDS) and for rejecting dangerous commands.
    // Returns a non-empty string (the AT response or an error message).
    void setAtCmdFn(std::function<String(int64_t, const String &)> fn) { atCmdFn_ = std::move(fn); }

    // RFC-0110: Optional stats reset callback. When set, /resetstats calls
    // this to zero all session counters in SmsHandler, CallHandler, SmsSender.
    void setResetStatsFn(std::function<void()> fn) { resetStatsFn_ = std::move(fn); }

    // RFC-0119: Optional /ping summary callback. When set, /ping replies with
    // this fn's result (e.g. "🏓 Pong [label] | ⏱ 1d 2h | CSQ 18").
    // When not set, falls back to the simple "🏓 Pong" reply.
    void setPingSummaryFn(std::function<String()> fn) { pingSummaryFn_ = std::move(fn); }

    // RFC-0118: Device label get/set. When set, /label replies with the
    // current label and /setlabel <name> validates and saves a new one.
    void setLabelGetFn(std::function<String()> fn) { labelGetFn_ = std::move(fn); }
    void setLabelSetFn(std::function<void(const String &)> fn) { labelSetFn_ = std::move(fn); }

    // RFC-0114: Optional balance USSD code provider. Returns the configured
    // USSD balance code (e.g. "*100#") or an empty string if not set.
    // /balance calls ussdFn_ with this code. Production wires to a lambda
    // returning the USSD_BALANCE_CODE compile-time define (or "" if absent).
    void setBalanceCodeFn(std::function<String()> fn) { balanceCodeFn_ = std::move(fn); }

    // RFC-0112: Optional soft-reboot callback. When set, /reboot sends a
    // "Rebooting..." confirmation reply then calls this fn.  The fn receives
    // the caller's fromId so the production lambda can gate on admin status
    // (e.g. set s_pendingRestart only if fromId is in the admin list).
    void setRebootFn(std::function<void(int64_t)> fn) { rebootFn_ = std::move(fn); }

    // RFC-0120: Optional uptime callback. When set, /uptime calls this fn
    // and replies with the result (e.g. "⏱ 2d 3h 15m 42s").
    // When not set, replies "(uptime not configured)".
    void setUptimeFn(std::function<String()> fn) { uptimeFn_ = std::move(fn); }

    // RFC-0123: Optional boot info callback. When set, /boot calls this fn
    // and replies with the result (e.g. "🔄 Boot #42 | Reason: WDT | 2026-04-10 14:32 UTC").
    // When not set, replies "(boot info not configured)".
    void setBootInfoFn(std::function<String()> fn) { bootInfoFn_ = std::move(fn); }

    // RFC-0124: Optional session counter callback. When set, /count calls
    // this fn and replies with a compact summary of SMS/call counters.
    // When not set, replies "(count not configured)".
    void setCountFn(std::function<String()> fn) { countFn_ = std::move(fn); }

    // RFC-0126: Optional WiFi/IP info callback. When set, /ip calls this fn
    // and replies with IP address, SSID, and RSSI.
    // When not set, replies "(ip info not configured)".
    void setIpFn(std::function<String()> fn) { ipFn_ = std::move(fn); }

    // RFC-0127: Optional SIM slot usage callback. When set, /smsslots calls
    // this fn and replies with a slot-usage one-liner.
    // When not set, replies "(SMS slots info not configured)".
    void setSmsSlotssFn(std::function<String()> fn) { smsSlotsFn_ = std::move(fn); }

    // RFC-0128: Optional lifetime stats callback. When set, /lifetime calls
    // this fn and replies with lifetime SMS count + boot count.
    // When not set, replies "(lifetime stats not configured)".
    void setLifetimeFn(std::function<String()> fn) { lifetimeFn_ = std::move(fn); }

    // RFC-0129: Optional announce callback. When set, /announce <msg> calls
    // this fn with the message text and sends it to all authorized users.
    // The fn returns the number of users notified (as a string, e.g. "3").
    // When not set, replies "(announce not configured)".
    using AnnounceFn = std::function<int(const String &msg)>;
    void setAnnounceFn(AnnounceFn fn) { announceFn_ = std::move(fn); }

    // RFC-0130: Optional digest callback. When set, /digest calls this fn
    // and replies with an on-demand stats digest.
    // When not set, replies "(digest not configured)".
    void setDigestFn(std::function<String()> fn) { digestFn_ = std::move(fn); }

    // RFC-0137: Optional heartbeat interval setter. When set, /setinterval <N>
    // validates N (0 = disable, max 86400) and calls this fn. Confirmed with
    // "✅ Heartbeat interval set to N seconds." When not set, replies
    // "(setinterval not configured)".
    void setIntervalFn(std::function<void(uint32_t)> fn) { intervalFn_ = std::move(fn); }

    // RFC-0131: /note and /setnote — persistent device note stored in NVS.
    void setNoteGetFn(std::function<String()> fn) { noteGetFn_ = std::move(fn); }
    void setNoteSetFn(std::function<void(const String &)> fn) { noteSetFn_ = std::move(fn); }

    // RFC-0138: Optional max-consecutive-failures setter. When set,
    // /setmaxfail <N> updates the threshold (0 = never reboot, max 99).
    void setMaxFailFn(std::function<void(int)> fn) { maxFailFn_ = std::move(fn); }

    // RFC-0139: Optional SIM flush fn. The fn deletes all SMS and returns
    // the count of deleted slots (-1 if unknown). /flushsim yes invokes it.
    void setFlushSimFn(std::function<int()> fn) { flushSimFn_ = std::move(fn); }

    // RFC-0151: /getautoreply, /setautoreply, /clearautoreply.
    void setAutoReplyGetFn(std::function<String()> fn) { autoReplyGetFn_ = std::move(fn); }
    void setAutoReplySetFn(std::function<void(const String &)> fn) { autoReplySetFn_ = std::move(fn); }

    // RFC-0152: Reset the update_id watermark so Telegram re-delivers
    // recent updates. Persists immediately.
    void resetWatermark()
    {
        lastUpdateId_ = 0;
        persist_.saveBlob("last_update_id", &lastUpdateId_, sizeof(lastUpdateId_));
    }

    // RFC-0153: Optional forward enable/disable fn. When set, /setforward
    // on|off calls this fn with true/false.
    void setForwardingEnabledFn(std::function<void(bool)> fn) { forwardingEnabledFn_ = std::move(fn); }

    // RFC-0148: Optional SIM sweep fn. When set, /sweepsim calls this fn
    // (which runs smsHandler.sweepExistingSms()) and replies with count.
    void setSweepFn(std::function<int()> fn) { sweepFn_ = std::move(fn); }

    // RFC-0149: Optional health fn. When set, /health calls this fn and
    // replies with a compact single-line health summary.
    void setHealthFn(std::function<String()> fn) { healthFn_ = std::move(fn); }

    // RFC-0156: Optional SIM status fn. When set, /simstatus calls this fn
    // and replies with ICCID, IMSI, operator, and CSQ.
    void setSimStatusFn(std::function<String()> fn) { simStatusFn_ = std::move(fn); }

    // RFC-0158: Optional WiFi scan fn. When set, /wifiscan calls this fn
    // and replies with nearby SSIDs, channels, and RSSI values.
    void setWifiScanFn(std::function<String()> fn) { wifiScanFn_ = std::move(fn); }

    // RFC-0160: Optional max-parts setter fn. When set, /setmaxparts <N>
    // calls this fn with the new limit (1–10).
    void setMaxPartsFn(std::function<void(int)> fn) { maxPartsFn_ = std::move(fn); }

    // RFC-0161: Optional SIM SMS count fn. When set, /smscount calls this fn
    // and replies with used/total storage counts from AT+CPMS?.
    void setSmsCntFn(std::function<String()> fn) { smsCntFn_ = std::move(fn); }

    // RFC-0162: Optional block-mode toggle fn. When set, /setblockmode on|off
    // calls this fn with true/false to enable/disable block list enforcement.
    void setBlockingEnabledFn(std::function<void(bool)> fn) { blockingEnabledFn_ = std::move(fn); }

    // RFC-0163: Optional block-check fn. When set, /blockcheck <phone> calls
    // this fn and replies with whether the number would be blocked.
    void setBlockCheckFn(std::function<String(const String &)> fn) { blockCheckFn_ = std::move(fn); }

    // RFC-0146: Optional SMS forward fn. When set, /forwardsim <idx> calls
    // this fn with the SIM index and replies success/failure.
    void setSmsForwardFn(std::function<bool(int)> fn) { smsForwardFn_ = std::move(fn); }

    // RFC-0147: Change Telegram poll interval directly. The fn called by
    // /setpollinterval also calls setPollIntervalMs() on this poller.
    // Range: 1000–300000ms. Takes effect on the next tick.
    void setPollIntervalMs(uint32_t ms)
    {
        if (ms < 1000) ms = 1000;
        if (ms > 300000) ms = 300000;
        pollIntervalMs_ = ms;
    }

    // RFC-0144: Optional dedup window setter. When set, /setdedup <seconds>
    // changes the dedup window (0 = disable, max 3600).
    void setDedupWindowFn(std::function<void(uint32_t)> fn) { dedupWindowFn_ = std::move(fn); }

    // RFC-0145: Optional dedup clear fn. When set, /cleardedup calls this
    // fn and replies "✅ Dedup buffer cleared."
    void setClearDedupFn(std::function<void()> fn) { clearDedupFn_ = std::move(fn); }

    // RFC-0142: Optional concat TTL setter. When set, /setconcatttl <seconds>
    // updates the fragment TTL (range 60–604800 seconds).
    void setConcatTtlFn(std::function<void(uint32_t)> fn) { concatTtlFn_ = std::move(fn); }

    // RFC-0143: Optional modem info fn. When set, /modeminfo calls this fn
    // and replies with IMEI, model name, and firmware revision.
    void setModemInfoFn(std::function<String()> fn) { modemInfoFn_ = std::move(fn); }

    // RFC-0140: Optional SIM list fn. When set, /simlist calls this fn
    // and replies with a compact index→sender list.
    void setSimListFn(std::function<String()> fn) { simListFn_ = std::move(fn); }

    // RFC-0141: Optional SIM read fn. When set, /simread <idx> calls
    // this fn with the index and replies with the decoded SMS content.
    void setSimReadFn(std::function<String(int)> fn) { simReadFn_ = std::move(fn); }

    // RFC-0121: Optional network info callback. When set, /network calls this
    // fn and replies with the result (e.g. "📶 Operator: T-Mobile | Reg: home | CSQ 18 (good)").
    // When not set, replies "(network info not configured)".
    void setNetworkFn(std::function<String()> fn) { networkFn_ = std::move(fn); }

    // Test introspection.
    int32_t lastUpdateId() const { return lastUpdateId_; }
    int pollAttempts() const { return pollAttempts_; }

private:
    // Process one parsed update. Returns the (possibly updated)
    // watermark to advance to. On any error path, still advances
    // past `u.updateId`.
    void processUpdate(const TelegramUpdate &u);

    // Send an error reply via the bot to the given chat ID. Best-effort;
    // failures are logged but not retried (the user-visible failure has
    // already been logged).
    void sendErrorReply(int64_t chatId, const String &reason);

    IBotClient &bot_;
    SmsSender &smsSender_;
    ReplyTargetMap &replyTargets_;
    IPersist &persist_;
    ClockFn clock_;
    AuthFn auth_;
    StatusFn statusFn_;
    ListMutatorFn mutator_;
    SmsBlockMutatorFn smsBlockMutator_;

    SmsDebugLog *debugLog_ = nullptr;
    std::function<void()> ntpSyncFn_;
    std::function<String()> concatSummaryFn_; // RFC-0069
    std::function<void()> wifiReconnectFn_;   // RFC-0071
    std::function<String()> heapFn_;          // RFC-0072
    String versionStr_ = "(unknown build)";   // RFC-0074
    SmsAliasStore *aliasStore_ = nullptr;     // RFC-0088
    std::function<String()> csqFn_;          // RFC-0092
    std::function<void(uint32_t)> muteFn_;  // RFC-0098
    std::function<void()> unmuteFn_;        // RFC-0098
    std::function<String(const String &)> ussdFn_;  // RFC-0103
    std::function<String()> simInfoFn_;              // RFC-0105
    std::function<String(int64_t, const String &)> atCmdFn_; // RFC-0107
    std::function<void()> resetStatsFn_;  // RFC-0110
    std::function<String()> pingSummaryFn_;                  // RFC-0119
    std::function<String()> labelGetFn_;                    // RFC-0118
    std::function<void(const String &)> labelSetFn_;        // RFC-0118
    std::function<String()> balanceCodeFn_; // RFC-0114
    std::function<void(int64_t)> rebootFn_; // RFC-0112
    std::function<String()> uptimeFn_;     // RFC-0120
    std::function<String()> networkFn_;    // RFC-0121
    std::function<String()> bootInfoFn_;   // RFC-0123
    std::function<String()> countFn_;      // RFC-0124
    std::function<String()> ipFn_;         // RFC-0126
    std::function<String()> smsSlotsFn_;   // RFC-0127
    std::function<String()> lifetimeFn_;  // RFC-0128
    AnnounceFn announceFn_;               // RFC-0129
    std::function<String()> digestFn_;    // RFC-0130
    std::function<void(uint32_t)> intervalFn_; // RFC-0137
    std::function<String()> noteGetFn_;   // RFC-0131
    std::function<void(const String &)> noteSetFn_; // RFC-0131
    std::function<void(int)> maxFailFn_;  // RFC-0138
    std::function<int()> flushSimFn_;     // RFC-0139
    std::function<String()> autoReplyGetFn_;              // RFC-0151
    std::function<void(const String &)> autoReplySetFn_; // RFC-0151
    std::function<void(bool)> forwardingEnabledFn_;       // RFC-0153
    std::function<int()> sweepFn_;                  // RFC-0148
    std::function<String()> healthFn_;             // RFC-0149
    std::function<String()> simStatusFn_;          // RFC-0156
    std::function<String()> wifiScanFn_;           // RFC-0158
    std::function<void(int)> maxPartsFn_;          // RFC-0160
    std::function<String()> smsCntFn_;             // RFC-0161
    std::function<void(bool)> blockingEnabledFn_;  // RFC-0162
    std::function<String(const String &)> blockCheckFn_; // RFC-0163
    std::function<bool(int)> smsForwardFn_;        // RFC-0146
    uint32_t pollIntervalMs_ = kPollIntervalMs;    // RFC-0147
    std::function<void(uint32_t)> dedupWindowFn_;  // RFC-0144
    std::function<void()> clearDedupFn_;           // RFC-0145
    std::function<void(uint32_t)> concatTtlFn_;    // RFC-0142
    std::function<String()> modemInfoFn_;       // RFC-0143
    std::function<String()> simListFn_;   // RFC-0140
    std::function<String(int)> simReadFn_; // RFC-0141
    int32_t lastUpdateId_ = 0;
    uint32_t lastPollMs_ = 0;
    bool firstPollDone_ = false;
    int pollAttempts_ = 0;
};
