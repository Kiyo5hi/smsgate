//! Dedicated, retrying Telegram outbound worker.

use super::{
    http::TelegramHttpClient, send_retry_delay, should_retry_send, TelegramMessenger,
    SEND_RESTART_AFTER,
};
use crate::{
    im::{ConversationId, InlineKeyboard, MessageFormat, MessageId, MessageSink, MessengerError},
    log_ring::{LogEntry, LogKind},
    modem::ModemPort,
};
use std::{
    sync::{
        mpsc::{sync_channel, Receiver, RecvTimeoutError, Sender, SyncSender},
        Arc, Mutex,
    },
    time::{Duration, Instant},
};

const QUEUE_DEPTH: usize = 8;
const WAIT_SLICE: Duration = Duration::from_secs(10);

#[derive(Debug, Clone)]
pub enum TelegramSendEvent {
    Log(LogEntry),
    Restart(LogEntry),
}

enum Request {
    Send {
        conversation_id: Option<ConversationId>,
        text: String,
        keyboard: Option<InlineKeyboard>,
        format: MessageFormat,
        reply: SyncSender<Result<MessageId, MessengerError>>,
    },
    Edit {
        conversation_id: ConversationId,
        message_id: MessageId,
        text: String,
        keyboard: Option<InlineKeyboard>,
        format: MessageFormat,
        reply: SyncSender<Result<(), MessengerError>>,
    },
    Answer {
        callback_id: String,
        text: Option<String>,
        reply: SyncSender<Result<(), MessengerError>>,
    },
    Register {
        commands: Vec<(String, String)>,
        reply: SyncSender<Result<(), MessengerError>>,
    },
}

pub struct TelegramSendWorker {
    tx: SyncSender<Request>,
    events: Sender<TelegramSendEvent>,
}

impl TelegramSendWorker {
    pub fn spawn(
        wifi: bool,
        modem: Arc<Mutex<dyn ModemPort + Send>>,
        token: String,
        chat_id: i64,
        trusted_chat_ids: Vec<i64>,
        events: Sender<TelegramSendEvent>,
    ) -> Self {
        let (tx, rx) = sync_channel(QUEUE_DEPTH);
        let worker_events = events.clone();
        std::thread::Builder::new()
            .name("tg-send".into())
            .stack_size(16 * 1024)
            .spawn(move || {
                unsafe {
                    let _ = esp_idf_sys::esp_task_wdt_add(std::ptr::null_mut());
                }
                let mut messenger: Option<TelegramMessenger> = None;
                loop {
                    let request = match rx.recv_timeout(WAIT_SLICE) {
                        Ok(request) => request,
                        Err(RecvTimeoutError::Timeout) => {
                            unsafe {
                                let _ = esp_idf_sys::esp_task_wdt_reset();
                            }
                            continue;
                        }
                        Err(RecvTimeoutError::Disconnected) => break,
                    };
                    unsafe {
                        let _ = esp_idf_sys::esp_task_wdt_reset();
                    }
                    match request {
                        Request::Send {
                            conversation_id,
                            text,
                            keyboard,
                            format,
                            reply,
                        } => {
                            let result = retry("sendMessage", &worker_events, || {
                                ensure_messenger(
                                    &mut messenger,
                                    wifi,
                                    &modem,
                                    &token,
                                    chat_id,
                                    &trusted_chat_ids,
                                )?;
                                let result = match (messenger.as_mut(), conversation_id) {
                                    (Some(client), Some(id)) => client
                                        .send_message_to_with_keyboard_and_format(
                                            id,
                                            &text,
                                            keyboard.as_ref(),
                                            format,
                                        ),
                                    (Some(client), None) => client
                                        .send_message_to_with_keyboard_and_format(
                                            chat_id,
                                            &text,
                                            keyboard.as_ref(),
                                            format,
                                        ),
                                    (None, _) => Err(MessengerError::Disconnected),
                                };
                                if result.as_ref().err().is_some_and(should_retry_send) {
                                    messenger = None;
                                }
                                result
                            });
                            let _ = reply.send(result);
                        }
                        Request::Edit {
                            conversation_id,
                            message_id,
                            text,
                            keyboard,
                            format,
                            reply,
                        } => {
                            let result = retry("editMessageText", &worker_events, || {
                                ensure_messenger(
                                    &mut messenger,
                                    wifi,
                                    &modem,
                                    &token,
                                    chat_id,
                                    &trusted_chat_ids,
                                )?;
                                let result = messenger
                                    .as_mut()
                                    .ok_or(MessengerError::Disconnected)?
                                    .edit_message_in_with_keyboard_and_format(
                                        conversation_id,
                                        message_id,
                                        &text,
                                        keyboard.as_ref(),
                                        format,
                                    );
                                if result.as_ref().err().is_some_and(should_retry_send) {
                                    messenger = None;
                                }
                                result
                            });
                            let _ = reply.send(result);
                        }
                        Request::Answer {
                            callback_id,
                            text,
                            reply,
                        } => {
                            let result = retry("answerCallbackQuery", &worker_events, || {
                                ensure_messenger(
                                    &mut messenger,
                                    wifi,
                                    &modem,
                                    &token,
                                    chat_id,
                                    &trusted_chat_ids,
                                )?;
                                let result = messenger
                                    .as_mut()
                                    .ok_or(MessengerError::Disconnected)?
                                    .answer_callback_query(&callback_id, text.as_deref());
                                if result.as_ref().err().is_some_and(should_retry_send) {
                                    messenger = None;
                                }
                                result
                            });
                            let _ = reply.send(result);
                        }
                        Request::Register { commands, reply } => {
                            let result = retry("setMyCommands", &worker_events, || {
                                ensure_messenger(
                                    &mut messenger,
                                    wifi,
                                    &modem,
                                    &token,
                                    chat_id,
                                    &trusted_chat_ids,
                                )?;
                                let borrowed: Vec<(&str, &str)> = commands
                                    .iter()
                                    .map(|(name, desc)| (name.as_str(), desc.as_str()))
                                    .collect();
                                let result = messenger
                                    .as_mut()
                                    .ok_or(MessengerError::Disconnected)?
                                    .register_commands(&borrowed);
                                if result.as_ref().err().is_some_and(should_retry_send) {
                                    messenger = None;
                                }
                                result
                            });
                            let _ = reply.send(result);
                        }
                    }
                }
            })
            .expect("failed to spawn tg-send thread");
        Self { tx, events }
    }

