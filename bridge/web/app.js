const state = {
  overview: null,
  busy: false,
  toastTimer: null,
  moduleSaving: {},
  xiaozhiLoaded: false,
  xiaozhiSaving: false,
  displayDirty: false,
  displaySaving: false,
  firmwarePortsSignature: "",
  bluetoothDevicesSignature: "",
};

const byId = (id) => document.getElementById(id);
const setText = (id, value) => {
  const target = byId(id);
  if (target) target.textContent = value;
};
const DISPLAY_TIMEOUT_OPTIONS = [10, 30, 60, 180, 300, 600, 1800, 0];
const DISPLAY_TIMEOUT_PAIRS = [
  { screenOff: "screen-off-timeout", sleep: "sleep-timeout" },
  { screenOff: "charging-screen-off-timeout", sleep: "charging-sleep-timeout" },
];


function closeUiSelect(control) {
  if (!control) return;
  control.classList.remove("open");
  control.querySelector(".ui-select-trigger")?.setAttribute("aria-expanded", "false");
  const menu = control.querySelector(".ui-select-menu");
  if (menu) menu.hidden = true;
}

function closeAllUiSelects(except = null) {
  document.querySelectorAll("[data-ui-select]").forEach((control) => {
    if (control !== except) closeUiSelect(control);
  });
}

function openUiSelect(control, focusSelected = false) {
  closeAllUiSelects(control);
  control.classList.add("open");
  control.querySelector(".ui-select-trigger")?.setAttribute("aria-expanded", "true");
  const menu = control.querySelector(".ui-select-menu");
  if (!menu) return;
  menu.hidden = false;
  if (focusSelected) {
    (menu.querySelector('[aria-selected="true"]:not(:disabled)') ||
      menu.querySelector("button:not(:disabled)"))?.focus();
  }
}

function setUiSelectValue(id, value, dispatch = false) {
  const input = byId(id);
  const control = input?.closest("[data-ui-select]");
  if (!input || !control) return;
  const normalized = String(value);
  const selected = [...control.querySelectorAll("[data-value]")]
    .find((option) => option.dataset.value === normalized);
  if (!selected) return;
  input.value = normalized;
  setText(`${id}-value`, selected.textContent.trim());
  control.querySelectorAll("[data-value]").forEach((option) => {
    option.setAttribute("aria-selected", String(option === selected));
  });
  if (dispatch) {
    input.dispatchEvent(new Event("input", { bubbles: true }));
    input.dispatchEvent(new Event("change", { bubbles: true }));
  }
}

function refreshTimeoutConstraints() {
  DISPLAY_TIMEOUT_PAIRS.forEach(({ screenOff: screenOffId, sleep: sleepId }) => {
    const screenOff = Number(byId(screenOffId).value);
    const sleep = Number(byId(sleepId).value);
    document.querySelectorAll(`[data-ui-select="${screenOffId}"] [data-value]`).forEach((option) => {
      const value = Number(option.dataset.value);
      option.disabled = sleep !== 0 && (value === 0 || value > sleep);
    });
    document.querySelectorAll(`[data-ui-select="${sleepId}"] [data-value]`).forEach((option) => {
      const value = Number(option.dataset.value);
      option.disabled = screenOff === 0 ? value !== 0 : value !== 0 && value < screenOff;
    });
  });
}

function normalizeTimeoutPair(changedId, screenOffId, sleepId) {
  const screenOff = Number(byId(screenOffId).value);
  const sleep = Number(byId(sleepId).value);
  if (screenOff === 0) {
    if (sleep !== 0) setUiSelectValue(sleepId, 0);
  } else if (sleep !== 0 && sleep < screenOff) {
    if (changedId === screenOffId) {
      const nextSleep = DISPLAY_TIMEOUT_OPTIONS.find((value) => value === 0 || value >= screenOff) ?? 0;
      setUiSelectValue(sleepId, nextSleep);
    } else {
      const earlier = DISPLAY_TIMEOUT_OPTIONS.filter((value) => value > 0 && value <= sleep);
      setUiSelectValue(screenOffId, earlier.at(-1) ?? 10);
    }
  }
  refreshTimeoutConstraints();
}

