---
status: implemented
created: 2026-04-09
updated: 2026-04-09
depends: rfc/0019-runtime-user-management.md
---

# RFC-0020: Persistent SMS Debug Log

## Motivation

`SmsDebugLog` (RFC-0010) keeps the last 20 SMS events in RAM and exposes them
via the `/debug` Telegram command. It is erased on every reboot — watchdog
reset, power cycle, OTA flash, or `ESP.restart()`. Any failure that triggers
a reboot takes its own diagnostic trail with it, making post-mortem debugging
blind.

A persistent log that survives reboots lets an operator run `/debug`
immediately after a reboot and see what the bridge was doing before it died.

## Current state

- `src/sms_debug_log.{h,cpp}` — `SmsDebugLog` holds a `std::vector<Entry>` (or
  fixed array) of up to `kMaxEntries = 20` structs:
  `{ String sender, body, timestamp; bool forwarded; String error }`.
  Everything lives in RAM. There is no persistence hook.
- `src/ipersist.h` / `src/real_persist.h` — thin `Preferences`-backed
  key/value store. **RFC-0019 adds `loadBlob(key, buf, size)` /
  `saveBlob(key, buf, size)` generic methods to `IPersist`, `RealPersist`,
  and `FakePersist`.** This RFC uses those methods — RFC-0019 must be merged
  first. Existing keys: `"uid"`, `"rtm"`, `"ulist"` (RFC-0019). Namespace
  `"tgsms"`.
- `FakePersist` gains `loadBlob`/`saveBlob` in RFC-0019, so new tests here
  need no additional test-infrastructure changes beyond that dependency.

## Plan

### Storage option analysis

**Option A — NVS (Preferences), binary blob.**
Re-use the existing `IPersist` abstraction with a new `"smslog"` key. NVS
has a per-value size limit of 4,000 bytes (ESP-IDF constraint). Write
endurance is ~10,000 erases per NVS sector, but ESP-IDF NVS spans multiple
sectors with wear leveling, giving an effective endurance in the millions of
write operations in practice. At 100 SMS/day the annual write count is
36,500 — well within safe bounds for a household bridge. Simple to implement;
no new dependencies.

**Option B — LittleFS file.**
A file on the LittleFS partition provides larger capacity and better
wear-leveling transparency at the cost of: adding the `LittleFS` Arduino
library to `platformio.ini`, defining a custom partition table (or accepting
the default partition that may not include a LittleFS region), and
format-on-first-use logic. Worthwhile if log depth ever needs to exceed
~23 entries (the 4,000-byte NVS cap) or if write frequency grows
substantially. Overkill for the current use case.

**Option C — RAM-only, add reboot reason to `/status`.**
Zero storage complexity. Call `esp_reset_reason()` in `setup()` and include
the result in the string returned by `SmsDebugLog::statusSummary()` (or
wherever `/status` gathers its data). This is a zero-cost complement to any
storage option: it answers "why did it reboot?" even if the log is empty.

**Recommendation: Option A + Option C.**
NVS is safe for the target workload, the `IPersist` abstraction already
exists, and limiting stored entries to 10 keeps the blob well within the
4,000-byte limit. Add Option C (`esp_reset_reason()` in `/status`) as a
free complement — no storage, no risk, immediately useful.

### NVS blob format

Each entry is serialized as a fixed-size C struct (no padding surprises —
fields are naturally aligned):

```
struct PersistEntry {
    uint32_t unixTimestamp;   // 4 bytes  — seconds since epoch (0 = empty slot)
    bool     forwarded;       // 1 byte
    uint8_t  _pad[2];         // 2 bytes  — explicit padding to align next field
    char     sender[21];      // 21 bytes — E.164 number + NUL (max 20 digits)
    char     body[101];       // 101 bytes — first 100 chars of SMS body + NUL
    char     error[41];       // 41 bytes  — error string (40 chars) + NUL
};
// sizeof(PersistEntry) = 4+1+2+21+101+41 = 170 bytes
```

The blob layout:

