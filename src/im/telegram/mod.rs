//! Telegram Bot API backend.

#[cfg(feature = "esp32")]
pub mod http;
pub mod types;

#[cfg(feature = "esp32")]
use super::{InboundMessage, MessageId, MessageSink, MessageSource, MessengerError};
#[cfg(feature = "esp32")]
use crate::config::Config;
#[cfg(feature = "esp32")]
use http::TelegramHttpClient;
#[cfg(feature = "esp32")]
use types::{ApiResult, SendMessageResult, Update};

/// Telegram Bot API messenger.
#[cfg(feature = "esp32")]
pub struct TelegramMessenger {
    http: TelegramHttpClient,
    chat_id: i64,
    token: String,
}

#[cfg(feature = "esp32")]
impl TelegramMessenger {
    pub fn new(http: TelegramHttpClient) -> Self {
        TelegramMessenger {
            http,
            chat_id: Config::CHAT_ID,
            token: Config::BOT_TOKEN.to_string(),
        }
    }

    fn post_json(&mut self, method: &str, body: &str) -> Result<String, MessengerError> {
        let path = format!("/bot{}/{}", self.token, method);
        self.http.post(&path, body)
            .map_err(|e| MessengerError::Http(e.to_string()))
    }

    fn post_and_parse<T: serde::de::DeserializeOwned>(
        &mut self, method: &str, body: &str,
    ) -> Result<ApiResult<T>, MessengerError> {
        let resp = self.post_json(method, body)?;
        serde_json::from_str(&resp).map_err(|e| MessengerError::Json(e.to_string()))
    }

    fn check_ok<T>(result: ApiResult<T>) -> Result<Option<T>, MessengerError> {
        if result.ok {
            Ok(result.result)
        } else {
            Err(MessengerError::Api(result.description.unwrap_or_default()))
        }
    }

    /// Register bot commands with Telegram (called once at startup).
    pub fn register_commands(&mut self, commands: &[(&str, &str)]) -> Result<(), MessengerError> {
        let cmds_json: Vec<String> = commands.iter()
            .map(|(name, desc)| format!(r#"{{"command":"{}","description":"{}"}}"#, name, desc))
            .collect();
        let body = format!(r#"{{"commands":[{}]}}"#, cmds_json.join(","));
        let result: ApiResult<bool> = self.post_and_parse("setMyCommands", &body)?;
        Self::check_ok(result)?;
        Ok(())
    }
}

#[cfg(feature = "esp32")]
impl MessageSink for TelegramMessenger {
    fn send_message(&mut self, text: &str) -> Result<MessageId, MessengerError> {
        let escaped = types::json_escape(text);
        let body = format!(
            r#"{{"chat_id":{},"text":"{}"}}"#,
            self.chat_id, escaped
        );
        let result: ApiResult<SendMessageResult> = self.post_and_parse("sendMessage", &body)?;
        let r = Self::check_ok(result)?;
        Ok(r.map(|r| r.message_id).unwrap_or(0))
    }
}

#[cfg(feature = "esp32")]
impl MessageSource for TelegramMessenger {
    fn poll(&mut self, since: i64, timeout_sec: u32) -> Result<Vec<InboundMessage>, MessengerError> {
        let body = format!(
            r#"{{"offset":{},"timeout":{},"limit":100,"allowed_updates":["message"]}}"#,
            since, timeout_sec
        );
        let result: ApiResult<Vec<Update>> = self.post_and_parse("getUpdates", &body)?;
        let updates = Self::check_ok(result)?.unwrap_or_default();
        let messages = updates.into_iter().filter_map(|u| {
            let msg = u.message?;
            let text = msg.text.clone().unwrap_or_default();
            if text.is_empty() { return None; }
            Some(InboundMessage {
                cursor: u.update_id + 1,
                text,
                reply_to: msg.reply_to_message.map(|r| r.message_id),
            })
        }).collect();
        Ok(messages)
    }
}
