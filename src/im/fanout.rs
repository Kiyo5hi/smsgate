//! Fan-out delivery: broadcasts each message to multiple sinks.
//!
//! The first sink in the list is the "primary" — its MessageId is returned
//! to the caller (needed for reply routing). Remaining sinks are
//! fire-and-forget: errors are logged but do not fail the call.

use super::{MessageId, MessageSink, MessengerError};

pub struct FanoutSink {
    sinks: Vec<Box<dyn MessageSink>>,
}

impl FanoutSink {
    /// Create a fanout with a pre-built list of sinks.
    /// The first entry is the primary (its ID is returned from `send_message`).
    pub fn new(sinks: Vec<Box<dyn MessageSink>>) -> Self {
        assert!(!sinks.is_empty(), "FanoutSink requires at least one sink");
        FanoutSink { sinks }
    }

    pub fn sink_count(&self) -> usize {
        self.sinks.len()
    }
}

impl MessageSink for FanoutSink {
    fn send_message(&mut self, text: &str) -> Result<MessageId, MessengerError> {
        let mut primary_result: Option<Result<MessageId, MessengerError>> = None;

        for (i, sink) in self.sinks.iter_mut().enumerate() {
            match sink.send_message(text) {
                Ok(id) => {
                    if primary_result.is_none() {
                        primary_result = Some(Ok(id));
                    }
                }
                Err(e) => {
                    if i == 0 {
                        primary_result = Some(Err(e));
                    } else {
                        log::error!("[fanout] sink #{} failed: {}", i, e);
                    }
                }
            }
        }

        primary_result.unwrap_or(Err(MessengerError::Disconnected))
    }
}
