---
status: proposed
created: 2026-04-12
---

# RFC-0003: i18n and UI string polish

## 1. Motivation

All user-visible strings are currently hard-coded in English.
The primary operator is Chinese-speaking; an English-only interface adds
unnecessary friction for everyday use (reading forwarded SMS, checking status).

Additionally, some existing EN strings have minor wording inconsistencies
worth cleaning up while the catalogue is being formalised.

---

## 2. Scope

**In scope**
- Every string sent to the Telegram chat (system events, forwarded SMS, command replies)
- Command `description()` fields (shown in Telegram's command autocomplete list)
- Two locales: `en` (default) and `zh` (Simplified Chinese)

**Out of scope**
- Serial log messages (`log::info!` / `log::warn!` / `log::error!`) — debug output stays in EN
- Runtime locale switching — compile-time selection only
- Traditional Chinese, any other locale

---

## 3. Locale selection

Add one key to `config.toml`:

```toml
[ui]
locale = "zh"   # "en" | "zh"  (default: "en" if key is absent)
```

`build.rs` injects it as `CFG_LOCALE`. `src/i18n/mod.rs` re-exports the correct
locale module at compile time:

```rust
#[cfg(cfg_locale = "zh")]  pub use zh::*;
#[cfg(not(cfg_locale = "zh"))]  pub use en::*;
```

Each locale module (`en.rs`, `zh.rs`) exposes the same set of functions.
The compiler enforces completeness — a missing function in either module is a build error.

---

## 4. Implementation shape

```
src/
  i18n/
    mod.rs       — cfg-based re-export; no string literals here
    en.rs        — all EN strings and format functions
    zh.rs        — all ZH strings and format functions
```

Pure static strings are `&'static str`.
Strings with runtime parameters are `fn(...) -> String`.
No heap cost for the unused locale (dead code elimination removes it).

Call sites change from inline literals to `i18n::started()`, `i18n::low_heap(free)`, etc.

---

## 5. String catalogue

### 5.1 System / lifecycle  (`main.rs`)

| Key | EN (current → proposed) | ZH |
|-----|--------------------------|-----|
| `started` | ✅ smsgate started | ✅ smsgate 已启动 |
| `nvs_fail` | ⚠️ NVS init failed — running without persistence. Block list and cursor will reset on reboot. | ⚠️ NVS 初始化失败，以无持久化模式运行。黑名单和游标将在重启后重置。 |
| `rebooting` | ♻️ Rebooting now… | ♻️ 正在重启… |
| `low_heap(free: u32)` | ⚠️ Low heap: {free} bytes | ⚠️ 可用内存不足：{free} 字节 |

### 5.2 SMS forwarding  (`bridge/forwarder.rs`)

| Key | EN | ZH |
|-----|----|----|
| `sms_forward(sender, ts, body)` | 📱 SMS from {sender}\n🕐 {ts}\n\n{body} | 📱 来自 {sender}\n🕐 {ts}\n\n{body} |

### 5.3 Call handler  (`bridge/call_handler.rs`)

| Key | EN | ZH |
|-----|----|----|
| `incoming_call(display)` | 📞 Incoming call from {display} | 📞 来电：{display} |

### 5.4 `/status`

| Key | EN (current → proposed) | ZH |
|-----|--------------------------|-----|
| `status_title` | 📊 smsgate status | 📊 smsgate 状态 |
| `status_uptime(h,m,s)` | ⏱ Uptime: {h}h {m}m {s}s | ⏱ 运行时间：{h}h {m}m {s}s |
| `status_signal(sig, op)` | 📶 Signal: {sig} — {op} | 📶 信号：{sig} — {op} |
| `status_network(reg)` | 🌐 Network: {reg} | 🌐 网络：{reg} |
| `status_reg_ok` | registered | 已注册 |
| `status_reg_no` | not registered | 未注册 |
| `status_op_unknown` | unknown | 未知 |
| `status_queue(n)` | 📬 Queue: {n} pending | 📬 队列：{n} 条待发 |
| `status_log(n)` | 📋 Log: {n} messages | 📋 日志：{n} 条 |
| `status_fwd_on` | 🔄 Forwarding: enabled | 🔄 转发：已启用 |
| `status_fwd_off` | 🔄 Forwarding: PAUSED | 🔄 转发：已暂停 |

### 5.5 `/send`

| Key | EN (current → proposed) | ZH |
|-----|--------------------------|-----|
| `send_usage` | Usage: /send \<number\> \<message text\> | 用法：/send \<号码\> \<消息内容\> |
| `send_invalid_number` | Invalid phone number | 无效号码 |
| `send_empty_body` | Message body is empty | 消息内容为空 |
| `send_too_long` | Message too long (> 10 SMS parts) | 消息过长（超过 10 条短信） |
| `send_queued(phone, preview, parts)` | Queued: {phone} → "{preview}…" ({parts} part(s)) | 已入队：{phone} → "{preview}…"（{parts} 条） |

### 5.6 `/log`

| Key | EN | ZH |
|-----|----|----|
| `log_empty` | No SMS history. | 暂无短信记录。 |
| `log_header(n)` | Last {n} SMS: | 最近 {n} 条短信： |

### 5.7 `/queue`

| Key | EN | ZH |
|-----|----|----|
| `queue_empty` | Outbound queue is empty. | 发送队列为空。 |
| `queue_header(n)` | {n} pending: | {n} 条待发： |

### 5.8 `/block` and `/unblock`

| Key | EN | ZH |
|-----|----|----|
| `block_usage` | Usage: /block \<number\> | 用法：/block \<号码\> |
| `block_ok(phone)` | Blocked: {phone} | 已屏蔽：{phone} |
| `unblock_usage` | Usage: /unblock \<number\> | 用法：/unblock \<号码\> |
| `unblock_not_found(phone)` | {phone} is not in the block list. | {phone} 不在屏蔽名单中。 |
| `unblock_ok(phone)` | Unblocked: {phone} | 已解除屏蔽：{phone} |

### 5.9 `/pause` and `/resume`

| Key | EN | ZH |
|-----|----|----|
| `pause_ok(mins)` | Forwarding paused for {mins} min. | 转发已暂停 {mins} 分钟。 |
| `resume_already_active` | Forwarding is already active. | 转发已处于启用状态。 |
| `resume_ok` | Forwarding resumed. | 转发已恢复。 |

### 5.10 `/restart`

| Key | EN | ZH |
|-----|----|----|
| `restart_ok` | Rebooting… | 正在重启… |

### 5.11 Command descriptions (Telegram autocomplete)

| Command | EN | ZH |
|---------|----|----|
| `/help` | Show this help | 显示帮助 |
| `/status` | Show device health and stats | 设备状态 |
| `/send` | Send an SMS to a number | 发送短信 |
| `/log` | Last N forwarded messages (default 10) | 最近 N 条转发记录 |
| `/queue` | Show outbound queue | 查看发送队列 |
| `/block` | Add number to block list | 屏蔽号码 |
| `/unblock` | Remove number from block list | 解除屏蔽 |
| `/pause` | Pause forwarding for N minutes (default 60) | 暂停转发 |
| `/resume` | Resume forwarding | 恢复转发 |
| `/restart` | Reboot the device | 重启设备 |

---

## 6. EN wording changes

Minor corrections applied alongside the i18n refactor (EN behaviour unchanged):

| Location | Before | After | Reason |
|----------|--------|-------|--------|
| `status.rs` | `"registered"` / `"not registered"` | unchanged | fine as-is |
| `log_cmd.rs` | `"Last {} SMS:"` | `"Last {} SMS:"` | unchanged |
| All `"Usage: …"` strings | inconsistent capitalisation | normalised to `"Usage:"` prefix | consistency |

No EN strings change meaning. The only edits are the ones listed above.

---

## 7. Constraints

- No fifth NVS key (locale is compile-time, not runtime state).
- `src/i18n/` must not import from `bridge/`, `commands/`, or any concrete backend —
  it is a pure string library.
- ZH strings use Simplified Chinese and UTF-8. The Telegram API accepts UTF-8; no
  encoding change is required.
- `Command::description()` returns `&'static str`; the locale modules return `&'static str`
  for all static strings so the signature is unchanged.

---

## 8. Test impact

- Existing tests assert on English strings. After this RFC, tests must import
  `harness::i18n_en` (or similar) and match against `i18n::log_empty()` rather than
  the literal `"No SMS history."`.
- No new test infrastructure needed; the locale module is just a thin string layer.
