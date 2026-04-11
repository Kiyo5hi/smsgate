//! Telegram Bot API JSON types.

use serde::Deserialize;
use crate::im::MessageId;

#[derive(Debug, Deserialize)]
pub struct ApiResult<T> {
    pub ok: bool,
    pub result: Option<T>,
    pub description: Option<String>,
}

#[derive(Debug, Deserialize)]
pub struct SendMessageResult {
    pub message_id: MessageId,
}

#[derive(Debug, Deserialize)]
pub struct Update {
    pub update_id: i64,
    pub message: Option<Message>,
}

#[derive(Debug, Deserialize)]
pub struct Message {
    pub message_id: MessageId,
    pub text: Option<String>,
    pub reply_to_message: Option<ReplyToMessage>,
    pub chat: Chat,
}

#[derive(Debug, Deserialize)]
pub struct ReplyToMessage {
    pub message_id: MessageId,
}

#[derive(Debug, Deserialize)]
pub struct Chat {
    pub id: i64,
}
