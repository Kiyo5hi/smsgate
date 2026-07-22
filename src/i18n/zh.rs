//! 中文 UI 字符串。

// ── 系统 / 生命周期 ─────────────────────────────────────────────────────────

pub fn started() -> &'static str {
    "✅ smsgate 已启动"
}
pub fn nvs_fail() -> &'static str {
    "⚠️ NVS 初始化失败，以无持久化模式运行。\
     黑名单和游标将在重启后重置。"
}
pub fn rebooting() -> &'static str {
    "♻️ 正在重启…"
}
pub fn low_heap(free_bytes: u32) -> String {
    format!("⚠️ 可用内存不足：{} 字节", free_bytes)
}
pub fn sms_sent_ok(phone: &str) -> String {
    format!("✅ 短信已发出：{}", phone)
}
pub fn sms_failed(phone: &str) -> String {
    format!("❌ 发往 {} 的短信发送失败（已达最大重试次数）", phone)
}
pub fn poll_thread_stale(mins: u32) -> String {
    format!("⚠️ Telegram 轮询线程已 {} 分钟无响应", mins)
}
pub fn low_signal(csq: u8) -> String {
    format!("⚠️ 蜂窝信号弱（CSQ {}）", csq)
}
pub fn signal_restored(csq: u8) -> String {
    format!("✅ 蜂窝信号已恢复（CSQ {}）", csq)
}
pub fn operator_changed(old: &str, new: &str) -> String {
    format!("⚠️ 运营商变更：{} → {}", old, new)
}

// ── 转发 ────────────────────────────────────────────────────────────────────

pub fn sms_received(sender: &str, ts: &str, body: &str) -> String {
    format!("📱 来自 {}\n🕐 {}\n\n{}", sender, ts, body)
}
pub fn sms_received_html(sender: &str, ts: &str, body: &str) -> String {
    format!("📱 来自 <code>{}</code>\n🕐 {}\n\n{}", sender, ts, body)
}
pub fn mms_notification(
    url: &str,
    message_size: Option<u64>,
    expiry: Option<crate::mms::MmsExpiry>,
) -> String {
    use core::fmt::Write as _;
    let mut out = String::with_capacity(url.len() + 96);
    out.push_str("📎 彩信通知（未下载内容）\n下载地址：");
    out.push_str(url);
    if let Some(bytes) = message_size {
        let _ = write!(out, "\n大小：{} B", bytes);
    }
    if let Some(expiry) = expiry {
        match expiry {
            crate::mms::MmsExpiry::RelativeSeconds(seconds) => {
                let _ = write!(out, "\n过期：收到后{}", format_duration(seconds));
            }
            crate::mms::MmsExpiry::AbsoluteUnixSeconds(seconds) => {
                let _ = write!(out, "\n过期：Unix 时间戳 {}", seconds);
            }
        }
    }
    out
}

fn format_duration(seconds: u64) -> String {
    if seconds.is_multiple_of(86_400) {
        format!("{} 天", seconds / 86_400)
    } else if seconds.is_multiple_of(3_600) {
        format!("{} 小时", seconds / 3_600)
    } else if seconds.is_multiple_of(60) {
        format!("{} 分钟", seconds / 60)
    } else {
        format!("{} 秒", seconds)
    }
}
pub fn incoming_call(display: &str) -> String {
    format!("📞 来电：{}", display)
}

// ── /status ──────────────────────────────────────────────────────────────────

pub fn status_op_unknown() -> &'static str {
    "未知"
}
pub fn status_reg_ok() -> &'static str {
    "已注册"
}
pub fn status_reg_no() -> &'static str {
    "未注册"
}
pub fn status_fwd_on() -> &'static str {
    "已启用"
}
pub fn status_fwd_off() -> &'static str {
    "已暂停"
}
pub fn status_build(commit: &str) -> String {
    format!("🔖 版本：{}", commit)
}

#[allow(clippy::too_many_arguments)]
pub fn format_status(
    uptime: &str,
    signal: &str,
    operator: &str,
    registered: bool,
    free_heap_kb: u32,
    queue_n: usize,
    blocked_n: usize,
    log_n: usize,
    fwd_on: bool,
    last_sms: Option<(&str, &str)>,
    wifi_info: &str,
) -> String {
    let reg = if registered {
        status_reg_ok()
    } else {
        status_reg_no()
    };
    let fwd = if fwd_on {
        status_fwd_on()
    } else {
        status_fwd_off()
    };
    let wifi_line = if wifi_info.is_empty() {
        String::new()
    } else {
        format!("📶 WiFi：{}\n", wifi_info)
    };
    let heap_line = if free_heap_kb > 0 {
        format!("💾 内存：{} KB 可用\n", free_heap_kb)
    } else {
        String::new()
    };
    let last_line = match last_sms {
        Some((sender, ts)) => format!("📩 最近短信：{}（{}）\n", sender, ts),
        None => String::new(),
    };
    format!(
        "📊 smsgate 状态\n\
         ⏱ 运行时间：{}\n\
         {}📶 信号：{} — {}\n\
         🌐 网络：{}\n\
         {}📬 队列：{} 条待发\n\
         🚫 屏蔽：{} 个号码\n\
         📋 日志：{} 条\n\
         🔄 转发：{}\n\
         {}",
        uptime,
        wifi_line,
        signal,
        operator,
        reg,
        heap_line,
        queue_n,
        blocked_n,
        log_n,
        fwd,
        last_line,
    )
}

