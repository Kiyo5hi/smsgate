//! Fan-out delivery: broadcasts each message to multiple sinks.
//!
//! The first sink in the list is the "primary" — its MessageId is returned
//! to the caller (needed for reply routing). Remaining sinks are
//! fire-and-forget: errors are logged but do not fail the call.

use super::{
    ConversationId, InlineKeyboard, MessageFormat, MessageId, MessageSink, MessengerError,
};

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

    fn deliver(
        &mut self,
        conversation_id: Option<ConversationId>,
        text: &str,
    ) -> Result<MessageId, MessengerError> {
        let mut primary_result: Option<Result<MessageId, MessengerError>> = None;

        for (i, sink) in self.sinks.iter_mut().enumerate() {
            let result = match conversation_id {
                Some(id) => sink.send_message_to(id, text),
                None => sink.send_message(text),
            };
            match result {
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

impl MessageSink for FanoutSink {
    fn send_message(&mut self, text: &str) -> Result<MessageId, MessengerError> {
        self.deliver(None, text)
    }

    fn send_message_with_format(
        &mut self,
        text: &str,
        format: MessageFormat,
    ) -> Result<MessageId, MessengerError> {
        let mut primary = None;
        for (index, sink) in self.sinks.iter_mut().enumerate() {
            let result = sink.send_message_with_format(text, format);
            if index == 0 {
                primary = Some(result);
            } else if let Err(error) = result {
                log::error!("[fanout] sink #{} failed: {}", index, error);
            }
        }
        primary.unwrap_or(Err(MessengerError::Disconnected))
    }

    fn send_message_to(
        &mut self,
        conversation_id: ConversationId,
        text: &str,
    ) -> Result<MessageId, MessengerError> {
        self.deliver(Some(conversation_id), text)
    }

    fn send_message_to_with_keyboard_and_format(
        &mut self,
        conversation_id: ConversationId,
        text: &str,
        keyboard: Option<&InlineKeyboard>,
        format: MessageFormat,
    ) -> Result<MessageId, MessengerError> {
        let mut primary = None;
        for (index, sink) in self.sinks.iter_mut().enumerate() {
            let result = sink.send_message_to_with_keyboard_and_format(
                conversation_id,
                text,
                keyboard,
                format,
            );
            if index == 0 {
                primary = Some(result);
            } else if let Err(error) = result {
                log::error!("[fanout] sink #{} failed: {}", index, error);
            }
        }
        primary.unwrap_or(Err(MessengerError::Disconnected))
    }

    fn edit_message_in_with_keyboard_and_format(
        &mut self,
        conversation_id: ConversationId,
        message_id: MessageId,
        text: &str,
        keyboard: Option<&InlineKeyboard>,
        format: MessageFormat,
    ) -> Result<(), MessengerError> {
        self.sinks[0].edit_message_in_with_keyboard_and_format(
            conversation_id,
            message_id,
            text,
            keyboard,
            format,
        )
    }

    fn answer_callback_query(
        &mut self,
        callback_query_id: &str,
        text: Option<&str>,
    ) -> Result<(), MessengerError> {
        self.sinks[0].answer_callback_query(callback_query_id, text)
    }
}