function setupUiSelect(control) {
    if (!control || control.dataset.uiSelectReady === "true") return;
    const id = control.dataset.uiSelect;
    const trigger = control.querySelector(".ui-select-trigger");
    const menu = control.querySelector(".ui-select-menu");
    if (!id || !trigger || !menu || !byId(id)) return;
    control.dataset.uiSelectReady = "true";
    trigger.addEventListener("click", () => {
      if (control.classList.contains("open")) closeUiSelect(control);
      else openUiSelect(control);
    });
    trigger.addEventListener("keydown", (event) => {
      if (event.key === "ArrowDown" || event.key === "ArrowUp") {
        event.preventDefault();
        openUiSelect(control, true);
      } else if (event.key === "Escape") {
        closeUiSelect(control);
      }
    });
    menu.addEventListener("keydown", (event) => {
      const options = [...menu.querySelectorAll("button:not(:disabled)")];
      const index = options.indexOf(document.activeElement);
      if (["ArrowDown", "ArrowUp", "Home", "End"].includes(event.key)) {
        event.preventDefault();
        const next = event.key === "Home" ? 0 : event.key === "End" ? options.length - 1 :
          event.key === "ArrowDown" ? (index + 1 + options.length) % options.length :
          (index - 1 + options.length) % options.length;
        options[next]?.focus();
      } else if (event.key === "Escape") {
        event.preventDefault();
        closeUiSelect(control);
        trigger.focus();
      }
    });
    menu.addEventListener("click", (event) => {
      const option = event.target.closest("[data-value]");
      if (!option || option.disabled) return;
      setUiSelectValue(id, option.dataset.value, true);
      closeUiSelect(control);
      trigger.focus();
    });
    setUiSelectValue(id, byId(id).value);
}

function setupUiSelects() {
  document.querySelectorAll("[data-ui-select]").forEach((control) => {
    setupUiSelect(control);
  });
  refreshTimeoutConstraints();
  document.addEventListener("click", (event) => {
    if (!event.target.closest("[data-ui-select]")) closeAllUiSelects();
  });
}

function showToast(message) {
  const toast = byId("toast");
  toast.textContent = message;
  toast.classList.add("visible");
  window.clearTimeout(state.toastTimer);
  state.toastTimer = window.setTimeout(() => toast.classList.remove("visible"), 2400);
}

function formatNumber(value, available = true) {
  if (!available || !Number.isFinite(Number(value))) return "--";
  return new Intl.NumberFormat("zh-CN").format(Number(value));
}

function formatDuration(seconds) {
  const value = Math.max(0, Number(seconds) || 0);
  if (value < 60) return `${Math.floor(value)} 秒`;
  if (value < 3600) return `${Math.floor(value / 60)} 分钟`;
  const hours = Math.floor(value / 3600);
  const minutes = Math.floor((value % 3600) / 60);
  return `${hours} 小时 ${minutes} 分钟`;
}

function formatTaskDuration(seconds) {
  const value = Math.max(0, Math.floor(Number(seconds) || 0));
  const hours = Math.floor(value / 3600);
  const minutes = Math.floor((value % 3600) / 60);
  const remainingSeconds = value % 60;
  if (hours > 0) return `耗时 ${hours} 小时 ${minutes} 分钟 ${remainingSeconds} 秒`;
  if (minutes > 0) return `耗时 ${minutes} 分钟 ${remainingSeconds} 秒`;
  return `耗时 ${remainingSeconds} 秒`;
}

function formatTime(epoch) {
  if (!epoch) return "尚未更新";
  const date = new Date(Number(epoch) * 1000);
  if (Number.isNaN(date.getTime())) return "尚未更新";
  return new Intl.DateTimeFormat("zh-CN", {
    month: "2-digit", day: "2-digit", hour: "2-digit", minute: "2-digit", second: "2-digit",
  }).format(date);
}

function formatClock(epoch) {
  if (!epoch) return "--";
  const date = new Date(Number(epoch) * 1000);
  if (Number.isNaN(date.getTime())) return "--";
  return new Intl.DateTimeFormat("zh-CN", {hour: "2-digit", minute: "2-digit"}).format(date);
}

function formatMinutes(value) {
  const minutes = Math.max(0, Number(value) || 0);
  if (!minutes) return "--";
  if (minutes < 60) return `${minutes} 分钟`;
  return `${Math.floor(minutes / 60)} 小时 ${minutes % 60} 分钟`;
}

