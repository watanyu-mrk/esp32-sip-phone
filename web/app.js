// Configuration frontend for the XIAO ESP32-S3 SIP phone.
// All routes require HTTP Basic auth; the browser prompts once per session.

const $ = (id) => document.getElementById(id);

function setText(id, value) {
  const el = $(id);
  if (el) el.textContent = value;
}

function toast(message, isError = false) {
  const el = $("toast");
  if (!el) return;
  el.textContent = message;
  el.className = isError ? "toast show error" : "toast show";
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => { el.className = "toast"; }, 4000);
}

async function api(path, options = {}) {
  const res = await fetch(path, {
    credentials: "same-origin",
    ...options,
    headers: { "Content-Type": "application/json", ...(options.headers || {}) },
  });
  if (!res.ok) {
    let msg = `HTTP ${res.status}`;
    try {
      const data = await res.json();
      if (data && data.message) msg = data.message;
    } catch (_) { /* non-JSON error */ }
    throw new Error(msg);
  }
  return res.json();
}

function formatUptime(seconds) {
  const total = Math.max(0, Math.floor(seconds || 0));
  const h = Math.floor(total / 3600);
  const m = Math.floor((total % 3600) / 60);
  const s = total % 60;
  return `${h}h ${String(m).padStart(2, "0")}m ${String(s).padStart(2, "0")}s`;
}

function formatKB(bytes) {
  return `${Math.round((bytes || 0) / 1024)} KB`;
}

async function refreshStatus() {
  try {
    const s = await api("/api/status");
    setText("st-device", `${s.device_name} · ext ${s.extension}`);
    setText("st-wifi", s.wifi_connected ? s.ssid : "disconnected");
    setText("st-ip", s.ip || (s.ap_active ? "192.168.4.1 (AP)" : "—"));
    setText("st-rssi", s.wifi_connected ? `${s.wifi_rssi} dBm` : "—");
    setText("st-ap", s.ap_active ? `${s.ap_ssid} @ 192.168.4.1` : "inactive");
    setText("st-ext", s.extension || "—");
    setText("st-server", s.sip_server || "—");
    setText("st-sip", s.sip_state);
    setText("st-call", s.call_state);
    setText("st-uptime", formatUptime(s.uptime_s));
    setText("st-heap", formatKB(s.heap_free));
    setText("st-psram", formatKB(s.psram_free));
    setText("st-firmware", s.firmware || "—");
  } catch (err) {
    setText("st-wifi", `error: ${err.message}`);
  }
}

function setValue(id, value) {
  const el = $(id);
  if (el) el.value = value ?? "";
}

function setChecked(id, value) {
  const el = $(id);
  if (el) el.checked = !!value;
}

async function loadConfig() {
  const c = await api("/api/config");

  setValue("wifi_ssid", c.wifi.ssid);
  setValue("wifi_ip", c.wifi.ip);
  setValue("wifi_netmask", c.wifi.netmask);
  setValue("wifi_gateway", c.wifi.gateway);
  setValue("wifi_dns", c.wifi.dns);
  setChecked("wifi_dhcp", c.wifi.use_dhcp);
  $("wifi_password").placeholder = c.wifi.password_set ? "unchanged" : "none";

  setValue("sip_server", c.sip.server);
  setValue("sip_port", c.sip.port);
  setValue("sip_username", c.sip.username);
  setValue("sip_auth_id", c.sip.auth_id);
  setValue("sip_display_name", c.sip.display_name);
  setValue("sip_extension", c.sip.extension);
  setValue("sip_transport", c.sip.transport);
  setValue("sip_expiry", c.sip.register_expiry_s);
  setValue("sip_outbound_proxy", c.sip.outbound_proxy);
  setValue("sip_stun", c.sip.stun_server);
  setChecked("sip_prefer_pcma", c.sip.prefer_pcma);
  $("sip_password").placeholder = c.sip.password_set ? "unchanged" : "none";

  setValue("audio_mic_gain", c.audio.mic_gain_db);
  setValue("audio_volume", c.audio.speaker_volume);
  setValue("audio_jitter", c.audio.jitter_frames);
  setChecked("audio_nr", c.audio.noise_reduction);
  setChecked("audio_aec", c.audio.aec_enabled);

  if (c.phone) {
    setValue("phone_default_number", c.phone.default_number);
    setValue("phone_display_name", c.phone.display_name);
    setValue("phone_dtmf", c.phone.dtmf_mode);
    setChecked("phone_auto_answer", c.phone.auto_answer);
    setValue("phone_auto_delay", c.phone.auto_answer_delay_ms);
    setValue("phone_ring_s", c.phone.ring_duration_s);
    setValue("phone_timeout_s", c.phone.call_timeout_s);
    setValue("phone_ringtone", c.phone.ringtone);
    setValue("phone_ring_volume", c.phone.ring_volume);
    setValue("phone_key_volume", c.phone.key_volume);
    updateRingVolumeLabel();
    updateKeyVolumeLabel();
  }

  setValue("sys_device_name", c.system.device_name);
  setValue("sys_hostname", c.system.hostname);
  setValue("sys_web_user", c.system.web_user);
  $("sys_web_password").placeholder = c.system.web_password_set ? "unchanged" : "none";

  updateStaticFields();
}

function updateStaticFields() {
  const dhcp = $("wifi_dhcp").checked;
  for (const id of ["wifi_ip", "wifi_netmask", "wifi_gateway", "wifi_dns"]) {
    $(id).disabled = dhcp;
  }
}

async function submitForm(formId, path, payload, successMessage) {
  const form = $(formId);
  form.addEventListener("submit", async (event) => {
    event.preventDefault();
    try {
      const result = await api(path, { method: "POST", body: JSON.stringify(payload()) });
      toast(result.message || successMessage);
      await refreshStatus();
    } catch (err) {
      toast(err.message, true);
    }
  });
}

