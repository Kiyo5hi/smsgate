# RFC-0004: Fan-out Delivery Model

**Status:** Implemented  
**Author:** Agent (Cursor)  
**Date:** 2026-04-12

## Summary

Split the monolithic `Messenger` trait into `MessageSink` (outbound) and
`MessageSource` (inbound), enabling fan-out delivery where each SMS is
forwarded to multiple targets simultaneously.

## Motivation

Users may want SMS forwarded not only to Telegram but also to a message
queue, webhook endpoint, or other notification systems. The previous
design coupled sending and receiving into a single `Messenger` trait,
making multi-target delivery impossible without duplicating the full
interface.

## Design

### Trait Split

```text
MessageSink     send_message(&mut self, text) → Result<MessageId>
MessageSource   poll(&mut self, since, timeout) → Result<Vec<InboundMessage>>
Messenger       = MessageSink + MessageSource  (blanket impl)
```

`MessageSink` is the only trait business logic depends on. `MessageSource`
is used exclusively by the Telegram polling thread.

### FanoutSink

```text
FanoutSink {
    sinks: Vec<Box<dyn MessageSink>>
}
```

- First sink is the **primary** — its `MessageId` is returned (needed for
  reply routing).
- Remaining sinks are fire-and-forget: errors are logged but do not fail
  the call.
- Each `send_message` call iterates all sinks sequentially.

### WebhookSink

Generic HTTP(S) POST sink. Each `send_message`:

1. Opens a fresh TCP/TLS connection
2. POSTs `{"text":"..."}` with `Connection: close`
3. Drains the response and closes

Zero idle RAM cost. ~40 KB peak during TLS handshake, released immediately.

### Configuration

```toml
[[sink]]
type = "webhook"
url  = "https://example.com/hook/sms"

[[sink]]
type = "webhook"
url  = "http://192.168.1.100:8080/mq/publish"
```

`build.rs` serializes the `[[sink]]` array as a JSON string into
`CFG_SINKS`. `main.rs` parses it at startup and constructs the sink list.

## Constraints

- ESP32 has ~160 KB usable heap. Each TLS connection peaks at ~40 KB.
  Since connections are not persistent, only one extra TLS connection
  exists at a time during delivery. Practical limit: ~5 webhook sinks
  per message before delivery latency becomes noticeable (~2s × N).
- The primary sink (Telegram) failing still aborts the call — the caller
  retains the SMS slot for retry on next boot.
- Webhook sinks receive the formatted human-readable text (same as
  Telegram), not raw PDU data.

## Files Changed

| File | Change |
|------|--------|
| `src/im/mod.rs` | Split `Messenger` → `MessageSink` + `MessageSource` |
| `src/im/fanout.rs` | New: `FanoutSink` dispatcher |
| `src/im/webhook/mod.rs` | New: `WebhookSink` (HTTP/HTTPS POST) |
| `src/im/telegram/mod.rs` | Implement split traits |
| `src/bridge/*.rs` | `&mut dyn Messenger` → `&mut dyn MessageSink` |
| `src/main.rs` | Build `FanoutSink` from config at startup |
| `src/config.rs` | Expose `Config::SINKS` |
| `build.rs` | Serialize `[[sink]]` to `CFG_SINKS` env var |
| `config.toml.example` | Document `[[sink]]` syntax |
| `tests/test_fanout.rs` | New: fanout dispatch tests |