async function apiPost(path, payload = {}) {
  const response = await fetch(path, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(payload),
  });
  if (!response.ok) throw new Error(await response.text());
  return response.json();
}

function qqmusicVipLabel(status = {}) {
  if (!status.vip) return "普通账号";
  // 豪华版 (Super VIP) and 年费 are distinct badges on the QQ Music profile,
  // so show both rather than a single generic "VIP".
  const parts = [];
  if (status.vip_year) parts.push("年费");
  parts.push(status.vip_super ? "豪华版" : "绿钻");
  if (status.vip_level) parts.push("Lv" + status.vip_level);
  return parts.join(" ");
}

function renderQqmusic(status = {}) {
  const logged = Boolean(status.logged_in);
  byId("qqmusic-state").textContent = logged ? "已登录" : "未登录";
  byId("qqmusic-logged-out").hidden = logged;
  byId("qqmusic-logged-in").hidden = !logged;
  if (logged) {
    byId("qqmusic-nickname").textContent = status.nickname || status.uin || "QQ 音乐用户";
    byId("qqmusic-vip").textContent = qqmusicVipLabel(status);
  } else {
    byId("qqmusic-vip").textContent = "-";
    byId("qqmusic-qr-wrap").hidden = true;
  }
}

let qqPollTimer = null;

async function startQqLogin() {
  byId("qqmusic-qr-wrap").hidden = false;
  byId("qqmusic-qr-hint").textContent = "已打开登录窗口，请在新窗口里用手机 QQ 扫码登录…";
  await apiPost("/api/v1/admin/qqmusic/browser-login");
  if (qqPollTimer) clearInterval(qqPollTimer);
  const deadline = Date.now() + 180000;
  qqPollTimer = setInterval(async () => {
    try {
      const status = await fetch("/api/v1/admin/qqmusic/status").then((r) => r.json());
      if (status.logged_in) {
        clearInterval(qqPollTimer);
        qqPollTimer = null;
        renderQqmusic(status);
        byId("qqmusic-qr-hint").textContent = "登录成功，设备已获得授权";
      } else if (Date.now() > deadline) {
        clearInterval(qqPollTimer);
        qqPollTimer = null;
        byId("qqmusic-qr-hint").textContent = "登录窗口已超时，请重新点击登录";
      }
    } catch (error) {
      console.warn("qqmusic status poll failed", error);
    }
  }, 2000);
}

async function logoutQq() {
  const result = await apiPost("/api/v1/admin/qqmusic/logout");
  renderQqmusic(result);
}

function renderXiaozhi(config = {}, enabled = true, bluetooth = {}) {
  if (arguments.length > 3) renderQqmusic(arguments[3] || {});
  if (!state.xiaozhiLoaded && !state.xiaozhiSaving) {
    byId("xiaozhi-server-url").value = config.server_url || "https://api.tenclass.net/xiaozhi/ota/";
    byId("xiaozhi-wake-word").value = config.wake_word || "你好小智";
    byId("xiaozhi-voice").value = config.voice || "default";
    byId("xiaozhi-mcp-enabled").checked = config.mcp_enabled !== false;
byId("xiaozhi-mcp-url").value = config.mcp_url || "";
    state.xiaozhiLoaded = true;
  }
  if (!state.moduleSaving.xiaozhi) byId("xiaozhi-enabled").checked = enabled;
  byId("xiaozhi-content").hidden = !enabled;
  const connection = byId("xiaozhi-connection");
  const device = bluetooth.device_status?.xiaozhi || {};
  const active = Boolean(config.enabled) && enabled;
  const connected = Boolean(device.connected);
  const activationRequired = Boolean(device.activation_required);
  connection.dataset.state = connected ? "online" : active ? "waiting" : "offline";
  connection.textContent = !enabled ? "模块已关闭" :
    connected ? "小智在线" :
    activationRequired ? "等待配对" : active ? "已启用" : "已保存，未启用";
  setText("xiaozhi-nav-state", !enabled ? "已关闭" :
    connected ? "在线" : activationRequired ? "待配对" : active ? "已启用" : "已配置");
  const activation = byId("xiaozhi-activation");
  const code = device.activation_code || "";
  if (activation) {
    activation.hidden = !enabled || (!activationRequired && !code);
    setText("xiaozhi-activation-code", code || "等待设备上报");
    setText("xiaozhi-activation-message",
      device.activation_message || "请在小智官网登录后输入设备码完成配对。");
    const link = byId("xiaozhi-pairing-link");
    if (link) link.href = "https://xiaozhi.me/";
  }
}


