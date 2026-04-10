#include "telegram_poller.h"
#include "sms_debug_log.h"
#include "sms_codec.h"

#include <vector>

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

        // RFC-0042: /ping — instant liveness check.
        if (lower == "/ping")
        {
            bot_.sendMessageTo(u.chatId, String("\xF0\x9F\x8F\x93 Pong")); // 🏓
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

        // RFC-0040: /cleardebug — wipe the SMS debug log.
        if (lower == "/cleardebug")
        {
            if (debugLog_)
            {
                debugLog_->clear();
                bot_.sendMessageTo(u.chatId, String("\xF0\x9F\x97\x91 Debug log cleared.")); // 🗑
            }
            else
            {
                bot_.sendMessageTo(u.chatId, String("(debug log not configured)"));
            }
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
                bot_.sendMessageTo(u.chatId, String("Usage: /block <number>"));
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
            bot_.sendMessageTo(u.chatId,
                String("Blocked: ") + arg +
                String(". Note: matching is exact — check the serial log to confirm the format your carrier sends."));
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
            String phone = arg.substring(0, spacePos);
            String body  = arg.substring(spacePos + 1);
            body.trim();
            if (body.length() == 0)
            {
                bot_.sendMessageTo(u.chatId,
                    String("Usage: /send <number> <message>\nExample: /send +8613800138000 Hello!"));
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
                    // RFC-0032: delivery confirmation.
                    bot_.sendMessageTo(requesterChatId,
                        String("\xF0\x9F\x93\xA8 Sent to ") + capturedPhone); // U+1F4E8
                });
            // RFC-0029: Include a body preview so the user can catch typos.
            String preview = body.substring(0, 30);
            if (body.length() > 30) preview += "\xE2\x80\xA6"; // U+2026 ellipsis
            // RFC-0037: Append part count when message will be split.
            int parts = sms_codec::countSmsParts(body);
            String confirmText = String("\xE2\x9C\x85 Queued to ") + phone + String(": ") + preview;
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
                        + String("/") + String(SmsSender::kMaxAttempts) + String(")\n");
                }
            }
            bot_.sendMessageTo(u.chatId, msg);
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
            help += "Commands: /ping, /debug, /cleardebug, /status, /restart, /send <num> <msg>, /queue, /cancel <N>";
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

    String capturedPhone = phone; // copy for lambda capture
    int64_t requesterChatId = u.chatId; // copy for lambda capture (u is a local, lambda may fire later)
    smsSender_.enqueue(phone, u.text,
        [this, capturedPhone, requesterChatId]() {
            sendErrorReply(requesterChatId, String("SMS to ") + capturedPhone + " failed after retries.");
        },
        [this, capturedPhone, requesterChatId]() {
            // RFC-0032: delivery confirmation fires from drainQueue on success.
            bot_.sendMessageTo(requesterChatId,
                String("\xF0\x9F\x93\xA8 Sent to ") + capturedPhone); // U+1F4E8 envelope
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
