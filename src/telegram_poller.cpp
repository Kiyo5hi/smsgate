#include "telegram_poller.h"
#include "sms_debug_log.h"
#include "sms_codec.h"

#include <vector>
#include <memory>
#include <time.h>

#ifdef ESP_PLATFORM
#include <esp_task_wdt.h>
#endif

// RFC-0202: Format a scheduled-send ETA string. If wallTimeFn is set and
// returns a valid epoch (> 1e9), appends the absolute UTC time; otherwise
// falls back to relative-only. Examples:
//   "now (overdue)"          — sendAtMs already past
//   "in 30m (14:32 UTC)"    — fires today
//   "in 2880m (04/12 09:00 UTC)" — fires more than 24h from now
static String schedEtaStr(uint32_t sendAtMs, uint32_t nowMs,
                           const std::function<long()> &wallTimeFn)
{
    if (sendAtMs == 0) return String("(free)");
    // RFC-0273: wraparound-safe overdue check. (uint32_t)(now - T) < 0x80000000 ↔ now >= T.
    if ((uint32_t)(nowMs - sendAtMs) < 0x80000000UL) {
        return String("now (overdue)");
    }
    uint32_t diffMs = sendAtMs - nowMs;
    uint32_t diffMin = (diffMs + 59999U) / 60000U;
    String rel = String("in ") + String((int)diffMin) + String("m");
    if (!wallTimeFn) return rel;
    long wallNow = wallTimeFn();
    if (wallNow <= 1000000000L) return rel; // NTP not synced
    long sendAtUnix = wallNow + (long)diffMs / 1000L;
    time_t t = (time_t)sendAtUnix;
    struct tm *tm_info = gmtime(&t);
    char buf[20];
    if (diffMs > 86400000UL)
        strftime(buf, sizeof(buf), "%m/%d %H:%M UTC", tm_info);
    else
        strftime(buf, sizeof(buf), "%H:%M UTC", tm_info);
    return rel + String(" (") + String(buf) + String(")");
}

// RFC-0211: Return true if wallTimeFn returns a valid epoch and the current
// UTC hour falls within the quiet window [start, end). Handles overnight
// wrap-around (e.g. start=22 end=8: quiet from 22:xx to 07:59).
// Returns false if start < 0 (disabled) or wallTimeFn is unset / pre-NTP.
static bool isInQuietHoursAt(long unixTs, int start, int end)
{
    if (start < 0 || end < 0) return false;
    int utcHour = (int)((unixTs % 86400L) / 3600L);
    if (start < end)
        return utcHour >= start && utcHour < end;
    else
        return utcHour >= start || utcHour < end;
}

static bool isInQuietHours(int start, int end,
                            const std::function<long()> &wallTimeFn)
{
    if (start < 0 || end < 0) return false;
    if (!wallTimeFn) return false;
    long wallNow = wallTimeFn();
    if (wallNow <= 1000000000L) return false; // NTP not synced
    return isInQuietHoursAt(wallNow, start, end);
}

// RFC-0212: Build a quiet-hours warning suffix if sendAtUnix falls inside
// the configured window. Returns "" if quiet hours not configured or NTP
// not synced.
static String quietHoursWarning(uint32_t sendAtMs, uint32_t nowMs,
                                 int qStart, int qEnd,
                                 const std::function<long()> &wallTimeFn)
{
    if (qStart < 0 || qEnd < 0 || !wallTimeFn) return String();
    long wallNow = wallTimeFn();
    if (wallNow <= 1000000000L) return String(); // NTP not synced
    // RFC-0274: signed reinterpret of unsigned subtraction avoids UB at millis() wrap.
    long deltaMs = (long)(int32_t)(sendAtMs - nowMs);
    long sendAtUnix = wallNow + deltaMs / 1000L;
    if (!isInQuietHoursAt(sendAtUnix, qStart, qEnd)) return String();
    char buf[48];
    snprintf(buf, sizeof(buf),
        "\n\xe2\x9a\xa0\xef\xb8\x8f Quiet hours (%02d:00\xe2\x80\x93%02d:00 UTC) \xe2\x80\x94 delivery deferred.", // ⚠️ –
        qStart, qEnd);
    return String(buf);
}

