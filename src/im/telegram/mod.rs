//! Telegram Bot API backend.

#[cfg(feature = "esp32")]
pub mod http;
pub mod types;
use super::{
    InboundCallback, InboundDocument, InboundMessage, InlineKeyboard, MessageFormat, MessageId,
};
use types::Update;
#[cfg(feature = "esp32")]
pub mod worker;

/// Return whether a Telegram chat may issue commands to the bot.
pub fn is_trusted_chat(primary_chat_id: i64, trusted_chat_ids: &[i64], candidate: i64) -> bool {
    candidate == primary_chat_id || trusted_chat_ids.contains(&candidate)
}

pub fn poll_retry_after(error: &super::MessengerError) -> Option<std::time::Duration> {
    let seconds = match error {
        super::MessengerError::RateLimited {
            retry_after_secs, ..
        } => *retry_after_secs,
        super::MessengerError::Api(description) => parse_retry_after_description(description)?,
        _ => return None,
    };
    Some(std::time::Duration::from_secs(u64::from(seconds.max(1))))
}

pub const SEND_RETRY_INTERVAL: std::time::Duration = std::time::Duration::from_secs(10);
pub const SEND_RESTART_AFTER: std::time::Duration = std::time::Duration::from_secs(5 * 60);

pub fn should_retry_send(error: &super::MessengerError) -> bool {
    matches!(
        error,
        super::MessengerError::Http(_)
            | super::MessengerError::Json(_)
            | super::MessengerError::RateLimited { .. }
            | super::MessengerError::Disconnected
            | super::MessengerError::Timeout(_)
    )
}

pub fn send_retry_delay(error: &super::MessengerError) -> std::time::Duration {
    poll_retry_after(error).unwrap_or(SEND_RETRY_INTERVAL)
}

fn parse_retry_after_description(description: &str) -> Option<u32> {
    let retry_after = description.split_once("retry after ")?.1;
    let digits = retry_after.bytes().take_while(u8::is_ascii_digit).count();
    (digits > 0)
        .then(|| retry_after[..digits].parse().ok())
        .flatten()
}

#[cfg(feature = "esp32")]
use super::{ConversationId, MessageSink, MessageSource, MessengerError};
#[cfg(feature = "esp32")]
use crate::modem::ModemPort;
#[cfg(feature = "esp32")]
use http::TelegramHttpClient;
#[cfg(feature = "esp32")]
use std::sync::{Arc, Mutex};
#[cfg(feature = "esp32")]
use types::{ApiResult, SendMessageResult};

#[cfg(feature = "esp32")]
enum Transport {
    Wifi(TelegramHttpClient),
    Modem(Arc<Mutex<dyn ModemPort + Send>>),
}

/// Telegram Bot API messenger.
#[cfg(feature = "esp32")]
pub struct TelegramMessenger {
    transport: Transport,
    chat_id: i64,
    trusted_chat_ids: Vec<i64>,
    token: String,
}

#[cfg(feature = "esp32")]
impl TelegramMessenger {
    pub fn new_wifi(http: TelegramHttpClient, token: String, chat_id: i64) -> Self {
        TelegramMessenger {
            transport: Transport::Wifi(http),
            chat_id,
            trusted_chat_ids: Vec::new(),
            token,
        }
    }

    /// IM over the modem's built-in HTTP stack (cellular PDP).
    /// Use a short `timeout_sec` in `poll` — the UART is shared with SMS.
    pub fn new_modem(modem: Arc<Mutex<dyn ModemPort + Send>>, token: String, chat_id: i64) -> Self {
        TelegramMessenger {
            transport: Transport::Modem(modem),
            chat_id,
            trusted_chat_ids: Vec::new(),
            token,
        }
    }

    pub fn with_trusted_chat_ids(mut self, trusted_chat_ids: Vec<i64>) -> Self {
        self.trusted_chat_ids = trusted_chat_ids;
        self.trusted_chat_ids
            .retain(|id| *id != 0 && *id != self.chat_id);
        self.trusted_chat_ids.sort_unstable();
        self.trusted_chat_ids.dedup();
        self
    }

