---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0195: `/clearschedule` — cancel all pending scheduled SMS

## Motivation

When the operator wants to cancel all pending scheduled SMS at once
(e.g. accidentally scheduled too many, or shutting down for maintenance),
running `/cancelsched 1` through `/cancelsched 5` separately is tedious.
`/clearschedule` clears all occupied slots in one command.

## Plan

### TelegramPoller: `/clearschedule`

Iterates `scheduledQueue_`, clears all occupied slots (sendAtMs != 0),
replies "✅ Cleared N scheduled SMS." or "(no scheduled SMS)".

No setter needed — all state is internal to TelegramPoller.
