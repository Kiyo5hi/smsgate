#include "sms_debug_log.h"
#include <cstring>
#include <cstdio>

void SmsDebugLog::push(const Entry &e)
{
    entries_[head_] = e;
    head_ = (head_ + 1) % kMaxEntries;
    if (count_ < kMaxEntries)
        ++count_;

    // RFC-0020: persist the 10 most-recent entries to NVS on every push.
    if (persist_)
        persist();
}

// RFC-0020: Load persisted entries from NVS into the RAM ring.
// Entries in the blob are stored in chronological order (oldest at [0]).
// After loading: entries_[0..count_-1] hold the restored entries,
// head_ == count_ (next write position is just past the last loaded entry),
// count_ == number of loaded entries.
void SmsDebugLog::loadFrom(IPersist &p)
{
    SmsLogBlob blob{};
    size_t got = p.loadBlob("smslog", &blob, sizeof(blob));
    if (got == 0)
        return;  // key absent — first boot, nothing to load
    if (got < sizeof(blob))
        return;  // partial read — blob truncated or corrupt, discard
    if (blob.version != 1)
        return;  // schema mismatch, discard

    uint8_t n = blob.count;
    if (n > 10) n = 10;  // defensive clamp

    // Reset ring so entries are placed at slots 0..n-1 in order.
    head_  = 0;
    count_ = 0;

    for (uint8_t i = 0; i < n; i++)
    {
        const PersistEntry &pe = blob.entries[i];
        Entry e;
        e.unixTimestamp  = pe.unixTimestamp;
        e.timestampMs    = 0;    // millis()-since-boot is meaningless on reload
        e.sender         = String(pe.sender);
        e.bodyChars      = 0;    // not stored in the blob
        e.isConcat       = false;
        e.concatRef      = 0;
        e.concatTotal    = 0;
        e.concatPart     = 0;
        // body field stores the outcome string; error field stores pduPrefix.
        e.outcome        = String(pe.body);
        e.pduPrefix      = String(pe.error);

        entries_[head_] = e;
        head_ = (head_ + 1) % kMaxEntries;
        ++count_;
    }
}

// RFC-0020: Serialize the 10 most-recent RAM-ring entries to NVS.
// The blob always stores entries in chronological order (oldest at [0]).
// Each push() rewrites the full 1,704-byte blob; see the Option A
// analysis in rfc/0020-persistent-sms-log.md for the wear budget.
void SmsDebugLog::persist()
{
    SmsLogBlob blob{};
    blob.version = 1;
    blob.head    = 0;  // blob always written chronologically; head is always 0

    int n     = (int)count_;
    // oldest entry index in entries_[]
    int start = (n < (int)kMaxEntries) ? 0 : (int)head_;

    // We only persist the 10 most-recent entries.
    int persist_n = (n > 10) ? 10 : n;
    // skip the oldest (n - persist_n) entries when n > 10
    int skip  = n - persist_n;
    blob.count = (uint8_t)persist_n;

    for (int i = 0; i < persist_n; i++)
    {
        int ring_idx = (start + skip + i) % (int)kMaxEntries;
        const Entry &e = entries_[ring_idx];
        PersistEntry &pe = blob.entries[i];

        pe.unixTimestamp = e.unixTimestamp;
        // forwarded flag: outcome starts with "fwd"
        pe.forwarded = (e.outcome.length() >= 3 &&
                        e.outcome[0] == 'f' &&
                        e.outcome[1] == 'w' &&
                        e.outcome[2] == 'd');

        // sender — truncate to 20 chars
        std::strncpy(pe.sender, e.sender.c_str(), sizeof(pe.sender) - 1);
        pe.sender[sizeof(pe.sender) - 1] = '\0';

        // body field stores the outcome string (first 100 chars).
        // Outcome ("fwd", "buf", "err: ...") is the most diagnostically
        // useful text available after a reboot.
        std::strncpy(pe.body, e.outcome.c_str(), sizeof(pe.body) - 1);
        pe.body[sizeof(pe.body) - 1] = '\0';

        // error field stores pduPrefix (first 40 chars) as a compact diagnostic.
        std::strncpy(pe.error, e.pduPrefix.c_str(), sizeof(pe.error) - 1);
        pe.error[sizeof(pe.error) - 1] = '\0';
    }

    persist_->saveBlob("smslog", &blob, sizeof(blob));
}

