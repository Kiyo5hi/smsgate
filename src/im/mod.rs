//! IM backend abstraction.
//!
//! Two orthogonal traits:
//! - `MessageSink`   — fire-and-forget outbound delivery (SMS → IM / webhook / MQ)
//! - `MessageSource` — inbound command polling (Telegram only, typically)
//!
//! The legacy `Messenger` trait is kept as a convenience super-trait for backends
//! that implement both (e.g. Telegram).

pub mod fanout;
pub mod telegram;
#[cfg(feature = "esp32")]
pub mod webhook;

use thiserror::Error;

/// Opaque handle to a previously sent IM message.
pub type MessageId = i64;
/// Opaque conversation identifier used for routing a direct reply.
pub type ConversationId = i64;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MessageFormat {
    Plain,
    Html,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct InlineKeyboardButton {
    pub text: String,
    pub callback_data: String,
}

impl InlineKeyboardButton {
    pub fn new(text: impl Into<String>, callback_data: impl Into<String>) -> Self {
        Self {
            text: text.into(),
            callback_data: callback_data.into(),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct InlineKeyboard {
    pub rows: Vec<Vec<InlineKeyboardButton>>,
}

impl InlineKeyboard {
    pub fn single_row(buttons: Vec<InlineKeyboardButton>) -> Self {
        Self {
            rows: if buttons.is_empty() {
                Vec::new()
            } else {
                vec![buttons]
            },
        }
    }
    pub fn is_empty(&self) -> bool {
        self.rows.iter().all(Vec::is_empty)
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct InboundDocument {
    pub file_id: String,
    pub file_unique_id: String,
    pub file_name: Option<String>,
    pub mime_type: Option<String>,
    pub file_size: Option<u64>,
    pub caption: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct InboundCallback {
    pub id: String,
    pub data: String,
    pub message_id: MessageId,
    pub conversation_id: ConversationId,
}

/// A message received from the IM backend.
#[derive(Debug, Clone)]
pub struct InboundMessage {
    /// Monotonically increasing cursor; pass as `since` on the next poll.
    pub cursor: i64,
    /// Message text (starts with "/" for commands).
    pub text: String,
    /// If this is a reply to a previously sent message, its ID.
    pub reply_to: Option<MessageId>,
    /// Conversation that originated this message, when the backend supports it.
    pub conversation_id: Option<ConversationId>,
    pub document: Option<InboundDocument>,
    pub callback: Option<InboundCallback>,
}

/// Errors from the IM layer.
#[derive(Debug, Error)]
pub enum MessengerError {
    #[error("HTTP error: {0}")]
    Http(String),
    #[error("JSON parse error: {0}")]
    Json(String),
    #[error("API rate limited: retry after {retry_after_secs}s: {description}")]
    RateLimited {
        retry_after_secs: u32,
        description: String,
    },
    #[error("API error: {0}")]
    Api(String),
    #[error("not connected")]
    Disconnected,
    #[error("operation timed out: {0}")]
    Timeout(String),
}

/// Outbound-only delivery target. Implement this for webhooks, MQ, etc.
pub trait MessageSink {
    /// Deliver a text notification. Returns a backend-specific message ID
    /// (0 if the backend doesn't track IDs).
    fn send_message(&mut self, text: &str) -> Result<MessageId, MessengerError>;

    fn send_message_with_format(
        &mut self,
        text: &str,
        _format: MessageFormat,
    ) -> Result<MessageId, MessengerError> {
        self.send_message(text)
    }

    /// Deliver a reply to a specific conversation. Backends without native
    /// conversation routing retain their normal delivery behavior.
    fn send_message_to(
        &mut self,
        _conversation_id: ConversationId,
        text: &str,
    ) -> Result<MessageId, MessengerError> {
        self.send_message(text)
    }

    fn send_message_to_with_keyboard_and_format(
        &mut self,
        conversation_id: ConversationId,
        text: &str,
        keyboard: Option<&InlineKeyboard>,
        format: MessageFormat,
    ) -> Result<MessageId, MessengerError> {
        let _ = (keyboard, format);
        self.send_message_to(conversation_id, text)
    }

    fn edit_message_in_with_keyboard_and_format(
        &mut self,
        conversation_id: ConversationId,
        message_id: MessageId,
        text: &str,
        keyboard: Option<&InlineKeyboard>,
        format: MessageFormat,
    ) -> Result<(), MessengerError> {
        let _ = message_id;
        self.send_message_to_with_keyboard_and_format(conversation_id, text, keyboard, format)
            .map(|_| ())
    }

    fn answer_callback_query(
        &mut self,
        _callback_query_id: &str,
        _text: Option<&str>,
    ) -> Result<(), MessengerError> {
        Ok(())
    }
}

/// Inbound command source. Only the "primary" backend (e.g. Telegram) needs this.
pub trait MessageSource {
    /// Poll for new messages. `since` = cursor from last poll (0 on first call).
    fn poll(&mut self, since: i64, timeout_sec: u32)
        -> Result<Vec<InboundMessage>, MessengerError>;
}

/// Full bidirectional backend (sink + source). Telegram implements this.
/// All business logic in bridge/, commands/, etc. depends only on `MessageSink`.
/// Only the polling thread depends on `MessageSource`.
pub trait Messenger: MessageSink + MessageSource {}

/// Blanket impl: anything that is both a sink and a source is a Messenger.
impl<T: MessageSink + MessageSource> Messenger for T {}
