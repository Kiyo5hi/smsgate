# Troubleshooting

## SMS Arrives Only After Reboot

Observation from the July 2026 session: a delayed SMS appeared after restart.
One plausible explanation is a stored SMS whose CMTI notification was missed;
late network delivery or a forwarding failure is not excluded by that fact alone.

Capture CNMI, CPMS, storage listing and transport errors before restarting when
possible. The current uncommitted recovery change periodically checks CNMI and
sweeps ME; see [handoff.md](handoff.md) for its exact verification limits.

Use CNMI=2,1,0,0,0 for stored SMS plus CMTI notifications. Main has a two-line
direct CMT handling path, but direct delivery is not the configured mode.
A periodic sweep recovers stored messages; it cannot recover SMS never delivered
by the carrier.

Select the memory named by CMTI before CMGR. Avoid blindly selecting
CPMS="SM","SM","SM": on the reference board this historically produced a
notification flood and UART corruption. A failed CPMS query does not prove
the selected memory or the reason for failure.

Forwarding failures may leave a slot for retry. Slot deletion also has special
cases (invalid PDU, partial multipart processing); consult
[sms_handler.rs](../src/bridge/sms_handler.rs), not a blanket claim of guaranteed
end-to-end delivery.

## LTE and Data Charges

See [decisions.md](decisions.md). Registration and IP traffic are distinct.
Do not use CGATT=0 as an SMS-only data guard on LTE.
A modem command returning OK does not establish zero carrier billing.
Capture registration/context state and compare carrier records.

## Modem Silent After ESP Reset

ESP and modem can retain independent power state. A blind PWRKEY pulse may turn
an already-running modem off. Inspect the board power sequence and AT probe
before changing pins or adding pulses. If it remains silent, a full USB/power
cycle can reset both domains; an ESP-only restart may not.

Boot usually progresses through AT probe, SIM ready, CNMI, registration, WiFi,
storage sweep, ready in tens of seconds. Registration timeout permits startup
to continue; later registration is possible but not guaranteed.

## Serial Capture

Detect the current port; the July 2026 R2 was on COM5, which is not a portable
default. Use a Python environment with pyserial installed. Open passively:

```python
import serial
port = serial.Serial()
port.port = "<PORT>"
port.baudrate = 115200
port.timeout = 0.2
port.dtr = False
port.rts = False
port.open()
# Read with a bounded deadline and close the port afterward.
```

Do not open a second monitor or send modem AT commands to the ESP console unless
the firmware explicitly supports a passthrough.

Legacy PlatformIO/C++ context: exception_decoder can hide noninteractive logs;
use a raw monitor. TinyGSM maintain() historically consumed unhandled URCs.
These are legacy lessons, not descriptions of the current Rust driver.

## Registration Responses

is_urc excludes CREG/CGREG/CEREG query responses deliberately. Classifying them
as URCs can steal responses from status queries. Changing unsolicited
registration mode requires updating the command/URC handling together.

## ESP-IDF Build Cache

SDK defaults seed configuration; cached sdkconfig may override changed defaults.
Inspect the selected target's esp-idf-sys output and remove only that generated
sdkconfig when regeneration is necessary.

A fresh dependency build can fail because ESP-IDF needs the partition CSV before
the project's build.rs copies it. Inspect the missing path and selected layout,
then copy the correct board CSV to that exact generated path and retry.
Do not choose an arbitrary first esp-idf-sys output directory or use the R2
CSV for an S3 build.
