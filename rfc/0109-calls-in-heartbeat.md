---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0109 — Calls count in heartbeat

## Motivation

The periodic heartbeat message shows forwarded SMS count and queue depth but
not incoming call count. With the call handler now auto-rejecting calls, the
call count in the heartbeat helps the operator see call traffic at a glance.

## Plan

Append `| calls N` to the heartbeat string in `main.cpp` using
`callHandler.callsReceived()`.