async function saveXiaozhi(event) {
  event.preventDefault();
  if (state.xiaozhiSaving) return;
  state.xiaozhiSaving = true;
  byId("xiaozhi-save-state").textContent = "正在保存";
  try {
    const response = await fetch("/api/v1/admin/xiaozhi", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        enabled: byId("xiaozhi-enabled").checked,
        server_url: byId("xiaozhi-server-url").value.trim(),
        wake_word: byId("xiaozhi-wake-word").value.trim(),
        voice: byId("xiaozhi-voice").value.trim(),
        mcp_enabled: byId("xiaozhi-mcp-enabled").checked,
mcp_url: byId("xiaozhi-mcp-url").value.trim(),
      }),
    });
    const result = await response.json();
    if (!response.ok) throw new Error(result.error || "小智设置保存失败");
    state.xiaozhiLoaded = false;
    setText("xiaozhi-save-state", "已保存");
    showToast("小智设置已保存");
  } catch (error) {
    setText("xiaozhi-save-state", "保存失败");
    showToast(error.message || "小智设置保存失败");
  } finally {
    state.xiaozhiSaving = false;
    await refreshOverview();
  }
}

function setBridgeState(bridge) {
  const indicator = byId("bridge-state");
  const bridgeOnline = bridge.online !== false;
  indicator.dataset.state = bridgeOnline ? "online" : "error";
  setText("bridge-state-label", bridgeOnline ? "管理中心在线" : "管理中心连接失败");
  setText("bridge-uptime", formatDuration(Date.now() / 1000 - Number(bridge.started_at_epoch || 0)));
  setText("device-url", bridge.device_url || "--");
  setText("bridge-token", bridge.token || "--");
  setText("local-url", bridge.local_url || "--");
}

