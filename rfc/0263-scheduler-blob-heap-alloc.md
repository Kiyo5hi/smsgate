---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0263: Heap-allocate scheduler NVS blob to prevent stack overflow

## Motivation

RFC-0261 expanded the per-slot body field from 128 → 1531 bytes, making the
full 5-slot scheduler blob `1 + 5 × 1571 = 7856 bytes`.  Both the persist
lambda and the setup() load block allocate this as a stack array:

```cpp
uint8_t blob[7856] = {};
```

The ESP32 Arduino loop task stack defaults to 8192 bytes.  7856 bytes for the
blob alone, plus the existing call-frame depth when tick() fires (loop() →
tick() → lambda), leaves fewer than 336 bytes for all other variables and
return addresses — a near-certain stack overflow on the first scheduler
persist call, and possible corruption in setup() as well.

The previous v0x02 layout had `char body[128]` → blob = 841 bytes → safe.
RFC-0261 made the allocation 9× larger without addressing stack impact.

## Plan

Replace both `uint8_t blob[N] = {}` stack arrays with heap allocations via
`std::unique_ptr<uint8_t[]>`:

```cpp
const size_t blobSize = 1 + kNumSlots * kSlotSize;
auto blob = std::make_unique<uint8_t[]>(blobSize); // heap, not stack
memset(blob.get(), 0, blobSize);
blob[0] = 0x03;
// ...access via blob[i] or blob.get() + offset...
realPersist.saveBlob("sched_queue", blob.get(), blobSize);
```

And for load:

```cpp
auto blob = std::make_unique<uint8_t[]>(kExpectedSize);
memset(blob.get(), 0, kExpectedSize);
if (realPersist.loadBlob("sched_queue", blob.get(), kExpectedSize) == kExpectedSize
    && blob[0] == 0x03)
```

Add `#include <memory>` to main.cpp.

### Heap impact

7856 bytes allocated once per persist call, freed on exit from the lambda.
Persist calls are infrequent (only on scheduler mutations — schedule, cancel,
fire).  With ≥80 KB normally available heap, this is safe.

The setup() load is a one-time allocation before the main loop starts, so it
is even less of a concern.

## Notes for handover

- The only change is the allocation site; the blob layout and version byte
  are unchanged.
- `char body[kBodyLen + 1]` (1532 bytes) in the load loop is also moved to
  heap to remove the secondary stack pressure in setup().
- No logic changes; no test changes needed.