// RFC-0222: Convert a UTC date+time to a Unix timestamp.
// Uses the proleptic Gregorian calendar formula — no mktime dependency.
// Returns -1 if any field is out of range.
static long dateTimeToUnix(int year, int month, int day, int hour, int minute)
{
    if (year  < 2020 || year  > 2099) return -1;
    if (month < 1    || month > 12  ) return -1;
    if (day   < 1    || day   > 31  ) return -1;
    if (hour  < 0    || hour  > 23  ) return -1;
    if (minute< 0    || minute> 59  ) return -1;
    // Days in each month (non-leap year).
    static const int kDaysInMonth[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    bool isLeap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    int maxDay = kDaysInMonth[month - 1] + (isLeap && month == 2 ? 1 : 0);
    if (day > maxDay) return -1;
    // Days from 1970-01-01 to start of `year`.
    long y = year;
    long days = 365L * (y - 1970L)
              + (y - 1969L) / 4L
              - (y - 1901L) / 100L
              + (y - 1601L) / 400L;
    // Add days in months before `month`.
    for (int m = 1; m < month; m++)
        days += kDaysInMonth[m - 1] + (isLeap && m == 2 ? 1 : 0);
    days += day - 1; // 0-based day offset
    return days * 86400L + (long)hour * 3600L + (long)minute * 60L;
}

// File-static helper: strips the given prefix from `lower` and returns
// the trimmed remainder. Returns an empty String if lower does not start
// with prefix.
static String extractArg(const String &lower, const char *prefix)
{
    if (!lower.startsWith(prefix)) return String();
    String arg = lower.substring(strlen(prefix));
    arg.trim();
    return arg;
}

// RFC-0224: Resolve a phone token to a normalized E.164 number.
// If rawPhone starts with '@', performs alias lookup via aliasStore (may be nullptr).
// Returns the resolved phone on success, or "" on failure; fills errOut with a
// user-facing error message on failure.
static String resolvePhone(const String &rawPhone,
                            SmsAliasStore *aliasStore,
                            String &errOut)
{
    if (rawPhone.length() > 0 && rawPhone[0] == '@')
    {
        if (!aliasStore)
        {
            errOut = String("(aliases not configured)");
            return String();
        }
        String resolved = aliasStore->lookup(rawPhone.substring(1));
        if (resolved.length() == 0)
        {
            errOut = String("Unknown alias: ") + rawPhone;
            return String();
        }
        return resolved;
    }
    return sms_codec::normalizePhoneNumber(rawPhone);
}

TelegramPoller::TelegramPoller(IBotClient &bot,
                               SmsSender &smsSender,
                               ReplyTargetMap &replyTargets,
                               IPersist &persist,
                               TelegramPoller::ClockFn clock,
                               TelegramPoller::AuthFn auth,
                               TelegramPoller::StatusFn status,
                               TelegramPoller::ListMutatorFn mutator,
                               TelegramPoller::SmsBlockMutatorFn smsBlockMutator)
    : bot_(bot), smsSender_(smsSender), replyTargets_(replyTargets),
      persist_(persist), clock_(std::move(clock)), auth_(std::move(auth)),
      statusFn_(std::move(status)), mutator_(std::move(mutator)),
      smsBlockMutator_(std::move(smsBlockMutator))
{
}

void TelegramPoller::begin()
{
    lastUpdateId_ = persist_.loadLastUpdateId();
    Serial.print("TelegramPoller: starting from update_id watermark ");
    Serial.println(lastUpdateId_);
}

void TelegramPoller::sendErrorReply(int64_t chatId, const String &reason)
{
    String msg = String("\xE2\x9D\x8C ") + reason; // U+274C cross mark
    bot_.sendMessageTo(chatId, msg);
}

void TelegramPoller::processUpdate(const TelegramUpdate &u)
{
    // 1. Authorization gate. We always advance the watermark, even if
    // we drop the update — that's the whole point of fail-closed
    // parsing in RFC-0003 §5.
    if (!u.valid)
    {
        Serial.print("TelegramPoller: dropping invalid update_id=");
        Serial.println(u.updateId);
        return;
    }

    // RFC-0125: /me — reply with the caller's own fromId and chatId.
    // Allowed for EVERYONE (even unauthorized) so new users can self-onboard.
    // Read-only — returns only the caller's own IDs.
    {
        String lowerMe = u.text;
        lowerMe.toLowerCase();
        if (lowerMe == "/me")
        {
            String reply = String("\xF0\x9F\x91\xA4 fromId: "); // 👤
            reply += String((long)u.fromId);
            reply += String(" | chatId: ");
            reply += String((long)u.chatId);
            bot_.sendMessageTo(u.chatId, reply);
            return;
        }
    }

    if (auth_ && !auth_(u.fromId))
    {
        Serial.print("TelegramPoller: rejecting unauthorized from_id=");
        Serial.print((long)u.fromId);
        Serial.print(" update_id=");
        Serial.println(u.updateId);
        // Do NOT post an error reply — that would let an attacker
        // probe whether the bot is alive.
        return;
    }

    // 2. Check for debug commands (non-reply messages).
    if (u.replyToMessageId == 0)
    {
        String lower;
        for (unsigned int i = 0; i < u.text.length(); ++i)
        {
            char c = u.text[i];
            if (c >= 'A' && c <= 'Z')
                c = (char)(c + 32);
            lower += c;
        }
        lower.trim();

        // RFC-0053: /help — list available commands.
        if (lower == "/help")
        {
            String help;
            help += "/ping \xe2\x80\x94 Liveness check\n";
            help += "/echo <text> \xe2\x80\x94 Reflect text back (connectivity test)\n";
            help += "/time \xe2\x80\x94 Show current UTC time\n";
            help += "/ntp \xe2\x80\x94 Force NTP time resync\n";
            help += "/status \xe2\x80\x94 Device health & stats\n";
            help += "/last [N] \xe2\x80\x94 Show last N forwarded SMS (default 5)\n";
            help += "/logs [N] \xe2\x80\x94 Show last N SMS log entries (default 10)\n";
            help += "/logsince <hours> \xe2\x80\x94 Show log entries from the past N hours (1\xe2\x80\x93168)\n";
            help += "/logdate <YYYY-MM-DD> \xe2\x80\x94 Show log entries for a specific UTC date\n";
            help += "/logstats \xe2\x80\x94 Aggregate outcome statistics from debug log\n";
            help += "/loginfo \xe2\x80\x94 Debug log ring buffer status (count/capacity + newest entry)\n";
            help += "/smsrate \xe2\x80\x94 SMS forwarding rate (last 1h and 24h from debug log)\n";
            help += "/topn [N] \xe2\x80\x94 Top N SMS senders by message count (default 5)\n";
            help += "/logsoutcome <keyword> \xe2\x80\x94 Filter log entries by outcome (fail/fwd/dup/...)\n";
            help += "/simstatus \xe2\x80\x94 SIM card + network status (ICCID, IMSI, operator, CSQ)\n";
            help += "/setmaxparts <N> \xe2\x80\x94 Set max outbound SMS concat parts (1\xe2\x80\x9310, default 10)\n";
            help += "/wifiscan \xe2\x80\x94 Scan nearby WiFi networks (SSID, channel, RSSI)\n";
            help += "/history <filter> \xe2\x80\x94 Show log entries matching phone substring\n";
            help += "/concat \xe2\x80\x94 Show in-flight concat reassembly groups\n";
            help += "/debug \xe2\x80\x94 Show SMS diagnostic log\n";
            help += "/logcsv \xe2\x80\x94 Export SMS debug log as CSV (unix_ts,sender,outcome,chars)\n";
            help += "/cleardebug \xe2\x80\x94 Clear SMS diagnostic log\n";
            help += "/send <num> <msg> \xe2\x80\x94 Send outbound SMS\n";
            help += "/multicast <n1,n2,...> <msg> \xe2\x80\x94 Send same SMS to multiple numbers\n"; // RFC-0213
            help += "/sendall <msg> \xe2\x80\x94 Broadcast to all aliases\n";
            help += "/test <num> \xe2\x80\x94 Send a test SMS to verify outbound path\n";
            help += "/queue \xe2\x80\x94 Show pending outbound queue\n";
            help += "/queueinfo <N> \xe2\x80\x94 Full details of outbound queue entry N\n"; // RFC-0214
            help += "/flushqueue \xe2\x80\x94 Immediately retry all pending outbound SMS\n";
            help += "/retry <N> \xe2\x80\x94 Force immediate retry of queue entry N\n"; // RFC-0216
            help += "/clearqueue \xe2\x80\x94 Discard all pending outbound SMS\n";
            help += "/resetstats \xe2\x80\x94 Reset session counters (SMS fwd/fail, calls)\n";
            help += "/cancel <N> \xe2\x80\x94 Cancel queued entry N\n";
            help += "/cancelnum <phone> \xe2\x80\x94 Cancel all queued entries for a phone number\n";
            help += "/schedulesend <min> <phone> <msg> \xe2\x80\x94 Schedule SMS for future delivery (1\xe2\x80\x931440 min)\n";
            help += "/schedqueue \xe2\x80\x94 List pending scheduled SMS (up to 5 slots)\n";
            help += "/schedexport \xe2\x80\x94 Export scheduled queue as re-entrant commands\n"; // RFC-0226
            help += "/schedimport \xe2\x80\x94 Import /schedulesend|/scheduleat|/recurring commands\n"; // RFC-0228
            help += "/cancelsched <N> \xe2\x80\x94 Cancel a scheduled SMS slot\n";
            help += "/clearschedule \xe2\x80\x94 Cancel all pending scheduled SMS\n"; // RFC-0195
            help += "/scheddelay <N> <min> \xe2\x80\x94 Extend scheduled slot N by extra minutes\n"; // RFC-0196
            help += "/delayall <min> \xe2\x80\x94 Extend all scheduled slots by extra minutes\n"; // RFC-0204
            help += "/sendafter <HH:MM> <phone> <body> \xe2\x80\x94 Schedule SMS at a specific UTC time\n"; // RFC-0205
            help += "/scheduleat <YYYY-MM-DD HH:MM> <phone> <body> \xe2\x80\x94 Schedule SMS at an exact UTC date+time\n"; // RFC-0222
            help += "/schedinfo <N> \xe2\x80\x94 Show full body and ETA of scheduled slot N\n"; // RFC-0198
            help += "/schedrename <N> <phone> \xe2\x80\x94 Change destination phone of scheduled slot N\n"; // RFC-0197
            help += "/schedbody <N> <text> \xe2\x80\x94 Edit the body of scheduled slot N\n"; // RFC-0207
            help += "/schedclone <N> <min> \xe2\x80\x94 Duplicate a scheduled slot with a new delay\n"; // RFC-0215
            help += "/schedrepeat <N> <min> \xe2\x80\x94 Make slot N repeat every <min> min (0=one-shot)\n"; // RFC-0221
            help += "/recurring <min> <phone> <body> \xe2\x80\x94 Create a repeating scheduled SMS in one step\n"; // RFC-0223
            help += "/schedpause \xe2\x80\x94 Pause all scheduled SMS delivery (volatile)\n"; // RFC-0218
            help += "/schedresume \xe2\x80\x94 Resume scheduled SMS delivery\n"; // RFC-0218
            help += "/snooze <phone> <min> \xe2\x80\x94 Suppress forwarding from a number for N minutes\n"; // RFC-0219
            help += "/unsnooze <phone> \xe2\x80\x94 Cancel a snooze\n"; // RFC-0219
            help += "/snoozelist \xe2\x80\x94 Show active snoozes with remaining time\n"; // RFC-0219
            help += "/pending \xe2\x80\x94 Terse snapshot of all pending work (queue/sched/concat)\n"; // RFC-0209
            help += "/setquiethours <start>-<end> \xe2\x80\x94 Defer sched SMS during UTC window (e.g. 22-08)\n"; // RFC-0211
            help += "/clearquiethours \xe2\x80\x94 Disable quiet hours\n"; // RFC-0211
            help += "/quiethours \xe2\x80\x94 Show current quiet hours setting\n"; // RFC-0211
            help += "/wifi \xe2\x80\x94 Force WiFi reconnect\n";
            help += "/mute [min] \xe2\x80\x94 Snooze proactive alerts (default 60m)\n";
            help += "/unmute \xe2\x80\x94 Cancel alert snooze\n";
            help += "/heap \xe2\x80\x94 Show free/min/max-block heap\n";
            help += "/csq \xe2\x80\x94 Quick signal strength snapshot\n";
            help += "/sim \xe2\x80\x94 SIM identity (ICCID, IMEI, operator)\n";
            help += "/ussd <code> \xe2\x80\x94 Send USSD code (e.g. *100#) and get response\n";
            help += "/balance \xe2\x80\x94 Check SIM balance (shortcut for USSD_BALANCE_CODE)\n";
            help += "/version \xe2\x80\x94 Show firmware build timestamp\n";
            help += "/label \xe2\x80\x94 Show device label\n";
            help += "/setlabel <name> \xe2\x80\x94 Set device label (persisted to NVS)\n";
            help += "/uptime \xe2\x80\x94 Quick uptime one-liner\n";
            help += "/network \xe2\x80\x94 Cellular operator + registration + CSQ\n";
            help += "/boot \xe2\x80\x94 Boot count, reset reason, and boot timestamp\n";
            help += "/count \xe2\x80\x94 Session SMS/call counter summary\n";
            help += "/ip \xe2\x80\x94 WiFi IP address, SSID, and RSSI\n";
            help += "/smsslots \xe2\x80\x94 SIM SMS slot usage\n";
            help += "/smscount \xe2\x80\x94 SIM SMS storage capacity (used/total) via AT+CPMS?\n";
            help += "/smshandlerinfo \xe2\x80\x94 SMS handler config + stats snapshot\n";
            help += "/setblockmode on|off \xe2\x80\x94 Enable/suspend SMS block list enforcement\n";
            help += "/blockcheck <phone> \xe2\x80\x94 Test if a number would be blocked\n";
            help += "/phoneinfo <phone|@alias> \xe2\x80\x94 Block/snooze/alias/log summary for a number\n"; // RFC-0225
            help += "/setcallnotify on|off \xe2\x80\x94 Enable/mute call Telegram notifications\n";
            help += "/callstatus \xe2\x80\x94 Show call handler config and state\n";
            help += "/setcalldedup <s> \xe2\x80\x94 Call dedup cooldown window in seconds (1\xe2\x80\x9360)\n";
            help += "/setunknowndeadline <ms> \xe2\x80\x94 RING-without-CLIP deadline in ms (500\xe2\x80\x9310000)\n";
            help += "/setgmtoffset <h> \xe2\x80\x94 Timezone for SMS timestamps (-12 to +14, default +8)\n";
            help += "/setgmtoffsetmin <m> \xe2\x80\x94 Timezone in total minutes (-720 to +840, e.g. 330=UTC+5:30)\n";
            help += "/setfwdtag <text> \xe2\x80\x94 Custom prefix tag on forwarded SMS (max 20 chars)\n";
            help += "/clearfwdtag \xe2\x80\x94 Remove custom forward prefix tag\n";
            help += "/fwdtest \xe2\x80\x94 Preview forwarded SMS format with current settings\n";
            help += "/testfmt <phone> <body> \xe2\x80\x94 Format preview with custom sender and body\n";
            help += "/setsmsagefilter <h> \xe2\x80\x94 Skip SMS older than N hours (0=off, max 8760)\n"; // RFC-0190
            help += "/testpdu <hex> \xe2\x80\x94 Decode a raw PDU hex string for debugging\n"; // RFC-0191
            help += "/pausefwd <min> \xe2\x80\x94 Pause SMS forwarding for N minutes (1\xe2\x80\x931440)\n"; // RFC-0192
            help += "/sendnow \xe2\x80\x94 Immediately fire all scheduled SMS\n"; // RFC-0193
            help += "/settings \xe2\x80\x94 Show all runtime-configurable parameters\n";
            help += "/nvsinfo \xe2\x80\x94 NVS flash storage usage (used/free/total entries)\n";
            help += "/lifetime \xe2\x80\x94 Lifetime SMS forwarded and boot count\n";
            help += "/announce <msg> \xe2\x80\x94 Broadcast message to all authorized users\n";
            help += "/digest \xe2\x80\x94 Show on-demand stats digest\n";
            help += "/setinterval <s> \xe2\x80\x94 Set heartbeat interval (0=disable, max 86400)\n";
            help += "/hbnow \xe2\x80\x94 Trigger an immediate heartbeat (force-send now)\n";
            help += "/note \xe2\x80\x94 Show device note\n";
            help += "/setnote <text> \xe2\x80\x94 Save device note (max 120 chars)\n";
            help += "/me \xe2\x80\x94 Show your Telegram fromId and chatId\n";
            help += "/reboot \xe2\x80\x94 Soft reboot\n";
            help += "/factoryreset \xe2\x80\x94 Erase all NVS settings and reboot (two-step confirm)\n";
            help += "/at <cmd> \xe2\x80\x94 Admin: raw AT command passthrough\n";
            if (smsBlockMutator_) {
                help += "/blocklist \xe2\x80\x94 Show block list\n";
                help += "/block <num|prefix*> \xe2\x80\x94 Block sender\n";
                help += "/unblock <num|prefix*> \xe2\x80\x94 Unblock sender\n";
            }
            if (aliasStore_) {
                help += "/aliases \xe2\x80\x94 List phone aliases\n";
                help += "/addalias <name> <num> \xe2\x80\x94 Add/replace alias\n";
                help += "/rmalias <name> \xe2\x80\x94 Remove alias\n";
                help += "/exportaliases \xe2\x80\x94 Export aliases as name=number lines\n";
                help += "/importaliases \xe2\x80\x94 Batch import aliases (name=phone lines)\n";
                help += "/clearaliases \xe2\x80\x94 Remove all aliases\n";
            }
            if (templateStore_) {
                help += "/tsave <name> <body> \xe2\x80\x94 Save SMS template\n";
                help += "/tlist \xe2\x80\x94 List all templates\n";
                help += "/trm <name> \xe2\x80\x94 Remove template\n";
                help += "/tclear \xe2\x80\x94 Remove all templates\n";
                help += "/tsend <name> <phone|@alias> \xe2\x80\x94 Send template as SMS\n";
                help += "/tschedule <name> <min> <phone|@alias> \xe2\x80\x94 Schedule template SMS\n";
            }
            help += "/shortcuts \xe2\x80\x94 Quick command reference\n";
            help += "\nReply to a forwarded SMS to send a response.";
            // RFC-0260: /help output is ~9100 chars, well over Telegram's 4096-char
            // limit. Paginate into ≤3800-char chunks (line-boundary aligned) so
            // every command is actually delivered. 3 pages × 27 s each = 81 s —
            // within the 120 s WDT window covered by the per-update kick in tick().
            if (help.length() <= 4096)
            {
                bot_.sendMessageTo(u.chatId, help);
            }
            else
            {
                static constexpr unsigned int kHelpPageLen = 3800;
                unsigned int start = 0;
                while (start < help.length())
                {
                    unsigned int end = start + kHelpPageLen;
                    if (end >= help.length())
                    {
                        bot_.sendMessageTo(u.chatId, help.substring(start));
                        break;
                    }
                    // Find the last newline before `end` to break at a line boundary.
                    while (end > start && help[end] != '\n')
                        --end;
                    if (end == start)
                        end = start + kHelpPageLen; // no newline found; hard cut
                    bot_.sendMessageTo(u.chatId, help.substring(start, end));
                    start = end + 1; // skip the newline
                }
            }
            return;
        }

        // RFC-0055: /ntp — force an NTP resync.
        if (lower == "/ntp")
        {
            if (ntpSyncFn_)
            {
                bot_.sendMessageTo(u.chatId, String("Syncing NTP..."));
                ntpSyncFn_();
                time_t now = time(nullptr);
                if (now > 8 * 3600 * 2)
                {
                    // Format the new time as YYYY-MM-DD HH:MM UTC.
                    char buf[32];
                    struct tm *t2 = gmtime(&now);
                    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M UTC", t2);
                    bot_.sendMessageTo(u.chatId,
                        String("\xE2\x9C\x85 NTP synced: ") + String(buf)); // ✅
                }
                else
                {
                    sendErrorReply(u.chatId, String("NTP sync failed (clock still invalid)."));
                }
            }
            else
            {
                bot_.sendMessageTo(u.chatId, String("(NTP sync not configured)"));
            }
            return;
        }

        // RFC-0042: /ping — instant liveness check.
        // RFC-0119: if pingSummaryFn_ is set, use its result; else plain "🏓 Pong".
        if (lower == "/ping")
        {
            if (pingSummaryFn_)
                bot_.sendMessageTo(u.chatId, pingSummaryFn_());
            else
                bot_.sendMessageTo(u.chatId, String("\xF0\x9F\x8F\x93 Pong")); // 🏓
            return;
        }

        // RFC-0063: /time — show current UTC time (quick NTP sanity check).
        if (lower == "/time")
        {
            time_t now = time(nullptr);
            if (now > 8 * 3600 * 2)
            {
                char buf[32];
                struct tm *t2 = gmtime(&now);
                strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M UTC", t2);
                bot_.sendMessageTo(u.chatId, String("\xF0\x9F\x95\x90 ") + String(buf)); // 🕐
            }
            else
            {
                bot_.sendMessageTo(u.chatId, String("\xF0\x9F\x95\x90 (no NTP sync yet)")); // 🕐
            }
            return;
        }

        // RFC-0058: /last [N] — condensed recent SMS history (newest first).
        if (lower == "/last" || lower.startsWith("/last "))
        {
            if (!debugLog_)
            {
                bot_.sendMessageTo(u.chatId, String("(debug log not configured)"));
                return;
            }
            String arg = extractArg(lower, "/last ");
            size_t n = 5; // default: last 5
            if (arg.length() > 0)
            {
                int parsed = arg.toInt();
                if (parsed > 0 && parsed <= 20) n = (size_t)parsed;
                else if (parsed > 20) n = 20;
            }
            bot_.sendMessageTo(u.chatId, debugLog_->dumpBrief(n));
            return;
        }

        // RFC-0117: /history <filter> — show log entries for a specific contact.
        if (lower.startsWith("/history"))
        {
            if (!debugLog_)
            {
                bot_.sendMessageTo(u.chatId, String("(debug log not configured)"));
                return;
            }
            String filter = extractArg(u.text, "/history ");
            if (filter.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /history <phone>\nExample: /history +8613"));
                return;
            }
            bot_.sendMessageTo(u.chatId, debugLog_->dumpBriefFiltered(10, filter));
            return;
        }

        // RFC-0122: /logs [N] — show last N (default 10, max 50) debug log entries.
        if (lower == "/logs" || lower.startsWith("/logs "))
        {
            if (!debugLog_)
            {
                bot_.sendMessageTo(u.chatId, String("(debug log not configured)"));
                return;
            }
            size_t n = 10;
            String arg = extractArg(u.text, "/logs ");
            if (arg.length() > 0)
            {
                int parsed = arg.toInt();
                if (parsed > 0)
                    n = (size_t)(parsed > 50 ? 50 : parsed);
            }
            bot_.sendMessageTo(u.chatId, debugLog_->dumpBrief(n));
            return;
        }

        // RFC-0159: /logsince <hours> — show log entries from the past N hours.
        if (lower.startsWith("/logsince"))
        {
            if (!debugLog_)
            {
                bot_.sendMessageTo(u.chatId, String("(debug log not configured)"));
                return;
            }
            String arg = extractArg(u.text, "/logsince ");
            if (arg.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /logsince <hours>\nExample: /logsince 2  (shows last 2 hours)"));
                return;
            }
            int hrs = arg.toInt();
            if (hrs < 1 || hrs > 168)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Error: hours must be 1\xe2\x80\x93" "168"));
                return;
            }
            uint32_t nowUnix = (uint32_t)time(nullptr);
            uint32_t cutoff  = nowUnix > (uint32_t)(hrs * 3600)
                               ? nowUnix - (uint32_t)(hrs * 3600)
                               : 0;
            bot_.sendMessageTo(u.chatId, debugLog_->dumpBriefSince(cutoff));
            return;
        }

        // RFC-0178: /logdate YYYY-MM-DD — show log entries for a specific UTC date.
        if (lower.startsWith("/logdate"))
        {
            if (!debugLog_)
            {
                bot_.sendMessageTo(u.chatId, String("(debug log not configured)"));
                return;
            }
            String arg = extractArg(u.text, "/logdate ");
            // Parse YYYY-MM-DD. Require exactly 10 chars with dashes at [4] and [7].
            if (arg.length() != 10 || arg[4] != '-' || arg[7] != '-')
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /logdate YYYY-MM-DD\nExample: /logdate 2026-04-08\n(timestamps are UTC)"));
                return;
            }
            int yr  = arg.substring(0, 4).toInt();
            int mo  = arg.substring(5, 7).toInt();
            int day = arg.substring(8, 10).toInt();
            if (yr < 2020 || yr > 2099 || mo < 1 || mo > 12 || day < 1 || day > 31)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Invalid date.")); // ❌
                return;
            }
            // Compute UTC midnight Unix timestamp using the proleptic Gregorian formula.
            // Days from 1970-01-01 to YYYY-MM-DD:
            static const uint16_t kMD[12] = {0,31,59,90,120,151,181,212,243,273,304,334};
            long y = yr; long m = mo; long d = day;
            long leaps = (y - 1) / 4 - (y - 1) / 100 + (y - 1) / 400
                       - (1969 / 4 - 1969 / 100 + 1969 / 400);
            bool thisLeap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
            long doy = kMD[m - 1] + d - 1 + (m > 2 && thisLeap ? 1 : 0);
            long daysSince1970 = (y - 1970) * 365L + leaps + doy;
            uint32_t dayStart = (uint32_t)(daysSince1970 * 86400L);
            uint32_t dayEnd   = dayStart + 86400u;
            String result = debugLog_->dumpBriefRange(dayStart, dayEnd);
            result += String("(UTC)");
            bot_.sendMessageTo(u.chatId, result);
            return;
        }

        // RFC-0154: /logstats — aggregate outcome statistics over the debug log.
        if (lower == "/logstats")
        {
            if (!debugLog_)
            {
                bot_.sendMessageTo(u.chatId, String("(debug log not configured)"));
                return;
            }
            bot_.sendMessageTo(u.chatId, debugLog_->stats());
            return;
        }

        // RFC-0170: /loginfo — debug log ring buffer status.
        if (lower == "/loginfo")
        {
            if (!debugLog_)
            {
                bot_.sendMessageTo(u.chatId, String("(debug log not configured)"));
                return;
            }
            size_t cnt = debugLog_->count();
            size_t cap = SmsDebugLog::kMaxEntries;
            String msg = String("\xF0\x9F\x93\x8A SMS debug log: ") // 📊
                + String((int)cnt) + "/" + String((int)cap) + " entries\n";
            if (cnt > 0)
            {
                // Show the newest entry via dumpBrief(1).
                msg += "Newest: ";
                msg += debugLog_->dumpBrief(1);
            }
            else
            {
                msg += "(empty \xe2\x80\x94 no SMS received yet)";
            }
            bot_.sendMessageTo(u.chatId, msg);
            return;
        }

        // RFC-0171: /smsrate — show SMS forwarding rate from the debug log.
        if (lower == "/smsrate")
        {
            if (!debugLog_)
            {
                bot_.sendMessageTo(u.chatId, String("(debug log not configured)"));
                return;
            }
            uint32_t nowUnix = (uint32_t)time(nullptr);
            size_t in1h  = debugLog_->countForwarded(nowUnix > 3600u ? nowUnix - 3600u : 0, nowUnix);
            size_t in24h = debugLog_->countForwarded(nowUnix > 86400u ? nowUnix - 86400u : 0, nowUnix);
            String msg = String("\xF0\x9F\x93\x88 SMS forwarding rate:\n") // 📈
                + String("  Last 1h:  ") + String((int)in1h)  + String(" fwd\n")
                + String("  Last 24h: ") + String((int)in24h) + String(" fwd\n")
                + String("  (based on debug log — ring holds last ")
                + String((int)SmsDebugLog::kMaxEntries) + String(" entries)");
            bot_.sendMessageTo(u.chatId, msg);
            return;
        }

        // RFC-0157: /topn [N] — top N senders by message frequency.
        if (lower == "/topn" || lower.startsWith("/topn "))
        {
            if (!debugLog_)
            {
                bot_.sendMessageTo(u.chatId, String("(debug log not configured)"));
                return;
            }
            size_t n = 5;
            String arg = extractArg(u.text, "/topn ");
            if (arg.length() > 0)
            {
                int parsed = arg.toInt();
                if (parsed >= 1 && parsed <= 10) n = (size_t)parsed;
                else if (parsed > 10) n = 10;
            }
            bot_.sendMessageTo(u.chatId, debugLog_->topSenders(n));
            return;
        }

        // RFC-0155: /logsoutcome <keyword> — show log entries filtered by outcome.
        if (lower.startsWith("/logsoutcome"))
        {
            if (!debugLog_)
            {
                bot_.sendMessageTo(u.chatId, String("(debug log not configured)"));
                return;
            }
            String kw = extractArg(u.text, "/logsoutcome ");
            if (kw.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /logsoutcome <keyword>\nExamples: /logsoutcome fail  /logsoutcome fwd  /logsoutcome dup"));
                return;
            }
            bot_.sendMessageTo(u.chatId, debugLog_->dumpBriefByOutcome(10, kw));
            return;
        }

        if (lower == "/debug")
        {
            if (debugLog_)
            {
                bot_.sendMessageTo(u.chatId, debugLog_->dump());
            }
            else
            {
                bot_.sendMessageTo(u.chatId, String("(debug log not configured)"));
            }
            return;
        }

        // RFC-0179: /logcsv — export debug log as CSV (unix_ts,sender,outcome,chars).
        if (lower == "/logcsv")
        {
            if (!debugLog_)
            {
                bot_.sendMessageTo(u.chatId, String("(debug log not configured)"));
                return;
            }
            bot_.sendMessageTo(u.chatId, debugLog_->dumpCsv());
            return;
        }

        // RFC-0040/0067: /cleardebug — wipe the SMS debug log, report count.
        if (lower == "/cleardebug")
        {
            if (debugLog_)
            {
                size_t cleared = debugLog_->count();
                debugLog_->clear();
                String msg = String("\xF0\x9F\x97\x91 Cleared "); // 🗑
                msg += String((int)cleared);
                msg += String(" debug log entr");
                msg += (cleared == 1 ? "y." : "ies.");
                bot_.sendMessageTo(u.chatId, msg);
            }
            else
            {
                bot_.sendMessageTo(u.chatId, String("(debug log not configured)"));
            }
            return;
        }

        // RFC-0067: /echo — reflect back the argument; useful for connectivity testing.
        if (lower.startsWith("/echo"))
        {
            String arg = u.text.length() > 5 ? u.text.substring(5) : String();
            arg.trim();
            bot_.sendMessageTo(u.chatId, arg.length() > 0 ? arg : String("(empty)"));
            return;
        }

        // RFC-0069: /concat — show in-flight concat reassembly groups.
        if (lower == "/concat")
        {
            if (concatSummaryFn_)
                bot_.sendMessageTo(u.chatId, concatSummaryFn_());
            else
                bot_.sendMessageTo(u.chatId, String("(concat summary not configured)"));
            return;
        }

        // RFC-0071: /wifi — trigger a deferred WiFi reconnect.
        if (lower == "/wifi")
        {
            if (wifiReconnectFn_)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xF0\x9F\x94\x84 WiFi reconnect initiated.")); // 🔄
                wifiReconnectFn_();
            }
            else
            {
                bot_.sendMessageTo(u.chatId, String("(WiFi reconnect not configured)"));
            }
            return;
        }

        // RFC-0098: /mute [minutes] — snooze proactive alerts.
        if (lower == "/mute" || lower.startsWith("/mute "))
        {
            if (!muteFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(mute not configured)"));
                return;
            }
            String arg = extractArg(lower, "/mute ");
            uint32_t minutes = 60; // default 1 hour
            if (arg.length() > 0)
            {
                int parsed = arg.toInt();
                if (parsed > 0 && parsed <= 1440) minutes = (uint32_t)parsed;
                else if (parsed > 1440) minutes = 1440; // cap at 24h
            }
            muteFn_(minutes);
            String reply = String("\xF0\x9F\x94\x95 Alerts muted for "); // 🔕
            reply += String((int)minutes);
            reply += String(" minute"); reply += (minutes == 1 ? "." : "s.");
            bot_.sendMessageTo(u.chatId, reply);
            return;
        }

        // RFC-0098: /unmute — cancel alert snooze.
        if (lower == "/unmute")
        {
            if (!unmuteFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(unmute not configured)"));
                return;
            }
            unmuteFn_();
            bot_.sendMessageTo(u.chatId,
                String("\xF0\x9F\x94\x94 Alerts unmuted.")); // 🔔
            return;
        }

        // RFC-0072: /heap — show free/min/max-block heap.
        if (lower == "/heap")
        {
            if (heapFn_)
                bot_.sendMessageTo(u.chatId, heapFn_());
            else
                bot_.sendMessageTo(u.chatId, String("(heap info not configured)"));
            return;
        }

        // RFC-0092: /csq — compact signal health snapshot.
        if (lower == "/csq")
        {
            if (csqFn_)
                bot_.sendMessageTo(u.chatId, csqFn_());
            else
                bot_.sendMessageTo(u.chatId, String("(signal info not configured)"));
            return;
        }

        // RFC-0105: /sim — compact SIM identity snapshot (ICCID, IMEI, operator, CSQ).
        if (lower == "/sim")
        {
            if (simInfoFn_)
                bot_.sendMessageTo(u.chatId, simInfoFn_());
            else
                bot_.sendMessageTo(u.chatId, String("(SIM info not configured)"));
            return;
        }

        // RFC-0107: /at <cmd> — admin-only raw AT passthrough.
        if (lower == "/at" || lower.startsWith("/at "))
        {
            if (!atCmdFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(AT passthrough not configured)"));
                return;
            }
            // Extract command from ORIGINAL text (preserve case for AT strings).
            String atArg = u.text.length() > 3 ? u.text.substring(3) : String();
            atArg.trim();
            if (atArg.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /at <cmd>\nExample: /at +CSQ\nNote: do not include the leading AT"));
                return;
            }
            // Strip a leading "AT" or "at" if the user included it (normalize).
            if (atArg.length() >= 2)
            {
                char c0 = atArg[0]; if (c0 >= 'a' && c0 <= 'z') c0 = (char)(c0 - 32);
                char c1 = atArg[1]; if (c1 >= 'a' && c1 <= 'z') c1 = (char)(c1 - 32);
                if (c0 == 'A' && c1 == 'T') atArg = atArg.substring(2);
                atArg.trim();
            }
            // Blacklist destructive / state-changing commands.
            String atUpper;
            for (unsigned int ci = 0; ci < atArg.length() && ci < 10; ci++)
            {
                char ch = atArg[ci];
                if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
                atUpper += ch;
            }
            if (atUpper.startsWith("+CMGD") || atUpper.startsWith("+CMGS") ||
                atUpper.startsWith("+CPBW") || atUpper.startsWith("Z") ||
                atUpper.startsWith("&F")    || atUpper.startsWith("+CFUN=0") ||
                atUpper.startsWith("+CPIN="))
            {
                sendErrorReply(u.chatId,
                    String("Command blocked for safety. Use serial monitor for destructive AT commands."));
                return;
            }
            String resp = atCmdFn_(u.fromId, atArg);
            // Truncate long responses.
            if (resp.length() > 500) resp = resp.substring(0, 500) + "\xE2\x80\xA6"; // …
            bot_.sendMessageTo(u.chatId, String("\xF0\x9F\x93\x9F AT") + atArg + String("\r\n") + resp); // 📟
            return;
        }

        // RFC-0103: /ussd <code> — relay a USSD code to the modem and return
        // the carrier's text response.
        if (lower == "/ussd" || lower.startsWith("/ussd "))
        {
            String code = extractArg(lower, "/ussd ");
            if (code.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /ussd <code>\nExample: /ussd *100#"));
                return;
            }
            // Validate: USSD codes consist of digits, *, and # only.
            bool codeOk = true;
            for (unsigned int ci = 0; ci < code.length(); ci++)
            {
                char ch = code[ci];
                if (!((ch >= '0' && ch <= '9') || ch == '*' || ch == '#'))
                {
                    codeOk = false;
                    break;
                }
            }
            if (!codeOk)
            {
                sendErrorReply(u.chatId,
                    String("Invalid USSD code. Use digits, * and # only."));
                return;
            }
            if (!ussdFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(USSD not configured)"));
                return;
            }
            bot_.sendMessageTo(u.chatId, String("\xF0\x9F\x93\xA1 Sending USSD..."));  // 📡
            String resp = ussdFn_(code);
            if (resp.length() == 0)
            {
                sendErrorReply(u.chatId, String("USSD timed out or no response."));
            }
            else
            {
                bot_.sendMessageTo(u.chatId, String("\xF0\x9F\x93\xB1 ") + resp);  // 📱
            }
            return;
        }

        // RFC-0114: /balance — on-demand USSD balance check using configured code.
        if (lower == "/balance")
        {
            String code;
            if (balanceCodeFn_)
                code = balanceCodeFn_();
            if (code.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Balance check not configured (define USSD_BALANCE_CODE)."));
                return;
            }
            if (!ussdFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(USSD not configured)"));
                return;
            }
            String resp = ussdFn_(code);
            if (resp.length() == 0)
            {
                sendErrorReply(u.chatId, String("No response from carrier."));
            }
            else
            {
                bot_.sendMessageTo(u.chatId, String("\xF0\x9F\x92\xB3 ") + resp);  // 💳
            }
            return;
        }

        // RFC-0074: /version — firmware build timestamp.
        if (lower == "/version")
        {
            bot_.sendMessageTo(u.chatId, versionStr_);
            return;
        }

        // RFC-0118: /label — show current device label.
        if (lower == "/label")
        {
            if (labelGetFn_)
            {
                String lbl = labelGetFn_();
                if (lbl.length() > 0)
                    bot_.sendMessageTo(u.chatId,
                        String("\xF0\x9F\x8F\xB7 Label: ") + lbl); // 🏷
                else
                    bot_.sendMessageTo(u.chatId, String("(no label set)"));
            }
            else
            {
                bot_.sendMessageTo(u.chatId, String("(label not configured)"));
            }
            return;
        }

        // RFC-0118: /setlabel <name> — set device label (max 32 printable chars).
        if (lower == "/setlabel" || lower.startsWith("/setlabel "))
        {
            if (!labelSetFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(label not configured)"));
                return;
            }
            String name = extractArg(u.text, "/setlabel ");
            if (name.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /setlabel <name>\nExample: /setlabel Office SIM"));
                return;
            }
            if (name.length() > 32)
            {
                sendErrorReply(u.chatId,
                    String("Label too long (max 32 chars)."));
                return;
            }
            // Validate: printable ASCII only.
            for (unsigned int ci = 0; ci < name.length(); ci++)
            {
                char c = name[ci];
                if (c < 0x20 || c > 0x7E)
                {
                    sendErrorReply(u.chatId,
                        String("Label must contain printable ASCII only."));
                    return;
                }
            }
            labelSetFn_(name);
            bot_.sendMessageTo(u.chatId,
                String("\xE2\x9C\x85 Label set to: ") + name); // ✅
            return;
        }

        if (lower == "/status")
        {
            if (statusFn_)
            {
                bot_.sendMessageTo(u.chatId, statusFn_());
            }
            else
            {
                bot_.sendMessageTo(u.chatId, String("(status not configured)"));
            }
            return;
        }

        // RFC-0167: /settings — snapshot of all runtime-configurable parameters.
        if (lower == "/settings")
        {
            if (settingsFn_)
            {
                bot_.sendMessageTo(u.chatId, settingsFn_());
            }
            else
            {
                bot_.sendMessageTo(u.chatId, String("(settings not configured)"));
            }
            return;
        }

        // RFC-0173: /callstatus — CallHandler configuration and state snapshot.
        if (lower == "/callstatus")
        {
            if (callStatusFn_)
            {
                bot_.sendMessageTo(u.chatId, callStatusFn_());
            }
            else
            {
                bot_.sendMessageTo(u.chatId, String("(callstatus not configured)"));
            }
            return;
        }

        // RFC-0174: /smshandlerinfo — SmsHandler configuration and stats snapshot.
        if (lower == "/smshandlerinfo")
        {
            if (smsHandlerInfoFn_)
            {
                bot_.sendMessageTo(u.chatId, smsHandlerInfoFn_());
            }
            else
            {
                bot_.sendMessageTo(u.chatId, String("(smshandlerinfo not configured)"));
            }
            return;
        }

        // RFC-0168: /nvsinfo — NVS storage usage statistics.
        if (lower == "/nvsinfo")
        {
            if (nvsInfoFn_)
            {
                bot_.sendMessageTo(u.chatId, nvsInfoFn_());
            }
            else
            {
                bot_.sendMessageTo(u.chatId, String("(nvsinfo not configured)"));
            }
            return;
        }

        // RFC-0023: /restart — admin-only soft reboot (deferred via flag in main.cpp).
        if (lower == "/restart")
        {
            if (!mutator_)
            {
                bot_.sendMessageTo(u.chatId, String("Restart not configured."));
                return;
            }
            String reason;
            bool ok = mutator_(u.fromId, String("restart"), 0, reason);
            bot_.sendMessageTo(u.chatId, reason);
            (void)ok;
            return;
        }

        // RFC-0021: /blocklist, /block <number>, /unblock <number>.
        // IMPORTANT: /blocklist must be checked BEFORE /block to prevent
        // "/blocklist" being partially matched by the /block prefix check.

        if (lower == "/blocklist")
        {
            if (!smsBlockMutator_)
            {
                bot_.sendMessageTo(u.chatId, String("SMS block list management not configured."));
                return;
            }
            String reason;
            smsBlockMutator_(u.fromId, String("list"), String(), reason);
            bot_.sendMessageTo(u.chatId, reason);
            return;
        }

        // RFC-0163: /blockcheck <phone> — test if a number would be blocked.
        if (lower.startsWith("/blockcheck"))
        {
            if (!blockCheckFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(blockcheck not configured)"));
                return;
            }
            String arg = extractArg(u.text, "/blockcheck ");
            if (arg.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /blockcheck <phone>\nExample: /blockcheck +8613912345678"));
                return;
            }
            bot_.sendMessageTo(u.chatId, blockCheckFn_(arg));
            return;
        }

        // RFC-0225: /phoneinfo <phone|@alias> — consolidated info about a number.
        if (lower == "/phoneinfo" || lower.startsWith("/phoneinfo "))
        {
            String piArg = extractArg(u.text, "/phoneinfo ");
            piArg.trim();
            if (piArg.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /phoneinfo <phone|@alias>\n"
                           "Example: /phoneinfo +8613912345678"));
                return;
            }
            String piErr;
            String piPhone = resolvePhone(piArg, aliasStore_, piErr);
            if (piPhone.length() == 0)
            {
                sendErrorReply(u.chatId, piErr.length() > 0
                    ? piErr : String("\xe2\x9d\x8c Invalid phone number.")); // ❌
                return;
            }
            String piOut = String("\xf0\x9f\x93\x9e ") + piPhone + String(":\n"); // 📞

            // Aliases.
            String piAliases;
            if (aliasStore_)
            {
                aliasStore_->forEach([&](const String &name, const String &phone) {
                    if (phone == piPhone)
                    {
                        if (piAliases.length() > 0) piAliases += String(", ");
                        piAliases += String("@") + name;
                    }
                });
            }
            piOut += String("  Aliases: ") + (piAliases.length() > 0 ? piAliases : String("(none)")) + String("\n");

            // Block status.
            if (blockCheckFn_)
            {
                String bcResult = blockCheckFn_(piPhone);
                bool isBlocked = (bcResult.indexOf("BLOCKED") >= 0);
                piOut += String("  Block: ") + (isBlocked ? String("BLOCKED") : String("allowed")) + String("\n");
            }

            // Snooze status.
            uint32_t piNow = clock_ ? clock_() : 0;
            bool snoozed = false;
            for (const auto &s : snoozeList_)
            {
                if (s.first == piPhone && (uint32_t)(s.second - piNow) < 0x80000000UL) // RFC-0269
                {
                    uint32_t remMs = s.second - piNow;
                    uint32_t remMin = (remMs + 59999U) / 60000U;
                    piOut += String("  Snooze: ") + String(remMin) + String("m remaining\n");
                    snoozed = true;
                    break;
                }
            }
            if (!snoozed)
                piOut += String("  Snooze: (not snoozed)\n");

            // Recent log entries.
            if (debugLog_)
            {
                String recent = debugLog_->dumpBriefFiltered(3, piPhone);
                piOut += String("  Recent: ") + recent;
            }
            bot_.sendMessageTo(u.chatId, piOut);
            return;
        }

        if (lower == "/block" || lower.startsWith("/block "))
        {
            String arg = extractArg(lower, "/block ");
            if (arg.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /block <number>  (append * for prefix: /block +8613*)"));
                return;
            }
            if (!smsBlockMutator_)
            {
                bot_.sendMessageTo(u.chatId, String("SMS block list management not configured."));
                return;
            }
            String reason;
            if (!smsBlockMutator_(u.fromId, String("block"), arg, reason))
            {
                bot_.sendMessageTo(u.chatId, reason);
                return;
            }
            {
                bool isPrefix = arg.length() > 0 && arg[arg.length()-1] == '*';
                String note = isPrefix
                    ? String(". Prefix match — all numbers starting with ")
                      + arg.substring(0, arg.length()-1) + String(" will be blocked.")
                    : String(". Exact match — check the serial log to confirm the format your carrier sends.");
                bot_.sendMessageTo(u.chatId, String("Blocked: ") + arg + note);
            }
            return;
        }

        if (lower == "/unblock" || lower.startsWith("/unblock "))
        {
            String arg = extractArg(lower, "/unblock ");
            if (arg.length() == 0)
            {
                bot_.sendMessageTo(u.chatId, String("Usage: /unblock <number>"));
                return;
            }
            if (!smsBlockMutator_)
            {
                bot_.sendMessageTo(u.chatId, String("SMS block list management not configured."));
                return;
            }
            String reason;
            if (!smsBlockMutator_(u.fromId, String("unblock"), arg, reason))
            {
                bot_.sendMessageTo(u.chatId, reason);
                return;
            }
            bot_.sendMessageTo(u.chatId, String("Unblocked: ") + arg);
            return;
        }

        // RFC-0088: /aliases — list all defined phone aliases.
        if (lower == "/aliases")
        {
            bot_.sendMessageTo(u.chatId,
                aliasStore_ ? aliasStore_->list() : String("(aliases not configured)"));
            return;
        }

        // RFC-0088: /addalias <name> <number> — add or replace a phone alias.
        if (lower == "/addalias" || lower.startsWith("/addalias "))
        {
            String arg = extractArg(lower, "/addalias ");
            int spacePos = arg.indexOf(' ');
            if (arg.length() == 0 || spacePos <= 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /addalias <name> <number>\nExample: /addalias alice +447911123456"));
                return;
            }
            if (!aliasStore_)
            {
                bot_.sendMessageTo(u.chatId, String("(aliases not configured)"));
                return;
            }
            String aName  = arg.substring(0, spacePos);
            String aPhone = sms_codec::normalizePhoneNumber(arg.substring(spacePos + 1));
            if (!SmsAliasStore::isValidName(aName))
            {
                sendErrorReply(u.chatId,
                    String("Invalid alias name \xe2\x80\x9c") + aName // "
                    + String("\xe2\x80\x9d. Use only letters, digits, _ and -.")); // "
                return;
            }
            if (!aliasStore_->set(aName, aPhone))
            {
                sendErrorReply(u.chatId,
                    String("Failed: name/number too long, or store full (max 10 aliases)."));
                return;
            }
            bot_.sendMessageTo(u.chatId,
                String("\xE2\x9C\x85 @") + aName + String(" \xe2\x86\x92 ") + aPhone); // ✅ @name → phone
            return;
        }

        // RFC-0088: /rmalias <name> — remove a phone alias.
        if (lower == "/rmalias" || lower.startsWith("/rmalias "))
        {
            String arg = extractArg(lower, "/rmalias ");
            if (arg.length() == 0)
            {
                bot_.sendMessageTo(u.chatId, String("Usage: /rmalias <name>"));
                return;
            }
            if (!aliasStore_)
            {
                bot_.sendMessageTo(u.chatId, String("(aliases not configured)"));
                return;
            }
            if (!aliasStore_->remove(arg))
            {
                sendErrorReply(u.chatId, String("Alias @") + arg + String(" not found."));
                return;
            }
            bot_.sendMessageTo(u.chatId,
                String("\xE2\x9C\x85 Alias @") + arg + String(" removed.")); // ✅
            return;
        }

        // RFC-0132: /exportaliases — export all aliases as "name=number" lines.
        if (lower == "/exportaliases")
        {
            if (!aliasStore_)
            {
                bot_.sendMessageTo(u.chatId, String("(alias store not configured)"));
                return;
            }
            if (aliasStore_->count() == 0)
            {
                bot_.sendMessageTo(u.chatId, String("(no aliases)"));
                return;
            }
            String out;
            aliasStore_->forEach([&out](const String &name, const String &phone) {
                out += name; out += String("="); out += phone; out += String("\n");
            });
            bot_.sendMessageTo(u.chatId, out);
            return;
        }

        // RFC-0134: /clearaliases — remove all aliases at once.
        if (lower == "/clearaliases")
        {
            if (!aliasStore_)
            {
                bot_.sendMessageTo(u.chatId, String("(alias store not configured)"));
                return;
            }
            int n = aliasStore_->count();
            if (n == 0)
            {
                bot_.sendMessageTo(u.chatId, String("(no aliases)"));
                return;
            }
            aliasStore_->clear();
            bot_.sendMessageTo(u.chatId,
                String("\xe2\x9c\x85 Cleared ") // ✅
                + String(n) + String(n == 1 ? " alias." : " aliases."));
            return;
        }

        // RFC-0186: /importaliases — batch import of name=phone lines.
        if (lower.startsWith("/importaliases"))
        {
            if (!aliasStore_)
            {
                bot_.sendMessageTo(u.chatId, String("(alias store not configured)"));
                return;
            }
            // Extract everything after "/importaliases" from the original text,
            // then split by newlines. Each line is expected to be "name=phone".
            String body = u.text.length() > 14 ? u.text.substring(14) : String();
            body.trim();
            if (body.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /importaliases\nAlice=+13800138000\nBob=+14155551234"));
                return;
            }
            int imported = 0, skipped = 0;
            // Walk through lines (split by \n).
            int start = 0;
            while (start <= (int)body.length())
            {
                int nl = body.indexOf('\n', start);
                String line = (nl < 0) ? body.substring(start)
                                       : body.substring(start, nl);
                start = (nl < 0) ? (int)body.length() + 1 : nl + 1;
                line.trim();
                if (line.length() == 0) continue;
                int eq = line.indexOf('=');
                if (eq <= 0)
                {
                    skipped++;
                    continue;
                }
                String aName  = line.substring(0, eq);
                String aPhone = sms_codec::normalizePhoneNumber(line.substring(eq + 1));
                aName.trim();
                if (!SmsAliasStore::isValidName(aName) || aPhone.length() == 0)
                {
                    skipped++;
                    continue;
                }
                if (aliasStore_->set(aName, aPhone))
                    imported++;
                else
                    skipped++;
            }
            if (imported == 0)
            {
                sendErrorReply(u.chatId,
                    String("\xe2\x9d\x8c No valid aliases found. Format: name=phone (one per line)")); // ❌
            }
            else
            {
                String reply = String("\xe2\x9c\x85 Imported "); // ✅
                reply += String(imported);
                reply += String(imported == 1 ? " alias" : " aliases");
                if (skipped > 0)
                {
                    reply += String(", skipped ");
                    reply += String(skipped);
                    reply += String(skipped == 1 ? " invalid line." : " invalid lines.");
                }
                else
                {
                    reply += String(".");
                }
                bot_.sendMessageTo(u.chatId, reply);
            }
            return;
        }

        // RFC-0227: SMS template commands.
        if (lower.startsWith("/tsave ") && templateStore_)
        {
            String rest = u.text.substring(7);
            rest.trim();
            int sp = rest.indexOf(' ');
            if (sp < 1)
            {
                bot_.sendMessageTo(u.chatId, String("Usage: /tsave <name> <body>"));
                return;
            }
            String tname = rest.substring(0, sp);
            String tbody = rest.substring(sp + 1);
            tbody.trim();
            if (!templateStore_->set(tname, tbody))
            {
                String err = String("Failed to save template \xe2\x80\x9c") + tname + String("\xe2\x80\x9d");
                err += String(". Check: name \xe2\x89\xa4 20 chars [a-zA-Z0-9_-], body 1\xe2\x80\x93160 chars, store not full.");
                bot_.sendMessageTo(u.chatId, err);
            }
            else
            {
                bot_.sendMessageTo(u.chatId, String("\xe2\x9c\x85 Template saved: ") + tname);
            }
            return;
        }

        if ((lower == "/tlist") && templateStore_)
        {
            String reply = String("Templates (") + String(templateStore_->count()) + String("):\n");
            reply += templateStore_->list();
            bot_.sendMessageTo(u.chatId, reply);
            return;
        }

        if (lower.startsWith("/trm ") && templateStore_)
        {
            String tname = u.text.substring(5);
            tname.trim();
            if (!templateStore_->remove(tname))
                bot_.sendMessageTo(u.chatId, String("Template not found: ") + tname);
            else
                bot_.sendMessageTo(u.chatId, String("\xe2\x9c\x85 Template removed: ") + tname);
            return;
        }

        if (lower == "/tclear" && templateStore_)
        {
            templateStore_->clear();
            bot_.sendMessageTo(u.chatId, String("\xe2\x9c\x85 All templates cleared."));
            return;
        }

        if (lower.startsWith("/tsend ") && templateStore_)
        {
            String rest = u.text.substring(7);
            rest.trim();
            int sp = rest.indexOf(' ');
            if (sp < 1)
            {
                bot_.sendMessageTo(u.chatId, String("Usage: /tsend <name> <phone|@alias>"));
                return;
            }
            String tname = rest.substring(0, sp);
            String rawPhone = rest.substring(sp + 1);
            rawPhone.trim();
            String tbody = templateStore_->get(tname);
            if (tbody.length() == 0)
            {
                bot_.sendMessageTo(u.chatId, String("Template not found: ") + tname);
                return;
            }
            String errStr;
            String phone = resolvePhone(rawPhone, aliasStore_, errStr);
            if (phone.length() == 0)
            {
                bot_.sendMessageTo(u.chatId, errStr.length() > 0 ? errStr : String("Invalid phone number."));
                return;
            }
            if (!smsSender_.send(phone, tbody))
                bot_.sendMessageTo(u.chatId, String("Failed to send SMS: ") + smsSender_.lastError());
            else
                bot_.sendMessageTo(u.chatId, String("\xe2\x9c\x85 SMS sent to ") + phone);
            return;
        }

        if (lower.startsWith("/tschedule ") && templateStore_)
        {
            String rest = u.text.substring(11);
            rest.trim();
            // Format: <name> <min> <phone|@alias>
            int sp1 = rest.indexOf(' ');
            if (sp1 < 1)
            {
                bot_.sendMessageTo(u.chatId, String("Usage: /tschedule <name> <min> <phone|@alias>"));
                return;
            }
            String tname = rest.substring(0, sp1);
            String rest2 = rest.substring(sp1 + 1);
            rest2.trim();
            int sp2 = rest2.indexOf(' ');
            if (sp2 < 1)
            {
                bot_.sendMessageTo(u.chatId, String("Usage: /tschedule <name> <min> <phone|@alias>"));
                return;
            }
            int delayMin = rest2.substring(0, sp2).toInt();
            String rawPhone = rest2.substring(sp2 + 1);
            rawPhone.trim();
            if (delayMin < 1 || delayMin > 1440)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Delay must be 1\xe2\x80\x93 1440 minutes.")); // ❌ –
                return;
            }
            String tbody = templateStore_->get(tname);
            if (tbody.length() == 0)
            {
                bot_.sendMessageTo(u.chatId, String("Template not found: ") + tname);
                return;
            }
            String errStr;
            String phone = resolvePhone(rawPhone, aliasStore_, errStr);
            if (phone.length() == 0)
            {
                bot_.sendMessageTo(u.chatId, errStr.length() > 0 ? errStr : String("Invalid phone number."));
                return;
            }
            int slot = -1;
            for (int i = 0; i < (int)kScheduledQueueSize; i++)
                if (scheduledQueue_[i].sendAtMs == 0) { slot = i; break; }
            if (slot < 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Scheduled queue full (5 slots). Use /schedqueue to see pending."));
                return;
            }
            uint32_t nowMs  = clock_ ? clock_() : 0;
            uint32_t sendAt = nowMs + (uint32_t)delayMin * 60000U;
            scheduledQueue_[slot].sendAtMs = sendAt;
            scheduledQueue_[slot].phone    = phone;
            scheduledQueue_[slot].body     = tbody;
            if (persistSchedFn_) persistSchedFn_();
            String reply = String("\xe2\x9c\x85 Scheduled \xe2\x80\x9c") + tname
                         + String("\xe2\x80\x9d to ") + phone
                         + String(" in ") + String(delayMin) + String("m");
            bot_.sendMessageTo(u.chatId, reply);
            return;
        }

        // RFC-0133: /shortcuts — condensed quick reference of common commands.
        if (lower == "/shortcuts")
        {
            String s;
            s += "Quick reference:\n";
            s += "/ping \xe2\x80\x94 Liveness\n";
            s += "/status \xe2\x80\x94 Full health dump\n";
            s += "/uptime \xe2\x80\x94 Uptime\n";
            s += "/network \xe2\x80\x94 Cell + CSQ\n";
            s += "/count \xe2\x80\x94 SMS/call counters\n";
            s += "/send <num> <msg> \xe2\x80\x94 Send SMS\n";
            s += "/queue \xe2\x80\x94 Pending outbound\n";
            s += "/last \xe2\x80\x94 Recent received SMS\n";
            s += "/reboot \xe2\x80\x94 Soft reboot\n";
            s += "/help \xe2\x80\x94 All commands\n";
            bot_.sendMessageTo(u.chatId, s);
            return;
        }

        // RFC-0026: /send <number> <body> — one-shot outbound SMS that
        // doesn't require a prior incoming SMS to reply to.
        if (lower == "/send" || lower.startsWith("/send "))
        {
            // Extract the argument from the ORIGINAL (non-lowercased) text
            // so the body preserves its original case and Unicode characters.
            String arg = u.text.substring(strlen("/send"));
            arg.trim();
            int spacePos = arg.indexOf(' ');
            if (arg.length() == 0 || spacePos <= 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /send <number> <message>\nExample: /send +8613800138000 Hello!"));
                return;
            }
            String rawPhone = arg.substring(0, spacePos);
            String phone;
            if (rawPhone.length() > 0 && rawPhone[0] == '@')
            {
                // RFC-0088: @name alias expansion.
                if (!aliasStore_)
                {
                    bot_.sendMessageTo(u.chatId, String("(aliases not configured)"));
                    return;
                }
                phone = aliasStore_->lookup(rawPhone.substring(1));
                if (phone.length() == 0)
                {
                    sendErrorReply(u.chatId, String("Unknown alias: ") + rawPhone);
                    return;
                }
            }
            else
            {
                phone = sms_codec::normalizePhoneNumber(rawPhone); // RFC-0078
            }
            String body  = arg.substring(spacePos + 1);
            body.trim();
            if (body.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /send <number> <message>\nExample: /send +8613800138000 Hello!"));
                return;
            }
            // RFC-0049: Reject immediately if body exceeds 10-part limit.
            // countSmsParts returns 0 when buildSmsSubmitPduMulti returns
            // empty (body too long for the 10-part cap).
            int parts = sms_codec::countSmsParts(body);
            if (parts == 0)
            {
                sendErrorReply(u.chatId,
                    String("Message too long (max ~1530 GSM-7 / ~670 Unicode chars)."));
                return;
            }
            int64_t requesterChatId = u.chatId;
            String capturedPhone = phone;
            // RFC-0111: enqueue returns false for duplicates.
            bool enqueued = smsSender_.enqueue(phone, body,
                [this, requesterChatId, capturedPhone]() {
                    sendErrorReply(requesterChatId,
                        String("SMS to ") + capturedPhone + " failed after retries.");
                },
                [this, requesterChatId, capturedPhone]() {
                    // RFC-0032 / RFC-0054: delivery confirmation. Store the
                    // message_id so replying to it also routes to capturedPhone.
                    int32_t delivId = bot_.sendMessageToReturningId(requesterChatId,
                        String("\xF0\x9F\x93\xA8 Sent to ") + capturedPhone); // U+1F4E8
                    if (delivId > 0)
                        replyTargets_.put(delivId, capturedPhone);
                });
            if (!enqueued)
            {
                sendErrorReply(u.chatId,
                    String("Already queued to ") + phone + String(". Use /queue to check."));
                return;
            }
            // RFC-0029: Include a body preview so the user can catch typos.
            String preview = body.substring(0, 30);
            if (body.length() > 30) preview += "\xE2\x80\xA6"; // U+2026 ellipsis
            // RFC-0037: Append part count when message will be split.
            // `parts` already computed above for the length check (RFC-0049).
            // RFC-0093: display alias name when used.
            String displayPhone = (rawPhone.length() > 0 && rawPhone[0] == '@')
                ? rawPhone + String(" (") + phone + String(")")
                : phone;
            String confirmText = String("\xE2\x9C\x85 Queued to ") + displayPhone + String(": ") + preview;
            if (parts > 1)
                confirmText += String(" (") + String(parts) + String(" parts)");
            // RFC-0030: Use sendMessageReturningId so we can store the
            // confirmation message_id in the reply-target map. This lets the
            // user reply to the "✅ Queued to..." confirmation to send another
            // SMS to the same number without typing /send again.
            int32_t confirmId = bot_.sendMessageReturningId(confirmText);
            if (confirmId > 0)
                replyTargets_.put(confirmId, phone);
            Serial.print("TelegramPoller: /send queued to ");
            Serial.println(phone);
            return;
        }

        // RFC-0213: /multicast <phone1,phone2,...> <body> — enqueue same SMS to multiple numbers.
        if (lower == "/multicast" || lower.startsWith("/multicast "))
        {
            static constexpr int kMulticastMaxRecipients = 10;
            String arg = u.text.substring(strlen("/multicast"));
            arg.trim();
            int spacePos = arg.indexOf(' ');
            if (arg.length() == 0 || spacePos <= 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /multicast <phone1,phone2,...> <message>\n"
                           "Example: /multicast +111,+222 Hello everyone!"));
                return;
            }
            String phonesStr = arg.substring(0, spacePos);
            String body = arg.substring(spacePos + 1);
            body.trim();
            if (body.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /multicast <phone1,phone2,...> <message>"));
                return;
            }
            int parts = sms_codec::countSmsParts(body);
            if (parts == 0)
            {
                sendErrorReply(u.chatId,
                    String("Message too long (max ~1530 GSM-7 / ~670 Unicode chars)."));
                return;
            }
            // Parse comma-separated phone numbers.
            std::vector<String> phones;
            {
                String tmp = phonesStr;
                while (tmp.length() > 0)
                {
                    int comma = tmp.indexOf(',');
                    String tok = (comma >= 0) ? tmp.substring(0, comma) : tmp;
                    tok.trim();
                    if (tok.length() > 0)
                        phones.push_back(sms_codec::normalizePhoneNumber(tok));
                    if (comma < 0) break;
                    tmp = tmp.substring(comma + 1);
                }
            }
            if (phones.empty())
            {
                sendErrorReply(u.chatId, String("No valid phone numbers found."));
                return;
            }
            if ((int)phones.size() > kMulticastMaxRecipients)
            {
                sendErrorReply(u.chatId,
                    String("\xe2\x9d\x8c Max ") + String(kMulticastMaxRecipients) // ❌
                    + String(" recipients per /multicast."));
                return;
            }
            int queued = 0;
            int skipped = 0;
            String queuedList;
            int64_t requesterChatId = u.chatId;
            for (const auto &ph : phones)
            {
                if (ph.length() == 0) { skipped++; continue; }
                String capturedPhone = ph;
                bool ok = smsSender_.enqueue(ph, body,
                    [this, requesterChatId, capturedPhone]() {
                        sendErrorReply(requesterChatId,
                            String("SMS to ") + capturedPhone + " failed after retries.");
                    },
                    [this, requesterChatId, capturedPhone]() {
                        int32_t delivId = bot_.sendMessageToReturningId(requesterChatId,
                            String("\xF0\x9F\x93\xA8 Sent to ") + capturedPhone); // 📨
                        if (delivId > 0)
                            replyTargets_.put(delivId, capturedPhone);
                    });
                if (ok)
                {
                    queued++;
                    if (queuedList.length() > 0) queuedList += String(", ");
                    queuedList += ph;
                }
                else skipped++;
            }
            if (queued == 0)
            {
                sendErrorReply(u.chatId, String("No messages queued (queue full or all numbers invalid)."));
                return;
            }
            String reply;
            if (skipped > 0)
            {
                reply = String("\xe2\x9a\xa0\xef\xb8\x8f Skipped ") + String(skipped) // ⚠️
                      + String(" number(s). Queued to ")
                      + String(queued) + String(": ") + queuedList;
            }
            else
            {
                reply = String("\xe2\x9c\x85 Multicast queued to ") + String(queued) // ✅
                      + String(" number") + (queued == 1 ? "" : "s") + String(": ")
                      + queuedList;
            }
            if (parts > 1)
                reply += String(" (") + String(parts) + String(" parts each)");
            bot_.sendMessageTo(u.chatId, reply);
            return;
        }


        // RFC-0085: /test <number> — outbound SMS self-test.
        if (lower == "/test" || lower.startsWith("/test "))
        {
            String arg = u.text.substring(strlen("/test"));
            arg.trim();
            if (arg.length() == 0) {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /test <number>\nExample: /test +8613800138000"));
                return;
            }
            // arg is the phone number (one word, no body).
            String phone;
            if (arg.length() > 0 && arg[0] == '@')
            {
                // RFC-0088: @name alias expansion.
                if (!aliasStore_)
                {
                    bot_.sendMessageTo(u.chatId, String("(aliases not configured)"));
                    return;
                }
                phone = aliasStore_->lookup(arg.substring(1));
                if (phone.length() == 0)
                {
                    sendErrorReply(u.chatId, String("Unknown alias: ") + arg);
                    return;
                }
            }
            else
            {
                phone = sms_codec::normalizePhoneNumber(arg);
            }
            // Build body with current UTC time if clock is valid.
            String body = String("Bridge test OK");
            time_t nowT = time(nullptr);
            if (nowT > 8 * 3600 * 2) {
                char tbuf[20];
                struct tm *t2 = gmtime(&nowT);
                strftime(tbuf, sizeof(tbuf), " %Y-%m-%dT%H:%MZ", t2);
                body += tbuf;
            }
            int64_t requesterChatId = u.chatId;
            String capturedPhone = phone;
            smsSender_.enqueue(phone, body,
                [this, requesterChatId, capturedPhone]() {
                    sendErrorReply(requesterChatId,
                        String("\xE2\x9D\x8C Test SMS to ") + capturedPhone + " failed."); // ❌
                },
                [this, requesterChatId, capturedPhone]() {
                    bot_.sendMessageTo(requesterChatId,
                        String("\xE2\x9C\x85 Test SMS to ") + capturedPhone + " sent."); // ✅
                });
            // RFC-0093: display alias name when used.
            String displayPhone = (arg.length() > 0 && arg[0] == '@')
                ? arg + String(" (") + phone + String(")")
                : phone;
            bot_.sendMessageTo(u.chatId,
                String("\xF0\x9F\x93\xA4 Test SMS queued to ") + displayPhone); // 📤
            return;
        }

        // RFC-0094 / RFC-0104: /sendall <body> — broadcast to all defined aliases.
        // RFC-0104 adds a delivery summary: "📊 N/M delivered" when the batch completes.
        if (lower == "/sendall" || lower.startsWith("/sendall "))
        {
            if (!aliasStore_)
            {
                bot_.sendMessageTo(u.chatId, String("(aliases not configured)"));
                return;
            }
            if (aliasStore_->count() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("No aliases defined \xe2\x80\x94 use /addalias first.")); // —
                return;
            }
            String body = u.text.substring(strlen("/sendall"));
            body.trim();
            if (body.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /sendall <message>"));
                return;
            }
            if (sms_codec::countSmsParts(body) == 0)
            {
                sendErrorReply(u.chatId,
                    String("Message too long (max ~1530 GSM-7 / ~670 Unicode chars)."));
                return;
            }

            // RFC-0104: shared batch state for the delivery summary.
            struct BatchState {
                int total;
                int succeeded = 0;
                int failed = 0;
                bool reported = false;
                int64_t chatId;
            };
            int total = aliasStore_->count();
            auto batch = std::make_shared<BatchState>();
            batch->total = total;
            batch->chatId = u.chatId;

            int queued = 0;
            String capturedBody = body;
            aliasStore_->forEach([this, capturedBody, batch, &queued](
                const String & /*name*/, const String &aliasPhone) {
                smsSender_.enqueue(aliasPhone, capturedBody,
                    [this, batch]() {
                        // Failure callback.
                        batch->failed++;
                        if (batch->succeeded + batch->failed == batch->total && !batch->reported)
                        {
                            batch->reported = true;
                            String s = String("\xF0\x9F\x93\x8A "); // 📊
                            s += String(batch->succeeded); s += "/"; s += String(batch->total);
                            s += String(" delivered");
                            if (batch->failed > 0)
                            {
                                s += String(", "); s += String(batch->failed); s += String(" failed");
                            }
                            bot_.sendMessageTo(batch->chatId, s);
                        }
                    },
                    [this, batch]() {
                        // Success callback.
                        batch->succeeded++;
                        if (batch->succeeded + batch->failed == batch->total && !batch->reported)
                        {
                            batch->reported = true;
                            String s = String("\xF0\x9F\x93\x8A "); // 📊
                            s += String(batch->succeeded); s += "/"; s += String(batch->total);
                            s += String(" delivered");
                            if (batch->failed > 0)
                            {
                                s += String(", "); s += String(batch->failed); s += String(" failed");
                            }
                            bot_.sendMessageTo(batch->chatId, s);
                        }
                    });
                queued++;
            });
            String preview = body.substring(0, 30);
            if (body.length() > 30) preview += "\xE2\x80\xA6"; // …
            bot_.sendMessageTo(u.chatId,
                String("\xE2\x9C\x85 Queued to ") + String(queued) // ✅
                + String(" recipient") + (queued == 1 ? "" : "s") + String(": ") + preview);
            return;
        }

        // RFC-0033: /queue — show pending outbound SMS queue.
        if (lower == "/queue")
        {
            auto entries = smsSender_.getQueueSnapshot();
            String msg;
            if (entries.empty())
            {
                msg = String("\xF0\x9F\x93\xA4 Queue empty"); // U+1F4E4 outbox
            }
            else
            {
                uint32_t nowMs = clock_ ? clock_() : 0;
                msg = String("\xF0\x9F\x93\xA4 Queue: ") + String((int)entries.size()) + String(" pending\n");
                int n = 1;
                for (const auto &e : entries)
                {
                    msg += String(n++) + String(". ") + e.phone + String(" \xe2\x80\x9c"); // "
                    msg += e.bodyPreview;
                    if (e.bodyPreview.length() == 20)
                        msg += String("\xE2\x80\xA6"); // U+2026 ellipsis
                    msg += String("\xe2\x80\x9d"); // "
                    msg += String(" (attempt ") + String(e.attempts + 1)
                        + String("/") + String(SmsSender::kMaxAttempts) + String(")");
                    // RFC-0095: show age since first drain attempt.
                    // RFC-0273: unsigned subtraction is wraparound-safe; drop the >= guard.
                    if (e.queuedAtMs > 0)
                    {
                        uint32_t ageSec = (uint32_t)(nowMs - e.queuedAtMs) / 1000;
                        if (ageSec < 60)
                            msg += String(" ") + String((int)ageSec) + String("s");
                        else
                            msg += String(" ") + String((int)(ageSec / 60)) + String("m");
                    }
                    msg += "\n";
                }
            }
            bot_.sendMessageTo(u.chatId, msg);
            return;
        }

        // RFC-0214: /queueinfo <N> — full details of outbound queue entry N.
        if (lower == "/queueinfo" || lower.startsWith("/queueinfo "))
        {
            String arg = extractArg(lower, "/queueinfo ");
            if (arg.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /queueinfo <N>  (see /queue for numbers)"));
                return;
            }
            int n = (int)arg.toInt();
            auto entries = smsSender_.getQueueSnapshot();
            if (n < 1 || n > (int)entries.size())
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c No entry ") + String(n) // ❌
                    + String(" in queue (") + String((int)entries.size()) + String(" total)."));
                return;
            }
            const auto &e = entries[n - 1];
            uint32_t nowMs = clock_ ? clock_() : 0;
            String msg = String("\xf0\x9f\x93\xa4 Queue slot ") + String(n) // 📤
                       + String("/") + String(SmsSender::kQueueSize) + String("\n");
            msg += String("Phone:    ") + e.phone + String("\n");
            msg += String("Body:     ") + e.bodyFull + String("\n");
            msg += String("Attempts: ") + String(e.attempts) + String("/")
                 + String(SmsSender::kMaxAttempts) + String("\n");
            // RFC-0273: unsigned subtraction is wraparound-safe; drop the >= guard.
            if (e.queuedAtMs > 0)
            {
                uint32_t ageSec = (uint32_t)(nowMs - e.queuedAtMs) / 1000;
                if (ageSec < 60)
                    msg += String("Queued:   ") + String((int)ageSec) + String("s ago\n");
                else
                    msg += String("Queued:   ") + String((int)(ageSec / 60)) + String("m ago\n");
            }
            // RFC-0273: wraparound-safe "is retry still in future?" check.
            if (e.nextRetryMs > 0 && (uint32_t)(nowMs - e.nextRetryMs) >= 0x80000000UL)
            {
                // now < nextRetryMs: compute remaining wait via unsigned subtraction.
                uint32_t waitSec = (e.nextRetryMs - nowMs) / 1000;
                msg += String("Next retry: in ") + String((int)waitSec) + String("s");
            }
            else
            {
                msg += String("Next retry: now");
            }
            bot_.sendMessageTo(u.chatId, msg);
            return;
        }

        // RFC-0087: /flushqueue — reset retry timers so entries drain immediately.
        if (lower == "/flushqueue")
        {
            int n = smsSender_.queueSize();
            smsSender_.resetRetryTimers();
            if (n > 0)
                bot_.sendMessageTo(u.chatId,
                    String("\xF0\x9F\x94\x84 Retry timers reset. ") + String(n) // 🔄
                    + String(" entr") + (n == 1 ? "y" : "ies") + String(" will drain on next tick."));
            else
                bot_.sendMessageTo(u.chatId, String("Queue is empty."));
            return;
        }

        // RFC-0216: /retry <N> — reset retry timer for the Nth queue entry only.
        if (lower == "/retry" || lower.startsWith("/retry "))
        {
            String arg = extractArg(lower, "/retry ");
            if (arg.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /retry <N>  (see /queue for numbers)"));
                return;
            }
            int n = (int)arg.toInt();
            if (!smsSender_.resetRetryTimer(n))
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c No entry ") + String(n) // ❌
                    + String(" in queue (")
                    + String(smsSender_.queueSize()) + String(" total)."));
                return;
            }
            bot_.sendMessageTo(u.chatId,
                String("\xF0\x9F\x94\x84 Entry ") + String(n) // 🔄
                + String(" will retry on next tick."));
            return;
        }

        // RFC-0110: /resetstats — reset all session-level counters.
        if (lower == "/resetstats")
        {
            if (resetStatsFn_)
            {
                resetStatsFn_();
                bot_.sendMessageTo(u.chatId,
                    String("\xE2\x9C\x85 Session counters reset.")); // ✅
            }
            else
            {
                bot_.sendMessageTo(u.chatId, String("(resetstats not configured)"));
            }
            return;
        }

        // RFC-0112: /reboot — soft reboot via injected callback.
        if (lower == "/reboot")
        {
            if (rebootFn_)
            {
                bot_.sendMessageTo(u.chatId, String("Rebooting..."));
                rebootFn_(u.fromId);
            }
            else
            {
                bot_.sendMessageTo(u.chatId, String("(reboot not configured)"));
            }
            return;
        }

        // RFC-0184: /factoryreset — two-step NVS wipe + reboot.
        if (lower == "/factoryreset")
        {
            bot_.sendMessageTo(u.chatId,
                String("\xe2\x9a\xa0\xef\xb8\x8f This will erase ALL persisted NVS settings and reboot.\n" // ⚠️
                       "Aliases, block list, labels, timezone, and all runtime\n"
                       "configuration will return to firmware defaults.\n\n"
                       "Type /factoryreset confirm to proceed."));
            return;
        }
        if (lower == "/factoryreset confirm")
        {
            bot_.sendMessageTo(u.chatId, String("\xf0\x9f\x97\x91 NVS cleared \xe2\x80\x94 rebooting now...")); // 🗑 —
            if (clearNvsFn_) clearNvsFn_();
            if (rebootFn_)   rebootFn_(u.fromId);
            return;
        }

        // RFC-0120: /uptime — quick uptime one-liner.
        if (lower == "/uptime")
        {
            if (uptimeFn_)
                bot_.sendMessageTo(u.chatId, uptimeFn_());
            else
                bot_.sendMessageTo(u.chatId, String("(uptime not configured)"));
            return;
        }

        // RFC-0121: /network — cellular registration + operator + CSQ snapshot.
        if (lower == "/network")
        {
            if (networkFn_)
                bot_.sendMessageTo(u.chatId, networkFn_());
            else
                bot_.sendMessageTo(u.chatId, String("(network info not configured)"));
            return;
        }

        // RFC-0123: /boot — boot count, reset reason, boot timestamp.
        if (lower == "/boot")
        {
            if (bootInfoFn_)
                bot_.sendMessageTo(u.chatId, bootInfoFn_());
            else
                bot_.sendMessageTo(u.chatId, String("(boot info not configured)"));
            return;
        }

        // RFC-0124: /count — compact SMS/call counter summary.
        if (lower == "/count")
        {
            if (countFn_)
                bot_.sendMessageTo(u.chatId, countFn_());
            else
                bot_.sendMessageTo(u.chatId, String("(count not configured)"));
            return;
        }

        // RFC-0126: /ip — WiFi IP, SSID, and RSSI snapshot.
        if (lower == "/ip")
        {
            if (ipFn_)
                bot_.sendMessageTo(u.chatId, ipFn_());
            else
                bot_.sendMessageTo(u.chatId, String("(ip info not configured)"));
            return;
        }

        // RFC-0127: /smsslots — SIM SMS slot usage one-liner.
        if (lower == "/smsslots")
        {
            if (smsSlotsFn_)
                bot_.sendMessageTo(u.chatId, smsSlotsFn_());
            else
                bot_.sendMessageTo(u.chatId, String("(SMS slots info not configured)"));
            return;
        }

        // RFC-0161: /smscount — SIM SMS storage capacity via AT+CPMS?.
        if (lower == "/smscount")
        {
            if (smsCntFn_)
                bot_.sendMessageTo(u.chatId, smsCntFn_());
            else
                bot_.sendMessageTo(u.chatId, String("(smscount not configured)"));
            return;
        }

        // RFC-0128: /lifetime — lifetime SMS count and boot count.
        if (lower == "/lifetime")
        {
            if (lifetimeFn_)
                bot_.sendMessageTo(u.chatId, lifetimeFn_());
            else
                bot_.sendMessageTo(u.chatId, String("(lifetime stats not configured)"));
            return;
        }

        // RFC-0129: /announce <msg> — broadcast to all authorized users.
        if (lower.startsWith("/announce"))
        {
            if (!announceFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(announce not configured)"));
                return;
            }
            String msg = extractArg(u.text, "/announce ");
            if (msg.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /announce <message>\nExample: /announce Maintenance in 5 minutes"));
                return;
            }
            int count = announceFn_(msg);
            String reply = String("\xe2\x9c\x85 Announced to "); // ✅
            reply += String(count);
            reply += String(count == 1 ? " user." : " users.");
            bot_.sendMessageTo(u.chatId, reply);
            return;
        }

        // RFC-0130: /digest — on-demand stats digest.
        if (lower == "/digest")
        {
            if (digestFn_)
                bot_.sendMessageTo(u.chatId, digestFn_());
            else
                bot_.sendMessageTo(u.chatId, String("(digest not configured)"));
            return;
        }

        // RFC-0143: /modeminfo — show IMEI, model, firmware revision.
        if (lower == "/modeminfo")
        {
            if (modemInfoFn_)
                bot_.sendMessageTo(u.chatId, modemInfoFn_());
            else
                bot_.sendMessageTo(u.chatId, String("(modem info not configured)"));
            return;
        }

        // RFC-0152: /resetwatermark — reset Telegram update_id watermark.
        if (lower == "/resetwatermark")
        {
            resetWatermark();
            bot_.sendMessageTo(u.chatId,
                String("\xe2\x9c\x85 Watermark reset. " // ✅
                       "Recent updates will be re-processed on next poll."));
            return;
        }

        // RFC-0153: /setforward on|off — toggle SMS forwarding.
        if (lower == "/setforward on" || lower == "/setforward off")
        {
            if (!forwardingEnabledFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(setforward not configured)"));
                return;
            }
            bool enable = (lower == "/setforward on");
            forwardingEnabledFn_(enable);
            bot_.sendMessageTo(u.chatId,
                enable
                    ? String("\xe2\x9c\x85 SMS forwarding enabled.") // ✅
                    : String("\xe2\x9a\xa0\xef\xb8\x8f SMS forwarding PAUSED. SMS stay in SIM.")); // ⚠️
            return;
        }
        if (lower == "/setforward")
        {
            bot_.sendMessageTo(u.chatId, String("Usage: /setforward on\n/setforward off"));
            return;
        }

        // RFC-0164: /setcallnotify on|off — toggle call Telegram notifications.
        if (lower == "/setcallnotify on" || lower == "/setcallnotify off")
        {
            if (!callNotifyFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(setcallnotify not configured)"));
                return;
            }
            bool enable = (lower == "/setcallnotify on");
            callNotifyFn_(enable);
            bot_.sendMessageTo(u.chatId,
                enable
                    ? String("\xe2\x9c\x85 Call notifications ENABLED.")  // ✅
                    : String("\xF0\x9F\x94\x95 Call notifications MUTED. Calls still auto-rejected.")); // 🔕
            return;
        }
        if (lower == "/setcallnotify")
        {
            bot_.sendMessageTo(u.chatId, String("Usage: /setcallnotify on\n/setcallnotify off"));
            return;
        }

        // RFC-0165: /setcalldedup <seconds> — set call dedup/cooldown window.
        if (lower == "/setcalldedup" || lower.startsWith("/setcalldedup "))
        {
            if (!callDedupFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(setcalldedup not configured)"));
                return;
            }
            String arg = extractArg(lower, "/setcalldedup ");
            if (arg.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /setcalldedup <seconds>\nRange: 1\xe2\x80\x93" "60 (default 6)"));
                return;
            }
            int secs = (int)arg.toInt();
            if (secs < 1 || secs > 60)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Error: seconds must be 1\xe2\x80\x93" "60"));
                return;
            }
            callDedupFn_((uint32_t)secs * 1000UL);
            bot_.sendMessageTo(u.chatId,
                String("\xe2\x9c\x85 Call dedup window set to ") // ✅
                + String(secs) + String("s."));
            return;
        }

        // RFC-0166: /setunknowndeadline <ms> — RING-without-+CLIP commit deadline.
        if (lower == "/setunknowndeadline" || lower.startsWith("/setunknowndeadline "))
        {
            if (!callUnknownDeadlineFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(setunknowndeadline not configured)"));
                return;
            }
            String arg = extractArg(lower, "/setunknowndeadline ");
            if (arg.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /setunknowndeadline <ms>\nRange: 500\xe2\x80\x931" "0000 (default 1500)"));
                return;
            }
            int ms = (int)arg.toInt();
            if (ms < 500 || ms > 10000)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Error: ms must be 500\xe2\x80\x931" "0000"));
                return;
            }
            callUnknownDeadlineFn_((uint32_t)ms);
            bot_.sendMessageTo(u.chatId,
                String("\xe2\x9c\x85 Unknown-number deadline set to ") // ✅
                + String(ms) + String("ms."));
            return;
        }

        // RFC-0169/0175: /setgmtoffset <hours> — set timezone offset for SMS timestamps.
        // Calls gmtOffsetFn_ with hours*60 (minutes). Use /setgmtoffsetmin for
        // fractional offsets (UTC+5:30 etc).
        if (lower == "/setgmtoffset" || lower.startsWith("/setgmtoffset "))
        {
            if (!gmtOffsetFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(setgmtoffset not configured)"));
                return;
            }
            String arg = extractArg(lower, "/setgmtoffset ");
            if (arg.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /setgmtoffset <hours>\nRange: -12 to +14\nExamples: /setgmtoffset 8  /setgmtoffset -5\nFor UTC+5:30 use /setgmtoffsetmin 330"));
                return;
            }
            int hours = (int)arg.toInt();
            if (hours < -12 || hours > 14)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Error: hours must be -12 to +14"));
                return;
            }
            gmtOffsetFn_(hours * 60); // RFC-0175: fn now takes minutes
            String sign = (hours >= 0) ? "+" : "";
            bot_.sendMessageTo(u.chatId,
                String("\xe2\x9c\x85 GMT offset set to UTC") // ✅
                + sign + String(hours) + String("."));
            return;
        }

        // RFC-0175: /setgmtoffsetmin <minutes> — fractional timezone offsets.
        if (lower == "/setgmtoffsetmin" || lower.startsWith("/setgmtoffsetmin "))
        {
            if (!gmtOffsetFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(setgmtoffset not configured)"));
                return;
            }
            String arg = extractArg(lower, "/setgmtoffsetmin ");
            if (arg.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /setgmtoffsetmin <total_minutes>\nRange: -720 to +840\nExamples: /setgmtoffsetmin 330 (UTC+5:30)  /setgmtoffsetmin -300 (UTC-5)"));
                return;
            }
            int mins = (int)arg.toInt();
            if (mins < -720 || mins > 840)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Error: minutes must be -720 to +840"));
                return;
            }
            gmtOffsetFn_(mins);
            int absM = mins < 0 ? -mins : mins;
            int h = absM / 60, m = absM % 60;
            char buf[12];
            if (m == 0)
                snprintf(buf, sizeof(buf), "%c%d", mins < 0 ? '-' : '+', h);
            else
                snprintf(buf, sizeof(buf), "%c%d:%02d", mins < 0 ? '-' : '+', h, m);
            bot_.sendMessageTo(u.chatId,
                String("\xe2\x9c\x85 GMT offset set to UTC") + String(buf) + String(".")); // ✅
            return;
        }

        // RFC-0172: /setfwdtag <text> — set a custom prefix tag on forwarded SMS.
        if (lower.startsWith("/setfwdtag"))
        {
            if (!fwdTagFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(setfwdtag not configured)"));
                return;
            }
            String tag = extractArg(u.text, "/setfwdtag ");
            if (tag.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /setfwdtag <text>\nMax 20 chars.\nExample: /setfwdtag [Home]"));
                return;
            }
            if ((int)tag.length() > 20)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Tag too long (max 20 chars).")); // ❌
                return;
            }
            fwdTagFn_(tag);
            bot_.sendMessageTo(u.chatId,
                String("\xe2\x9c\x85 Forward tag set to \"") + tag + String("\"")); // ✅
            return;
        }

        // RFC-0172: /clearfwdtag — remove the custom forward prefix tag.
        if (lower == "/clearfwdtag")
        {
            if (!fwdTagFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(setfwdtag not configured)"));
                return;
            }
            fwdTagFn_(String());
            bot_.sendMessageTo(u.chatId,
                String("\xe2\x9c\x85 Forward tag cleared.")); // ✅
            return;
        }

        // RFC-0181: /fwdtest — preview forwarded SMS format with current settings.
        if (lower == "/fwdtest")
        {
            if (!fwdTestFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(fwdtest not configured)"));
                return;
            }
            String preview = fwdTestFn_();
            bot_.sendMessageTo(u.chatId, String("\xF0\x9F\x94\x8D Format preview:\n") + preview); // 🔍
            return;
        }

        // RFC-0187: /testfmt <phone> <body> — format preview with custom sender.
        if (lower == "/testfmt" || lower.startsWith("/testfmt "))
        {
            if (!fwdTestPhoneBodyFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(fwdtest not configured)"));
                return;
            }
            String arg = extractArg(u.text, "/testfmt ");
            arg.trim();
            int spacePos = arg.indexOf(' ');
            if (arg.length() == 0 || spacePos <= 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /testfmt <phone> <body>\nExample: /testfmt +13800138000 Hello!"));
                return;
            }
            String phone = arg.substring(0, spacePos);
            String body  = arg.substring(spacePos + 1);
            body.trim();
            if (body.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /testfmt <phone> <body>\nExample: /testfmt +13800138000 Hello!"));
                return;
            }
            String preview = fwdTestPhoneBodyFn_(phone, body);
            bot_.sendMessageTo(u.chatId, String("\xF0\x9F\x94\x8D Format preview:\n") + preview); // 🔍
            return;
        }

        // RFC-0162: /setblockmode on|off — toggle block list enforcement.
        if (lower == "/setblockmode on" || lower == "/setblockmode off")
        {
            if (!blockingEnabledFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(setblockmode not configured)"));
                return;
            }
            bool enable = (lower == "/setblockmode on");
            blockingEnabledFn_(enable);
            bot_.sendMessageTo(u.chatId,
                enable
                    ? String("\xe2\x9c\x85 Block list enforcement ENABLED.")  // ✅
                    : String("\xe2\x9a\xa0\xef\xb8\x8f Block list SUSPENDED. All senders pass through.")); // ⚠️
            return;
        }
        if (lower == "/setblockmode")
        {
            bot_.sendMessageTo(u.chatId, String("Usage: /setblockmode on\n/setblockmode off"));
            return;
        }

        // RFC-0151: /getautoreply — show current SMS auto-reply text.
        if (lower == "/getautoreply")
        {
            if (!autoReplyGetFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(auto-reply not configured)"));
                return;
            }
            String text = autoReplyGetFn_();
            bot_.sendMessageTo(u.chatId,
                text.length() > 0
                    ? String("Auto-reply: \"") + text + String("\"")
                    : String("(auto-reply not set)"));
            return;
        }

        // RFC-0151: /setautoreply <text> — set SMS auto-reply (max 160 chars).
        if (lower.startsWith("/setautoreply"))
        {
            if (!autoReplySetFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(auto-reply not configured)"));
                return;
            }
            String text = u.text.length() > 14 ? u.text.substring(14) : String();
            text.trim();
            if (text.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /setautoreply <text>\nMax 160 chars."));
                return;
            }
            if ((int)text.length() > 160)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Text too long (max 160 chars).")); // ❌
                return;
            }
            autoReplySetFn_(text);
            bot_.sendMessageTo(u.chatId,
                String("\xe2\x9c\x85 Auto-reply set: \"") + text + String("\"")); // ✅
            return;
        }

        // RFC-0151: /clearautoreply — disable SMS auto-reply.
        if (lower == "/clearautoreply")
        {
            if (!autoReplySetFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(auto-reply not configured)"));
                return;
            }
            autoReplySetFn_(String());
            bot_.sendMessageTo(u.chatId, String("\xe2\x9c\x85 Auto-reply cleared.")); // ✅
            return;
        }

        // RFC-0148: /sweepsim — manually trigger a SIM sweep.
        if (lower == "/sweepsim")
        {
            if (!sweepFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(sweepsim not configured)"));
                return;
            }
            int n = sweepFn_();
            if (n == 0)
                bot_.sendMessageTo(u.chatId, String("(no SMS found in SIM)"));
            else
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9c\x85 Swept ") + String(n) // ✅
                    + String(n == 1 ? " SMS." : " SMS."));
            return;
        }

        // RFC-0149: /health — compact single-line health check.
        if (lower == "/health")
        {
            if (healthFn_)
                bot_.sendMessageTo(u.chatId, healthFn_());
            else
                bot_.sendMessageTo(u.chatId, String("(health not configured)"));
            return;
        }

        // RFC-0156: /simstatus — SIM card + network status snapshot.
        if (lower == "/simstatus")
        {
            if (simStatusFn_)
                bot_.sendMessageTo(u.chatId, simStatusFn_());
            else
                bot_.sendMessageTo(u.chatId, String("(simstatus not configured)"));
            return;
        }

        // RFC-0158: /wifiscan — scan nearby WiFi networks.
        if (lower == "/wifiscan")
        {
            if (wifiScanFn_)
                bot_.sendMessageTo(u.chatId, wifiScanFn_());
            else
                bot_.sendMessageTo(u.chatId, String("(wifiscan not configured)"));
            return;
        }

        // RFC-0146: /forwardsim <idx> — force-forward a SIM slot to Telegram.
        if (lower == "/forwardsim" || lower.startsWith("/forwardsim "))
        {
            if (!smsForwardFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(forwardsim not configured)"));
                return;
            }
            String arg = extractArg(lower, "/forwardsim ");
            if (arg.length() == 0)
            {
                bot_.sendMessageTo(u.chatId, String("Usage: /forwardsim <idx>"));
                return;
            }
            long idx = arg.toInt();
            if (idx < 1 || idx > 255)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Invalid index (1\xe2\x80\x93 255).")); // ❌
                return;
            }
            if (smsForwardFn_((int)idx))
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9c\x85 Forwarded slot ") + String((int)idx) + String(".")); // ✅
            else
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Failed to forward slot ") + String((int)idx) + String(".")); // ❌
            return;
        }

        // RFC-0147: /setpollinterval <seconds> — change Telegram poll interval.
        if (lower == "/setpollinterval" || lower.startsWith("/setpollinterval "))
        {
            String arg = extractArg(lower, "/setpollinterval ");
            if (arg.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /setpollinterval <seconds>\nRange: 1\xe2\x80\x93 300 seconds")); // –
                return;
            }
            long val = arg.toInt();
            if (val < 1 || val > 300)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Invalid interval (1\xe2\x80\x93 300 seconds).")); // ❌
                return;
            }
            setPollIntervalMs((uint32_t)val * 1000u);
            bot_.sendMessageTo(u.chatId,
                String("\xe2\x9c\x85 Poll interval set to ") // ✅
                + String((int)val) + String(" seconds."));
            return;
        }

        // RFC-0144: /setdedup <seconds> — change SMS dedup window at runtime.
        if (lower == "/setdedup" || lower.startsWith("/setdedup "))
        {
            if (!dedupWindowFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(setdedup not configured)"));
                return;
            }
            String arg = extractArg(lower, "/setdedup ");
            if (arg.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /setdedup <seconds>\n0 = disable dedup, max 3600 (1h)"));
                return;
            }
            long val = arg.toInt();
            if (val < 0 || val > 3600)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Invalid dedup window (0\xe2\x80\x93 3600 seconds).")); // ❌
                return;
            }
            dedupWindowFn_((uint32_t)val);
            if (val == 0)
                bot_.sendMessageTo(u.chatId, String("\xe2\x9c\x85 Dedup disabled.")); // ✅
            else
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9c\x85 Dedup window set to ") // ✅
                    + String((int)val) + String(" seconds."));
            return;
        }

        // RFC-0145: /cleardedup — clear the SMS dedup ring buffer.
        if (lower == "/cleardedup")
        {
            if (!clearDedupFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(cleardedup not configured)"));
                return;
            }
            clearDedupFn_();
            bot_.sendMessageTo(u.chatId, String("\xe2\x9c\x85 Dedup buffer cleared.")); // ✅
            return;
        }

        // RFC-0142: /setconcatttl <seconds> — change concat fragment TTL at runtime.
        if (lower == "/setconcatttl" || lower.startsWith("/setconcatttl "))
        {
            if (!concatTtlFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(setconcatttl not configured)"));
                return;
            }
            String arg = extractArg(lower, "/setconcatttl ");
            if (arg.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /setconcatttl <seconds>\n"
                           "Range: 60 (1 min) \xe2\x80\x93 604800 (7 days)")); // –
                return;
            }
            long val = arg.toInt();
            if (val < 60 || val > 604800)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Invalid TTL (60\xe2\x80\x93 604800 seconds).")); // ❌
                return;
            }
            concatTtlFn_((uint32_t)val);
            bot_.sendMessageTo(u.chatId,
                String("\xe2\x9c\x85 Concat TTL set to ") + String((int)val) // ✅
                + String(" seconds."));
            return;
        }

        // RFC-0137: /setinterval <seconds> — change heartbeat interval at runtime.
        if (lower == "/setinterval" || lower.startsWith("/setinterval "))
        {
            if (!intervalFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(setinterval not configured)"));
                return;
            }
            String arg = extractArg(lower, "/setinterval ");
            if (arg.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /setinterval <seconds>\n0 = disable, max 86400 (24h)"));
                return;
            }
            long val = arg.toInt();
            if (val < 0 || val > 86400)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Invalid interval (0\xe2\x80\x93 86400 seconds).")); // ❌
                return;
            }
            intervalFn_((uint32_t)val);
            if (val == 0)
                bot_.sendMessageTo(u.chatId, String("\xe2\x9c\x85 Heartbeat disabled.")); // ✅
            else
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9c\x85 Heartbeat interval set to ") // ✅
                    + String((int)val) + String(" seconds."));
            return;
        }

        // RFC-0177: /hbnow — trigger immediate heartbeat.
        if (lower == "/hbnow")
        {
            if (!heartbeatNowFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(hbnow not configured)"));
                return;
            }
            bool ok = heartbeatNowFn_();
            if (ok)
                bot_.sendMessageTo(u.chatId, String("\xe2\x9c\x85 Heartbeat triggered.")); // ✅
            else
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9a\xa0\xef\xb8\x8f Heartbeat is disabled \xe2\x80\x94 " // ⚠️ —
                           "enable with /setinterval first."));
            return;
        }

        // RFC-0138: /setmaxfail <N> — change consecutive-failure reboot threshold.
        if (lower == "/setmaxfail" || lower.startsWith("/setmaxfail "))
        {
            if (!maxFailFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(setmaxfail not configured)"));
                return;
            }
            String arg = extractArg(lower, "/setmaxfail ");
            if (arg.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /setmaxfail <N>\n0 = never reboot on failures, max 99"));
                return;
            }
            long val = arg.toInt();
            if (val < 0 || val > 99)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Invalid value (0\xe2\x80\x93 99).")); // ❌
                return;
            }
            maxFailFn_((int)val);
            if (val == 0)
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9c\x85 Auto-reboot on failure disabled.")); // ✅
            else
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9c\x85 Reboot threshold set to ") // ✅
                    + String((int)val) + String(" consecutive failures."));
            return;
        }

        // RFC-0160: /setmaxparts <N> — set max concat parts for outbound SMS (1-10).
        if (lower == "/setmaxparts" || lower.startsWith("/setmaxparts "))
        {
            if (!maxPartsFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(setmaxparts not configured)"));
                return;
            }
            String arg = extractArg(lower, "/setmaxparts ");
            if (arg.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /setmaxparts <N>\nRange: 1\xe2\x80\x93" "10 (default 10)"));
                return;
            }
            int val = (int)arg.toInt();
            if (val < 1 || val > 10)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Error: max parts must be 1\xe2\x80\x93" "10"));
                return;
            }
            maxPartsFn_(val);
            int charLimitGsm = val * 153;
            int charLimitUcs = val * 67;
            bot_.sendMessageTo(u.chatId,
                String("\xe2\x9c\x85 Max SMS parts set to ") // ✅
                + String(val)
                + String(". Body limit: ~")
                + String(charLimitGsm)
                + String(" GSM-7 / ~")
                + String(charLimitUcs)
                + String(" Unicode chars."));
            return;
        }

        // RFC-0139: /flushsim yes — delete all SMS from SIM.
        if (lower == "/flushsim" || lower.startsWith("/flushsim "))
        {
            if (!flushSimFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(flushsim not configured)"));
                return;
            }
            String arg = extractArg(lower, "/flushsim ");
            if (arg != "yes")
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /flushsim yes\n"
                           "\xe2\x9a\xa0\xef\xb8\x8f This deletes ALL SMS from the SIM. " // ⚠️
                           "Type /flushsim yes to confirm."));
                return;
            }
            int n = flushSimFn_();
            if (n < 0)
                bot_.sendMessageTo(u.chatId, String("\xe2\x9c\x85 SIM flushed.")); // ✅
            else
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9c\x85 SIM flushed: ") + String(n) // ✅
                    + String(n == 1 ? " SMS deleted." : " SMS deleted."));
            return;
        }

        // RFC-0140: /simlist — list all SMS currently in SIM.
        if (lower == "/simlist")
        {
            if (!simListFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(simlist not configured)"));
                return;
            }
            bot_.sendMessageTo(u.chatId, simListFn_());
            return;
        }

        // RFC-0141: /simread <idx> — decode and show a specific SIM slot.
        if (lower == "/simread" || lower.startsWith("/simread "))
        {
            if (!simReadFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(simread not configured)"));
                return;
            }
            String arg = extractArg(lower, "/simread ");
            if (arg.length() == 0)
            {
                bot_.sendMessageTo(u.chatId, String("Usage: /simread <idx>"));
                return;
            }
            long idx = arg.toInt();
            if (idx < 1 || idx > 255)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Invalid index (1\xe2\x80\x93 255).")); // ❌
                return;
            }
            bot_.sendMessageTo(u.chatId, simReadFn_((int)idx));
            return;
        }

        // RFC-0131: /note — show current device note.
        if (lower == "/note")
        {
            if (noteGetFn_)
            {
                String n = noteGetFn_();
                bot_.sendMessageTo(u.chatId, n.length() > 0 ? n : String("(no note set)"));
            }
            else
            {
                bot_.sendMessageTo(u.chatId, String("(note not configured)"));
            }
            return;
        }

        // RFC-0131: /setnote <text> — save device note (max 120 chars).
        if (lower.startsWith("/setnote"))
        {
            if (!noteSetFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(note not configured)"));
                return;
            }
            String note = extractArg(u.text, "/setnote ");
            if (note.length() == 0)
            {
                bot_.sendMessageTo(u.chatId, String("Usage: /setnote <text>\nMax 120 chars."));
                return;
            }
            if (note.length() > 120)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xE2\x9D\x8C Note too long (") // ❌
                    + String(note.length()) + String(" chars, max 120)."));
                return;
            }
            noteSetFn_(note);
            bot_.sendMessageTo(u.chatId, String("\xe2\x9c\x85 Note saved.")); // ✅
            return;
        }

        // RFC-0089: /clearqueue — discard all pending outbound SMS entries.
        if (lower == "/clearqueue")
        {
            int n = smsSender_.clearQueue();
            if (n > 0)
                bot_.sendMessageTo(u.chatId,
                    String("\xF0\x9F\x97\x91 Cleared ") + String(n) // 🗑
                    + String(" queued entr") + (n == 1 ? "y." : "ies."));
            else
                bot_.sendMessageTo(u.chatId, String("Queue is already empty."));
            return;
        }

        // RFC-0046: /cancel <N> — remove the Nth pending queue entry.
        if (lower == "/cancel" || lower.startsWith("/cancel "))
        {
            String arg = extractArg(lower, "/cancel ");
            if (arg.length() == 0)
            {
                bot_.sendMessageTo(u.chatId, String("Usage: /cancel <N>  (see /queue for numbers)"));
                return;
            }
            int n = arg.toInt();
            if (n <= 0)
            {
                bot_.sendMessageTo(u.chatId, String("Usage: /cancel <N>  (N must be a positive integer)"));
                return;
            }
            if (smsSender_.cancelQueueEntry(n))
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xE2\x9C\x85 Queue entry ") + String(n) + String(" cancelled.")); // ✅
            }
            else
            {
                sendErrorReply(u.chatId,
                    String("No entry ") + String(n) + String(" in queue (use /queue to list)."));
            }
            return;
        }

        // RFC-0136: /cancelnum <phone> — cancel all queued entries for a phone number.
        if (lower == "/cancelnum" || lower.startsWith("/cancelnum "))
        {
            String arg = extractArg(u.text, "/cancelnum ");
            if (arg.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /cancelnum <phone>\nExample: /cancelnum +447911123456"));
                return;
            }
            String phone = sms_codec::normalizePhoneNumber(arg);
            int n = smsSender_.cancelByPhone(phone);
            if (n > 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9c\x85 Cancelled ") // ✅
                    + String(n) + String(n == 1 ? " entry for " : " entries for ")
                    + phone + String("."));
            }
            else
            {
                bot_.sendMessageTo(u.chatId,
                    String("(no queued entries for ") + phone + String(")"));
            }
            return;
        }

        // RFC-0188: /schedulesend <delay_min> <phone> <body> — delayed SMS.
        if (lower == "/schedulesend" || lower.startsWith("/schedulesend "))
        {
            String arg = extractArg(u.text, "/schedulesend ");
            arg.trim();
            int sp1 = arg.indexOf(' ');
            if (sp1 <= 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /schedulesend <delay_min> <phone> <body>\n"
                           "Example: /schedulesend 30 +13800138000 Hello!"));
                return;
            }
            long delayMin = arg.substring(0, sp1).toInt();
            if (delayMin < 1 || delayMin > 1440)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Delay must be 1\xe2\x80\x93 1440 minutes.")); // ❌
                return;
            }
            String rest = arg.substring(sp1 + 1);
            rest.trim();
            int sp2 = rest.indexOf(' ');
            if (sp2 <= 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /schedulesend <delay_min> <phone> <body>"));
                return;
            }
            String schedErrMsg;
            String phone = resolvePhone(rest.substring(0, sp2), aliasStore_, schedErrMsg); // RFC-0224
            if (phone.length() == 0)
            {
                sendErrorReply(u.chatId, schedErrMsg.length() > 0
                    ? schedErrMsg : String("Usage: /schedulesend <delay_min> <phone> <body>"));
                return;
            }
            String body  = rest.substring(sp2 + 1);
            body.trim();
            if (body.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /schedulesend <delay_min> <phone> <body>"));
                return;
            }
            if (body.length() > 1530) // RFC-0261: enforce SMS body cap
            {
                sendErrorReply(u.chatId,
                    String("\xe2\x9d\x8c Message too long for scheduled queue (max 1530 chars).")); // ❌
                return;
            }
            // Find a free slot.
            uint32_t nowMs   = clock_ ? clock_() : 0;
            uint32_t sendAt  = nowMs + (uint32_t)delayMin * 60000UL;
            int slotIdx = -1;
            for (int i = 0; i < (int)kScheduledQueueSize; i++)
            {
                if (scheduledQueue_[i].sendAtMs == 0) { slotIdx = i; break; }
            }
            if (slotIdx < 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Scheduled queue full (5 slots). Use /schedqueue to see pending.")); // ❌
                return;
            }
            scheduledQueue_[slotIdx].sendAtMs = sendAt;
            scheduledQueue_[slotIdx].phone    = phone;
            scheduledQueue_[slotIdx].body     = body;
            if (persistSchedFn_) persistSchedFn_(); // RFC-0200
            uint32_t nowMsSched = clock_ ? clock_() : 0;
            String eta = schedEtaStr(sendAt, nowMsSched, wallTimeFn_); // RFC-0202
            String reply = String("\xe2\x8f\xb0 SMS to ") + phone // ⏰
                         + String(" scheduled ") + eta
                         + String(" (slot ") + String(slotIdx + 1) + String("/")
                         + String((int)kScheduledQueueSize) + String(").")
                         + quietHoursWarning(sendAt, nowMsSched, quietStart_, quietEnd_, wallTimeFn_); // RFC-0212
            bot_.sendMessageTo(u.chatId, reply);
            return;
        }

        // RFC-0188: /schedqueue — list pending scheduled SMS.
        if (lower == "/schedqueue")
        {
            uint32_t nowMs2 = clock_ ? clock_() : 0;
            int pending = 0;
            String out;
            for (int i = 0; i < (int)kScheduledQueueSize; i++)
            {
                if (scheduledQueue_[i].sendAtMs == 0) continue;
                pending++;
                String eta = schedEtaStr(scheduledQueue_[i].sendAtMs, nowMs2, wallTimeFn_); // RFC-0202
                out += String(i + 1) + String(". ") + scheduledQueue_[i].phone
                    + String(" ") + eta;
                if (scheduledQueue_[i].repeatIntervalMs > 0) // RFC-0221
                {
                    uint32_t rmin = scheduledQueue_[i].repeatIntervalMs / 60000U;
                    out += String(" \xf0\x9f\x94\x81"); // 🔁
                    out += String(rmin) + String("m");
                }
                out += String(": ");
                String preview = scheduledQueue_[i].body;
                if (preview.length() > 40) preview = preview.substring(0, 40) + String("...");
                out += preview;
                out += String("\n");
            }
            if (pending == 0)
                bot_.sendMessageTo(u.chatId, String("(no scheduled SMS)"));
            else
            {
                String header = String("\xe2\x8f\xb0 Scheduled SMS (") // ⏰
                              + String(pending) + String(" pending)");
                if (schedPaused_)
                    header += String(" \xe2\x8f\xb8 PAUSED"); // ⏸
                header += String(":\n");
                bot_.sendMessageTo(u.chatId, header + out);
            }
            return;
        }

        // RFC-0226: /schedexport — print each occupied slot as a re-entrant command.
        if (lower == "/schedexport")
        {
            uint32_t expNowMs = clock_ ? clock_() : 0;
            long expWallNow = wallTimeFn_ ? wallTimeFn_() : 0;
            int count = 0;
            int omitted = 0;
            String out;
            // RFC-0259: Telegram messages are capped at 4096 chars. A slot body
            // can be up to ~1530 chars (10-part concat), so 5 slots could reach
            // ~7800 chars. Stop adding lines once we'd exceed ~3900 chars (leaves
            // room for the header). Omitted slots are reported in a footer;
            // use /schedinfo <N> to inspect them individually.
            static constexpr unsigned int kExportMaxLen = 3900;
            for (int i = 0; i < (int)kScheduledQueueSize; i++)
            {
                const ScheduledSms &sl = scheduledQueue_[i];
                if (sl.sendAtMs == 0) continue;
                count++;
                String line;
                if (sl.repeatIntervalMs > 0)
                {
                    // Repeating slot: export as /recurring.
                    uint32_t rMin = sl.repeatIntervalMs / 60000U;
                    line = String("/recurring ") + String(rMin) + String(" ")
                         + sl.phone + String(" ") + sl.body + String("\n");
                }
                else if (expWallNow > 1000000000L)
                {
                    // Absolute timestamp available: export as /scheduleat.
                    // RFC-0273: unsigned subtraction is wraparound-safe; treat overdue as +60s.
                    uint32_t diffU = sl.sendAtMs - expNowMs;
                    long deltaMs = (diffU < 0x80000000UL) ? (long)diffU : 0L;
                    long fireUnix = expWallNow + deltaMs / 1000L;
                    if (fireUnix <= expWallNow) fireUnix = expWallNow + 60L; // overdue: use +1min
                    time_t ft = (time_t)fireUnix;
                    struct tm *tmFire = gmtime(&ft);
                    char dateBuf[20];
                    snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d %02d:%02d",
                             tmFire->tm_year + 1900, tmFire->tm_mon + 1, tmFire->tm_mday,
                             tmFire->tm_hour, tmFire->tm_min);
                    line = String("/scheduleat ") + String(dateBuf) + String(" ")
                         + sl.phone + String(" ") + sl.body + String("\n");
                }
                else
                {
                    // No wall time: export as relative /schedulesend.
                    // RFC-0273: unsigned subtraction; overdue slots export as 1m.
                    uint32_t diffU = sl.sendAtMs - expNowMs;
                    long deltaMs = (diffU < 0x80000000UL) ? (long)diffU : 0L;
                    long remMin = deltaMs > 0 ? (deltaMs + 59999L) / 60000L : 1L;
                    if (remMin < 1) remMin = 1;
                    line = String("/schedulesend ") + String((int)remMin) + String(" ")
                         + sl.phone + String(" ") + sl.body + String("\n");
                }
                if (out.length() + line.length() > kExportMaxLen)
                {
                    omitted++;
                    continue; // RFC-0259: body too long to fit; report in footer
                }
                out += line;
            }
            if (count == 0)
                bot_.sendMessageTo(u.chatId, String("(no scheduled SMS)"));
            else
            {
                String msg = String("\xf0\x9f\x93\x8b Scheduled queue export (") // 📋
                           + String(count) + String(" slot(s)):\n") + out;
                if (omitted > 0)
                    msg += String("(") + String(omitted)
                        + String(" slot(s) omitted — use /schedinfo <N> for long-body slots)");
                bot_.sendMessageTo(u.chatId, msg);
            }
            return;
        }

        // RFC-0228: /schedimport — batch import of /schedulesend, /scheduleat, /recurring lines.
        if (lower.startsWith("/schedimport"))
        {
            String importBody = u.text.length() > strlen("/schedimport") ?
                u.text.substring(strlen("/schedimport")) : String();
            importBody.trim();
            if (importBody.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /schedimport\n/schedulesend 30 +1234 body\n/scheduleat 2026-01-01 09:00 +1234 body\n/recurring 60 +1234 body"));
                return;
            }
            int imported = 0, skipped = 0;
            // Split on newlines.
            int pos = 0;
            while (pos <= (int)importBody.length())
            {
                int nl = importBody.indexOf('\n', pos);
                String line = (nl < 0)
                    ? importBody.substring(pos)
                    : importBody.substring(pos, nl);
                line.trim();
                pos = (nl < 0) ? (int)importBody.length() + 1 : nl + 1;
                if (line.length() == 0) continue;
                // Check allowed prefixes (compare only the slash-command prefix chars).
                String lpfx = line.substring(0, 20);
                lpfx.toLowerCase();
                bool allowed = lpfx.startsWith("/schedulesend ")
                            || lpfx.startsWith("/scheduleat ")
                            || lpfx.startsWith("/recurring ");
                if (!allowed) { skipped++; continue; }
                // Dispatch as synthetic update (re-uses all existing validation).
                TelegramUpdate syn;
                syn.updateId         = 0; // not persisted
                syn.fromId           = u.fromId;
                syn.chatId           = u.chatId;
                syn.replyToMessageId = 0;
                syn.text             = line;
                syn.valid            = true;
                int slotsBeforeImport = 0;
                for (int i = 0; i < (int)kScheduledQueueSize; i++)
                    if (scheduledQueue_[i].sendAtMs != 0) slotsBeforeImport++;
#ifdef ESP_PLATFORM
                esp_task_wdt_reset(); // RFC-0257: each recursive processUpdate sends one ~23 s message
#endif
                processUpdate(syn);
                int slotsAfterImport = 0;
                for (int i = 0; i < (int)kScheduledQueueSize; i++)
                    if (scheduledQueue_[i].sendAtMs != 0) slotsAfterImport++;
                if (slotsAfterImport > slotsBeforeImport)
                    imported++;
                else
                    skipped++;
            }
            String reply = String("\xe2\x9c\x85 Imported ") + String(imported)
                         + String(" slot(s), skipped ") + String(skipped) + String(" line(s).");
            bot_.sendMessageTo(u.chatId, reply);
            return;
        }

        // RFC-0205: /sendafter <HH:MM> <phone> <body> — schedule SMS at a wall-clock time.
        if (lower == "/sendafter" || lower.startsWith("/sendafter "))
        {
            if (!wallTimeFn_)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9a\xa0\xef\xb8\x8f NTP not configured. Use /schedulesend <min> instead.")); // ⚠️
                return;
            }
            long wallNow = wallTimeFn_();
            if (wallNow <= 1000000000L)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9a\xa0\xef\xb8\x8f NTP not synced. Use /schedulesend <min> instead.")); // ⚠️
                return;
            }
            String arg = extractArg(u.text, "/sendafter ");
            arg.trim();
            // Parse: first token is HH:MM, rest is "<phone> <body>".
            int sp1 = arg.indexOf(' ');
            if (sp1 <= 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /sendafter <HH:MM> <phone> <body>\n"
                           "Example: /sendafter 14:30 +13800138000 Hello!"));
                return;
            }
            String timeStr = arg.substring(0, sp1);
            String rest = arg.substring(sp1 + 1);
            rest.trim();
            // Parse HH:MM.
            int colon = timeStr.indexOf(':');
            if (colon != 2 || timeStr.length() < 5)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Invalid time format. Use HH:MM (e.g. 14:30).")); // ❌
                return;
            }
            int hh = timeStr.substring(0, 2).toInt();
            int mm = timeStr.substring(3, 5).toInt();
            if (hh < 0 || hh > 23 || mm < 0 || mm > 59)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Invalid time. HH must be 0\xe2\x80\x9323, MM must be 0\xe2\x80\x9359.")); // ❌
                return;
            }
            // Parse phone and body from rest.
            int sp2 = rest.indexOf(' ');
            if (sp2 <= 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /sendafter <HH:MM> <phone> <body>"));
                return;
            }
            String rawPhone2 = rest.substring(0, sp2);
            rawPhone2.trim();
            String sendAfterErr;
            String phone2 = resolvePhone(rawPhone2, aliasStore_, sendAfterErr); // RFC-0224
            if (phone2.length() == 0)
            {
                sendErrorReply(u.chatId, sendAfterErr.length() > 0
                    ? sendAfterErr : String("\xe2\x9d\x8c Invalid phone number.")); // ❌
                return;
            }
            String body2 = rest.substring(sp2 + 1);
            body2.trim();
            if (body2.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /sendafter <HH:MM> <phone> <body>"));
                return;
            }
            if (body2.length() > 1530) // RFC-0261: enforce SMS body cap
            {
                sendErrorReply(u.chatId,
                    String("\xe2\x9d\x8c Message too long for scheduled queue (max 1530 chars).")); // ❌
                return;
            }
            // Compute target Unix timestamp (UTC today at HH:MM:00; if past, tomorrow).
            time_t nowT = (time_t)wallNow;
            struct tm *tmNow = gmtime(&nowT);
            struct tm tmTarget = *tmNow;
            tmTarget.tm_hour = hh;
            tmTarget.tm_min  = mm;
            tmTarget.tm_sec  = 0;
            time_t targetUnix = mktime(&tmTarget); // mktime treats as local; correct for UTC below
            // mktime interprets tm as local time. To get UTC, use timegm or subtract timezone.
            // Portable approach: compute offset between mktime and actual UTC.
            // We know wallNow is UTC epoch. Compute target UTC directly:
            // days-into-epoch for today UTC + HH:MM offset.
            {
                long dayStart = wallNow - (tmNow->tm_hour * 3600L + tmNow->tm_min * 60L + tmNow->tm_sec);
                targetUnix = (time_t)(dayStart + hh * 3600L + mm * 60L);
                if (targetUnix <= (time_t)wallNow)
                    targetUnix += 86400L; // already past → tomorrow
            }
            long delayMs = ((long)targetUnix - wallNow) * 1000L;
            if (delayMs < 60000L) delayMs = 60000L; // minimum 1 min
            // Find a free slot.
            int slotIdx2 = -1;
            uint32_t nowMs5 = clock_ ? clock_() : 0;
            for (int i = 0; i < (int)kScheduledQueueSize; i++)
                if (scheduledQueue_[i].sendAtMs == 0) { slotIdx2 = i; break; }
            if (slotIdx2 < 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Scheduled queue full (5 slots). Use /schedqueue to see pending.")); // ❌
                return;
            }
            uint32_t sendAt2 = nowMs5 + (uint32_t)delayMs;
            scheduledQueue_[slotIdx2].sendAtMs = sendAt2;
            scheduledQueue_[slotIdx2].phone    = phone2;
            scheduledQueue_[slotIdx2].body     = body2;
            if (persistSchedFn_) persistSchedFn_(); // RFC-0200
            String eta2 = schedEtaStr(sendAt2, nowMs5, wallTimeFn_); // RFC-0202
            bot_.sendMessageTo(u.chatId,
                String("\xe2\x8f\xb0 SMS to ") + phone2 // ⏰
                + String(" scheduled ") + eta2
                + String(" (slot ") + String(slotIdx2 + 1) + String("/")
                + String((int)kScheduledQueueSize) + String(").")
                + quietHoursWarning(sendAt2, nowMs5, quietStart_, quietEnd_, wallTimeFn_)); // RFC-0212
            return;
        }

        // RFC-0222: /scheduleat <YYYY-MM-DD HH:MM> <phone> <body> — schedule at a
        // specific UTC date+time. Requires NTP. Max 365 days ahead.
        if (lower == "/scheduleat" || lower.startsWith("/scheduleat "))
        {
            if (!wallTimeFn_)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c /scheduleat requires NTP sync. Use /ntp first.")); // ❌
                return;
            }
            long wallNowAt = wallTimeFn_();
            if (wallNowAt <= 1000000000L)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c NTP not synced. Use /ntp first.")); // ❌
                return;
            }
            // Parse: /scheduleat YYYY-MM-DD HH:MM phone body
            String arg = extractArg(u.text, "/scheduleat ");
            arg.trim();
            // Format: "YYYY-MM-DD HH:MM <phone> <body>"
            // Minimum length: 16 chars for date+time ("2026-04-10 14:30")
            if (arg.length() < 16 || arg[4] != '-' || arg[7] != '-' || arg[13] != ':')
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /scheduleat YYYY-MM-DD HH:MM <phone> <body>\n"
                           "Example: /scheduleat 2026-12-25 09:00 +1234567890 Merry Christmas!"));
                return;
            }
            int yr  = arg.substring(0, 4).toInt();
            int mo  = arg.substring(5, 7).toInt();
            int dy  = arg.substring(8, 10).toInt();
            int hh  = arg.substring(11, 13).toInt();
            int mm2 = arg.substring(14, 16).toInt();
            long targetUnixAt = dateTimeToUnix(yr, mo, dy, hh, mm2);
            if (targetUnixAt < 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Invalid date/time. Check year(2020-2099), month, day, hour, minute.")); // ❌
                return;
            }
            long deltaSecAt = targetUnixAt - wallNowAt;
            if (deltaSecAt < -60L)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c That date/time is in the past.")); // ❌
                return;
            }
            // RFC-0267: cap at 24 days. The millis()-based scheduler uses the
            // RFC-0266 subtraction idiom which is only correct for intervals
            // ≤ 2^31 ms ≈ 24.8 days. 365-day cap also overflows int32 (long on
            // ESP32) when multiplied by 1000 to convert to ms.
            if (deltaSecAt > 24L * 86400L)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Cannot schedule more than 24 days ahead"
                           " (millis() scheduler limit).")); // ❌
                return;
            }
            // Parse phone + body from the rest (after "YYYY-MM-DD HH:MM ").
            String rest = arg.length() > 16 ? arg.substring(16) : String();
            rest.trim();
            int spAt = rest.indexOf(' ');
            if (spAt < 0 || rest.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /scheduleat YYYY-MM-DD HH:MM <phone> <body>"));
                return;
            }
            String schedAtErr;
            String phoneAt = resolvePhone(rest.substring(0, spAt), aliasStore_, schedAtErr); // RFC-0224
            String bodyAt  = rest.substring(spAt + 1);
            bodyAt.trim();
            if (phoneAt.length() < 5 || bodyAt.length() == 0)
            {
                sendErrorReply(u.chatId, schedAtErr.length() > 0
                    ? schedAtErr : String("Usage: /scheduleat YYYY-MM-DD HH:MM <phone> <body>"));
                return;
            }
            if (bodyAt.length() > 1530) // RFC-0261: enforce SMS body cap
            {
                sendErrorReply(u.chatId,
                    String("\xe2\x9d\x8c Message too long for scheduled queue (max 1530 chars).")); // ❌
                return;
            }
            // Find a free slot.
            int slotAt = -1;
            uint32_t nowMsAt = clock_ ? clock_() : 0;
            for (int i = 0; i < (int)kScheduledQueueSize; i++)
                if (scheduledQueue_[i].sendAtMs == 0) { slotAt = i; break; }
            if (slotAt < 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Scheduled queue full (5 slots). Use /schedqueue to see pending.")); // ❌
                return;
            }
            long delayMsAt = deltaSecAt > 0L ? deltaSecAt * 1000L : 1L; // 1ms = fire immediately
            uint32_t sendAtAt = nowMsAt + (uint32_t)delayMsAt;
            scheduledQueue_[slotAt].sendAtMs = sendAtAt;
            scheduledQueue_[slotAt].phone    = phoneAt;
            scheduledQueue_[slotAt].body     = bodyAt;
            if (persistSchedFn_) persistSchedFn_(); // RFC-0200
            String etaAt = schedEtaStr(sendAtAt, nowMsAt, wallTimeFn_); // RFC-0202
            bot_.sendMessageTo(u.chatId,
                String("\xe2\x8f\xb0 SMS to ") + phoneAt // ⏰
                + String(" scheduled ") + etaAt
                + String(" (slot ") + String(slotAt + 1) + String("/")
                + String((int)kScheduledQueueSize) + String(").")
                + quietHoursWarning(sendAtAt, nowMsAt, quietStart_, quietEnd_, wallTimeFn_)); // RFC-0212
            return;
        }

        // RFC-0188: /cancelsched <N> — cancel a scheduled SMS slot (1-based).
        if (lower == "/cancelsched" || lower.startsWith("/cancelsched "))
        {
            String arg = extractArg(lower, "/cancelsched ");
            if (arg.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /cancelsched <slot>\nUse /schedqueue to see slot numbers."));
                return;
            }
            int n = (int)arg.toInt();
            if (n < 1 || n > (int)kScheduledQueueSize || scheduledQueue_[n - 1].sendAtMs == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Slot ") + String(n) // ❌
                    + String(" is empty or out of range (1\xe2\x80\x93 ")
                    + String((int)kScheduledQueueSize) + String(")."));
                return;
            }
            scheduledQueue_[n - 1].sendAtMs = 0;
            scheduledQueue_[n - 1].phone    = String();
            scheduledQueue_[n - 1].body     = String();
            if (persistSchedFn_) persistSchedFn_(); // RFC-0200
            bot_.sendMessageTo(u.chatId,
                String("\xe2\x9c\x85 Slot ") + String(n) + String(" cancelled.")); // ✅
            return;
        }

        // RFC-0198: /schedinfo <N> — show full content of a scheduled slot.
        if (lower == "/schedinfo" || lower.startsWith("/schedinfo "))
        {
            String arg = extractArg(lower, "/schedinfo ");
            arg.trim();
            if (arg.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /schedinfo <slot>\nUse /schedqueue to see slot numbers."));
                return;
            }
            int n = (int)arg.toInt();
            if (n < 1 || n > (int)kScheduledQueueSize || scheduledQueue_[n - 1].sendAtMs == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Slot ") + String(n) // ❌
                    + String(" is empty or out of range (1\xe2\x80\x93 ")
                    + String((int)kScheduledQueueSize) + String(")."));
                return;
            }
            uint32_t nowMs4 = clock_ ? clock_() : 0;
            String eta = schedEtaStr(scheduledQueue_[n - 1].sendAtMs, nowMs4, wallTimeFn_); // RFC-0202
            String reply = String("\xe2\x8f\xb0 Slot ") + String(n) + String(":\n") // ⏰
                         + String("To: ") + scheduledQueue_[n - 1].phone + String("\n")
                         + String("ETA: ") + eta + String("\n");
            if (scheduledQueue_[n - 1].repeatIntervalMs > 0) // RFC-0221
            {
                uint32_t rmin = scheduledQueue_[n - 1].repeatIntervalMs / 60000U;
                reply += String("Repeat: every ") + String(rmin) + String("m\n"); // 🔁
            }
            reply += String("Body: ") + scheduledQueue_[n - 1].body;
            bot_.sendMessageTo(u.chatId, reply);
            return;
        }

        // RFC-0197: /schedrename <N> <phone> — change destination of a scheduled slot.
        if (lower == "/schedrename" || lower.startsWith("/schedrename "))
        {
            String arg = extractArg(u.text, "/schedrename ");
            arg.trim();
            int sp = arg.indexOf(' ');
            if (sp <= 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /schedrename <slot> <phone>\n"
                           "Example: /schedrename 1 +447911123456"));
                return;
            }
            int n = (int)arg.substring(0, sp).toInt();
            String rawPhone = arg.substring(sp + 1);
            rawPhone.trim();
            String renameErr;
            String newPhone = resolvePhone(rawPhone, aliasStore_, renameErr); // RFC-0224
            if (n < 1 || n > (int)kScheduledQueueSize || scheduledQueue_[n - 1].sendAtMs == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Slot ") + String(n) // ❌
                    + String(" is empty or out of range."));
                return;
            }
            if (newPhone.length() == 0)
            {
                sendErrorReply(u.chatId, renameErr.length() > 0
                    ? renameErr : String("\xe2\x9d\x8c Phone number cannot be empty.")); // ❌
                return;
            }
            scheduledQueue_[n - 1].phone = newPhone;
            if (persistSchedFn_) persistSchedFn_(); // RFC-0200
            bot_.sendMessageTo(u.chatId,
                String("\xe2\x9c\x85 Slot ") + String(n) // ✅
                + String(" phone changed to ") + newPhone + String("."));
            return;
        }

        // RFC-0207: /schedbody <N> <new_body> — edit the body of a scheduled slot.
        if (lower == "/schedbody" || lower.startsWith("/schedbody "))
        {
            String arg = extractArg(u.text, "/schedbody ");
            arg.trim();
            int sp = arg.indexOf(' ');
            if (sp <= 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /schedbody <slot> <text>\n"
                           "Example: /schedbody 1 Updated message body"));
                return;
            }
            int n = (int)arg.substring(0, sp).toInt();
            String newBody = arg.substring(sp + 1);
            newBody.trim();
            if (n < 1 || n > (int)kScheduledQueueSize || scheduledQueue_[n - 1].sendAtMs == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Slot ") + String(n) // ❌
                    + String(" is empty or out of range."));
                return;
            }
            if (newBody.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Body cannot be empty.")); // ❌
                return;
            }
            if (newBody.length() > 1530) // RFC-0261: enforce SMS body cap
            {
                sendErrorReply(u.chatId,
                    String("\xe2\x9d\x8c Message too long for scheduled queue (max 1530 chars).")); // ❌
                return;
            }
            scheduledQueue_[n - 1].body = newBody;
            if (persistSchedFn_) persistSchedFn_(); // RFC-0200
            String preview = newBody;
            if (preview.length() > 40) preview = preview.substring(0, 40) + String("...");
            bot_.sendMessageTo(u.chatId,
                String("\xe2\x9c\x85 Slot ") + String(n) // ✅
                + String(" body updated: ") + preview);
            return;
        }

        // RFC-0215: /schedclone <N> <delay_min> — duplicate a scheduled slot.
        if (lower == "/schedclone" || lower.startsWith("/schedclone "))
        {
            String arg = extractArg(u.text, "/schedclone ");
            arg.trim();
            int sp = arg.indexOf(' ');
            if (sp <= 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /schedclone <slot> <delay_min>\n"
                           "Example: /schedclone 1 60"));
                return;
            }
            int srcN = (int)arg.substring(0, sp).toInt();
            long delayMin2 = arg.substring(sp + 1).toInt();
            if (srcN < 1 || srcN > (int)kScheduledQueueSize
                || scheduledQueue_[srcN - 1].sendAtMs == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Slot ") + String(srcN) // ❌
                    + String(" is empty or out of range."));
                return;
            }
            if (delayMin2 < 1 || delayMin2 > 1440)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Delay must be 1\xe2\x80\x931440 minutes.")); // ❌ –
                return;
            }
            // Find a free slot.
            int dstIdx = -1;
            for (int i = 0; i < (int)kScheduledQueueSize; i++)
                if (scheduledQueue_[i].sendAtMs == 0) { dstIdx = i; break; }
            if (dstIdx < 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Scheduled queue full (5 slots).")); // ❌
                return;
            }
            uint32_t nowMsClone = clock_ ? clock_() : 0;
            uint32_t sendAtClone = nowMsClone + (uint32_t)delayMin2 * 60000UL;
            scheduledQueue_[dstIdx].sendAtMs = sendAtClone;
            scheduledQueue_[dstIdx].phone    = scheduledQueue_[srcN - 1].phone;
            scheduledQueue_[dstIdx].body     = scheduledQueue_[srcN - 1].body;
            if (persistSchedFn_) persistSchedFn_(); // RFC-0200
            String eta = schedEtaStr(sendAtClone, nowMsClone, wallTimeFn_); // RFC-0202
            String reply = String("\xe2\x9c\x85 Slot ") + String(srcN) // ✅
                         + String(" cloned \xe2\x86\x92 slot ") + String(dstIdx + 1) // →
                         + String(" (fires ") + eta + String(")")
                         + quietHoursWarning(sendAtClone, nowMsClone, quietStart_, quietEnd_, wallTimeFn_); // RFC-0212
            bot_.sendMessageTo(u.chatId, reply);
            return;
        }

        // RFC-0221: /schedrepeat <N> <min> — make slot N repeat every <min> minutes.
        // /schedrepeat <N> 0 converts the slot back to one-shot.
        if (lower == "/schedrepeat" || lower.startsWith("/schedrepeat "))
        {
            String arg = extractArg(lower, "/schedrepeat ");
            int sp = arg.indexOf(' ');
            if (sp < 0 || arg.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /schedrepeat <slot> <min>\n"
                           "Example: /schedrepeat 1 60\n"
                           "/schedrepeat <slot> 0 converts to one-shot."));
                return;
            }
            int n = (int)arg.substring(0, sp).toInt();
            long rmin = arg.substring(sp + 1).toInt();
            if (n < 1 || n > (int)kScheduledQueueSize || scheduledQueue_[n - 1].sendAtMs == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Slot ") + String(n) // ❌
                    + String(" is empty or out of range."));
                return;
            }
            if (rmin < 0 || rmin > 10080)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Interval must be 0\xe2\x80\x93 10080 min (7 days).")); // ❌ –
                return;
            }
            scheduledQueue_[n - 1].repeatIntervalMs = (uint32_t)rmin * 60000UL;
            if (persistSchedFn_) persistSchedFn_(); // RFC-0200
            if (rmin == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9c\x85 Slot ") + String(n) // ✅
                    + String(" is now one-shot (no repeat)."));
            }
            else
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xf0\x9f\x94\x81 Slot ") + String(n) // 🔁
                    + String(" will repeat every ") + String(rmin) + String("m after each send."));
            }
            return;
        }

        // RFC-0223: /recurring <interval_min> <phone> <body> — create a repeating
        // scheduled SMS slot in one step (combines /schedulesend + /schedrepeat).
        if (lower == "/recurring" || lower.startsWith("/recurring "))
        {
            String arg = extractArg(u.text, "/recurring ");
            arg.trim();
            int sp1 = arg.indexOf(' ');
            if (sp1 <= 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /recurring <interval_min> <phone> <body>\n"
                           "Example: /recurring 1440 +13800138000 Daily check-in"));
                return;
            }
            long rIntervalMin = arg.substring(0, sp1).toInt();
            if (rIntervalMin < 1 || rIntervalMin > 10080)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Interval must be 1\xe2\x80\x93 10080 min (7 days).")); // ❌ –
                return;
            }
            String rest2 = arg.substring(sp1 + 1);
            rest2.trim();
            int sp2 = rest2.indexOf(' ');
            if (sp2 <= 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /recurring <interval_min> <phone> <body>"));
                return;
            }
            String recurErr;
            String rPhone = resolvePhone(rest2.substring(0, sp2), aliasStore_, recurErr); // RFC-0224
            String rBody  = rest2.substring(sp2 + 1);
            rBody.trim();
            if (rPhone.length() < 5 || rBody.length() == 0)
            {
                sendErrorReply(u.chatId, recurErr.length() > 0
                    ? recurErr : String("Usage: /recurring <interval_min> <phone> <body>"));
                return;
            }
            if (rBody.length() > 1530) // RFC-0261: enforce SMS body cap
            {
                sendErrorReply(u.chatId,
                    String("\xe2\x9d\x8c Message too long for scheduled queue (max 1530 chars).")); // ❌
                return;
            }
            // Find a free slot.
            int rSlot = -1;
            uint32_t rNowMs = clock_ ? clock_() : 0;
            for (int i = 0; i < (int)kScheduledQueueSize; i++)
                if (scheduledQueue_[i].sendAtMs == 0) { rSlot = i; break; }
            if (rSlot < 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Scheduled queue full (5 slots). Use /schedqueue to see pending.")); // ❌
                return;
            }
            uint32_t rIntervalMs = (uint32_t)rIntervalMin * 60000UL;
            scheduledQueue_[rSlot].sendAtMs         = rNowMs + rIntervalMs;
            scheduledQueue_[rSlot].phone            = rPhone;
            scheduledQueue_[rSlot].body             = rBody;
            scheduledQueue_[rSlot].repeatIntervalMs = rIntervalMs; // RFC-0221
            if (persistSchedFn_) persistSchedFn_(); // RFC-0200
            bot_.sendMessageTo(u.chatId,
                String("\xf0\x9f\x94\x81 Recurring SMS to ") + rPhone // 🔁
                + String(" every ") + String(rIntervalMin) + String("m"
                  " (slot ") + String(rSlot + 1) + String("/")
                + String((int)kScheduledQueueSize) + String(")."
                  " First send in ") + String(rIntervalMin) + String("m."));
            return;
        }

        // RFC-0218: /schedpause — globally pause scheduled SMS delivery.
        if (lower == "/schedpause")
        {
            if (schedPaused_)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x8f\xb8 Already paused. Use /schedresume to resume.")); // ⏸
            }
            else
            {
                schedPaused_ = true;
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x8f\xb8 Scheduled SMS delivery paused. Use /schedresume to resume.")); // ⏸
            }
            return;
        }

        // RFC-0218: /schedresume — resume scheduled SMS delivery.
        if (lower == "/schedresume")
        {
            if (!schedPaused_)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x96\xb6\xef\xb8\x8f Delivery not paused.")); // ▶️
            }
            else
            {
                schedPaused_ = false;
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x96\xb6\xef\xb8\x8f Scheduled SMS delivery resumed.")); // ▶️
            }
            return;
        }

        // RFC-0219: /snooze <phone> <min> — suppress forwarding from a number.
        if (lower == "/snooze" || lower.startsWith("/snooze "))
        {
            String arg = extractArg(u.text, "/snooze ");
            arg.trim();
            int sp = arg.indexOf(' ');
            if (sp <= 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /snooze <phone> <minutes>\n"
                           "Example: /snooze +13800138000 60"));
                return;
            }
            String rawPhone = arg.substring(0, sp);
            String phone = sms_codec::normalizePhoneNumber(rawPhone);
            long mins = arg.substring(sp + 1).toInt();
            if (phone.length() == 0)
            {
                sendErrorReply(u.chatId, String("Invalid phone number."));
                return;
            }
            if (mins < 1 || mins > 480)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Minutes must be 1\xe2\x80\x93480.")); // ❌ –
                return;
            }
            // Check cap (allow update of existing entry without counting against cap).
            bool existing = false;
            for (const auto &kv : snoozeList_)
                if (kv.first == phone) { existing = true; break; }
            if (!existing && (int)snoozeList_.size() >= kMaxSnoozes)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Snooze limit reached (20)."));  // ❌
                return;
            }
            uint32_t nowMs = clock_ ? clock_() : 0;
            uint32_t expiry = nowMs + (uint32_t)mins * 60000U;
            bool updated = false;
            for (auto &kv : snoozeList_)
                if (kv.first == phone) { kv.second = expiry; updated = true; break; }
            if (!updated)
                snoozeList_.push_back({phone, expiry});
            bot_.sendMessageTo(u.chatId,
                String("\xf0\x9f\x94\x87 ") + phone // 🔇
                + String(" snoozed for ") + String((int)mins) + String(" min."));
            return;
        }

        // RFC-0219: /unsnooze <phone> — remove a snooze.
        if (lower == "/unsnooze" || lower.startsWith("/unsnooze "))
        {
            String arg = extractArg(u.text, "/unsnooze ");
            arg.trim();
            if (arg.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /unsnooze <phone>"));
                return;
            }
            String phone = sms_codec::normalizePhoneNumber(arg);
            bool found = false;
            for (auto it = snoozeList_.begin(); it != snoozeList_.end(); ++it)
            {
                if (it->first == phone) { snoozeList_.erase(it); found = true; break; }
            }
            if (!found)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c ") + phone + String(" not in snooze list.")); // ❌
                return;
            }
            bot_.sendMessageTo(u.chatId,
                String("\xe2\x9c\x85 ") + phone + String(" unsnoozed.")); // ✅
            return;
        }

        // RFC-0219: /snoozelist — show active snoozes.
        if (lower == "/snoozelist")
        {
            uint32_t nowMs = clock_ ? clock_() : 0;
            // Reap expired entries first.
            for (auto it = snoozeList_.begin(); it != snoozeList_.end(); )
            {
                if ((uint32_t)(nowMs - it->second) < 0x80000000UL) // RFC-0269
                    it = snoozeList_.erase(it);
                else
                    ++it;
            }
            if (snoozeList_.empty())
            {
                bot_.sendMessageTo(u.chatId, String("(no active snoozes)"));
                return;
            }
            String out = String("\xf0\x9f\x94\x87 Active snoozes:\n"); // 🔇
            for (const auto &kv : snoozeList_)
            {
                uint32_t remSec = (kv.second - nowMs) / 1000U;
                if (remSec < 60)
                    out += kv.first + String(": ") + String((int)remSec) + String("s\n");
                else
                    out += kv.first + String(": ") + String((int)(remSec / 60)) + String("m\n");
            }
            bot_.sendMessageTo(u.chatId, out);
            return;
        }

        // RFC-0196: /scheddelay <N> <extra_min> — extend a scheduled SMS deadline.
        if (lower == "/scheddelay" || lower.startsWith("/scheddelay "))
        {
            String arg = extractArg(u.text, "/scheddelay ");
            arg.trim();
            int sp = arg.indexOf(' ');
            if (sp <= 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /scheddelay <slot> <extra_min>\n"
                           "Example: /scheddelay 1 15"));
                return;
            }
            int n = (int)arg.substring(0, sp).toInt();
            long extra = arg.substring(sp + 1).toInt();
            if (n < 1 || n > (int)kScheduledQueueSize || scheduledQueue_[n - 1].sendAtMs == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Slot ") + String(n) // ❌
                    + String(" is empty or out of range."));
                return;
            }
            if (extra < 1 || extra > 1440)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c extra_min must be 1\xe2\x80\x93 1440.")); // ❌ –
                return;
            }
            scheduledQueue_[n - 1].sendAtMs += (uint32_t)extra * 60000UL;
            if (persistSchedFn_) persistSchedFn_(); // RFC-0200
            bot_.sendMessageTo(u.chatId,
                String("\xe2\x9c\x85 Slot ") + String(n) // ✅
                + String(" extended by ") + String((int)extra) + String(" min."));
            return;
        }

        // RFC-0195: /clearschedule — cancel all pending scheduled SMS at once.
        if (lower == "/clearschedule")
        {
            int cleared = 0;
            for (auto &slot : scheduledQueue_)
            {
                if (slot.sendAtMs != 0)
                {
                    slot.sendAtMs = 0;
                    slot.phone    = String();
                    slot.body     = String();
                    cleared++;
                }
            }
            if (cleared > 0 && persistSchedFn_) persistSchedFn_(); // RFC-0200
            if (cleared == 0)
                bot_.sendMessageTo(u.chatId, String("(no scheduled SMS)"));
            else
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9c\x85 Cleared ") // ✅
                    + String(cleared)
                    + String(cleared == 1 ? " scheduled SMS." : " scheduled SMS."));
            return;
        }

        // RFC-0193: /sendnow — immediately fire all scheduled SMS.
        if (lower == "/sendnow")
        {
            int fired = 0;
            uint32_t nowMs3 = clock_ ? clock_() : 0;
            for (auto &slot : scheduledQueue_)
            {
                if (slot.sendAtMs != 0)
                {
                    slot.sendAtMs = nowMs3; // fire on next tick
                    fired++;
                }
            }
            if (fired > 0 && persistSchedFn_) persistSchedFn_(); // RFC-0200
            if (fired == 0)
                bot_.sendMessageTo(u.chatId, String("(no scheduled SMS to send)"));
            else
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9c\x85 Triggering ") // ✅
                    + String(fired)
                    + String(fired == 1 ? " scheduled SMS." : " scheduled SMS."));
            return;
        }

        // RFC-0204: /delayall <min> — extend all scheduled slots at once.
        if (lower == "/delayall" || lower.startsWith("/delayall "))
        {
            String arg = extractArg(u.text, "/delayall ");
            arg.trim();
            if (arg.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /delayall <minutes>\n"
                           "Extends ALL pending scheduled SMS by extra minutes (1\xe2\x80\x931440).\n" // –
                           "Example: /delayall 30"));
                return;
            }
            long extra = arg.toInt();
            if (extra < 1 || extra > 1440)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Minutes must be 1\xe2\x80\x931440.")); // ❌ –
                return;
            }
            uint32_t extraMs = (uint32_t)extra * 60000UL;
            int extended = 0;
            for (int i = 0; i < (int)kScheduledQueueSize; i++)
            {
                if (scheduledQueue_[i].sendAtMs == 0) continue;
                // Cap at UINT32_MAX to avoid overflow.
                uint32_t remaining = 0xFFFFFFFFUL - scheduledQueue_[i].sendAtMs;
                if (extraMs > remaining)
                    scheduledQueue_[i].sendAtMs = 0xFFFFFFFFUL;
                else
                    scheduledQueue_[i].sendAtMs += extraMs;
                extended++;
            }
            if (extended > 0 && persistSchedFn_) persistSchedFn_(); // RFC-0200
            if (extended == 0)
                bot_.sendMessageTo(u.chatId, String("(no scheduled SMS to delay)"));
            else
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x8f\xb0 Extended ") + String(extended) // ⏰
                    + String(extended == 1 ? " slot by " : " slot(s) by ")
                    + String((int)extra) + String(" min."));
            return;
        }

        // RFC-0192: /pausefwd <minutes> — temporarily pause SMS forwarding.
        if (lower == "/pausefwd" || lower.startsWith("/pausefwd "))
        {
            if (!pauseFwdFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(pausefwd not configured)"));
                return;
            }
            String arg = extractArg(u.text, "/pausefwd ");
            arg.trim();
            if (arg.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /pausefwd <minutes>\n"
                           "Range: 1\xe2\x80\x93 1440. Forwarding auto-resumes after the period.\n" // –
                           "Example: /pausefwd 30"));
                return;
            }
            long mins = arg.toInt();
            if (mins < 1 || mins > 1440)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Minutes must be 1\xe2\x80\x93 1440.")); // ❌ –
                return;
            }
            uint32_t durMs = (uint32_t)mins * 60000UL;
            String reply = pauseFwdFn_(durMs);
            bot_.sendMessageTo(u.chatId,
                String("\xe2\x8f\xb8 ") + reply); // ⏸
            return;
        }

        // RFC-0191: /testpdu <hex> — decode a raw PDU hex string for debugging.
        if (lower == "/testpdu" || lower.startsWith("/testpdu "))
        {
            if (!testPduFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(testpdu not configured)"));
                return;
            }
            String arg = extractArg(u.text, "/testpdu ");
            arg.trim();
            if (arg.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /testpdu <hex>\n"
                           "Paste a raw SMS-DELIVER PDU hex string to decode it."));
                return;
            }
            bot_.sendMessageTo(u.chatId, testPduFn_(arg));
            return;
        }

        // RFC-0190: /setsmsagefilter <hours> — skip forwarding SMS older than N hours.
        if (lower == "/setsmsagefilter" || lower.startsWith("/setsmsagefilter "))
        {
            if (!smsAgeFilterFn_)
            {
                bot_.sendMessageTo(u.chatId, String("(smsagefilter not configured)"));
                return;
            }
            String arg = extractArg(u.text, "/setsmsagefilter ");
            arg.trim();
            if (arg.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /setsmsagefilter <hours>\n"
                           "0 = disable (forward all SMS). Max 8760 (1 year).\n"
                           "Example: /setsmsagefilter 24"));
                return;
            }
            long h = arg.toInt();
            if (h < 0 || h > 8760)
            {
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9d\x8c Hours must be 0\xe2\x80\x93 8760.")); // ❌
                return;
            }
            smsAgeFilterFn_((int)h);
            if (h == 0)
                bot_.sendMessageTo(u.chatId, String("\xe2\x9c\x85 SMS age filter disabled.")); // ✅
            else
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9c\x85 SMS age filter set to ") // ✅
                    + String((int)h) + String("h."));
            return;
        }

        // RFC-0211: /setquiethours <start>-<end> — configure UTC quiet window.
        if (lower == "/setquiethours" || lower.startsWith("/setquiethours "))
        {
            String arg = extractArg(u.text, "/setquiethours ");
            arg.trim();
            int dash = arg.indexOf('-');
            bool ok = false;
            int qs = -1, qe = -1;
            if (dash > 0)
            {
                String startStr = arg.substring(0, dash);
                String endStr   = arg.substring(dash + 1);
                startStr.trim(); endStr.trim();
                qs = (int)startStr.toInt();
                qe = (int)endStr.toInt();
                if (qs >= 0 && qs <= 23 && qe >= 0 && qe <= 23 && qs != qe)
                    ok = true;
            }
            if (!ok || arg.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /setquiethours <start>-<end> (UTC hours 0-23)\n"
                           "Example: /setquiethours 22-8\n"
                           "Scheduled SMS will be deferred until the window ends."));
                return;
            }
            setQuietHours(qs, qe);
            if (persistQuietFn_) persistQuietFn_(); // RFC-0211
            char buf[32];
            snprintf(buf, sizeof(buf), "\xe2\x9c\x85 Quiet hours set: %02d:00\xe2\x80\x93%02d:00 UTC.", qs, qe); // ✅ –
            bot_.sendMessageTo(u.chatId, String(buf));
            return;
        }

        // RFC-0211: /clearquiethours — disable quiet hours.
        if (lower == "/clearquiethours")
        {
            if (quietStart_ < 0)
            {
                bot_.sendMessageTo(u.chatId, String("Quiet hours not set."));
            }
            else
            {
                clearQuietHours();
                if (persistQuietFn_) persistQuietFn_(); // RFC-0211
                bot_.sendMessageTo(u.chatId,
                    String("\xe2\x9c\x85 Quiet hours cleared. Scheduled SMS will fire normally.")); // ✅
            }
            return;
        }

        // RFC-0211: /quiethours — show current quiet hours setting.
        if (lower == "/quiethours")
        {
            if (quietStart_ < 0)
            {
                bot_.sendMessageTo(u.chatId, String("Quiet hours: disabled."));
            }
            else
            {
                char buf[64];
                bool nowQuiet = isInQuietHours(quietStart_, quietEnd_, wallTimeFn_);
                snprintf(buf, sizeof(buf),
                    "\xf0\x9f\x94\x95 Quiet hours: %02d:00\xe2\x80\x93%02d:00 UTC%s.", // 🔕 –
                    quietStart_, quietEnd_,
                    nowQuiet ? " (currently active)" : "");
                bot_.sendMessageTo(u.chatId, String(buf));
            }
            return;
        }

        // RFC-0209: /pending — terse summary of all pending work items.
        if (lower == "/pending")
        {
            String reply;
            if (pendingFn_)
            {
                reply = pendingFn_();
            }
            else
            {
                // Inline fallback: queue + sched only (no concat count without fn).
                int q = smsSender_.queueSize();
                int s = 0;
                for (const auto &slot : scheduledQueue_)
                    if (slot.sendAtMs != 0) s++;
                if (q == 0 && s == 0)
                {
                    reply = String("\xf0\x9f\x93\x8b All clear (nothing pending)"); // 📋
                }
                else
                {
                    reply = String("\xf0\x9f\x93\x8b Queue: ") // 📋
                           + String(q) + String("/") + String(SmsSender::kQueueSize)
                           + String(" | Sched: ") + String(s) + String("/")
                           + String((int)kScheduledQueueSize);
                }
            }
            bot_.sendMessageTo(u.chatId, reply);
            return;
        }

        Serial.println("TelegramPoller: no reply_to_message_id, dropping");
        {
            String help = "Reply to a forwarded SMS to send a response. ";
            help += "Commands: /help, /ping, /ntp, /debug, /cleardebug, /status, /restart, /send <num> <msg>, /queue, /cancel <N>";
            if (smsBlockMutator_)
                help += ", /blocklist, /block <num>, /unblock <num>";
            sendErrorReply(u.chatId, help);
        }
        return;
    }

    String phone;
    if (!replyTargets_.lookup(u.replyToMessageId, phone))
    {
        Serial.print("TelegramPoller: reply target slot stale or missing for msg_id=");
        Serial.println(u.replyToMessageId);
        sendErrorReply(u.chatId,
                       String("Reply target expired (the original SMS is too ") +
                       "old; only the last " + String((int)ReplyTargetMap::kSlotCount) +
                       " forwards are routable).");
        return;
    }

    // 3. Enqueue via SmsSender (RFC-0012). Delivery is decoupled: the
    // message is accepted here and sent (with exponential-backoff retry)
    // by SmsSender::drainQueue() in the next loop() iteration.
    // On final failure after kMaxAttempts retries, the onFinalFailure
    // lambda fires from inside drainQueue() on the loop() task — safe
    // to call bot_.sendMessage() from it (same thread as here).
    // NOTE: 'this' capture is safe — TelegramPoller and SmsSender are
    // both process-lifetime objects in main.cpp (RFC-0012 §3).
    if (u.text.length() == 0)
    {
        sendErrorReply(u.chatId, String("Empty reply body — nothing to send."));
        return;
    }
    // RFC-0050: Reject immediately if body exceeds 10-part limit (same
    // check as in the /send path — RFC-0049).
    if (sms_codec::countSmsParts(u.text) == 0)
    {
        sendErrorReply(u.chatId,
            String("Reply too long (max ~1530 GSM-7 / ~670 Unicode chars)."));
        return;
    }

    String capturedPhone = phone; // copy for lambda capture
    int64_t requesterChatId = u.chatId; // copy for lambda capture (u is a local, lambda may fire later)
    smsSender_.enqueue(phone, u.text,
        [this, capturedPhone, requesterChatId]() {
            sendErrorReply(requesterChatId, String("SMS to ") + capturedPhone + " failed after retries.");
        },
        [this, capturedPhone, requesterChatId]() {
            // RFC-0032 / RFC-0054: delivery confirmation. Store the
            // message_id so replying to it also routes to capturedPhone.
            int32_t delivId = bot_.sendMessageToReturningId(requesterChatId,
                String("\xF0\x9F\x93\xA8 Sent to ") + capturedPhone); // U+1F4E8 envelope
            if (delivId > 0)
                replyTargets_.put(delivId, capturedPhone);
        });

    // 4. Confirm enqueueing immediately (delivery confirmation comes
    // asynchronously via the queue; we tell the user the reply is queued).
    bot_.sendMessageTo(u.chatId, String("\xE2\x9C\x85 Queued reply to ") + phone); // U+2705 check mark
    Serial.print("TelegramPoller: SMS reply queued to ");
    Serial.println(phone);
}

