---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0118 — Custom device label

## Motivation

When an operator runs more than one bridge, all devices send identical
"🚀 Bridge online" banners to the same Telegram chat. A custom label
like "Office SIM" or "Site-B" lets the operator immediately know which
device sent a message without checking ICCID or IMEI.

## Plan

1. Add two setters to `TelegramPoller`:
   ```cpp
   setLabelGetFn(std::function<String()> fn);     // returns current label
   setLabelSetFn(std::function<void(const String&)> fn); // persists new label
   ```

2. Add commands:
   - `/label` — reply with current label or "(no label set)".
   - `/setlabel <name>` — validate (max 32 printable chars, non-empty),
     save via setFn_, reply "✅ Label set to: <name>".
     No argument → reply usage.

3. In `main.cpp`:
   - Add `static String s_deviceLabel;` at file scope.
   - In `setup()`, after NVS init, load the label:
     ```cpp
     char labelBuf[33] = {};
     size_t n = realPersist.loadBlob("device_label", labelBuf, 32);
     if (n > 0) { labelBuf[n] = '\0'; s_deviceLabel = String(labelBuf); }
     ```
   - Wire `setLabelGetFn([]() { return s_deviceLabel; })` and
     `setLabelSetFn([](const String &l) { s_deviceLabel = l; realPersist.saveBlob("device_label", l.c_str(), l.length()); })`.
   - Append label to boot banner header if set:
     ```cpp
     if (s_deviceLabel.length() > 0)
         bootHeader += String("[") + s_deviceLabel + String("] ");
     ```

4. Tests:
   - `/label` with label set → replies with label.
   - `/label` with no label → "(no label set)".
   - `/setlabel foo` → setter called with "foo", confirmation reply.
   - `/setlabel` no arg → usage reply.
   - `/setlabel` too long → error reply.

## Notes for handover

Label validation: only printable ASCII (0x20–0x7E), max 32 chars.
Spaces are allowed (e.g. "Office SIM" is valid). NVS key is
"device_label" (12 chars, well within the 15-char key limit).
