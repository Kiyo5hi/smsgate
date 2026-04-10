---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0260: Paginate /help output to stay within Telegram's 4096-char limit

## Motivation

The `/help` command in `telegram_poller.cpp` builds a single String with
every command listed (~130 lines × ~70 chars ≈ **9100 chars total**).
This is 2.2× Telegram's 4096-character message limit.

`bot_.sendMessageTo()` calls `doSendMessage()`, which relies on Telegram
returning `"ok":true`. When the text exceeds 4096 chars, Telegram returns
`"ok":false` and the send silently fails — the user receives **no reply
at all** when they type `/help`.

## Plan

After building the `help` String, paginate it into ≤3800-char chunks:

```cpp
if (help.length() <= 4096)
    bot_.sendMessageTo(u.chatId, help);
else
{
    constexpr unsigned int kPage = 3800;
    unsigned int start = 0;
    while (start < help.length())
    {
        unsigned int end = start + kPage;
        if (end >= help.length()) {
            bot_.sendMessageTo(u.chatId, help.substring(start));
            break;
        }
        while (end > start && help[end] != '\n')
            --end;
        if (end == start) end = start + kPage;
        bot_.sendMessageTo(u.chatId, help.substring(start, end));
        start = end + 1;
    }
}
```

With ~9100 chars and 3800-char pages the loop sends exactly 3 pages.
3 × `sendMessageTo()` ≤ 3 × 27 s = **81 s** from the per-update WDT
kick in `tick()` — well within the 120 s watchdog timeout.

Uses only `String::length()`, `String::operator[]`, and
`String::substring()` — all present in the native test stub.

## Notes for handover

- No change to the help text content; only the delivery mechanism changes.
- `/shortcuts` remains the compact single-message reference for users who
  want a one-page overview.
- The pagination loop is open-ended: if new commands are added and push
  the total past ~11400 chars (3 pages × 3800), a 4th page is sent
  automatically without any code change.