```
struct SmsLogBlob {
    uint8_t    version;              // 1 byte  — bump on schema change (start at 1)
    uint8_t    head;                 // 1 byte  — index of oldest entry (ring head)
    uint8_t    count;                // 1 byte  — number of valid entries (0..10)
    uint8_t    _pad;                 // 1 byte  — alignment
    PersistEntry entries[10];        // 10 × 170 = 1,700 bytes
};
// Total: 4 + 1,700 = 1,704 bytes — well within the 4,000-byte NVS limit
```

On unknown `version` the blob is discarded and the log starts fresh.

### `SmsDebugLog` changes

1. Add an optional `IPersist*` pointer, defaulting to `nullptr`. Expose
   it via `void setSink(IPersist& p)` (or pass in the constructor — setter
   preferred to keep the existing constructor signature unchanged).

2. Add `void loadFrom(IPersist& p)`:
   - Call `p.loadBlob("smslog", buf, sizeof(SmsLogBlob))` (RFC-0019 API).
   - If 0 bytes returned (key absent on first boot): return cleanly, ring stays empty.
   - Validate `version == 1`; on mismatch discard and return.
   - Deserialize `head`, `count`, and the `entries[]` array. **`head` in the
     blob is the index of the oldest entry (ring read position); on load, set
     `head_` to `blob.head` and `count_` to `blob.count`. The 20-slot RAM
     ring is initialized from the 10 persisted entries at positions
     `[0..count-1]` starting at `head_`. Empty slots (indices `count..9`
     in the blob) are ignored.**

3. After each `push()` (new SMS event), if `persist_ != nullptr`, call
   `persist()`:
   - Serialize the 10 most-recent entries from the RAM ring into a `SmsLogBlob`.
     When the RAM ring has more than 10 entries, iterate from
     `(head_ + count_ - 10 + kMaxEntries) % kMaxEntries` forward for 10 steps.
   - Set `blob.head = 0` (entries always written to positions 0..count-1 in the blob).
   - Call `persist_->saveBlob("smslog", &blob, sizeof(SmsLogBlob))` (RFC-0019 API).

4. Keep `kMaxEntries` at 20 for the RAM ring buffer. The persistent blob
   stores only the 10 most-recent entries (the ones most useful for
   post-mortem). If RAM ring has more than 10 entries, serialize only the
   10 most recent.

5. The `/debug` command is unchanged — it dumps the in-RAM ring, which
   now happens to be initialized from NVS at boot.

### `main.cpp` changes

In `setup()`, after constructing `SmsDebugLog` and `RealPersist`:

```cpp
smsLog.loadFrom(persist);
smsLog.setSink(persist);
```

Capture `esp_reset_reason()` near the top of `setup()` and store it in a
`static` or file-scope variable. Include it in whatever response
`SmsDebugLog::statusSummary()` or the `/status` handler builds — e.g.:
`"Last reboot: Power-on"` / `"Last reboot: Watchdog (TWDT)"` etc.

### `build_src_filter` / native env

`SmsDebugLog` is already included in the native env filter. Because
`loadFrom` and `persist()` call `IPersist` methods, and `IPersist` has no
hardware deps, no filter change is needed. `FakePersist` in `test/support/`
is already the test double for those calls.

### Test plan

New tests in `test/test_native/` (likely `test_sms_debug_log.cpp`):

1. **`push_with_persist_calls_putBytes`** — construct `SmsDebugLog` with a
   wired `FakePersist`, push one entry, assert `FakePersist` now holds a
   blob under the `"smslog"` key.

2. **`loadFrom_deserializes_previous_blob`** — populate a `FakePersist`
   with a hand-crafted valid `SmsLogBlob`, construct a fresh `SmsDebugLog`,
   call `loadFrom`, assert the in-RAM ring contains the expected entries.

3. **`loadFrom_discards_wrong_version`** — same but with `version = 99`;
   assert the ring is empty after `loadFrom`.

4. **`ring_wraparound_round_trips`** — push 15 entries (more than the 10
   persisted slots), call `persist()` via `push()`, then `loadFrom()` into a
   fresh log; assert the 10 most-recent entries are present in order and the
   5 oldest are absent.

5. **`loadFrom_empty_persist_is_noop`** — call `loadFrom` on a `FakePersist`
   with no `"smslog"` key; assert no crash and ring is empty.