String SmsDebugLog::dump() const
{
    if (count_ == 0)
        return String("(no SMS logged yet)");

    String out;
    // Header: use a simple monospace-friendly format.
    out += "SMS debug log (last ";
    out += String((unsigned long)count_);
    out += ")\n";

    // Walk oldest to newest.
    size_t start = (count_ < kMaxEntries) ? 0 : head_;
    for (size_t i = 0; i < count_; ++i)
    {
        size_t idx = (start + i) % kMaxEntries;
        const Entry &e = entries_[idx];

        out += "\n#";
        out += String((unsigned long)(i + 1));
        out += " | ";

        // If we have a unix timestamp (loaded from NVS or set by caller),
        // prefer it; otherwise fall back to elapsed time since boot.
        if (e.unixTimestamp > 0)
        {
            // Show as YYYY-MM-DD HH:MM UTC.
            // Avoid strftime to stay portable in the Arduino env.
            uint32_t t   = e.unixTimestamp;
            uint32_t mn  = (t / 60) % 60;
            uint32_t h   = (t / 3600) % 24;
            uint32_t days = t / 86400;

            uint32_t y = 1970;
            while (true)
            {
                bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
                uint32_t yd = leap ? 366u : 365u;
                if (days < yd) break;
                days -= yd;
                ++y;
            }
            static const uint8_t kMonLen[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
            bool leapYear = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
            uint32_t mo = 0;
            for (mo = 0; mo < 12; mo++)
            {
                uint32_t ml = kMonLen[mo] + (mo == 1 && leapYear ? 1u : 0u);
                if (days < ml) break;
                days -= ml;
            }
            uint32_t dom = days + 1;
            char buf[20];
            snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u",
                     (unsigned)y, (unsigned)(mo + 1), (unsigned)dom,
                     (unsigned)h, (unsigned)mn);
            out += String(buf);
            out += " UTC";
        }
        else if (e.timestampMs > 0)
        {
            unsigned long sec = e.timestampMs / 1000;
            unsigned long m2  = sec / 60;
            unsigned long s2  = sec % 60;
            if (m2 > 0)
            {
                out += String((unsigned long)m2);
                out += "m";
            }
            out += String((unsigned long)s2);
            out += "s";
        }
        else
        {
            out += "?";
        }
        out += " | ";
        out += e.sender;
        out += "\n  ";

        out += String((unsigned long)e.bodyChars);
        out += " chars";

        if (e.isConcat)
        {
            out += " | concat ref=";
            out += String((unsigned long)e.concatRef);
            out += " [";
            out += String((unsigned long)e.concatPart);
            out += "/";
            out += String((unsigned long)e.concatTotal);
            out += "]";
        }
        else
        {
            out += " | no concat";
        }

        out += " | ";
        out += e.outcome;

        // PDU prefix on a separate line for readability.
        if (e.pduPrefix.length() > 0)
        {
            out += "\n  PDU: ";
            out += e.pduPrefix;
            if (e.pduPrefix.length() >= 120)
                out += "...";
        }
        out += "\n";
    }
    return out;
}

String SmsDebugLog::dumpBrief(size_t n) const
{
    if (count_ == 0)
        return String("(no SMS logged yet)");

    // Clamp n to the number of entries we actually have.
    if (n > count_) n = count_;
    if (n == 0) return String("(no SMS logged yet)");

    String out;
    // Walk newest-first: the last written entry is at (head_ + kMaxEntries - 1) % kMaxEntries.
    size_t newest = (head_ + kMaxEntries - 1) % kMaxEntries;

    for (size_t i = 0; i < n; i++)
    {
        size_t idx = (newest + kMaxEntries - i) % kMaxEntries;
        const Entry &e = entries_[idx];

        // Timestamp.
        if (e.unixTimestamp > 0)
        {
            uint32_t t   = e.unixTimestamp;
            uint32_t mn  = (t / 60) % 60;
            uint32_t h   = (t / 3600) % 24;
            uint32_t days = t / 86400;
            uint32_t y = 1970;
            while (true) {
                bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
                uint32_t yd = leap ? 366u : 365u;
                if (days < yd) break;
                days -= yd; ++y;
            }
            static const uint8_t kML[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
            bool ly = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
            uint32_t mo = 0;
            for (mo = 0; mo < 12; mo++) {
                uint32_t ml = kML[mo] + (mo == 1 && ly ? 1u : 0u);
                if (days < ml) break; days -= ml;
            }
            char buf[18];
            snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u",
                     (unsigned)y, (unsigned)(mo+1), (unsigned)(days+1),
                     (unsigned)h, (unsigned)mn);
            out += String(buf);
        }
        else
        {
            out += String("?");
        }
        out += String(" | ");
        // Sender (truncate to 12 chars).
        out += e.sender.substring(0, 12);
        if (e.sender.length() > 12) out += "\xe2\x80\xa6"; // ellipsis
        out += String(" | ");
        // Outcome abbreviation + char count.
        out += e.outcome;
        if (e.bodyChars > 0) { out += String(" "); out += String((int)e.bodyChars); out += String("c"); }
        out += String("\n");
    }
    return out;
}