    fn post_json(&mut self, method: &str, body: &str) -> Result<String, MessengerError> {
        let path = format!("/bot{}/{}", self.token, method);
        match &mut self.transport {
            Transport::Wifi(http) => http
                .post(&path, body)
                .map_err(|e| MessengerError::Http(e.to_string())),
            Transport::Modem(m) => {
                let mut g = m.lock().map_err(|_| MessengerError::Disconnected)?;
                let raw = g
                    .post_telegram_https(&path, body)
                    .map_err(|e| MessengerError::Http(format!("modem {}", e)))?;
                Ok(raw)
            }
        }
    }

    fn post_and_parse<T: serde::de::DeserializeOwned>(
        &mut self,
        method: &str,
        body: &str,
    ) -> Result<ApiResult<T>, MessengerError> {
        let resp = self.post_json(method, body)?;
        serde_json::from_str(&resp).map_err(|e| MessengerError::Json(e.to_string()))
    }

    fn check_ok<T>(result: ApiResult<T>) -> Result<Option<T>, MessengerError> {
        if result.ok {
            Ok(result.result)
        } else {
            let description = result.description.unwrap_or_default();
            let retry_after_secs = result
                .parameters
                .and_then(|parameters| parameters.retry_after)
                .or_else(|| parse_retry_after_description(&description));
            match retry_after_secs {
                Some(retry_after_secs) => Err(MessengerError::RateLimited {
                    retry_after_secs,
                    description,
                }),
                None => Err(MessengerError::Api(description)),
            }
        }
    }