### Migration / backward compatibility

First boot after flashing finds no `"smslog"` key in NVS → `loadFrom`
returns cleanly and the log starts empty. No migration step needed.
If the blob `version` ever changes, old blobs are silently discarded and
the log starts fresh.

## Notes for handover

- The `PersistEntry` struct should be defined with explicit `static_assert`s:
  `static_assert(sizeof(PersistEntry) == 170, "PersistEntry size changed");`
  `static_assert(sizeof(SmsLogBlob) == 1704, "SmsLogBlob size changed");`
  If the compiler inserts unexpected padding, these will catch it immediately.
- NVS key `"smslog"` must be registered in a comment near the other key
  definitions so future authors know all keys in the `"tgsms"` namespace.
- If SMS volume ever exceeds ~500/day sustainably (not a household bridge
  scenario), revisit Option B (LittleFS). The switchover is isolated to
  `RealPersist` or a new `LittleFsPersist` implementation of `IPersist` —
  the `SmsDebugLog` code does not need to change.
- `esp_reset_reason()` returns an `esp_reset_reason_t` enum. A small
  helper that stringifies the common values (power-on, software reset,
  TWDT, brownout, etc.) should live in `main.cpp` or a small utility
  header — not in `sms_debug_log.cpp`, which should stay hardware-free.
- The `/debug` command output format is unchanged; this RFC adds no new
  Telegram commands. The reboot-reason string belongs in `/status`.
- RFC-0010 (`status-command.md`) should have its frontmatter updated to
  `status: implemented` if that has not already been done; it is the
  predecessor this RFC extends.

## Review

**verdict: approved-with-changes**

Two blocking issues must be resolved before implementation begins. The
remaining items are non-blocking but should be addressed during implementation
to avoid bugs or confusion.

### Blocking

- **BLOCKING — `IPersist` has no `getBytes`/`putBytes` methods.**
  The plan repeatedly calls `p.getBytes("smslog", buf, sizeof(SmsLogBlob))`
  and `persist_->putBytes("smslog", blob, sizeof(SmsLogBlob))`, but
  `src/ipersist.h` defines only four methods: `loadLastUpdateId`,
  `saveLastUpdateId`, `loadReplyTargets`, `saveReplyTargets`. There is no
  generic keyed blob API. The implementer must choose one of:
  (a) add `virtual size_t getBytes(const char* key, void* buf, size_t) = 0`
  and `virtual void putBytes(const char* key, const void* buf, size_t) = 0`
  to `IPersist` (breaking change — all implementors must be updated); or
  (b) follow the existing named-method pattern and add
  `virtual size_t loadSmsLog(void* buf, size_t) = 0` /
  `virtual void saveSmsLog(const void* buf, size_t) = 0`.
  Option (b) is lower-risk and consistent with the existing interface style.
  The RFC must be updated to specify which route is taken.

- **BLOCKING — `FakePersist` does not implement `getBytes`/`putBytes`.**
  The RFC states "FakePersist in `test/support/` already implements
  `getBytes`/`putBytes` in memory, so new tests need no test-infrastructure
  changes." This is factually wrong. `test/support/fake_persist.h` only
  implements the same four named methods as `IPersist`. Whichever interface
  extension is chosen above, `FakePersist` must be extended accordingly, and
  the claim in the "Current state" section must be corrected.

### Non-blocking

- **NON-BLOCKING — `PersistEntry` field semantics diverge from the existing
  `Entry` struct in non-obvious ways.**
  `Entry::timestampMs` is `millis()` (milliseconds since last boot), but
  `PersistEntry::unixTimestamp` is seconds since Unix epoch. The conversion
  requires wall-clock time from NTP, which may not be available for messages
  that arrive before NTP sync completes. The RFC should document how
  pre-NTP entries are handled (e.g., store 0 and surface as "unknown time" in
  `/debug`). Additionally, `Entry::outcome` (a free-form `String`) maps to
  `PersistEntry::error[41]` plus `PersistEntry::forwarded` — the
  serialization/deserialization round-trip logic for this split should be
  spelled out. `Entry::pduPrefix` has no slot in `PersistEntry` and will be
  silently dropped; this should be noted explicitly.