// RFC-0154: stats() — aggregate outcome counts over all ring entries.
String SmsDebugLog::stats() const
{
    if (count_ == 0)
        return String("(no SMS logged yet)");

    int forwarded = 0, failed = 0, buffered = 0, blocked = 0, dup = 0;

    size_t start = (count_ < kMaxEntries) ? 0 : head_;
    for (size_t i = 0; i < count_; ++i)
    {
        size_t idx = (start + i) % kMaxEntries;
        const String &o = entries_[idx].outcome;
        if (o.indexOf("fwd OK") >= 0)         ++forwarded;
        else if (o.indexOf("FAIL") >= 0)      ++failed;
        else if (o == "buffered")              ++buffered;
        else if (o == "blocked")              ++blocked;
        else if (o == "dup")                  ++dup;
    }

    char buf[128];
    snprintf(buf, sizeof(buf),
             "SMS log stats (%d total):\n"
             "  forwarded : %d\n"
             "  failed    : %d\n"
             "  buffered  : %d\n"
             "  blocked   : %d\n"
             "  dup       : %d",
             (int)count_, forwarded, failed, buffered, blocked, dup);
    return String(buf);
}

// RFC-0155: dumpBriefByOutcome — like dumpBrief but filtered by outcome substring.
String SmsDebugLog::dumpBriefByOutcome(size_t n, const String &keyword) const
{
    if (count_ == 0 || n == 0)
    {
        String msg = String("(no entries matching ");
        msg += keyword; msg += ")";
        return msg;
    }

    String out;
    size_t matched = 0;
    size_t newest = (head_ + kMaxEntries - 1) % kMaxEntries;

    for (size_t i = 0; i < count_ && matched < n; i++)
    {
        size_t idx = (newest + kMaxEntries - i) % kMaxEntries;
        const Entry &e = entries_[idx];

        if (e.outcome.indexOf(keyword) < 0)
            continue;

        ++matched;

        if (e.unixTimestamp > 0)
        {
            uint32_t t    = e.unixTimestamp;
            uint32_t mn   = (t / 60) % 60;
            uint32_t h    = (t / 3600) % 24;
            uint32_t days = t / 86400;
            uint32_t y = 1970;
            while (true) {
                bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
                uint32_t yd = leap ? 366u : 365u;
                if (days < yd) break;
                days -= yd; ++y;
            }
            static const uint8_t kML[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
            bool ly = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
            uint32_t mo = 0;
            for (mo = 0; mo < 12; mo++) {
                uint32_t ml = kML[mo] + (mo == 1 && ly ? 1u : 0u);
                if (days < ml) break; days -= ml;
            }
            char buf[18];
            snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u",
                     (unsigned)y, (unsigned)(mo+1), (unsigned)(days+1),
                     (unsigned)h, (unsigned)mn);
            out += String(buf);
        }
        else
        {
            out += String("?");
        }
        out += String(" | ");
        out += e.sender.substring(0, 12);
        if (e.sender.length() > 12) out += "\xe2\x80\xa6";
        out += String(" | ");
        out += e.outcome;
        if (e.bodyChars > 0) { out += String(" "); out += String((int)e.bodyChars); out += String("c"); }
        out += String("\n");
    }

    if (matched == 0)
    {
        String msg = String("(no entries matching ");
        msg += keyword; msg += ")";
        return msg;
    }
    return out;
}