void TelegramPoller::tick()
{
    uint32_t now = clock_ ? clock_() : 0;

    // RFC-0188: Drain scheduled SMS whose sendAtMs has passed.
    bool schedFired = false;
    // RFC-0211: suppress all slot firing during quiet hours.
    // RFC-0218: suppress during manual pause.
    bool inQuiet = isInQuietHours(quietStart_, quietEnd_, wallTimeFn_) || schedPaused_;
    for (auto &slot : scheduledQueue_)
    {
        // RFC-0266: use wraparound-safe subtraction instead of >=.
        // now >= sendAtMs is wrong when sendAtMs overflows uint32_t (e.g.
        // a 7-day recurring slot scheduled near the ~49.7-day millis() rollover).
        // (uint32_t)(now - sendAtMs) < 0x80000000UL is correct for any
        // interval ≤ 24.8 days (half the uint32_t range).
        if (slot.sendAtMs != 0 && (uint32_t)(now - slot.sendAtMs) < 0x80000000UL && !inQuiet)
        {
            // Try to enqueue. SmsSender::enqueue returns false only if the
            // retry queue is full — leave the slot and try again next tick.
            if (smsSender_.enqueue(slot.phone, slot.body))
            {
                // RFC-0203: Notify the admin chat that the scheduled SMS was sent.
                String preview = slot.body;
                if (preview.length() > 60) preview = preview.substring(0, 60) + String("...");
                // RFC-0240: kick WDT — multiple overdue slots may fire in one
                // tick; each bot_.sendMessage() can block up to ~12 s.
#ifdef ESP_PLATFORM
                esp_task_wdt_reset();
#endif
                bot_.sendMessage(String("\xe2\x9c\x85 Scheduled SMS to ") // ✅
                                 + slot.phone + String(" sent: ") + preview);
                // RFC-0221: re-arm repeating slots instead of clearing them.
                if (slot.repeatIntervalMs > 0)
                    slot.sendAtMs = now + slot.repeatIntervalMs;
                else
                    slot.sendAtMs = 0; // free the one-shot slot
                schedFired = true;
            }
        }
    }
    if (schedFired && persistSchedFn_) persistSchedFn_(); // RFC-0200

    // RFC-0217: Stuck outbound-queue alert. Scan for entries that have been
    // waiting longer than kQueueStuckThresholdMs since their first drain
    // attempt (queuedAtMs > 0). Fire at most once per kQueueStuckAlertCooldownMs.
    // Reset the cooldown when the queue fully drains.
    {
        int qsz = smsSender_.queueSize();
        if (qsz == 0)
        {
            if (queueWasNonEmpty_)
            {
                lastQueueStuckAlertMs_ = 0; // reset so next stuck event alerts immediately
                queueWasNonEmpty_ = false;
            }
        }
        else
        {
            queueWasNonEmpty_ = true;
            bool cooldownOk = (lastQueueStuckAlertMs_ == 0)
                || ((now - lastQueueStuckAlertMs_) >= kQueueStuckAlertCooldownMs);
            if (cooldownOk)
            {
                auto snap = smsSender_.getQueueSnapshot();
                int stuckCount = 0;
                uint32_t oldestAgeMs = 0;
                String oldestPhone;
                for (const auto &e : snap)
                {
                    if (e.queuedAtMs > 0 && (now - e.queuedAtMs) >= kQueueStuckThresholdMs)
                    {
                        stuckCount++;
                        uint32_t age = now - e.queuedAtMs;
                        if (age > oldestAgeMs) { oldestAgeMs = age; oldestPhone = e.phone; }
                    }
                }
                if (stuckCount > 0)
                {
                    lastQueueStuckAlertMs_ = now;
                    uint32_t oldestMin = oldestAgeMs / 60000U;
                    String alert = String("\xe2\x9a\xa0\xef\xb8\x8f Outbound queue stuck: ") // ⚠️
                                 + String(stuckCount)
                                 + String(stuckCount == 1 ? " entry" : " entries")
                                 + String(" waiting >30min.\n")
                                 + String("Oldest: ") + oldestPhone
                                 + String(" (") + String((int)oldestMin) + String(" min).")
                                 + String(" Use /queueinfo to inspect.");
                    bot_.sendMessage(alert);
                }
            }
        }
    }

    // RFC-0219: Lazily reap expired snooze entries in tick().
    if (!snoozeList_.empty())
    {
        for (auto it = snoozeList_.begin(); it != snoozeList_.end(); )
        {
            if ((uint32_t)(now - it->second) < 0x80000000UL) // RFC-0269
                it = snoozeList_.erase(it);
            else
                ++it;
        }
    }

    // Rate-limit the actual poll. First call always polls so a fresh
    // boot picks up any queued replies immediately.
    if (firstPollDone_)
    {
        if (now - lastPollMs_ < pollIntervalMs_)
        {
            return;
        }
    }
    lastPollMs_ = now;
    firstPollDone_ = true;

    pollAttempts_++;

    std::vector<TelegramUpdate> updates;
    if (!bot_.pollUpdates(lastUpdateId_, kPollTimeoutSec, updates))
    {
        // Transport-level failure. Don't advance — we'll retry next tick.
        // The CallHandler / SmsHandler reboot policy already covers the
        // case where the network has been down for too long.
        return;
    }

    // RFC-0208: Notify on each successful TCP/TLS contact with Telegram.
    if (onPollSuccessFn_) onPollSuccessFn_();

    if (updates.empty())
    {
        return;
    }

    // Track the highest update_id we've seen so we can persist once
    // at the end rather than after every individual update.
    int32_t highest = lastUpdateId_;
    for (const TelegramUpdate &u : updates)
    {
        if (u.updateId <= 0)
        {
            continue;
        }
        // Only act on each update once. The bot API guarantees that
        // we won't see the same update_id again as long as we
        // advance the offset, but be defensive.
        if (u.updateId <= lastUpdateId_)
        {
            continue;
        }
        // RFC-0240: Kick the WDT before each update. processUpdate() may
        // call doSendMessage() (up to ~12 s) plus bot_.sendMessage()
        // for the reply/error ACK. With limit=10 updates per batch,
        // worst-case batch time is 10×24 s = 240 s > WDT 120 s.
#ifdef ESP_PLATFORM
        esp_task_wdt_reset();
#endif
        processUpdate(u);
        if (u.updateId > highest)
        {
            highest = u.updateId;
        }
    }

    if (highest > lastUpdateId_)
    {
        lastUpdateId_ = highest;
        persist_.saveLastUpdateId(lastUpdateId_);
        Serial.print("TelegramPoller: watermark advanced to ");
        Serial.println(lastUpdateId_);
    }
}
