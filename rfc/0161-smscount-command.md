---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0161: /smscount command — SIM SMS storage capacity and usage

## Motivation

`/smsslots` lists individual occupied slots but doesn't show capacity.
`/smscount` calls `AT+CPMS?` to show used/total for each memory store
(SM = SIM card, ME = modem memory). Quick way to see if the SIM is
nearly full.

## Plan

Add `setSmsCntFn(std::function<String()>)` to TelegramPoller.
`/smscount` calls `smsCntFn_()` and sends the result.

In main.cpp the lambda issues `AT+CPMS?`, parses the response line
`+CPMS: "SM",<used>,<total>,"SM",<used>,<total>` and formats it.
