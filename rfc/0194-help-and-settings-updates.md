---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0194: Help text additions + /settings pause status

## Motivation

RFC-0190 through RFC-0193 added new commands (`/setsmsagefilter`, `/testpdu`,
`/pausefwd`, `/sendnow`) that are not yet listed in `/help`. Also, `/settings`
doesn't show the current forwarding pause state (RFC-0192).

## Plan

### `/help` additions

Add the following lines to the help text (before `/shortcuts`):

```
/setsmsagefilter <h> — Skip SMS older than N hours (0=off, max 8760)
/testpdu <hex>       — Decode a raw PDU hex string for debugging
/pausefwd <min>      — Pause SMS forwarding for N minutes (1–1440)
/sendnow             — Immediately fire all scheduled SMS
```

### `/settings` enhancement

After the existing `fwd_tag` line, add:

```
  Fwd pause: none          (or "active (~N min remain)")
```

Uses `s_fwdPauseUntilMs` directly in the settings lambda (same file).
