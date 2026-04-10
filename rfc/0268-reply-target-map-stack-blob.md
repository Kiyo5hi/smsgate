---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0268: ReplyTargetMap save/load allocates 5608-byte buffer on the stack

## Motivation

`ReplyTargetMap::save()` and `ReplyTargetMap::load()` each declare:

```cpp
uint8_t buf[kBlobSize];
```

`kBlobSize = sizeof(Header) + sizeof(Slot) * kSlotCount = 8 + 28 × 200 = 5608 bytes`.

`save()` is called from `put()` which is called from:
- `SmsHandler::forwardSingle()` / `forwardConcat()` on every forwarded SMS
- The call-handler callback in `main.cpp` on every inbound call

Call stack at `save()` entry (worst case from the loop task):

```
loop()            ~100 B
handleSmsIndex()  ~400 B
forwardSingle()   ~200 B
put()             ~ 50 B
save()           5608 B   ← local buf
                 ──────
                 ~6400 B  / 8192 B loop task stack
```

~1800 B of headroom — acceptable today, but fragile. Any addition to the
call chain (e.g. a deeper format helper or an extra String local) could
cross the overflow threshold silently.

RFC-0263 already fixed the same pattern for the `sched_queue` blob
(7856 bytes). The same treatment is needed here.

## Plan

Heap-allocate the buffer in both `save()` and `load()` using
`std::make_unique<uint8_t[]>`:

```cpp
// Before (save):
uint8_t buf[kBlobSize];

// After (save):
auto bufOwn = std::make_unique<uint8_t[]>(kBlobSize);
uint8_t *buf = bufOwn.get();
```

Same change in `load()`.

## Notes for handover

- `#include <memory>` is already present in the .cpp translation unit.
- `kBlobSize` is a `constexpr` compile-time constant, so
  `make_unique<uint8_t[]>(kBlobSize)` allocates the exact right amount.
- No functional change — only the allocation location changes from
  stack to heap.
- If the heap is exhausted `make_unique` will `std::terminate`; on
  ESP32 that resets the device, which is the correct behaviour for a
  fatal memory shortage.
