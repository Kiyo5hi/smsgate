use crate::commands::{Command, CommandContext};
use crate::log_ring::{LogEntry, LogKind, LogRing, LOG_PAGE_SIZE};
use crate::modem::CSQ_UNKNOWN;

pub struct StatusCommand;

impl Command for StatusCommand {
    fn name(&self) -> &'static str {
        "status"
    }
    fn description(&self) -> &'static str {
        crate::i18n::desc_status()
    }

    fn handle(&self, _args: &str, ctx: &CommandContext) -> String {
        let uptime = format_uptime(ctx.uptime_ms / 1000);

        let csq = ctx.modem_status.csq;
        let signal = if csq == CSQ_UNKNOWN {
            "N/A".to_string()
        } else {
            format!("{}/31 ({} dBm)", csq, csq_to_dbm(csq))
        };

        let operator = if ctx.modem_status.operator.is_empty() {
            crate::i18n::status_op_unknown().to_string()
        } else {
            ctx.modem_status.operator.clone()
        };

        let queue_n = ctx.send_queue.len();
        let log_n = ctx.log_ring.len();
        let blocked_n = crate::bridge::forwarder::load_blocklist(ctx.store).len();
        let fwd_on =
            crate::persist::load_bool(ctx.store, crate::persist::keys::FWD_ENABLED).unwrap_or(true);
        let free_heap_kb = ctx.free_heap_bytes / 1024;

        let latest_sms = latest_sms_entry(ctx.log_ring);
        let last_sms = latest_sms
            .as_ref()
            .map(|e| (e.sender.as_str(), e.timestamp.as_str()));

        let mut out = crate::i18n::format_status(
            &uptime,
            &signal,
            &operator,
            ctx.modem_status.registered,
            free_heap_kb,
            queue_n,
            blocked_n,
            log_n,
            fwd_on,
            last_sms,
            ctx.wifi_info,
        );
        out.push_str(&crate::i18n::status_build(
            crate::config::Config::GIT_COMMIT,
        ));
        out
    }
}

fn latest_sms_entry(log: &LogRing) -> Option<LogEntry> {
    let total = log.len();
    let mut offset = 0;
    while offset < total {
        let entries = match log.page(offset, LOG_PAGE_SIZE) {
            Ok(entries) => entries,
            Err(e) => {
                log::warn!("[status] log read failed: {}", e);
                return None;
            }
        };
        if entries.is_empty() {
            return None;
        }
        if let Some(entry) = entries.into_iter().find(|e| e.kind == LogKind::Sms) {
            return Some(entry);
        }
        offset = offset.saturating_add(LOG_PAGE_SIZE);
    }
    None
}

fn format_uptime(total_seconds: u64) -> String {
    let seconds = total_seconds % 60;
    let total_minutes = total_seconds / 60;
    let minutes = total_minutes % 60;
    let total_hours = total_minutes / 60;
    let hours = total_hours % 24;

    if total_hours < 24 {
        return format!("{:02}h {:02}m {:02}s", total_hours, minutes, seconds);
    }

    let total_days = total_hours / 24;
    if total_days < 365 {
        return format!("{}d {:02}h {:02}m", total_days, hours, minutes);
    }

    let years = total_days / 365;
    let days = total_days % 365;
    format!("{}y {}d {:02}h", years, days, hours)
}

fn csq_to_dbm(csq: u8) -> i32 {
    -113 + 2 * csq as i32
}
