#pragma once
// RFC-0018: SMS sender block list helpers.
//
// parseBlockList() and isBlocked() are pure functions (no hardware deps,
// no Arduino.h beyond <string.h>) so they live in this header as inlines
// and compile into both the firmware and the native test env.

#include <string.h>

static constexpr int kSmsBlockListMaxEntries = 20;
static constexpr int kSmsBlockListMaxNumberLen = 20; // chars, not including NUL

// Parse a comma-separated string of phone numbers into out[][21].
// Returns the number of entries parsed.
//
// Rules:
//   - Leading/trailing ASCII whitespace around each token is trimmed.
//   - Empty tokens (two adjacent commas, leading/trailing comma) are skipped.
//   - Tokens longer than kSmsBlockListMaxNumberLen are truncated with a
//     NUL terminator — the entry is still stored (the user will notice
//     the mismatch when block-listing fails to fire).
//   - Silently truncates at maxEntries; the caller should log if count
//     equals maxEntries and the CSV may have had more tokens.
//   - Uses strchr-based splitting — not strtok — to avoid the static-
//     pointer re-entrancy hazard.
inline int parseBlockList(const char *csv,
                          char out[][kSmsBlockListMaxNumberLen + 1],
                          int maxEntries)
{
    if (!csv || maxEntries <= 0)
        return 0;
    int count = 0;
    const char *p = csv;
    while (*p && count < maxEntries)
    {
        while (*p == ' ' || *p == '\t') ++p;
        const char *start = p;
        const char *comma = strchr(p, ',');
        const char *end = comma ? comma : (p + strlen(p));
        const char *trim = end;
        while (trim > start && (*(trim-1) == ' ' || *(trim-1) == '\t'))
            --trim;
        size_t len = (size_t)(trim - start);
        if (len > 0)
        {
            if (len > (size_t)kSmsBlockListMaxNumberLen)
                len = (size_t)kSmsBlockListMaxNumberLen;
            memcpy(out[count], start, len);
            out[count][len] = '\0';
            count++;
        }
        if (!comma) break;
        p = comma + 1;
    }
    return count;
}

// Return true if `number` exactly matches any entry in `list[0..count-1]`.
inline bool isBlocked(const char *number,
                      const char (*list)[kSmsBlockListMaxNumberLen + 1],
                      int count)
{
    if (!number || !list || count <= 0) return false;
    for (int i = 0; i < count; i++)
    {
        if (strcmp(number, list[i]) == 0)
            return true;
    }
    return false;
}
