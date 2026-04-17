//! Mock implementations of ModemPort and Messenger.

use crate::im::{InboundMessage, MessageId, MessageSink, MessageSource, MessengerError};
use crate::modem::{AtResponse, AtTransport, ModemError, ModemPort};
use std::time::Duration;
use std::collections::VecDeque;

// ---------------------------------------------------------------------------
// ScriptedModem
// ---------------------------------------------------------------------------

/// A scripted AT command/response pair.
pub struct AtScript {
    pub command_suffix: String, // what comes after "AT" (without CRLF)
    pub response_body: String,
    pub ok: bool,
}

/// Programmable modem mock.
///
/// Feed it a script of (command, response) pairs.
/// Unconsumed script steps fail the test via `check_consumed()`.
pub struct ScriptedModem {
    script: VecDeque<AtScript>,
    urc_queue: VecDeque<String>,
    pub sent_pdus: Vec<(String, u8)>, // (hex, tpdu_len)
    pub hang_up_count: usize,
}

impl ScriptedModem {
    pub fn new() -> Self {
        ScriptedModem {
            script: VecDeque::new(),
            urc_queue: VecDeque::new(),
            sent_pdus: Vec::new(),
            hang_up_count: 0,
        }
    }

    /// Push an expected (command_suffix, body, ok) interaction.
    pub fn expect(mut self, cmd: &str, body: &str, ok: bool) -> Self {
        self.script.push_back(AtScript {
            command_suffix: cmd.to_string(),
            response_body: body.to_string(),
            ok,
        });
        self
    }

    /// Push a URC that will be returned by the next `poll_urc()`.
    pub fn push_urc(mut self, urc: &str) -> Self {
        self.urc_queue.push_back(urc.to_string());
        self
    }

    /// Inject a URC at runtime (after construction).
    pub fn inject_urc(&mut self, urc: &str) {
        self.urc_queue.push_back(urc.to_string());
    }

    /// Assert all scripted steps were consumed. Panics if any remain.
    pub fn check_consumed(&self) {
        if !self.script.is_empty() {
            let remaining: Vec<_> = self.script.iter().map(|s| &s.command_suffix).collect();
            panic!("ScriptedModem: {} unconsumed script steps: {:?}", self.script.len(), remaining);
        }
    }
}

impl AtTransport for ScriptedModem {
    fn send_at(&mut self, cmd: &str) -> Result<AtResponse, ModemError> {
        let Some(step) = self.script.pop_front() else {
            return Err(ModemError::AtError(format!("unexpected AT command: AT{}", cmd)));
        };
        if step.command_suffix != cmd {
            panic!(
                "ScriptedModem: expected AT{} but got AT{}",
                step.command_suffix, cmd
            );
        }
        Ok(AtResponse { body: step.response_body, ok: step.ok })
    }

    fn poll_urc(&mut self) -> Option<String> {
        self.urc_queue.pop_front()
    }

    fn write_raw(&mut self, _data: &[u8]) -> Result<(), ModemError> {
        // ScriptedModem overrides send_pdu_sms, so write_raw is never reached.
        unreachable!("ScriptedModem: write_raw should not be called directly")
    }

    fn wait_for_prompt(&mut self, _prompt: u8, _timeout: Duration) -> bool {
        // ScriptedModem overrides send_pdu_sms, so this is never reached.
        unreachable!("ScriptedModem: wait_for_prompt should not be called directly")
    }
}

impl ModemPort for ScriptedModem {
    fn send_pdu_sms(&mut self, hex: &str, tpdu_len: u8) -> Result<u8, ModemError> {
        self.sent_pdus.push((hex.to_string(), tpdu_len));
        Ok(1) // fake MR = 1
    }

    fn hang_up(&mut self) -> Result<(), ModemError> {
        self.hang_up_count += 1;
        Ok(())
    }
}

impl Default for ScriptedModem {
    fn default() -> Self { Self::new() }
}

// ---------------------------------------------------------------------------
// RecordingMessenger
// ---------------------------------------------------------------------------

/// Captured outbound message.
#[derive(Debug, Clone)]
pub struct SentMessage {
    pub text: String,
    pub id: MessageId,
}

/// Records sent messages and serves injected inbound messages.
pub struct RecordingMessenger {
    pub sent: Vec<SentMessage>,
    inbound: VecDeque<InboundMessage>,
    next_id: i64,
}

impl RecordingMessenger {
    pub fn new() -> Self {
        RecordingMessenger {
            sent: Vec::new(),
            inbound: VecDeque::new(),
            next_id: 1000,
        }
    }

    /// Inject an inbound message that will be returned on the next `poll()`.
    pub fn inject(&mut self, cursor: i64, text: &str, reply_to: Option<MessageId>) {
        self.inbound.push_back(InboundMessage {
            cursor,
            text: text.to_string(),
            reply_to,
        });
    }

    pub fn sent_count(&self) -> usize { self.sent.len() }
    pub fn last_sent(&self) -> Option<&str> { self.sent.last().map(|m| m.text.as_str()) }
    pub fn contains_sent(&self, substr: &str) -> bool {
        self.sent.iter().any(|m| m.text.contains(substr))
    }
}

impl MessageSink for RecordingMessenger {
    fn send_message(&mut self, text: &str) -> Result<MessageId, MessengerError> {
        let id = self.next_id;
        self.next_id += 1;
        self.sent.push(SentMessage { text: text.to_string(), id });
        Ok(id)
    }
}

impl MessageSource for RecordingMessenger {
    fn poll(&mut self, _since: i64, _timeout_sec: u32) -> Result<Vec<InboundMessage>, MessengerError> {
        let msgs: Vec<_> = self.inbound.drain(..).collect();
        Ok(msgs)
    }
}

impl Default for RecordingMessenger {
    fn default() -> Self { Self::new() }
}

// ---------------------------------------------------------------------------
// FailingMessenger
// ---------------------------------------------------------------------------

/// A messenger that always returns an HTTP error on `send_message`.
pub struct FailingMessenger;

impl MessageSink for FailingMessenger {
    fn send_message(&mut self, _text: &str) -> Result<MessageId, MessengerError> {
        Err(MessengerError::Http("simulated failure".into()))
    }
}

impl MessageSource for FailingMessenger {
    fn poll(&mut self, _since: i64, _timeout_sec: u32) -> Result<Vec<InboundMessage>, MessengerError> {
        Ok(vec![])
    }
}
