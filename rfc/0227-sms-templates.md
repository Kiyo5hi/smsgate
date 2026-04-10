---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0227: SMS templates

## Motivation

Operators frequently send the same SMS body (e.g. "I'm on my way", "Meeting confirmed",
"Call me back") to different recipients. Typing the body each time is error-prone.
A lightweight template store lets operators save bodies under short names and reuse them.

## Plan

1. Add a new `SmsTemplateStore` class (header-only):
   - Max 10 templates, max name len 20, max body len 160 chars.
   - Persisted via `IPersist` as a binary blob (key: "sms_templates").
   - Methods: `set(name, body)`, `get(name) -> String` ("" on miss),
     `remove(name)`, `clear()`, `list() -> String`, `count()`.
   - Name validation: same rules as `SmsAliasStore` (alphanumeric + `_-`).

2. Commands in `TelegramPoller`:
   - `/tsave <name> <body>` — save template (or overwrite).
   - `/tlist` — list all templates (name → preview).
   - `/trm <name>` — remove template.
   - `/tclear` — remove all templates.
   - `/tsend <name> <phone|@alias>` — expand template and immediately send SMS.
   - `/tschedule <name> <min> <phone|@alias>` — expand template and schedule.

3. Wire in `main.cpp`: construct `SmsTemplateStore`, call `load()`, pass pointer to
   TelegramPoller via `setTemplateStore(SmsTemplateStore*)`.

4. Tests: save/get/list/rm, tsend expands template, tschedule fills sched slot.

## Notes for handover

`SmsTemplateStore` mirrors `SmsAliasStore` structure. The blob format is:
  magic (4) + count (4) + 10 × (name[21] + body[161]) = 10 × 182 = 1828 bytes.
The 160-char body limit aligns with single-part GSM-7 SMS capacity.