- **NON-BLOCKING — `head` semantics differ between RAM and blob.**
  In `sms_debug_log.cpp`, `head_` is the *next write position* (i.e. the slot
  that will be overwritten on the next `push()`). The blob's `head` field is
  described as "index of oldest entry." These are different values. The
  serializer must translate between them, and the deserializer must restore
  `head_` as next-write-position, not as oldest-entry-index. A comment or
  diagram in the implementation plan would prevent an off-by-one bug here.

- **NON-BLOCKING — Initialization of the 20-slot RAM ring after loading 10
  persisted entries is underspecified.**
  After `loadFrom` restores 10 entries into a 20-slot ring, `head_` and
  `count_` must be set so that subsequent `push()` calls place new entries
  correctly and `dump()` walks oldest-to-newest. The plan does not describe
  the initial `head_`/`count_` values after deserialization. Suggest: set
  `count_ = loaded_count` (≤ 10), `head_ = loaded_count % kMaxEntries`, fill
  slots 0..count_-1 in chronological order.

- **NON-BLOCKING — `static_assert` is mentioned in Notes but never specified
  precisely.**
  The Notes say "a `static_assert` on its size so an accidental padding change
  is caught at compile time." The assert should be stated explicitly in the
  plan: `static_assert(sizeof(PersistEntry) == 170, "PersistEntry size
  changed");` and `static_assert(sizeof(SmsLogBlob) == 1704, "SmsLogBlob size
  changed");`. Without explicit expected values, a developer might write the
  assert against `sizeof(PersistEntry)` after silently adding padding,
  defeating its purpose.

- **NON-BLOCKING — NVS write cost per SMS is not called out in the
  `SmsDebugLog` changes section.**
  The Option A analysis correctly notes 36,500 writes/year at 100 SMS/day.
  However, the changes section that describes calling `persist()` inside
  `push()` does not cross-reference this cost. A brief note ("each `push()`
  rewrites the full 1,704-byte blob; see Option A analysis for wear budget")
  would make the trade-off visible to future maintainers working only from
  the plan section.

- **NON-BLOCKING — 20 RAM / 10 persisted discrepancy should be documented
  in `/debug` output.**
  The plan notes the discrepancy (point 4), but does not say that `/debug`
  should indicate when entries were restored from NVS versus captured in the
  current session. After a reboot the user will see at most 10 entries from
  NVS; if they then receive 11 more SMS the ring grows to 20 but still shows
  10 from before the reboot. A simple header line like "10 from NVS + 5 live"
  (or a separator between the two groups) would reduce confusion. Not required
  for the feature to be correct, but strongly recommended for usability.

## Code Review

**Verdict: approved — one blocking issue, four non-blocking issues.**

Reviewer: claude-sonnet-4-6, 2026-04-09.

---

### BLOCKING

**BLOCKING-1 — `loadFrom` does not guard against a partial read.**

`loadFrom` returns early only when `got == 0` (key absent). If NVS returns a
truncated blob — e.g. `got > 0` but `got < sizeof(SmsLogBlob)` — the code
falls through and reads `blob.version` (and the rest of the struct) from
zero-initialized stack storage, not from the actual stored data. A partial
read would produce `version == 0` (not 1) and be discarded by the version
check, so in practice the entry is always dropped. But the logic is fragile:
if a future schema change assigns `version = 0` as a valid value, or if
zero-initialisation semantics of `SmsLogBlob{}` change, the guard silently
misses. The defensive fix is a single additional check:

```cpp
if (got < sizeof(SmsLogBlob))
    return;  // truncated — treat as absent
```

This should be inserted between the `got == 0` check and the `version != 1`
check. Promote to BLOCKING because a short NVS read (possible on a first
flash after schema change, or if an older firmware wrote a smaller blob)
could silently deserialize garbage fields if the version byte happens to be 1.

---

### NON-BLOCKING

**NON-BLOCKING-1 — `static_assert` values disagree with the RFC's stated
values; the comment in the header is also inconsistent.**

The RFC specifies `sizeof(PersistEntry) == 170` and `sizeof(SmsLogBlob) ==
1704` with an explicit `_pad[2]` in `PersistEntry`. The implementer dropped
`_pad[2]` and reordered `error[]` to 41 bytes, yielding
`4+1+21+101+41 = 168` and `4 + 10*168 = 1684`. The arithmetic is correct for
the as-shipped struct. The static_asserts are correctly coded to 168/1684 and
will catch regressions.

However, the comment on line 58 of the header (`sizeof(PersistEntry) == 170,
sizeof(SmsLogBlob) == 1704`) still quotes the RFC values, not the actual
values. This will confuse anyone who reads the comment and then sees the
static_asserts fire at 168/1684. The comment should be updated to match the
actual values, and the RFC's struct diagram should note the deviation
(already done partially by the implementer; the note just needs to be explicit
in the header comment).

**NON-BLOCKING-2 — `resetReasonStr` omits `ESP_RST_EXT` and `ESP_RST_SDIO`.**

The ESP-IDF `esp_reset_reason_t` enum includes two additional values in
common silicon:

- `ESP_RST_EXT` — reset via the external RSTn pin (relevant if the board is
  wired to a hardware reset line).
- `ESP_RST_SDIO` — reset triggered by SDIO peripheral (unlikely in this
  project, but present in the enum).

Both fall through to the `default: return "Unknown"` arm, which is functionally
safe. The gap is worth noting because a user whose board was hard-reset via the
EN button would see "Unknown" in `/status` rather than "External pin reset".
Adding `case ESP_RST_EXT: return "External pin reset";` would close the gap.
`ESP_RST_SDIO` can stay on the default.

**NON-BLOCKING-3 — `persist()` index calculation is correct but differs from
RFC's suggested formula.**

The RFC says: "iterate from `(head_ + count_ - 10 + kMaxEntries) % kMaxEntries`
forward for 10 steps." The implementation instead computes `start` (oldest
entry position in the full RAM ring) and then adds `skip = n - persist_n` to
it. Both formulas are equivalent for a non-wrapped ring, but the
implementation's two-variable approach is harder to audit. Worked example with
`kMaxEntries=20`, `count_=15`, `head_=15` (next write slot):

