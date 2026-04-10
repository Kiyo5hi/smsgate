#include "telegram_poller.h"
#include "sms_debug_log.h"
#include "sms_codec.h"

#include <vector>
#include <memory>
#include <time.h>

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
            help += "/cleardebug \xe2\x80\x94 Clear SMS diagnostic log\n";
            help += "/send <num> <msg> \xe2\x80\x94 Send outbound SMS\n";
            help += "/sendall <msg> \xe2\x80\x94 Broadcast to all aliases\n";
            help += "/test <num> \xe2\x80\x94 Send a test SMS to verify outbound path\n";
            help += "/queue \xe2\x80\x94 Show pending outbound queue\n";
            help += "/flushqueue \xe2\x80\x94 Immediately retry all pending outbound SMS\n";
            help += "/clearqueue \xe2\x80\x94 Discard all pending outbound SMS\n";
            help += "/resetstats \xe2\x80\x94 Reset session counters (SMS fwd/fail, calls)\n";
            help += "/cancel <N> \xe2\x80\x94 Cancel queued entry N\n";
            help += "/cancelnum <phone> \xe2\x80\x94 Cancel all queued entries for a phone number\n";
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
            help += "/setcallnotify on|off \xe2\x80\x94 Enable/mute call Telegram notifications\n";
            help += "/callstatus \xe2\x80\x94 Show call handler config and state\n";
            help += "/setcalldedup <s> \xe2\x80\x94 Call dedup cooldown window in seconds (1\xe2\x80\x9360)\n";
            help += "/setunknowndeadline <ms> \xe2\x80\x94 RING-without-CLIP deadline in ms (500\xe2\x80\x9310000)\n";
            help += "/setgmtoffset <h> \xe2\x80\x94 Timezone for SMS timestamps (-12 to +14, default +8)\n";
            help += "/setgmtoffsetmin <m> \xe2\x80\x94 Timezone in total minutes (-720 to +840, e.g. 330=UTC+5:30)\n";
            help += "/setfwdtag <text> \xe2\x80\x94 Custom prefix tag on forwarded SMS (max 20 chars)\n";
            help += "/clearfwdtag \xe2\x80\x94 Remove custom forward prefix tag\n";
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
                help += "/clearaliases \xe2\x80\x94 Remove all aliases\n";
            }
            help += "/shortcuts \xe2\x80\x94 Quick command reference\n";
            help += "\nReply to a forwarded SMS to send a response.";
            bot_.sendMessageTo(u.chatId, help);
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
                    if (e.queuedAtMs > 0 && nowMs >= e.queuedAtMs)
                    {
                        uint32_t ageSec = (nowMs - e.queuedAtMs) / 1000;
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
