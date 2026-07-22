use crate::commands::{Command, CommandContext};
use crate::im::{InlineKeyboard, InlineKeyboardButton, MessageFormat};
use crate::log_ring::LOG_PAGE_SIZE;

pub struct LogCommand;

pub struct LogPage {
    pub text: String,
    pub keyboard: Option<InlineKeyboard>,
    pub format: MessageFormat,
}

impl Command for LogCommand {
    fn name(&self) -> &'static str {
        "log"
    }
    fn description(&self) -> &'static str {
        crate::i18n::desc_log()
    }
    fn handle(&self, _args: &str, ctx: &CommandContext) -> String {
        render_log_page(ctx, 0).text
    }
}

pub fn parse_log_callback(data: &str) -> Option<usize> {
    data.strip_prefix("log:")?.parse().ok()
}

pub fn render_log_page(ctx: &CommandContext, offset: usize) -> LogPage {
    let total = ctx.log_ring.len();
    let entries = match ctx.log_ring.page(offset, LOG_PAGE_SIZE) {
        Ok(entries) => entries,
        Err(error) => {
            return LogPage {
                text: crate::i18n::log_read_failed(&error.to_string()),
                keyboard: None,
                format: MessageFormat::Plain,
            }
        }
    };
    let mut text = crate::i18n::log_header(entries.len(), total, offset, LOG_PAGE_SIZE);
    if entries.is_empty() {
        text.push_str(crate::i18n::log_empty());
    } else {
        for entry in &entries {
            text.push_str("<blockquote><b>");
            push_html_escaped(&mut text, &entry.timestamp);
            text.push_str("</b> ");
            text.push_str(if entry.forwarded { "✅ " } else { "🚫 " });
            text.push_str(entry.kind.label());
            text.push_str(" - ");
            push_html_escaped(&mut text, &entry.sender);
            text.push_str(": ");
            push_html_escaped(&mut text, &entry.body_preview);
            text.push_str("</blockquote>\n");
        }
    }
    let keyboard = log_keyboard(total, offset, entries.len());
    LogPage {
        text,
        keyboard,
        format: if entries.is_empty() {
            MessageFormat::Plain
        } else {
            MessageFormat::Html
        },
    }
}

fn push_html_escaped(out: &mut String, input: &str) {
    for ch in input.chars() {
        match ch {
            '&' => out.push_str("&amp;"),
            '<' => out.push_str("&lt;"),
            '>' => out.push_str("&gt;"),
            _ => out.push(ch),
        }
    }
}

fn log_keyboard(total: usize, offset: usize, page_len: usize) -> Option<InlineKeyboard> {
    let mut buttons = Vec::new();
    if offset > 0 {
        buttons.push(InlineKeyboardButton::new(
            crate::i18n::log_button_newer(),
            format!("log:{}", offset.saturating_sub(LOG_PAGE_SIZE)),
        ));
    }
    let older = offset.saturating_add(page_len);
    if page_len > 0 && older < total {
        buttons.push(InlineKeyboardButton::new(
            crate::i18n::log_button_older(),
            format!("log:{older}"),
        ));
    }
    (!buttons.is_empty()).then(|| InlineKeyboard::single_row(buttons))
}
