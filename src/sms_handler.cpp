#include "sms_handler.h"
#include "sms_codec.h"
#include "reply_target_map.h"
#include "sms_debug_log.h"

#include <algorithm>
#include <time.h>  // RFC-0159: time(nullptr) for unixTimestamp in log entries

SmsHandler::SmsHandler(IModem &modem, IBotClient &bot, RebootFn reboot, ClockFn clock)
    : modem_(modem), bot_(bot), reboot_(std::move(reboot)), clock_(std::move(clock))
{
    if (!clock_)
    {
        // Default: use Arduino millis(). In the host test env this is
        // the stubbed-zero millis() from test/support/Arduino.h — tests
        // that care about TTL/LRU pass their own clock lambda.
        clock_ = []() -> unsigned long { return millis(); };
    }
}

// ---------- helpers ----------

static String formatBotMessage(const String &sender, const String &timestamp,
                               const String &body,
                               int gmtOffsetHours = 8,
                               const String &fwdTag = String()) // RFC-0172
{
    String out;
    if (fwdTag.length() > 0)
    {
        out += fwdTag;
        out += " ";
    }
    out += sms_codec::humanReadablePhoneNumber(sender) + " | " +
           sms_codec::timestampToRFC3339(timestamp, gmtOffsetHours); // RFC-0169
    out += "\n-----\n";
    out += body;
    return out;
}

SmsHandler::ConcatGroup *SmsHandler::findGroup(const String &sender, uint16_t ref)
{
    for (auto &g : concatGroups_)
    {
        if (g.sender == sender && g.refNumber == ref)
            return &g;
    }
    return nullptr;
}