function wireForms() {
  submitForm("wifi-form", "/api/config/wifi", () => ({
    ssid: $("wifi_ssid").value.trim(),
    password: $("wifi_password").value,
    use_dhcp: $("wifi_dhcp").checked,
    ip: $("wifi_ip").value.trim(),
    netmask: $("wifi_netmask").value.trim(),
    gateway: $("wifi_gateway").value.trim(),
    dns: $("wifi_dns").value.trim(),
  }), "Wi-Fi saved");

  submitForm("sip-form", "/api/config/sip", () => ({
    server: $("sip_server").value.trim(),
    port: Number($("sip_port").value),
    username: $("sip_username").value.trim(),
    password: $("sip_password").value,
    auth_id: $("sip_auth_id").value.trim(),
    display_name: $("sip_display_name").value.trim(),
    extension: $("sip_extension").value.trim(),
    transport: $("sip_transport").value,
    register_expiry_s: Number($("sip_expiry").value),
    outbound_proxy: $("sip_outbound_proxy").value.trim(),
    stun_server: $("sip_stun").value.trim(),
    prefer_pcma: $("sip_prefer_pcma").checked,
  }), "SIP settings saved");

  submitForm("audio-form", "/api/config/audio", () => ({
    mic_gain_db: Number($("audio_mic_gain").value),
    speaker_volume: Number($("audio_volume").value),
    jitter_frames: Number($("audio_jitter").value),
    noise_reduction: $("audio_nr").checked,
    aec_enabled: $("audio_aec").checked,
  }), "Audio settings saved");

  submitForm("phone-form", "/api/config/phone", () => ({
    default_number: $("phone_default_number").value.trim(),
    display_name: $("phone_display_name").value.trim(),
    dtmf_mode: $("phone_dtmf").value,
    auto_answer: $("phone_auto_answer").checked,
    auto_answer_delay_ms: Number($("phone_auto_delay").value),
    ring_duration_s: Number($("phone_ring_s").value),
    call_timeout_s: Number($("phone_timeout_s").value),
    ringtone: Number($("phone_ringtone").value),
    ring_volume: Number($("phone_ring_volume").value),
    key_volume: Number($("phone_key_volume").value),
  }), "Phone settings saved");

  submitForm("system-form", "/api/config/admin", () => ({
    device_name: $("sys_device_name").value.trim(),
    hostname: $("sys_hostname").value.trim(),
    username: $("sys_web_user").value.trim(),
    password: $("sys_web_password").value,
  }), "Device settings saved");
}

function wireTabs() {
  document.querySelectorAll(".tab").forEach((tab) => {
    tab.addEventListener("click", () => {
      document.querySelectorAll(".tab").forEach((t) => t.classList.remove("active"));
      document.querySelectorAll(".panel").forEach((p) => p.classList.remove("active"));
      tab.classList.add("active");
      document.querySelector(`.panel[data-panel="${tab.dataset.tab}"]`).classList.add("active");
    });
  });
}

async function callAction(action) {
  const body = { action };
  if (action === "dial") {
    const number = $("call_number").value.trim();
    if (!number) {
      toast("Enter a number to dial", true);
      return;
    }
    body.number = number;
  }
  try {
    const result = await api("/api/call", { method: "POST", body: JSON.stringify(body) });
    toast(result.message || action);
    await refreshStatus();
  } catch (err) {
    toast(err.message, true);
  }
}

function updateRingVolumeLabel() {
  const slider = $("phone_ring_volume");
  setText("phone_ring_volume_val", `${slider.value}%`);
}

function updateKeyVolumeLabel() {
  const slider = $("phone_key_volume");
  setText("phone_key_volume_val", `${slider.value}%`);
}

async function previewRingtone() {
  try {
    const result = await api("/api/ringtone/preview", {
      method: "POST",
      body: JSON.stringify({
        ringtone: Number($("phone_ringtone").value),
        volume: Number($("phone_ring_volume").value),
      }),
    });
    toast(result.message || "Preview playing");
  } catch (err) {
    toast(err.message, true);
  }
}

function wireActions() {
  $("phone_ring_volume").addEventListener("input", updateRingVolumeLabel);
  $("phone_key_volume").addEventListener("input", updateKeyVolumeLabel);
  $("ring-preview").addEventListener("click", previewRingtone);

  $("call-dial").addEventListener("click", () => callAction("dial"));
  $("call-answer").addEventListener("click", () => callAction("answer"));
  $("call-hangup").addEventListener("click", () => callAction("hangup"));

  $("reboot-btn").addEventListener("click", async () => {
    try {
      const result = await api("/api/system", {
        method: "POST",
        body: JSON.stringify({ action: "reboot" }),
      });
      toast(result.message || "Restarting");
    } catch (err) {
      toast(err.message, true);
    }
  });

  $("factory-btn").addEventListener("click", async () => {
    if (!confirm("Erase all settings and restart the phone?")) return;
    try {
      const result = await api("/api/system", {
        method: "POST",
        body: JSON.stringify({ action: "factory_reset" }),
      });
      toast(result.message || "Factory reset complete");
    } catch (err) {
      toast(err.message, true);
    }
  });
}

document.addEventListener("DOMContentLoaded", async () => {
  wireTabs();
  wireForms();
  wireActions();
  $("wifi_dhcp").addEventListener("change", updateStaticFields);

  await refreshStatus();
  try {
    await loadConfig();
  } catch (err) {
    toast(`Could not load configuration: ${err.message}`, true);
  }

  setInterval(refreshStatus, 3000);
  window.__appReady = true;
});
