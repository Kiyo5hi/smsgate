// Storage for the one global declared in Arduino.h.
// Kept separate from the header so every translation unit in the host
// test binary sees the same Serial instance.

#include "Arduino.h"

SerialStub Serial;
