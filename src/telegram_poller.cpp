#include "telegram_poller.h"
#include "sms_debug_log.h"
#include "sms_codec.h"

#include <vector>
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
            help += "/concat \xe2\x80\x94 Show in-flight concat reassembly groups\n";
            help += "/debug \xe2\x80\x94 Show SMS diagnostic log\n";
            help += "/cleardebug \xe2\x80\x94 Clear SMS diagnostic log\n";
            help += "/send <num> <msg> \xe2\x80\x94 Send outbound SMS\n";
            help += "/sendall <msg> \xe2\x80\x94 Broadcast to all aliases\n";
            help += "/test <num> \xe2\x80\x94 Send a test SMS to verify outbound path\n";
            help += "/queue \xe2\x80\x94 Show pending outbound queue\n";
            help += "/flushqueue \xe2\x80\x94 Immediately retry all pending outbound SMS\n";
            help += "/clearqueue \xe2\x80\x94 Discard all pending outbound SMS\n";
            help += "/cancel <N> \xe2\x80\x94 Cancel queued entry N\n";
            help += "/wifi \xe2\x80\x94 Force WiFi reconnect\n";
            help += "/mute [min] \xe2\x80\x94 Snooze proactive alerts (default 60m)\n";
            help += "/unmute \xe2\x80\x94 Cancel alert snooze\n";
            help += "/heap \xe2\x80\x94 Show free/min/max-block heap\n";
            help += "/csq \xe2\x80\x94 Quick signal strength snapshot\n";
            help += "/version \xe2\x80\x94 Show firmware build timestamp\n";
            help += "/restart \xe2\x80\x94 Soft reboot\n";
            if (smsBlockMutator_) {
                help += "/blocklist \xe2\x80\x94 Show block list\n";
                help += "/block <num|prefix*> \xe2\x80\x94 Block sender\n";
                help += "/unblock <num|prefix*> \xe2\x80\x94 Unblock sender\n";
            }
            if (aliasStore_) {
                help += "/aliases \xe2\x80\x94 List phone aliases\n";
                help += "/addalias <name> <num> \xe2\x80\x94 Add/replace alias\n";
                help += "/rmalias <name> \xe2\x80\x94 Remove alias\n";
            }
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
        if (lower == "/ping")
        {
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

        // RFC-0074: /version — firmware build timestamp.
        if (lower == "/version")
        {
            bot_.sendMessageTo(u.chatId, versionStr_);
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
            smsSender_.enqueue(phone, body,
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

        // RFC-0094: /sendall <body> — broadcast to all defined aliases.
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
            int64_t requesterChatId = u.chatId;
            int queued = 0;
            String capturedBody = body;
            aliasStore_->forEach([this, requesterChatId, capturedBody, &queued](
                const String & /*name*/, const String &aliasPhone) {
                String capturedPhone = aliasPhone;
                smsSender_.enqueue(aliasPhone, capturedBody,
                    [this, requesterChatId, capturedPhone]() {
                        sendErrorReply(requesterChatId,
                            String("SMS to ") + capturedPhone + " failed after retries.");
                    },
                    nullptr);
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
        if (now - lastPollMs_ < kPollIntervalMs)
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
