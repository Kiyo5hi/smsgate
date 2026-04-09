---
status: rejected
created: 2026-04-09
updated: 2026-04-09
---

# RFC-0008: Voice prompt to caller + bridging calls into Telegram

## Motivation

Two closely related ideas that the user surfaced while discussing
RFC-0005 (incoming call notify):

1. **Voice prompt to the caller.** When someone calls, instead of just
   hanging up on them, play a short voice message like "this number
   cannot be reached right now, please message on Telegram" so the
   caller understands what happened and what to do.
2. **Bridge the call into Telegram.** Even better: forward the call
   itself to a Telegram voice call so the user can pick up remotely
   and actually talk to the caller.

This RFC exists to record **why we are not building either** and to
prevent a future agent from burning a cycle re-evaluating them. Both
ideas share the same underlying impossibilities on this hardware and
software stack.

## Current state

Neither feature is implemented. RFC-0005 handles the "caller reaches
the bridge" case by hanging up after pushing a Telegram notification,
which is the best outcome achievable on this hardware.

## Decision: do not implement either feature

### Voice prompt to caller (`AT+CTTS` or similar)

**Blocker 1 (firmware-level, possibly recoverable with work).** A7670G
voice support is uncertain on this specific SIM variant. Upstream
`examples/VoiceCalls/VoiceCalls.ino` explicitly warns:

> `A7670E-LNXY-UBL` this version does not support voice and SMS functions.

That is a different sub-variant (LNXY-UBL) from the one on this board
(`A7670G-LLSE`), but it demonstrates that the A7670 family has
voice-support variance between sub-models. Implementing this would
require first verifying with `AT+CTTS=2,"hello"` (or whichever
TTS command the module exposes) on the user's exact modem. That is
plausibly doable, but it is the smaller blocker.

**Blocker 2 (hardware-level, NOT recoverable without soldering).** Even
if `+CTTS` or an equivalent command works, the audio output goes out
the **SPK pin of the modem**, which on the LilyGo T-A7670X requires a
speaker physically welded to the SPK pad on the back of the board.
The user's board has no such hardware modification and the user is
not going to add one. Without a speaker, the TTS engine runs with no
acoustic output: the modem generates PCM samples and throws them
into a pin that is not connected to anything. The caller hears
nothing different from silence.

Note: the reason a speaker is needed *at all* for the caller to hear
the TTS is that the A76xx family's voice path expects the audio to
loop through the analog codec (SPK/MIC), not through a pure-digital
path where bytes from `+CTTS` go straight to the GSM voice channel.
Some modem families do offer a digital-only "play prompt to caller"
path, but the A76xx datasheet does not.

**Alternative already covered by RFC-0005.** `AT+CHUP` immediately
after pushing the Telegram notification. The caller hears busy tone,
which most carriers translate into a "user busy" or "call failed"
voice message of their own on the caller's handset. This is the
acceptable workaround and it is what 0005 already specifies.

**Alternative outside firmware (carrier-side).** `AT+CCFC` configures
**Conditional Call Forwarding** on the SIM (GSM 02.82):

- `CFNRy` (call forwarding on no-reply)
- `CFNRc` (call forwarding on not reachable)
- `CFU` (unconditional call forwarding)

Set the forward target to the user's real cell number. Their real
phone's voicemail then plays whatever greeting the user configures
on that phone, which is infinitely more flexible than anything we
could do on-device. This requires **zero firmware changes** — the
user can run `AT+CCFC` manually once via the existing serial path,
or from their carrier's web portal / dialer. That is the right
answer for this problem and it is documented here so the next agent
doesn't try to build it into the firmware.

### Bridge the call into Telegram

**Blocker 1 (protocol-level, not recoverable).** Telegram's bot API
has **zero** voice call capability. Bots cannot place voice calls,
receive voice calls, or bridge calls. Full stop. Telegram user-
account voice calls run on **MTProto**, not the bot API, and require
a full Telegram client implementation (login with phone number, 2FA,
session storage, etc.) — which is not usable from a microcontroller
in any practical sense. There is no "forward this call to my
Telegram" API anywhere in the Telegram ecosystem. A future agent who
thinks this has changed should link the relevant Telegram Bot API
changelog entry before reopening.

**Blocker 2 (signal-processing-level, not recoverable on ESP32).**
Even if the protocol existed, bridging a GSM voice call to a VoIP
call requires:

- Pulling GSM PCM off the modem (a cellular audio stream, typically
  8kHz PCM over PCM interface, I²S, or analog codec)
- Jitter buffering, echo cancellation, acoustic gain control
- Transcoding between GSM-FR/AMR-NB and whatever codec the IP side
  uses (Opus for Telegram, G.711 for SIP)
- Running an RTP or SIP stack with SRTP/DTLS-SRTP
- Bidirectional, real-time, sub-200ms round-trip budget

This is a Raspberry Pi + Asterisk class problem, not an ESP32 class
problem. The ESP32 has neither the DSP horsepower nor the audio I/O
path (we don't even have a speaker, see above) to attempt it. Any
estimate of "I'll just forward the audio" underestimates the problem
by two orders of magnitude.

**Alternative (carrier-side, same as voice prompt).** `AT+CCFC` with
the forward target set to the user's real cell phone number. The
carrier bridges the call at the network level, which is exactly the
right layer for it. Zero firmware involved.

## Plan

Do not implement. If a future agent re-evaluates either feature, the
answer is **still no** for the reasons above, and the right
redirection is `AT+CCFC` call forwarding configured once on the SIM
(outside this firmware entirely).

## Notes for handover

- **Cross-reference.** The live behaviour for incoming calls is
  specified in RFC-0005 (hang up + Telegram notification). This RFC
  documents only what we are NOT doing. RFC-0005 already links to
  this file in its "future extension" note; this paragraph exists to
  make the cross-link bidirectional so an agent reading 0008 first
  can find the live specification.
- This RFC is intentionally permanent at `status: rejected`. It is
  not "deferred" — there is no conceivable change to the user's
  requirements that would turn it into a reasonable firmware project
  on this hardware stack.
- If the user ever switches to a dev board with a speaker soldered on
  **and** an A76xx variant that does support `+CTTS`, the "voice
  prompt to caller" idea could be reopened. In that case, delete
  this RFC's rejection of that specific feature and write a new
  `0009-voice-prompt-to-caller.md` instead of editing this one —
  keeps the rejection history clear.
- Call bridging to Telegram stays rejected regardless of hardware.
  The blocker is Telegram's API surface, not anything we can fix.
- The user should be pointed at `AT+CCFC` call forwarding as the
  real solution for the underlying use case ("I want to actually
  talk to people who call this SIM when I'm not holding it"). That
  is a carrier-configuration problem, not a firmware problem, and
  nothing in this repo should try to own it.