// RFC-0157: topSenders — top n senders by message count.
String SmsDebugLog::topSenders(size_t n) const
{
    if (count_ == 0)
        return String("(no SMS logged yet)");

    if (n == 0 || n > 10) n = 10;

    // Simple frequency table: up to kMaxEntries distinct senders.
    // We use parallel arrays to avoid heap allocation (no std::map on some Arduino targets).
    static const size_t kMax = kMaxEntries;
    String senders[kMax];
    int    counts[kMax];
    size_t distinct = 0;

    size_t start = (count_ < kMaxEntries) ? 0 : head_;
    for (size_t i = 0; i < count_; ++i)
    {
        size_t idx = (start + i) % kMaxEntries;
        const String &s = entries_[idx].sender;
        bool found = false;
        for (size_t j = 0; j < distinct; ++j)
        {
            if (senders[j] == s) { ++counts[j]; found = true; break; }
        }
        if (!found && distinct < kMax)
        {
            senders[distinct] = s;
            counts[distinct]  = 1;
            ++distinct;
        }
    }

    // Partial selection sort to find top n.
    if (n > distinct) n = distinct;
    for (size_t i = 0; i < n; ++i)
    {
        size_t best = i;
        for (size_t j = i + 1; j < distinct; ++j)
            if (counts[j] > counts[best]) best = j;
        if (best != i)
        {
            { String tmp = senders[i]; senders[i] = senders[best]; senders[best] = tmp; }
            { int    tmp = counts[i];  counts[i]  = counts[best];  counts[best]  = tmp; }
        }
    }

    String out = String("Top ") + String((int)n) + String(" senders:\n");
    for (size_t i = 0; i < n; ++i)
    {
        out += String("  ");
        out += senders[i];
        out += String(": ");
        out += String(counts[i]);
        out += String(" msg");
        if (counts[i] != 1) out += String("s");
        out += String("\n");
    }
    return out;
}

// RFC-0117: dumpBriefFiltered — same format as dumpBrief but restricted to
// entries whose sender field contains `filter` as a substring.
String SmsDebugLog::dumpBriefFiltered(size_t n, const String &filter) const
{
    if (count_ == 0 || n == 0)
    {
        String msg = String("(no entries matching ");
        msg += filter; msg += ")";
        return msg;
    }

    String out;
    size_t matched = 0;
    size_t newest = (head_ + kMaxEntries - 1) % kMaxEntries;

    for (size_t i = 0; i < count_ && matched < n; i++)
    {
        size_t idx = (newest + kMaxEntries - i) % kMaxEntries;
        const Entry &e = entries_[idx];

        if (e.sender.indexOf(filter) < 0)
            continue;

        ++matched;

        if (e.unixTimestamp > 0)
        {
            uint32_t t   = e.unixTimestamp;
            uint32_t mn  = (t / 60) % 60;
            uint32_t h   = (t / 3600) % 24;
            uint32_t days = t / 86400;
            uint32_t y = 1970;
            while (true) {
                bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
                uint32_t yd = leap ? 366u : 365u;
                if (days < yd) break;
                days -= yd; ++y;
            }
            static const uint8_t kML[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
            bool ly = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
            uint32_t mo = 0;
            for (mo = 0; mo < 12; mo++) {
                uint32_t ml = kML[mo] + (mo == 1 && ly ? 1u : 0u);
                if (days < ml) break; days -= ml;
            }
            char buf[18];
            snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u",
                     (unsigned)y, (unsigned)(mo+1), (unsigned)(days+1),
                     (unsigned)h, (unsigned)mn);
            out += String(buf);
        }
        else
        {
            out += String("?");
        }
        out += String(" | ");
        out += e.sender.substring(0, 12);
        if (e.sender.length() > 12) out += "\xe2\x80\xa6";
        out += String(" | ");
        out += e.outcome;
        if (e.bodyChars > 0) { out += String(" "); out += String((int)e.bodyChars); out += String("c"); }
        out += String("\n");
    }

    if (matched == 0)
    {
        String msg = String("(no entries matching ");
        msg += filter; msg += ")";
        return msg;
    }
    return out;
}