void SmsHandler::evictExpiredLocked(unsigned long now)
{
    // Walk the list, drop any entries whose firstSeenMs is more than
    // CONCAT_TTL_MS ago. Note: unsigned subtraction handles wrap-around
    // safely on 32-bit millis().
    for (auto it = concatGroups_.begin(); it != concatGroups_.end();)
    {
        unsigned long age = now - it->firstSeenMs;
        if (age > concatTtlMs_)
        {
            Serial.print("SMS concat TTL expiry, dropping ref=");
            Serial.println(it->refNumber);
            totalBufferedBytes_ -= it->byteCount;
            it = concatGroups_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void SmsHandler::evictLruUntilUnderCaps(size_t reservedExtraBytes)
{
    // While we're over any cap, drop the LRU group. "LRU" here means
    // the lowest lastSeenMs.
    auto over = [&]() -> bool {
        if (concatGroups_.size() > MAX_CONCAT_KEYS)
            return true;
        if (totalBufferedBytes_ + reservedExtraBytes > MAX_BYTES_TOTAL)
            return true;
        return false;
    };

    while (over() && !concatGroups_.empty())
    {
        // Find the LRU.
        size_t lruIdx = 0;
        for (size_t i = 1; i < concatGroups_.size(); ++i)
        {
            if (concatGroups_[i].lastSeenMs < concatGroups_[lruIdx].lastSeenMs)
                lruIdx = i;
        }
        Serial.print("SMS concat LRU eviction, ref=");
        Serial.println(concatGroups_[lruIdx].refNumber);
        totalBufferedBytes_ -= concatGroups_[lruIdx].byteCount;
        concatGroups_.erase(concatGroups_.begin() + lruIdx);
    }
}

bool SmsHandler::forwardSingle(const sms_codec::SmsPdu &pdu, int /*simIndex*/)
{
    String formatted = formatBotMessage(pdu.sender, pdu.timestamp, pdu.content, gmtOffsetMinutes_, fwdTag_); // RFC-0169/0172
    int32_t mid = bot_.sendMessageReturningId(formatted);
    if (mid <= 0)
    {
        return false;
    }
    // RFC-0003 §2: write the (telegram_message_id, sms_sender) pair
    // into the reply-target ring buffer so a future user reply can
    // route back. The map is optional — only set when bidirectional
    // mode is enabled by main.cpp.
    if (replyTargets_ != nullptr)
    {
        replyTargets_->put(mid, pdu.sender);
    }
    // RFC-0070 + RFC-0080: Fan out to extra recipients with reply-routing.
    for (int i = 0; i < extraRecipientCount_; i++) {
        int32_t xmid = bot_.sendMessageToReturningId(extraRecipients_[i], formatted);
        if (xmid > 0 && replyTargets_ != nullptr)
            replyTargets_->put(xmid, pdu.sender);
    }
    smsForwarded_++;
    if (onForwarded_) onForwarded_();
    if (onSenderFn_) onSenderFn_(pdu.sender); // RFC-0150
    return true;
}

bool SmsHandler::insertFragmentAndMaybePost(const sms_codec::SmsPdu &pdu, int simIndex,
                                            std::vector<int> &deleteSlots,
                                            bool *pWasDuplicate)
{
    deleteSlots.clear();
    if (!pdu.isConcatenated || pdu.concatTotalParts == 0 || pdu.concatPartNumber == 0 ||
        pdu.concatPartNumber > pdu.concatTotalParts)
    {
        return false;
    }

    unsigned long now = clock_();

    // First, age out expired groups.
    evictExpiredLocked(now);

    // Reject oversized individual fragments: any fragment body > per-
    // key cap on its own is dropped (we still want to wipe the SIM
    // slot so we don't loop on it, so the handler upstream treats this
    // as a parse failure).
    size_t fragmentBytes = pdu.content.length();
    if (fragmentBytes > MAX_BYTES_PER_KEY)
    {
        Serial.println("SMS concat fragment exceeds per-key cap, dropping.");
        return false;
    }

    ConcatGroup *group = findGroup(pdu.sender, pdu.concatRefNumber);
    if (group)
    {
        // Reject if adding this fragment would blow the per-key cap.
        if (group->byteCount + fragmentBytes > MAX_BYTES_PER_KEY)
        {
            Serial.print("SMS concat group exceeds per-key cap, dropping ref=");
            Serial.println(group->refNumber);
            // Drop the whole group (LRU-style) — better to lose partial
            // than to keep a half-full ever-growing bucket.
            totalBufferedBytes_ -= group->byteCount;
            concatGroups_.erase(concatGroups_.begin() + (group - concatGroups_.data()));
            return false;
        }

        // Deduplicate by part number — if a retry or rehydrate hands us
        // the same part twice, overwrite in place.
        bool replaced = false;
        for (auto &f : group->fragments)
        {
            if (f.partNumber == pdu.concatPartNumber)
            {
                totalBufferedBytes_ -= f.content.length();
                group->byteCount -= f.content.length();
                f.content = pdu.content;
                f.simIndex = simIndex;
                group->byteCount += pdu.content.length();
                totalBufferedBytes_ += pdu.content.length();
                replaced = true;
                break;
            }
        }
        if (!replaced)
        {
            ConcatFragment frag;
            frag.partNumber = pdu.concatPartNumber;
            frag.simIndex = simIndex;
            frag.content = pdu.content;
            group->fragments.push_back(frag);
            group->byteCount += fragmentBytes;
            totalBufferedBytes_ += fragmentBytes;
        }
        group->lastSeenMs = now;

        // The current group may still be under its per-key cap but
        // could have pushed totalBufferedBytes_ over MAX_BYTES_TOTAL.
        // Evict OTHER LRU groups (not this one) until we're back under.
        // Snapshot our key so the evictor can skip it.
        String myKeySender = group->sender;
        uint16_t myKeyRef = group->refNumber;
        while (totalBufferedBytes_ > MAX_BYTES_TOTAL && concatGroups_.size() > 1)
        {
            // Find LRU that ISN'T the current group.
            int lruIdx = -1;
            for (size_t i = 0; i < concatGroups_.size(); ++i)
            {
                if (concatGroups_[i].sender == myKeySender &&
                    concatGroups_[i].refNumber == myKeyRef)
                    continue;
                if (lruIdx == -1 ||
                    concatGroups_[i].lastSeenMs < concatGroups_[lruIdx].lastSeenMs)
                    lruIdx = (int)i;
            }
            if (lruIdx == -1)
                break;
            Serial.print("SMS concat total-cap LRU eviction, ref=");
            Serial.println(concatGroups_[lruIdx].refNumber);
            totalBufferedBytes_ -= concatGroups_[lruIdx].byteCount;
            concatGroups_.erase(concatGroups_.begin() + lruIdx);
        }
        // Re-resolve group pointer — erasing may have invalidated it.
        group = findGroup(myKeySender, myKeyRef);
        if (!group)
            return false; // shouldn't happen but be defensive
    }
    else
    {
        // Need room for a new group. Make sure we don't exceed caps.
        // Tentatively evict to make room for one extra key + this
        // fragment's bytes.
        // First, simulate as if we'll add a new group of size fragmentBytes.
        // evictLruUntilUnderCaps needs to consider the new key too:
        if (concatGroups_.size() + 1 > MAX_CONCAT_KEYS)
        {
            // Add a phantom count by temporarily raising totalBufferedBytes.
            // Simpler: just evict until (size+1 <= cap && total+frag <= cap).
            while (concatGroups_.size() + 1 > MAX_CONCAT_KEYS && !concatGroups_.empty())
            {
                size_t lruIdx = 0;
                for (size_t i = 1; i < concatGroups_.size(); ++i)
                {
                    if (concatGroups_[i].lastSeenMs < concatGroups_[lruIdx].lastSeenMs)
                        lruIdx = i;
                }
                Serial.print("SMS concat key-count LRU eviction, ref=");
                Serial.println(concatGroups_[lruIdx].refNumber);
                totalBufferedBytes_ -= concatGroups_[lruIdx].byteCount;
                concatGroups_.erase(concatGroups_.begin() + lruIdx);
            }
        }
        evictLruUntilUnderCaps(fragmentBytes);

        ConcatGroup g;
        g.sender = pdu.sender;
        g.firstTimestamp = pdu.timestamp;
        g.refNumber = pdu.concatRefNumber;
        g.totalParts = pdu.concatTotalParts;
        g.firstSeenMs = now;
        g.lastSeenMs = now;

        ConcatFragment frag;
        frag.partNumber = pdu.concatPartNumber;
        frag.simIndex = simIndex;
        frag.content = pdu.content;
        g.fragments.push_back(frag);
        g.byteCount = fragmentBytes;
        totalBufferedBytes_ += fragmentBytes;

        concatGroups_.push_back(g);
        group = &concatGroups_.back();
    }

    // Check completeness.
    if (group->fragments.size() < group->totalParts)
    {
        return false; // still waiting
    }

    // Assemble in part order.
    std::vector<ConcatFragment> sorted = group->fragments;
    std::sort(sorted.begin(), sorted.end(),
              [](const ConcatFragment &a, const ConcatFragment &b) {
                  return a.partNumber < b.partNumber;
              });

    // Sanity: every part number 1..totalParts must be present exactly once.
    for (uint8_t i = 0; i < group->totalParts; ++i)
    {
        if (sorted[i].partNumber != (uint8_t)(i + 1))
        {
            // Missing or duplicate — leave the group in place. (This
            // shouldn't happen since dedupe collapses duplicates, but
            // be defensive.)
            return false;
        }
    }

    String assembled;
    for (const auto &f : sorted)
        assembled += f.content;

    // RFC-0061: Suppress exact duplicate assembled messages.
    // checkDup reads only; recordDedup is called below after a successful
    // bot send so failed attempts never populate the ring.
    if (checkDup(group->sender, assembled))
    {
        Serial.println("Duplicate concat SMS suppressed, deleting SIM slots.");
        smsDeduplicated_++; // RFC-0062
        for (const auto &f : sorted)
            if (f.simIndex > 0) deleteSlots.push_back(f.simIndex);
        totalBufferedBytes_ -= group->byteCount;
        concatGroups_.erase(concatGroups_.begin() + (group - concatGroups_.data()));
        if (pWasDuplicate) *pWasDuplicate = true;
        return true;
    }

    String formatted = formatBotMessage(group->sender, group->firstTimestamp, assembled, gmtOffsetMinutes_, fwdTag_); // RFC-0169/0172

    int32_t mid = bot_.sendMessageReturningId(formatted);
    if (mid <= 0)
    {
        // Keep fragments in place; the caller will bump the failure
        // counter and eventually reboot.
        return false;
    }
    if (replyTargets_ != nullptr)
    {
        replyTargets_->put(mid, group->sender);
    }
    // RFC-0070 + RFC-0080: Fan out to extra recipients with reply-routing.
    for (int i = 0; i < extraRecipientCount_; i++) {
        int32_t xmid = bot_.sendMessageToReturningId(extraRecipients_[i], formatted);
        if (xmid > 0 && replyTargets_ != nullptr)
            replyTargets_->put(xmid, group->sender);
    }
    recordDedup(group->sender, assembled); // RFC-0061: record after success
    smsForwarded_++;
    if (onForwarded_) onForwarded_();
    if (onSenderFn_) onSenderFn_(group->sender); // RFC-0150

    // Success: collect all SIM slots for deletion and drop the group.
    for (const auto &f : sorted)
    {
        if (f.simIndex > 0)
            deleteSlots.push_back(f.simIndex);
    }
    totalBufferedBytes_ -= group->byteCount;
    concatGroups_.erase(concatGroups_.begin() + (group - concatGroups_.data()));
    return true;
}

void SmsHandler::noteTelegramFailure()
{
    consecutiveFailures_++;
    telegramSendFailures_++;
    Serial.print("Post to Telegram FAILED (");
    Serial.print(consecutiveFailures_);
    Serial.println(" consecutive). Keeping SMS on SIM.");
    // RFC-0057: Warn the user one step before the reboot threshold so
    // they have a chance to see the alert (best-effort — Telegram may
    // be unreachable, which is exactly what's causing these failures).
    if (maxConsecutiveFailures_ > 0 && consecutiveFailures_ == maxConsecutiveFailures_ - 1)
    {
        bot_.sendMessage(
            String("\xE2\x9A\xA0\xEF\xB8\x8F ") + // ⚠️
            String(consecutiveFailures_) + "/" + String(maxConsecutiveFailures_) +
            String(" consecutive Telegram failures \xe2\x80\x94 bridge will reboot on next failure."));
    }
    if (maxConsecutiveFailures_ > 0 && consecutiveFailures_ >= maxConsecutiveFailures_)
    {
        Serial.println("Too many consecutive failures, rebooting to recover...");
        if (reboot_)
        {
            reboot_();
        }
    }
}

// ---------- RFC-0069: concat group summary ----------

String SmsHandler::concatGroupsSummary() const
{
    if (concatGroups_.empty())
        return String("(no concat groups in flight)");

    String out;
    out += String((int)concatGroups_.size());
    out += " group";
    if (concatGroups_.size() > 1) out += "s";
    out += " in flight:\n";

    for (size_t i = 0; i < concatGroups_.size(); i++)
    {
        const ConcatGroup &g = concatGroups_[i];
        out += "  ref=0x";
        // Print ref as 2-digit hex.
        char hexbuf[8];
        snprintf(hexbuf, sizeof(hexbuf), "%02X", (unsigned)g.refNumber);
        out += String(hexbuf);
        out += " ";
        out += g.sender.substring(0, 16);
        if (g.sender.length() > 16) out += "\xe2\x80\xa6"; // ellipsis
        out += " \xe2\x80\x94 "; // —
        out += String((int)g.fragments.size());
        out += "/";
        out += String((int)g.totalParts);
        out += " parts  ";
        out += String((int)g.byteCount);
        out += " B\n";
    }
    return out;
}

// ---------- RFC-0061: duplicate suppression ----------

uint32_t SmsHandler::dedupHash(const String &sender, const String &body)
{
    uint32_t h = 5381u;
    for (size_t i = 0; i < (size_t)sender.length(); i++)
        h = h * 33u ^ (uint8_t)sender[i];
    h = h * 33u ^ 0u; // null separator keeps (ab,c) distinct from (a,bc)
    for (size_t i = 0; i < (size_t)body.length(); i++)
        h = h * 33u ^ (uint8_t)body[i];
    return h == 0u ? 1u : h; // 0 is reserved as "empty slot" sentinel
}

bool SmsHandler::checkDup(const String &sender, const String &body) const
{
    uint32_t h = dedupHash(sender, body);
    unsigned long now = clock_();
    for (size_t i = 0; i < kDedupSlots; i++)
    {
        if (dedupRing_[i].hash == h)
        {
            unsigned long age = now - dedupRing_[i].tsMs;
            if (dedupWindowMs_ > 0 && age < dedupWindowMs_)
                return true; // duplicate within window
        }
    }
    return false;
}

void SmsHandler::recordDedup(const String &sender, const String &body)
{
    uint32_t h = dedupHash(sender, body);
    unsigned long now = clock_();
    dedupRing_[dedupHead_].hash = h;
    dedupRing_[dedupHead_].tsMs = now;
    dedupHead_ = (dedupHead_ + 1) % kDedupSlots;
}

// ---------- CMGR PDU response parsing ----------

// Extract the PDU hex blob from a PDU-mode +CMGR response. The
// expected shape is:
//   +CMGR: <stat>,[<alpha>],<length>\r\n
//   <hex PDU>\r\n
//   \r\nOK\r\n
// We accept a missing trailing OK for robustness (mirrors the old
// text-mode parser's behaviour).
static bool extractPduHexFromCmgr(const String &raw, String &hex)
{
    int header = raw.indexOf("+CMGR:");
    if (header == -1)
        return false;
    int headerEnd = raw.indexOf("\r\n", header);
    if (headerEnd == -1)
        return false;
    int bodyStart = headerEnd + 2;
    int bodyEnd = raw.indexOf("\r\n", bodyStart);
    if (bodyEnd == -1)
        bodyEnd = raw.length();
    hex = raw.substring(bodyStart, bodyEnd);
    hex.trim();
    return hex.length() > 0;
}

void SmsHandler::handleSmsIndex(int idx)
{
    if (!forwardingEnabled_) // RFC-0153: forwarding paused, leave SMS in SIM
    {
        Serial.print("Forwarding disabled, skipping SMS @ index ");
        Serial.println(idx);
        return;
    }
    Serial.print("-------- SMS @ index ");
    Serial.print(idx);
    Serial.println(" --------");

    String raw;
    modem_.sendAT("+CMGR=" + String(idx));
    int8_t res = modem_.waitResponse(5000UL, raw);
    if (res != 1)
    {
        Serial.println("CMGR failed");
        return;
    }

    String pduHex;
    if (!extractPduHexFromCmgr(raw, pduHex))
    {
        Serial.println("Unable to extract PDU from CMGR response. Raw:");
        Serial.println(raw);
        // Wipe the slot so we don't loop on a malformed response.
        modem_.sendAT("+CMGD=" + String(idx));
        modem_.waitResponseOk(1000UL);
        return;
    }

    sms_codec::SmsPdu pdu;
    if (!sms_codec::parseSmsPdu(pduHex, pdu))
    {
        Serial.println("Unable to parse PDU. Raw hex:");
        Serial.println(pduHex);
        modem_.sendAT("+CMGD=" + String(idx));
        modem_.waitResponseOk(1000UL);
        return;
    }

    Serial.print("Sender:    ");
    Serial.println(pdu.sender);
    Serial.print("Timestamp: ");
    Serial.println(pdu.timestamp);
    Serial.print("Content:   ");
    Serial.println(pdu.content);

    // RFC-0018/RFC-0021: Block list check (compile-time + runtime).
    // If the sender matches either list, silently delete the SIM slot
    // and return without forwarding. Deletion is UNCONDITIONAL — unlike
    // the normal path where deletion is conditional on a successful
    // Telegram POST. Omitting the CMGD would cause infinite boot-loop
    // replays of blocked fragments via sweepExistingSms.
    if (blockingEnabled_ &&  // RFC-0162: check only when enforcement is on
        ((blockList_   && isBlocked(pdu.sender.c_str(), blockList_,   blockListCount_))  ||
         (runtimeList_ && isBlocked(pdu.sender.c_str(), runtimeList_, runtimeListCount_))))
    {
        Serial.print("SMS from blocked sender ");
        Serial.print(pdu.sender);
        Serial.println(", deleting silently.");
        smsBlocked_++; // RFC-0062
        modem_.sendAT("+CMGD=" + String(idx));
        modem_.waitResponseOk(1000UL);
        // Log the block event so /debug shows it.
        if (debugLog_)
        {
            SmsDebugLog::Entry e;
            e.timestampMs = clock_ ? clock_() : 0;
            e.sender      = pdu.sender;
            e.bodyChars   = (uint16_t)pdu.content.length();
            e.isConcat    = pdu.isConcatenated;
            e.concatRef   = pdu.concatRefNumber;
            e.concatTotal = pdu.concatTotalParts;
            e.concatPart  = pdu.concatPartNumber;
            e.outcome     = String("blocked");
            debugLog_->push(e);
        }
        return;
    }

    // Capture diagnostic entry before any branching so we get a log
    // record regardless of the outcome.
    SmsDebugLog::Entry logEntry;
    if (debugLog_)
    {
        logEntry.timestampMs = clock_ ? clock_() : 0;
        logEntry.unixTimestamp = (uint32_t)time(nullptr); // RFC-0159
        logEntry.sender = pdu.sender;
        logEntry.bodyChars = (uint16_t)pdu.content.length();
        logEntry.isConcat = pdu.isConcatenated;
        logEntry.concatRef = pdu.concatRefNumber;
        logEntry.concatTotal = pdu.concatTotalParts;
        logEntry.concatPart = pdu.concatPartNumber;
        logEntry.pduPrefix = pduHex.substring(0, 120);
        // outcome is filled in below
    }

    if (!pdu.isConcatenated)
    {
        // RFC-0061: Suppress exact duplicates of recently forwarded SMS.
        // checkDup only reads the ring; recordDedup writes only on success
        // so that failed sends never consume a dedup slot (retries must reach
        // the bot).
        if (checkDup(pdu.sender, pdu.content))
        {
            Serial.println("Duplicate SMS suppressed, deleting SIM slot.");
            smsDeduplicated_++; // RFC-0062
            modem_.sendAT("+CMGD=" + String(idx));
            modem_.waitResponseOk(1000UL);
            if (debugLog_) { logEntry.outcome = "dup"; debugLog_->push(logEntry); }
            return;
        }
        if (forwardSingle(pdu, idx))
        {
            recordDedup(pdu.sender, pdu.content); // RFC-0061
            consecutiveFailures_ = 0;
            Serial.println("Posted to Telegram OK, deleting SMS.");
            modem_.sendAT("+CMGD=" + String(idx));
            modem_.waitResponseOk(1000UL);
            if (debugLog_) { logEntry.outcome = "fwd OK"; debugLog_->push(logEntry); }
        }
        else
        {
            noteTelegramFailure();
            if (debugLog_) { logEntry.outcome = "fwd FAIL"; debugLog_->push(logEntry); }
        }
        return;
    }

    // ---- Concatenated path ----
    Serial.print("Concat fragment: ref=");
    Serial.print(pdu.concatRefNumber);
    Serial.print(" part=");
    Serial.print((int)pdu.concatPartNumber);
    Serial.print("/");
    Serial.println((int)pdu.concatTotalParts);

    std::vector<int> deleteSlots;
    bool wasDuplicate = false;
    bool posted = insertFragmentAndMaybePost(pdu, idx, deleteSlots, &wasDuplicate);
    if (posted)
    {
        consecutiveFailures_ = 0;
        Serial.print(wasDuplicate ? "Duplicate concat suppressed, deleting "
                                  : "Assembled concat message posted OK, deleting ");
        Serial.print((int)deleteSlots.size());
        Serial.println(" SIM slot(s).");
        for (int slot : deleteSlots)
        {
            modem_.sendAT("+CMGD=" + String(slot));
            modem_.waitResponseOk(1000UL);
        }
        if (debugLog_) {
            logEntry.outcome = wasDuplicate ? "dup" : "assembled+fwd OK";
            debugLog_->push(logEntry);
        }
        return;
    }

    // Not posted. Either (a) still waiting for more parts -> leave SIM
    // slot in place (it's the source of truth across reboots), or (b)
    // Telegram POST failed mid-assembly -> count that as a failure.
    ConcatGroup *g = findGroup(pdu.sender, pdu.concatRefNumber);
    if (g != nullptr && g->fragments.size() >= g->totalParts)
    {
        // All parts landed but bot rejected the send. Failure path.
        noteTelegramFailure();
        if (debugLog_) { logEntry.outcome = "assembled+fwd FAIL"; debugLog_->push(logEntry); }
    }
    else
    {
        // Incomplete — do NOT bump the failure counter; this is
        // expected. Leave the SIM slot alone so a reboot rehydrates.
        Serial.println("Concat incomplete, leaving SIM slot in place.");
        if (debugLog_) { logEntry.outcome = "buffered"; debugLog_->push(logEntry); }
    }
}

int SmsHandler::sweepExistingSms()
{
    String data;
    modem_.sendAT("+CMGL=\"ALL\"");
    int8_t res = modem_.waitResponse(10000UL, data);
    if (res != 1)
    {
        Serial.println("Initial CMGL sweep failed");
        return 0;
    }

    int search = 0;
    int count = 0;
    while (true)
    {
        int start = data.indexOf("+CMGL:", search);
        if (start == -1)
            break;
        int colon = start + 6;
        int comma = data.indexOf(',', colon);
        if (comma == -1)
            break;
        int idx = data.substring(colon, comma).toInt();
        search = comma;
        if (idx > 0)
        {
            handleSmsIndex(idx);
            count++;
        }
    }
    return count;
}