    /// Register bot commands with Telegram (called once at startup).
    pub fn register_commands(&mut self, commands: &[(&str, &str)]) -> Result<(), MessengerError> {
        let cmds_json: Vec<String> = commands
            .iter()
            .map(|(name, desc)| format!(r#"{{"command":"{}","description":"{}"}}"#, name, desc))
            .collect();
        let body = format!(r#"{{"commands":[{}]}}"#, cmds_json.join(","));
        let result: ApiResult<bool> = self.post_and_parse("setMyCommands", &body)?;
        Self::check_ok(result)?;
        Ok(())
    }

    fn send_with_options(
        &mut self,
        conversation_id: ConversationId,
        text: &str,
        keyboard: Option<&InlineKeyboard>,
        format: MessageFormat,
    ) -> Result<MessageId, MessengerError> {
        if !is_trusted_chat(self.chat_id, &self.trusted_chat_ids, conversation_id) {
            return Err(MessengerError::Api("untrusted Telegram destination".into()));
        }
        let body = build_message_body(conversation_id, None, text, keyboard, format);
        let result: ApiResult<SendMessageResult> = self.post_and_parse("sendMessage", &body)?;
        Ok(Self::check_ok(result)?.map(|r| r.message_id).unwrap_or(0))
    }
}

#[cfg(feature = "esp32")]
impl MessageSink for TelegramMessenger {
    fn send_message(&mut self, text: &str) -> Result<MessageId, MessengerError> {
        self.send_message_to(self.chat_id, text)
    }

    fn send_message_with_format(
        &mut self,
        text: &str,
        format: MessageFormat,
    ) -> Result<MessageId, MessengerError> {
        self.send_with_options(self.chat_id, text, None, format)
    }

    fn send_message_to(
        &mut self,
        conversation_id: ConversationId,
        text: &str,
    ) -> Result<MessageId, MessengerError> {
        self.send_with_options(conversation_id, text, None, MessageFormat::Plain)
    }

    fn send_message_to_with_keyboard_and_format(
        &mut self,
        conversation_id: ConversationId,
        text: &str,
        keyboard: Option<&InlineKeyboard>,
        format: MessageFormat,
    ) -> Result<MessageId, MessengerError> {
        self.send_with_options(conversation_id, text, keyboard, format)
    }

    fn edit_message_in_with_keyboard_and_format(
        &mut self,
        conversation_id: ConversationId,
        message_id: MessageId,
        text: &str,
        keyboard: Option<&InlineKeyboard>,
        format: MessageFormat,
    ) -> Result<(), MessengerError> {
        if !is_trusted_chat(self.chat_id, &self.trusted_chat_ids, conversation_id) {
            return Err(MessengerError::Api("untrusted Telegram destination".into()));
        }
        let body = build_message_body(conversation_id, Some(message_id), text, keyboard, format);
        let result: ApiResult<serde_json::Value> = self.post_and_parse("editMessageText", &body)?;
        Self::check_ok(result).map(|_| ())
    }

    fn answer_callback_query(
        &mut self,
        callback_query_id: &str,
        text: Option<&str>,
    ) -> Result<(), MessengerError> {
        let mut body = format!(
            r#"{{"callback_query_id":"{}""#,
            types::json_escape(callback_query_id)
        );
        if let Some(text) = text {
            body.push_str(&format!(r#", "text":"{}""#, types::json_escape(text)));
        }
        body.push('}');
        let result: ApiResult<bool> = self.post_and_parse("answerCallbackQuery", &body)?;
        Self::check_ok(result).map(|_| ())
    }
}

#[cfg(feature = "esp32")]
impl MessageSource for TelegramMessenger {
    fn poll(
        &mut self,
        since: i64,
        timeout_sec: u32,
    ) -> Result<Vec<InboundMessage>, MessengerError> {
        let body = format!(
            r#"{{"offset":{},"timeout":{},"limit":100,"allowed_updates":["message","callback_query"]}}"#,
            since, timeout_sec
        );
        let result: ApiResult<Vec<Update>> = self.post_and_parse("getUpdates", &body)?;
        let updates = Self::check_ok(result)?.unwrap_or_default();
        let messages = updates
            .into_iter()
            .filter_map(|u| update_to_inbound_message(u, self.chat_id, &self.trusted_chat_ids))
            .collect();
        Ok(messages)
    }
}

pub fn update_to_inbound_message(
    update: Update,
    primary: i64,
    trusted: &[i64],
) -> Option<InboundMessage> {
    if let Some(callback) = update.callback_query {
        let message = callback.message?;
        if !is_trusted_chat(primary, trusted, message.chat.id) {
            return None;
        }
        return Some(InboundMessage {
            cursor: update.update_id + 1,
            text: String::new(),
            reply_to: None,
            conversation_id: Some(message.chat.id),
            document: None,
            callback: Some(InboundCallback {
                id: callback.id,
                data: callback.data?,
                message_id: message.message_id,
                conversation_id: message.chat.id,
            }),
        });
    }
    let message = update.message?;
    if !is_trusted_chat(primary, trusted, message.chat.id) {
        log::warn!(
            "[tg] message from unexpected chat {} — ignored",
            message.chat.id
        );
        return None;
    }
    let caption = message.caption.clone();
    let document = message.document.map(|doc| InboundDocument {
        file_id: doc.file_id,
        file_unique_id: doc.file_unique_id,
        file_name: doc.file_name,
        mime_type: doc.mime_type,
        file_size: doc.file_size,
        caption: caption.clone(),
    });
    let text = message.text.unwrap_or_else(|| caption.unwrap_or_default());
    if text.is_empty() && document.is_none() {
        return None;
    }
    Some(InboundMessage {
        cursor: update.update_id + 1,
        text,
        reply_to: message.reply_to_message.map(|r| r.message_id),
        conversation_id: Some(message.chat.id),
        document,
        callback: None,
    })
}

pub fn build_message_body(
    chat_id: i64,
    message_id: Option<MessageId>,
    text: &str,
    keyboard: Option<&InlineKeyboard>,
    format: MessageFormat,
) -> String {
    let mut body = format!(r#"{{"chat_id":{}"#, chat_id);
    if let Some(id) = message_id {
        body.push_str(&format!(r#", "message_id":{}"#, id));
    }
    body.push_str(&format!(r#", "text":"{}""#, types::json_escape(text)));
    if format == MessageFormat::Html {
        body.push_str(r#", "parse_mode":"HTML""#);
    }
    if let Some(keyboard) = keyboard {
        body.push_str(r#", "reply_markup":{"inline_keyboard":["#);
        for (row_index, row) in keyboard.rows.iter().enumerate() {
            if row_index > 0 {
                body.push(',');
            }
            body.push('[');
            for (button_index, button) in row.iter().enumerate() {
                if button_index > 0 {
                    body.push(',');
                }
                body.push_str(&format!(
                    r#"{{"text":"{}","callback_data":"{}"}}"#,
                    types::json_escape(&button.text),
                    types::json_escape(&button.callback_data),
                ));
            }
            body.push(']');
        }
        body.push_str("]}");
    }
    body.push('}');
    body
}
