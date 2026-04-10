#pragma once
// RFC-0014: Multi-user Telegram allow list helpers.
//
// parseAllowedIds() is a pure function (no hardware deps, no Arduino.h
// beyond <stdint.h> and <string.h>) so it lives in this header as an
// inline and is compiled into both the firmware and the native test env.

#include <stdint.h>
#include <string.h>

// Parse a comma-separated string of Telegram chat IDs into `out[]`
// (up to `maxIds` entries). Returns the number of IDs parsed.
//
// Rules:
//   - Leading/trailing ASCII whitespace around each token is trimmed.
//   - Tokens that parse to 0 (e.g. empty or non-numeric) are skipped.
//   - Silently truncates at maxIds; the caller is responsible for
//     logging a warning if needed.
//   - Uses strchr-based splitting (not strtok) to avoid the static-
//     pointer re-entrancy hazard.
inline int parseAllowedIds(const char *csv, int64_t *out, int maxIds)
{
    if (!csv || maxIds <= 0)
        return 0;
    int count = 0;
    const char *p = csv;
    while (*p && count < maxIds)
    {
        // Skip leading whitespace.
        while (*p == ' ' || *p == '\t') ++p;
        const char *start = p;
        // Find end of token (next comma or end-of-string).
        const char *comma = strchr(p, ',');
        const char *end = comma ? comma : (p + strlen(p));
        // Trim trailing whitespace.
        const char *trim = end;
        while (trim > start && (*(trim - 1) == ' ' || *(trim - 1) == '\t'))
            --trim;
        if (trim > start)
        {
            char buf[32] = {};
            size_t len = (size_t)(trim - start);
            if (len >= sizeof(buf)) len = sizeof(buf) - 1;
            memcpy(buf, start, len);
            int64_t id = (int64_t)strtoll(buf, nullptr, 10);
            if (id != 0)
                out[count++] = id;
        }
        if (!comma) break;
        p = comma + 1;
    }
    return count;
}