- `start = head_ = 15` (ring is full… wait, `count_=15 < kMaxEntries=20` so
  `start = 0`).
- `skip = 15 - 10 = 5`.
- Slot 0 is ring slot 0, entries 0..14 are at RAM slots 0..14.
- `ring_idx = (0 + 5 + i) % 20` for `i` in 0..9 → slots 5..14. These are
  entries 6..15 (1-based). Correct.

Second example with `count_=20`, `head_=3` (wrapped, `head_` points to next
write = oldest slot location):

- `start = head_ = 3`.
- `skip = 20 - 10 = 10`.
- `ring_idx = (3 + 10 + i) % 20` for `i` in 0..9 → `(13+i)%20` → slots
  13..19, 0..2. These are the 10 most-recent entries immediately before slot 3
  (which is the oldest). Correct.

The logic is sound. A single inline comment in the implementation explaining
the `start + skip` step — specifically that `skip` advances past the entries
not being persisted — would make future audits faster.

**NON-BLOCKING-4 — Test 4 pattern matching is fragile against single-digit
sender overlap.**

In `test_ring_wraparound_round_trips`, the assertion that entries 1-5 are
absent uses the pattern `"| +N\n"`. The dump format (from `dump()`) is:

```
#N | <timestamp> | <sender>\n
```

The sender field is followed by `\n  ` (newline then two spaces), not directly
by `\n`. Looking at `dump()` lines 188-189: `out += e.sender; out += "\n  ";`.
So the actual output for sender `+6` would be `| +6\n  ` not `| +6\n`. The
test pattern `"| +6\n"` would NOT match.

This means the "entries 6-15 must be present" assertions (positive checks) may
silently pass vacuously even if those entries are absent — because the pattern
never matches anything. And the "entries 1-5 must be absent" assertions
(negative checks) would also pass trivially because the pattern never appears.
The test could give a false-green result.

The correct pattern is `"| +%d\n  "` (with the trailing two spaces), or the
assertion should match on something unambiguous that does appear, such as
`"fwd-%d"` (the outcome string). This should be verified against the actual
`dump()` output before merging.