    pub fn register_commands(&self, commands: &[(&str, &str)]) -> Result<(), MessengerError> {
        let (reply, rx) = sync_channel(1);
        let commands = commands
            .iter()
            .map(|(a, b)| ((*a).to_string(), (*b).to_string()))
            .collect();
        self.tx
            .send(Request::Register { commands, reply })
            .map_err(|_| MessengerError::Disconnected)?;
        self.wait("setMyCommands", rx)
    }

    fn wait<T>(
        &self,
        operation: &str,
        rx: Receiver<Result<T, MessengerError>>,
    ) -> Result<T, MessengerError> {
        let started = Instant::now();
        loop {
            match rx.recv_timeout(WAIT_SLICE) {
                Ok(result) => return result,
                Err(RecvTimeoutError::Disconnected) => return Err(MessengerError::Disconnected),
                Err(RecvTimeoutError::Timeout) => {
                    unsafe {
                        let _ = esp_idf_sys::esp_task_wdt_reset();
                    }
                    if started.elapsed() >= SEND_RESTART_AFTER + WAIT_SLICE {
                        let detail = format!(
                            "{operation} worker unresponsive for {}s",
                            started.elapsed().as_secs()
                        );
                        let entry = LogEntry::runtime(
                            LogKind::Network,
                            "telegram",
                            &detail,
                            "-".into(),
                            false,
                        );
                        let _ = self.events.send(TelegramSendEvent::Restart(entry));
                        return Err(MessengerError::Timeout(detail));
                    }
                }
            }
        }
    }
}

impl MessageSink for TelegramSendWorker {
    fn send_message(&mut self, text: &str) -> Result<MessageId, MessengerError> {
        self.enqueue(None, text, None, MessageFormat::Plain)
    }

    fn send_message_with_format(
        &mut self,
        text: &str,
        format: MessageFormat,
    ) -> Result<MessageId, MessengerError> {
        self.enqueue(None, text, None, format)
    }

    fn send_message_to(
        &mut self,
        conversation_id: ConversationId,
        text: &str,
    ) -> Result<MessageId, MessengerError> {
        self.enqueue(Some(conversation_id), text, None, MessageFormat::Plain)
    }

    fn send_message_to_with_keyboard_and_format(
        &mut self,
        conversation_id: ConversationId,
        text: &str,
        keyboard: Option<&InlineKeyboard>,
        format: MessageFormat,
    ) -> Result<MessageId, MessengerError> {
        self.enqueue(Some(conversation_id), text, keyboard, format)
    }