// ── /send ────────────────────────────────────────────────────────────────────

pub fn send_usage() -> &'static str {
    "用法：/send <号码> <消息内容>"
}
pub fn send_invalid_number() -> &'static str {
    "无效号码"
}
pub fn send_empty_body() -> &'static str {
    "消息内容为空"
}
pub fn send_too_long() -> &'static str {
    "消息过长（超过 10 条短信）"
}

pub fn send_queued(phone: &str, preview: &str, truncated: bool, parts: usize) -> String {
    let ellipsis = if truncated { "…" } else { "" };
    format!(
        "已入队：{} → \"{}{}\"（{} 条）",
        phone, preview, ellipsis, parts
    )
}
pub fn send_rate_limited() -> &'static str {
    "频率限制：每分钟最多发送 5 条 /send 命令。"
}

// ── /log ─────────────────────────────────────────────────────────────────────

pub fn log_empty() -> &'static str {
    "暂无事件记录。"
}
pub fn log_header(n: usize, total: usize, offset: usize, page: usize) -> String {
    format!(
        "事件记录 {}-{} / {}：\n",
        offset.saturating_add(1).min(total),
        offset.saturating_add(n).min(total),
        total.max(page.min(total))
    )
}
pub fn log_read_failed(error: &str) -> String {
    format!("读取日志失败：{}", error)
}
pub fn log_button_newer() -> &'static str {
    "较新"
}
pub fn log_button_older() -> &'static str {
    "较旧"
}

// ── /block + /unblock ────────────────────────────────────────────────────────

pub fn block_usage() -> &'static str {
    "用法：/block <号码>"
}
pub fn block_ok(phone: &str) -> String {
    format!("已屏蔽：{}", phone)
}
pub fn blocklist_empty() -> &'static str {
    "屏蔽名单为空。"
}
pub fn blocklist_header(n: usize) -> String {
    format!("屏蔽名单（{} 个）：\n", n)
}

pub fn unblock_usage() -> &'static str {
    "用法：/unblock <号码>"
}
pub fn unblock_not_found(phone: &str) -> String {
    format!("{} 不在屏蔽名单中。", phone)
}
pub fn unblock_ok(phone: &str) -> String {
    format!("已解除屏蔽：{}", phone)
}

// ── /pause + /resume ─────────────────────────────────────────────────────────

pub fn pause_ok(mins: u32) -> String {
    format!("转发已暂停 {} 分钟。", mins)
}
pub fn resume_already_active() -> &'static str {
    "转发已处于启用状态。"
}
pub fn resume_ok() -> &'static str {
    "转发已恢复。"
}

// ── /restart ─────────────────────────────────────────────────────────────────

pub fn restart_ok() -> &'static str {
    "正在重启…"
}

// ── /update ─────────────────────────────────────────────────────────────────

pub fn update_disabled() -> &'static str {
    "OTA 已禁用（未配置 URL）。"
}
pub fn update_starting() -> &'static str {
    "正在开始 OTA 更新…请勿断电。"
}
pub fn update_confirming() -> &'static str {
    "正在确认当前固件有效。"
}
pub fn update_confirm_not_manual() -> &'static str {
    "确认模式为\"auto\"——无需手动确认。"
}
pub fn update_usage() -> &'static str {
    "用法：/update 或 /update confirm"
}
pub fn update_success() -> String {
    "OTA 完成——正在重启…".into()
}
pub fn update_failed(e: &str) -> String {
    format!("OTA 失败：{}", e)
}
pub fn update_confirmed() -> &'static str {
    "固件已确认——回滚已禁用。"
}
pub fn ota_upload_starting(name: &str, size: Option<u64>) -> String {
    format!(
        "开始安装固件：{}（{} 字节）",
        name,
        size.map(|v| v.to_string())
            .unwrap_or_else(|| "未知大小".into())
    )
}
pub fn ota_upload_progress(written: usize, total: Option<usize>) -> String {
    match total {
        Some(total) => format!("固件更新：{} / {} 字节", written, total),
        None => format!("固件更新：{} 字节", written),
    }
}
pub fn ota_upload_wifi_required() -> &'static str {
    "Telegram 文件 OTA 仅支持 WiFi 连接。"
}

// ── 命令描述（Telegram 自动补全）────────────────────────────────────────────

pub fn desc_help() -> &'static str {
    "显示帮助"
}
pub fn desc_status() -> &'static str {
    "设备状态"
}
pub fn desc_send() -> &'static str {
    "发送短信：/send <号码> <内容>"
}
pub fn desc_log() -> &'static str {
    "分页查看事件记录"
}
pub fn desc_block() -> &'static str {
    "屏蔽号码"
}
pub fn desc_unblock() -> &'static str {
    "解除屏蔽"
}
pub fn desc_pause() -> &'static str {
    "暂停转发（默认 60 分钟）"
}
pub fn desc_resume() -> &'static str {
    "恢复转发"
}
pub fn desc_restart() -> &'static str {
    "重启设备"
}
pub fn desc_update() -> &'static str {
    "OTA 固件更新（/update confirm 确认）"
}