function formatBytes(value) {
  const bytes = Number(value) || 0;
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KiB`;
  return `${(bytes / (1024 * 1024)).toFixed(2)} MiB`;
}

function renderFirmware(firmware = {}) {
  const packageInfo = firmware.package || {};
  const running = firmware.operation_state === "running";
  const success = firmware.operation_state === "success";
  const ready = Boolean(firmware.tool_ready && packageInfo.ready);
  const ports = Array.isArray(firmware.ports) ? firmware.ports : [];
  const dotiiPorts = ports.filter((port) => port.dotii);
  const badge = byId("firmware-badge");
  const identified = dotiiPorts.length > 0;
  badge.dataset.state = running ? "running" : success || identified || ready ? "ready" : "error";
  badge.textContent = running ? "烧录中" : success ? "已完成" : identified ? "已识别" : ready ? "固件就绪" : "需要处理";
  setText("firmware-version", packageInfo.version ? `v${packageInfo.version}` : "--");
  setText("firmware-size", packageInfo.app_size ? formatBytes(packageInfo.app_size) : "--");
  setText("firmware-hash", packageInfo.app_sha256 ? packageInfo.app_sha256.slice(0, 20) : "--");

  const signature = JSON.stringify(ports.map((port) => [port.port, port.name, port.dotii]));
  const input = byId("firmware-port");
  const control = input.closest("[data-ui-select]");
  const menu = control.querySelector(".ui-select-menu");
  if (signature !== state.firmwarePortsSignature) {
    state.firmwarePortsSignature = signature;
    const previous = input.value;
    menu.replaceChildren();
    ports.forEach((port) => {
      const option = document.createElement("button");
      option.type = "button";
      option.setAttribute("role", "option");
      option.dataset.value = port.port;
      option.disabled = !port.dotii;
      option.textContent = `${port.port} · ${port.dotii ? "Dotii" : port.name}`;
      menu.append(option);
    });
    const selected = dotiiPorts.some((port) => port.port === previous) ? previous : dotiiPorts[0]?.port || "";
    input.value = selected;
    if (selected) setUiSelectValue("firmware-port", selected);
    else setText("firmware-port-value", ports.length ? "未识别到 Dotii" : "未发现串口设备");
  }
  control.querySelector(".ui-select-trigger").disabled = running || !ports.length;
  const progress = Math.max(0, Math.min(100, Number(firmware.progress) || 0));
  byId("firmware-progress").firstElementChild.style.width = `${progress}%`;
  byId("firmware-progress").closest(".firmware-panel").dataset.running = String(running);
  const firmwareDetail = firmware.operation_state === "idle"
    ? identified
      ? "已识别 Dotii ESP32-S3，可执行固件烧录"
      : ready ? "请通过 USB 连接 Dotii" : packageInfo.error || "烧录组件不可用"
    : firmware.operation_detail;
  setText("firmware-detail", firmwareDetail);
  byId("firmware-refresh").disabled = running;
  byId("firmware-flash").disabled = running || !ready || !input.value || !dotiiPorts.some((port) => port.port === input.value);
  byId("firmware-flash").textContent = running ? `正在烧录 ${progress}%` : "烧录 Dotii";
}

function renderBluetooth(bluetooth = {}) {
  const available = bluetooth.available !== false;
  const ready = Boolean(bluetooth.dependency_ready);
  const running = bluetooth.operation_state === "running";
  const devices = Array.isArray(bluetooth.devices) ? bluetooth.devices : [];
  const status = bluetooth.device_status || {};
  const connected = bluetooth.device_connected === true;
  const recognized = devices.length > 0 || Boolean(bluetooth.last_address);
  const blocked = ["denied", "bluetooth_off", "unavailable"].includes(bluetooth.permission_state);
  const badge = byId("bluetooth-badge");
  badge.dataset.state = running ? "running" : blocked || !available || !ready ? "error" : "ready";
  badge.textContent = running ? "处理中" : blocked ? "需要处理" : connected ? "已连接" : recognized ? "已识别" : ready ? "蓝牙就绪" : "需要处理";

  const input = byId("bluetooth-device");
  const control = input.closest("[data-ui-select]");
  const menu = control.querySelector(".ui-select-menu");
  const signature = JSON.stringify(devices.map((device) => [device.address, device.name, device.rssi]));
  if (signature !== state.bluetoothDevicesSignature) {
    state.bluetoothDevicesSignature = signature;
    const previous = input.value || bluetooth.last_address || "";
    menu.replaceChildren();
    devices.forEach((device) => {
      const option = document.createElement("button");
      option.type = "button";
      option.setAttribute("role", "option");
      option.dataset.value = device.address;
      const signal = Number.isFinite(Number(device.rssi)) ? `${device.rssi} dBm` : "已登记";
      option.textContent = `${device.name || "Dotii"} · ${signal}`;
      menu.append(option);
    });
    const selected = devices.some((device) => device.address === previous) ? previous : devices[0]?.address || "";
    input.value = selected;
    if (selected) setUiSelectValue("bluetooth-device", selected);
    else setText("bluetooth-device-value", devices.length ? "请选择 Dotii" : "未发现附近设备");
  }
  control.querySelector(".ui-select-trigger").disabled = running || !devices.length;
  const managementStatus = typeof status.bridge === "string"
    ? status.bridge.replace("桥接", "管理中心")
    : "管理中心状态未知";
  const operationDetail = bluetooth.operation_state === "idle" ? "" : bluetooth.operation_detail;
  const liveDetail = status.name
    ? `${status.name} · ${connected ? "蓝牙已连接" : "已识别，蓝牙暂未连接"} · ${status.wifi ? "Wi-Fi 已连接" : "Wi-Fi 未连接"} · ${managementStatus}`
    : operationDetail;
  const permissionDetails = {
    unknown: "首次扫描时 macOS 会请求蓝牙权限",
    denied: "请在系统设置 > 隐私与安全性 > 蓝牙中允许 Dotii 管理中心",
    bluetooth_off: "蓝牙已关闭，请先在系统设置中打开蓝牙",
    unavailable: "当前 Mac 没有可用的低功耗蓝牙功能",
    timeout: "等待蓝牙连接或系统配对确认超时，请确认 Dotii 在附近后重试",
    stale_pairing: "Dotii 已重置配对，但这台 Mac 仍保留旧记录；请在系统设置 > 蓝牙中忽略 Dotii，然后重新扫描",
    not_found: "暂时无法重新连接已识别的 Dotii，请点击“扫描 Dotii”重试",
  };
  const fallbackDetail = !available
    ? "当前平台尚未支持 Dotii 蓝牙配网"
    : !ready
      ? "当前安装缺少蓝牙连接组件"
      : permissionDetails[bluetooth.permission_state] || "请扫描附近的 Dotii";
  const connectionDetail = !connected && bluetooth.connection_detail
    ? bluetooth.connection_detail : "";
  setText("bluetooth-detail", connectionDetail || liveDetail || fallbackDetail);
  byId("bluetooth-install").hidden = ready || !available;
  byId("bluetooth-install").disabled = running;
  byId("bluetooth-scan").disabled = running || !available || !ready;
  byId("bluetooth-configure").disabled = running || !available || !ready || !input.value || !byId("bluetooth-ssid").value.trim();
  byId("bluetooth-configure").textContent = running ? "正在连接…" : "保存并连接";
}


function renderOverview(overview) {
  state.overview = overview;
  const snapshot = overview.snapshot || {};
  const bridge = overview.bridge || {};
  const modules = Object.fromEntries((overview.modules || []).map((module) => [module.id, module]));
  const xiaozhiEnabled = modules.xiaozhi?.enabled !== false;

  setBridgeState(bridge);
  renderBluetooth(overview.bluetooth || {});
  renderFirmware(overview.firmware || {});
  renderXiaozhi(overview.xiaozhi_config || {}, xiaozhiEnabled, overview.bluetooth || {}, overview.qqmusic || {});
  const displayConfig = overview.display_config || snapshot.display || {};
  if (!state.displayDirty && !state.displaySaving) {
    const angle = Math.max(80, Math.min(100, Number(displayConfig.docked_rotation_tenths || 900) / 10));
    const screenOffTimeout = DISPLAY_TIMEOUT_OPTIONS.includes(Number(displayConfig.screen_off_timeout_seconds))
      ? Number(displayConfig.screen_off_timeout_seconds) : 60;
    const sleepTimeout = DISPLAY_TIMEOUT_OPTIONS.includes(Number(displayConfig.sleep_timeout_seconds))
      ? Number(displayConfig.sleep_timeout_seconds) : 300;
    const chargingScreenOffTimeout = DISPLAY_TIMEOUT_OPTIONS.includes(Number(displayConfig.charging_screen_off_timeout_seconds))
      ? Number(displayConfig.charging_screen_off_timeout_seconds) : screenOffTimeout;
    const chargingSleepTimeout = DISPLAY_TIMEOUT_OPTIONS.includes(Number(displayConfig.charging_sleep_timeout_seconds))
      ? Number(displayConfig.charging_sleep_timeout_seconds) : sleepTimeout;
    byId("dock-angle").value = angle.toFixed(1);
    setUiSelectValue("screen-off-timeout", screenOffTimeout);
    setUiSelectValue("sleep-timeout", sleepTimeout);
    setUiSelectValue("charging-screen-off-timeout", chargingScreenOffTimeout);
    setUiSelectValue("charging-sleep-timeout", chargingSleepTimeout);
    setUiSelectValue("screen-off-page", "none");
    refreshTimeoutConstraints();
    setText("dock-angle-output", `${angle.toFixed(1)}°`);
    setText("display-save-state", "已同步到 Dotii 管理中心");
  }
  byId("fatal-state").hidden = true;
}

function checkText(value) {
  return value ? "正常" : "异常";
}

async function refreshOverview({ announce = false } = {}) {
  if (state.busy) return;
  state.busy = true;
  byId("refresh-button").disabled = true;
  try {
    const response = await fetch("/api/v1/admin/overview", { cache: "no-store" });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    renderOverview(await response.json());
    if (announce) showToast("数据已刷新");
  } catch (error) {
    byId("fatal-state").hidden = false;
    const indicator = byId("bridge-state");
    indicator.dataset.state = "error";
    setText("bridge-state-label", "管理中心连接失败");
  } finally {
    state.busy = false;
    byId("refresh-button").disabled = false;
  }
}

async function saveDisplaySettings(event) {
  event.preventDefault();
  if (state.displaySaving) return;
  state.displaySaving = true;
  const angleTenths = Math.round(Number(byId("dock-angle").value) * 10);
  const screenOffTimeoutSeconds = Number(byId("screen-off-timeout").value);
  const sleepTimeoutSeconds = Number(byId("sleep-timeout").value);
  const chargingScreenOffTimeoutSeconds = Number(byId("charging-screen-off-timeout").value);
  const chargingSleepTimeoutSeconds = Number(byId("charging-sleep-timeout").value);
  const screenOffPage = byId("screen-off-page").value;
  try {
    const response = await fetch("/api/v1/admin/display", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        docked_rotation_tenths: angleTenths,
        screen_off_timeout_seconds: screenOffTimeoutSeconds,
        sleep_timeout_seconds: sleepTimeoutSeconds,
        charging_screen_off_timeout_seconds: chargingScreenOffTimeoutSeconds,
        charging_sleep_timeout_seconds: chargingSleepTimeoutSeconds,
        screen_off_page: screenOffPage,
      }),
    });
    const result = await response.json();
    if (!response.ok) throw new Error(result.error || "屏幕设置保存失败");
    state.displayDirty = false;
    setText("display-save-state", "已保存，Dotii 将自动同步");
    showToast("屏幕与睡眠设置已保存");
  } catch (error) {
    setText("display-save-state", "保存失败");
    showToast(error.message || "屏幕设置保存失败");
  } finally {
    state.displaySaving = false;
    await refreshOverview();
  }
}

async function updateModule(moduleId, enabled) {
  if (state.moduleSaving[moduleId]) return;
  state.moduleSaving[moduleId] = true;
  const checkbox = byId(`${moduleId}-enabled`);
  const content = byId(`${moduleId}-content`);
  if (content) content.hidden = !enabled;
  try {
    const response = await fetch("/api/v1/admin/modules", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ id: moduleId, enabled }),
    });
    const result = await response.json();
    if (!response.ok) throw new Error(result.error || "设置失败");
    const moduleName = { xiaozhi: "小智" }[moduleId] || moduleId;
    showToast(`${moduleName} 页面已${enabled ? "启用" : "关闭"}`);
  } catch (error) {
    checkbox.checked = !enabled;
    if (content) content.hidden = enabled;
    showToast(error.message || "设置失败");
  } finally {
    state.moduleSaving[moduleId] = false;
    await refreshOverview();
  }
}

async function refreshFirmware() {
  const button = byId("firmware-refresh");
  button.disabled = true;
  try {
    const response = await fetch("/api/v1/admin/firmware/refresh", {
      method: "POST", headers: { "Content-Type": "application/json" }, body: "{}",
    });
    const result = await response.json();
    if (!response.ok) throw new Error(result.error || "扫描失败");
    showToast("已重新扫描 Dotii 和固件包");
  } catch (error) {
    showToast(error.message || "扫描失败");
  } finally {
    await refreshOverview();
  }
}

async function bluetoothAction(path, payload = {}) {
  const response = await fetch(path, {
    method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(payload),
  });
  const result = await response.json();
  if (!response.ok) throw new Error(result.error || `HTTP ${response.status}`);
  return result;
}

async function installBluetooth() {
  try {
    await bluetoothAction("/api/v1/admin/bluetooth/install");
    showToast("正在准备蓝牙连接组件");
    await refreshOverview();
  } catch (error) { showToast(error.message || "蓝牙组件安装失败"); }
}

async function scanBluetooth() {
  try {
    await bluetoothAction("/api/v1/admin/bluetooth/scan");
    showToast("正在扫描附近的 Dotii");
    await refreshOverview();
  } catch (error) { showToast(error.message || "蓝牙扫描失败"); }
}

async function configureBluetooth() {
  const address = byId("bluetooth-device").value;
  const ssid = byId("bluetooth-ssid").value.trim();
  const password = byId("bluetooth-password").value;
  if (!address || !ssid) return showToast("请选择 Dotii 并填写 Wi-Fi 名称");
  try {
    await bluetoothAction("/api/v1/admin/bluetooth/configure", {
      address, ssid, password,
    });
    byId("bluetooth-password").value = "";
    showToast("正在通过蓝牙配置 Dotii");
    await refreshOverview();
  } catch (error) { showToast(error.message || "蓝牙配置失败"); }
}

async function flashFirmware() {
  const port = byId("firmware-port").value;
  if (!port) return;
  const version = state.overview?.firmware?.package?.version || "当前";
  if (!window.confirm(`将向 ${port} 写入 Dotii v${version}。烧录期间请勿拔线，是否继续？`)) return;
  const button = byId("firmware-flash");
  button.disabled = true;
  try {
    const response = await fetch("/api/v1/admin/firmware/flash", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ port, confirmation: "FLASH_DOTII" }),
    });
    const result = await response.json();
    if (!response.ok) throw new Error(result.error || "无法开始烧录");
    showToast("已开始烧录 Dotii");
    await refreshOverview();
  } catch (error) {
    showToast(error.message || "烧录启动失败");
    await refreshOverview();
  }
}


document.querySelectorAll(".module-tab").forEach((button) => {
  button.addEventListener("click", () => {
    document.querySelectorAll(".module-tab").forEach((tab) => {
      const active = tab === button;
      tab.classList.toggle("active", active);
      tab.setAttribute("aria-selected", active ? "true" : "false");
    });
    document.querySelectorAll(".panel").forEach((panel) => {
      const active = panel.id === `panel-${button.dataset.panel}`;
      panel.hidden = !active;
      panel.classList.toggle("active", active);
    });
  });
});

document.querySelectorAll("[data-copy]").forEach((button) => {
  button.addEventListener("click", async () => {
    const value = byId(button.dataset.copy).textContent;
    try {
      await navigator.clipboard.writeText(value);
      showToast("已复制");
    } catch (error) {
      showToast("复制失败，请手动选择文本");
    }
  });
});

byId("refresh-button").addEventListener("click", () => refreshOverview({ announce: true }));
byId("display-form").addEventListener("submit", saveDisplaySettings);
byId("dock-angle").addEventListener("input", (event) => {
  const angle = Number(event.target.value);
  state.displayDirty = true;
  setText("dock-angle-output", `${angle.toFixed(1)}°`);
  setText("display-save-state", "有尚未保存的修改");
});
DISPLAY_TIMEOUT_PAIRS.forEach(({ screenOff, sleep }) => {
  [screenOff, sleep].forEach((id) => byId(id).addEventListener("change", () => {
    normalizeTimeoutPair(id, screenOff, sleep);
    state.displayDirty = true;
    setText("display-save-state", "有尚未保存的修改");
  }));
});
byId("screen-off-page").addEventListener("change", () => {
  state.displayDirty = true;
  setText("display-save-state", "有尚未保存的修改");
});
byId("xiaozhi-enabled").addEventListener("change", (event) => updateModule("xiaozhi", event.target.checked));
byId("xiaozhi-form").addEventListener("submit", saveXiaozhi);
byId("xiaozhi-server-url").addEventListener("input", () => setText("xiaozhi-save-state", "有尚未保存的修改"));
byId("xiaozhi-wake-word").addEventListener("input", () => setText("xiaozhi-save-state", "有尚未保存的修改"));
byId("xiaozhi-voice").addEventListener("input", () => setText("xiaozhi-save-state", "有尚未保存的修改"));
byId("xiaozhi-mcp-enabled").addEventListener("change", () => setText("xiaozhi-save-state", "有尚未保存的修改"));
byId("xiaozhi-mcp-url").addEventListener("input", () => setText("xiaozhi-save-state", "有尚未保存的修改"));
byId("bluetooth-install").addEventListener("click", installBluetooth);
byId("bluetooth-scan").addEventListener("click", scanBluetooth);
byId("bluetooth-configure").addEventListener("click", configureBluetooth);
byId("bluetooth-ssid").addEventListener("input", () => renderBluetooth(state.overview?.bluetooth || {}));
byId("qqmusic-login")?.addEventListener("click", () => {
  startQqLogin().catch((error) => {
    console.error("qqmusic login failed", error);
    setText("qqmusic-qr-hint", "获取二维码失败：" + (error.message || error));
  });
});
byId("qqmusic-logout")?.addEventListener("click", () => {
  logoutQq().catch((error) => console.error("qqmusic logout failed", error));
});
byId("firmware-refresh").addEventListener("click", refreshFirmware);
byId("firmware-flash").addEventListener("click", flashFirmware);

setupUiSelects();
refreshOverview();
window.setInterval(refreshOverview, 3000);