    fn edit_message_in_with_keyboard_and_format(
        &mut self,
        conversation_id: ConversationId,
        message_id: MessageId,
        text: &str,
        keyboard: Option<&InlineKeyboard>,
        format: MessageFormat,
    ) -> Result<(), MessengerError> {
        let (reply, rx) = sync_channel(1);
        self.tx
            .send(Request::Edit {
                conversation_id,
                message_id,
                text: text.to_string(),
                keyboard: keyboard.cloned(),
                format,
                reply,
            })
            .map_err(|_| MessengerError::Disconnected)?;
        self.wait("editMessageText", rx)
    }

    fn answer_callback_query(
        &mut self,
        callback_query_id: &str,
        text: Option<&str>,
    ) -> Result<(), MessengerError> {
        let (reply, rx) = sync_channel(1);
        self.tx
            .send(Request::Answer {
                callback_id: callback_query_id.to_string(),
                text: text.map(str::to_string),
                reply,
            })
            .map_err(|_| MessengerError::Disconnected)?;
        self.wait("answerCallbackQuery", rx)
    }
}

impl TelegramSendWorker {
    fn enqueue(
        &self,
        conversation_id: Option<ConversationId>,
        text: &str,
        keyboard: Option<&InlineKeyboard>,
        format: MessageFormat,
    ) -> Result<MessageId, MessengerError> {
        let (reply, rx) = sync_channel(1);
        self.tx
            .send(Request::Send {
                conversation_id,
                text: text.to_string(),
                keyboard: keyboard.cloned(),
                format,
                reply,
            })
            .map_err(|_| MessengerError::Disconnected)?;
        self.wait("sendMessage", rx)
    }
}

fn build_messenger(
    wifi: bool,
    modem: Arc<Mutex<dyn ModemPort + Send>>,
    token: String,
    chat_id: i64,
    trusted_chat_ids: Vec<i64>,
) -> Result<TelegramMessenger, MessengerError> {
    let messenger = if wifi {
        let http =
            TelegramHttpClient::new(None).map_err(|e| MessengerError::Http(e.to_string()))?;
        TelegramMessenger::new_wifi(http, token, chat_id)
    } else {
        TelegramMessenger::new_modem(modem, token, chat_id)
    };
    Ok(messenger.with_trusted_chat_ids(trusted_chat_ids))
}

fn ensure_messenger(
    messenger: &mut Option<TelegramMessenger>,
    wifi: bool,
    modem: &Arc<Mutex<dyn ModemPort + Send>>,
    token: &str,
    chat_id: i64,
    trusted_chat_ids: &[i64],
) -> Result<(), MessengerError> {
    if messenger.is_none() {
        *messenger = Some(build_messenger(
            wifi,
            modem.clone(),
            token.to_string(),
            chat_id,
            trusted_chat_ids.to_vec(),
        )?);
    }
    Ok(())
}

fn retry<T, F>(
    operation: &str,
    events: &Sender<TelegramSendEvent>,
    mut f: F,
) -> Result<T, MessengerError>
where
    F: FnMut() -> Result<T, MessengerError>,
{
    let started = Instant::now();
    let mut attempt = 1u16;
    loop {
        unsafe {
            let _ = esp_idf_sys::esp_task_wdt_reset();
        }
        match f() {
            Ok(value) => return Ok(value),
            Err(error) => {
                let detail = format!("{operation} attempt {attempt} failed: {error}");
                log::error!("[tg-send] {}", detail);
                // Keep flash diagnostics useful without turning a prolonged
                // outage into a high-frequency erase workload.
                if attempt == 1 || attempt.is_multiple_of(6) {
                    let _ = events.send(TelegramSendEvent::Log(LogEntry::runtime(
                        LogKind::Network,
                        "telegram",
                        &detail,
                        "-".into(),
                        false,
                    )));
                }
                if !should_retry_send(&error) {
                    return Err(error);
                }
                if started.elapsed() >= SEND_RESTART_AFTER {
                    let detail = format!(
                        "{operation} failed for {}s; rebooting",
                        started.elapsed().as_secs()
                    );
                    let _ = events.send(TelegramSendEvent::Restart(LogEntry::runtime(
                        LogKind::Network,
                        "telegram",
                        &detail,
                        "-".into(),
                        false,
                    )));
                    return Err(MessengerError::Timeout(detail));
                }
                let delay = send_retry_delay(&error)
                    .min(SEND_RESTART_AFTER.saturating_sub(started.elapsed()));
                sleep_with_watchdog(delay);
                attempt = attempt.saturating_add(1);
            }
        }
    }
}

fn sleep_with_watchdog(duration: Duration) {
    let deadline = Instant::now() + duration;
    while Instant::now() < deadline {
        std::thread::sleep(WAIT_SLICE.min(deadline.saturating_duration_since(Instant::now())));
        unsafe {
            let _ = esp_idf_sys::esp_task_wdt_reset();
        }
    }
}
