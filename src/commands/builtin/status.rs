use crate::commands::{Command, CommandContext};

pub struct StatusCommand;

impl Command for StatusCommand {
    fn name(&self) -> &'static str { "status" }
    fn description(&self) -> &'static str { "Show device health and stats" }

    fn handle(&self, _args: &str, ctx: &CommandContext) -> String {
        let uptime_s = ctx.uptime_ms / 1000;
        let h = uptime_s / 3600;
        let m = (uptime_s % 3600) / 60;
        let s = uptime_s % 60;

        let csq = ctx.modem_status.csq;
        let signal = if csq == 99 { "N/A".to_string() }
                     else { format!("{}/31 ({} dBm)", csq, csq_to_dbm(csq)) };

        let operator = if ctx.modem_status.operator.is_empty() {
            "unknown".to_string()
        } else {
            ctx.modem_status.operator.clone()
        };

        let reg = if ctx.modem_status.registered { "registered" } else { "not registered" };

        let queue_depth = ctx.send_queue.len();
        let log_count = ctx.log_ring.len();

        let fwd = crate::persist::load_bool(ctx.store, crate::persist::keys::FWD_ENABLED)
            .unwrap_or(true);

        format!(
            "📊 smsgate status\n\
             ⏱ Uptime: {:02}h {:02}m {:02}s\n\
             📶 Signal: {} — {}\n\
             🌐 Network: {}\n\
             📬 Queue: {} pending\n\
             📋 Log: {} messages\n\
             🔄 Forwarding: {}\n",
            h, m, s,
            signal, operator,
            reg,
            queue_depth,
            log_count,
            if fwd { "enabled" } else { "PAUSED" },
        )
    }
}

fn csq_to_dbm(csq: u8) -> i32 {
    -113 + 2 * csq as i32
}
