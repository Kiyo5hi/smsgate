//! IM backend abstraction.

pub mod telegram;

use thiserror::Error;

/// Opaque handle to a previously sent IM message.
pub type MessageId = i64;

/// A message received from the IM backend.
#[derive(Debug, Clone)]
pub struct InboundMessage {
    /// Monotonically increasing cursor; pass as `since` on the next poll.
    pub cursor: i64,
    /// Message text (starts with "/" for commands).
    pub text: String,
    /// If this is a reply to a previously sent message, its ID.
    pub reply_to: Option<MessageId>,
}

/// Errors from the IM layer.
#[derive(Debug, Error)]
pub enum MessengerError {
    #[error("HTTP error: {0}")]
    Http(String),
    #[error("JSON parse error: {0}")]
    Json(String),
    #[error("API error: {0}")]
    Api(String),
    #[error("not connected")]
    Disconnected,
}

/// Abstracts any IM backend capable of sending and receiving messages.
///
/// Implementing this trait = supporting a new IM app.
/// All business logic in bridge/, commands/, etc. depends only on this trait.
pub trait Messenger {
    /// Send a text message to the admin; return the sent message's ID.
    fn send_message(&mut self, text: &str) -> Result<MessageId, MessengerError>;

    /// Poll for new messages. `since` = cursor from last poll (0 on first call).
    /// `timeout_sec = 0` → short poll (return immediately).
    fn poll(&mut self, since: i64, timeout_sec: u32) -> Result<Vec<InboundMessage>, MessengerError>;
}
