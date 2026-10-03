const $ = (selector) => document.querySelector(selector);

const ui = {
  connectionPill: $('#connectionPill'), connectionText: $('#connectionText'),
  statusHero: $('#statusHero'), stateEyebrow: $('#stateEyebrow'), stateTitle: $('#stateTitle'), stateDescription: $('#stateDescription'),
  thresholdFill: $('#thresholdFill'), thresholdLabel: $('#thresholdLabel'),
  personSeconds: $('#personSeconds'), phoneSeconds: $('#phoneSeconds'), personSignal: $('#personSignal'), phoneSignal: $('#phoneSignal'),
  overlapState: $('#overlapState'), overlapSignal: $('#overlapSignal'), eventCount: $('#eventCount'), armText: $('#armText'),
  rearmStatus: $('.rearm-status'), cameraClock: $('#cameraClock'), cameraFeed: $('#cameraFeed'), demoFeed: $('#demoFeed'), cameraEmpty: $('#cameraEmpty'),
  personBox: $('#personBox'), phoneBox: $('#phoneBox'), personConfidence: $('#personConfidence'), phoneConfidence: $('#phoneConfidence'),
  latencyLabel: $('#latencyLabel'), focusToast: $('#focusToast'), settingsDialog: $('#settingsDialog'), settingsButton: $('#settingsButton'),
  settingsForm: $('#settingsForm'), boardIp: $('#boardIp'), rtspPort: $('#rtspPort'), statusPort: $('#statusPort'), formMessage: $('#formMessage')
};

const TRIGGER_MS = 3000;
const REARM_MS = 3000;
let toastTimer;
let lastEventCount = 0;
let liveMode = false;
let liveFailures = 0;
let demoStart = performance.now();

class FocusTracker {
  constructor() { this.reset(); }
  reset() {
    this.personStart = null; this.phoneStart = null; this.overlapStart = null; this.clearStart = null;
    this.eventCount = 0; this.armed = true; this.eventLatched = false;
  }
  elapsed(start, now) { return start === null ? 0 : Math.max(0, now - start); }
  update(frame, now) {
    const reliable = frame.reliable !== false;
    const person = reliable && frame.personDetected;
    const phone = reliable && frame.phoneDetected;
    const overlap = person && phone && frame.overlap;
    this.personStart = person ? (this.personStart ?? now) : null;
    this.phoneStart = phone ? (this.phoneStart ?? now) : null;
    this.overlapStart = overlap ? (this.overlapStart ?? now) : null;
    const overlapMs = this.elapsed(this.overlapStart, now);
    if (this.armed && overlapMs >= TRIGGER_MS && !this.eventLatched) {
      this.eventCount += 1; this.armed = false; this.eventLatched = true; showReminder();
    }
    if (!overlap) this.eventLatched = false;
    const clearEvidence = reliable && person && !phone;
    this.clearStart = !this.armed && clearEvidence ? (this.clearStart ?? now) : null;
    if (!this.armed && this.elapsed(this.clearStart, now) >= REARM_MS) { this.armed = true; this.clearStart = null; }
    return {
      ...frame, reliable, personDetected: person, phoneDetected: phone, overlap,
      personSeconds: this.elapsed(this.personStart, now) / 1000,
      phoneSeconds: this.elapsed(this.phoneStart, now) / 1000,
      overlapSeconds: overlapMs / 1000,
      clearSeconds: this.elapsed(this.clearStart, now) / 1000,
      distracted: overlapMs >= TRIGGER_MS,
      eventCount: this.eventCount, armed: this.armed
    };
  }
}
const demoTracker = new FocusTracker();

function demoFrame(now) {
  const t = ((now - demoStart) / 1000) % 19;
  const frame = {
    reliable: t >= 1.7,
    personDetected: t >= 1.7 && t < 18.2,
    phoneDetected: t >= 5.5 && t < 12.4,
    overlap: t >= 5.5 && t < 10.4,
    personConfidence: 88, phoneConfidence: 76,
    overlapRatio: t >= 5.5 && t < 10.4 ? .82 : 0,
    boxes: {}
  };
  const sway = Math.sin(t * .7) * 1.2;
  frame.boxes.person = { x: 19 + sway, y: 15, w: 48, h: 70 };
  frame.boxes.phone = frame.overlap
    ? { x: 51 + sway, y: 47, w: 9, h: 18 }
    : { x: 76, y: 62, w: 9, h: 18 };
  return demoTracker.update(frame, now);
}

function setBox(element, box, visible) {
  element.classList.toggle('visible', Boolean(visible && box));
  if (!box) return;
  element.style.left = `${box.x}%`; element.style.top = `${box.y}%`; element.style.width = `${box.w}%`; element.style.height = `${box.h}%`;
}

function showReminder() {
  ui.focusToast.classList.add('show');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => ui.focusToast.classList.remove('show'), 4200);
}

function render(data, source = 'demo') {
  const personSeconds = Number(data.personSeconds || 0);
  const phoneSeconds = Number(data.phoneSeconds || 0);
  const overlapSeconds = Number(data.overlapSeconds || 0);
  const reliable = data.reliable !== false;
  const progress = Math.min(100, overlapSeconds / 3 * 100);
  ui.personSeconds.textContent = personSeconds.toFixed(1);
  ui.phoneSeconds.textContent = phoneSeconds.toFixed(1);
  ui.eventCount.textContent = data.eventCount ?? 0;
  ui.thresholdFill.style.width = `${progress}%`;
  ui.thresholdLabel.textContent = `${Math.min(overlapSeconds, 3).toFixed(1)} / 3.0 秒`;
  ui.personSignal.textContent = data.personDetected ? '辨識中' : '未偵測';
  ui.phoneSignal.textContent = data.phoneDetected ? '辨識中' : '未偵測';
  ui.personSignal.className = `signal ${data.personDetected ? 'on' : ''}`;
  ui.phoneSignal.className = `signal ${data.phoneDetected ? 'warn' : ''}`;
  ui.overlapState.textContent = data.overlap ? '是' : '否';
  ui.overlapSignal.textContent = !reliable ? '結果不確定' : data.overlap ? '持續計時' : '沒有重疊';
  ui.overlapSignal.className = `signal ${data.overlap ? 'warn' : ''}`;
  ui.rearmStatus.classList.toggle('locked', !data.armed);
  ui.armText.textContent = data.armed ? '已準備' : `等待手機離開 ${Math.max(0, 3 - Number(data.clearSeconds || 0)).toFixed(1)} 秒`;
  ui.statusHero.className = 'status-hero';
  if (!reliable) {
    ui.statusHero.classList.add('uncertain'); ui.stateEyebrow.textContent = '辨識結果不確定'; ui.stateTitle.textContent = '等待清楚畫面'; ui.stateDescription.textContent = '光線、遮擋或信心值不足時，不判定分心，也不增加次數。';
  } else if (data.distracted) {
    ui.statusHero.classList.add('alert'); ui.stateEyebrow.textContent = '已確認分心事件'; ui.stateTitle.textContent = '專心一下！'; ui.stateDescription.textContent = `人與手機持續重疊 ${overlapSeconds.toFixed(1)} 秒，本次事件只計數一次。`;
  } else if (data.overlap) {
    ui.statusHero.classList.add('warning'); ui.stateEyebrow.textContent = '可能正在使用手機'; ui.stateTitle.textContent = '持續確認中'; ui.stateDescription.textContent = '目前仍在 3 秒確認期內，短暫出現不會提醒或計次。';
  } else {
    ui.stateEyebrow.textContent = data.personDetected ? '學習狀態正常' : '系統待命中'; ui.stateTitle.textContent = data.personDetected ? '保持專注' : '等待學生進入'; ui.stateDescription.textContent = '持續辨識 3 秒才會觸發提醒，短暫出現不計次。';
  }
  $('#rulePerson').classList.toggle('active', Boolean(data.personDetected));
  $('#rulePhone').classList.toggle('active', Boolean(data.phoneDetected));
  $('#ruleOverlap').classList.toggle('active', Boolean(data.overlap));
  $('#ruleDuration').classList.toggle('active', Boolean(data.distracted));
  const boxes = data.boxes || {};
  // 實機 RTSP 畫面已由 AMB82 的 OSD 畫框；展示模式才使用網頁疊加框，避免實機重複顯示。
  const showWebBoxes = source === 'demo';
  setBox(ui.personBox, boxes.person, showWebBoxes && data.personDetected);
  setBox(ui.phoneBox, boxes.phone, showWebBoxes && data.phoneDetected);
  ui.personConfidence.textContent = data.personDetected ? `${Math.round(data.personConfidence || 0)}%` : '--';
  ui.phoneConfidence.textContent = data.phoneDetected ? `${Math.round(data.phoneConfidence || 0)}%` : '--';
  if (Number(data.eventCount || 0) > lastEventCount && source === 'live') showReminder();
  lastEventCount = Number(data.eventCount || 0);
}

function setConnection(mode, message) {
  ui.connectionPill.className = `connection-pill ${mode}`;
  ui.connectionText.textContent = message;
}

async function pollLiveStatus() {
  try {
    const response = await fetch('/api/status', { cache: 'no-store', signal: AbortSignal.timeout(1200) });
    if (!response.ok) throw new Error('status unavailable');
    const data = await response.json();
    liveMode = true; liveFailures = 0; setConnection('connected', 'AMB82 已連線');
    ui.latencyLabel.textContent = `${Math.round(data.latencyMs || 0)} ms`;
    ui.demoFeed.style.display = 'none'; ui.cameraFeed.style.display = 'block'; ui.cameraEmpty.classList.remove('show');
    if (!ui.cameraFeed.src) ui.cameraFeed.src = `/stream?ts=${Date.now()}`;
    render(data, 'live');
  } catch (error) {
    liveFailures += 1;
    if (liveMode && liveFailures < 5) return;
    liveMode = false; setConnection('', '展示模式'); ui.latencyLabel.textContent = '展示';
    ui.cameraFeed.removeAttribute('src'); ui.cameraFeed.style.display = 'none'; ui.demoFeed.style.display = 'block';
  }
}

async function loadConfig() {
  try {
    const response = await fetch('/api/config', { cache: 'no-store', signal: AbortSignal.timeout(900) });
    if (!response.ok) throw new Error();
    const config = await response.json();
    ui.boardIp.value = config.boardIp || ''; ui.rtspPort.value = config.rtspPort || 554; ui.statusPort.value = config.statusPort || 8080;
  } catch (_) { /* Hosted demo has no local gateway. */ }
}

ui.settingsButton.addEventListener('click', () => { loadConfig(); ui.formMessage.textContent = liveMode ? '目前已連線，可修改後重新連線。' : '展示網站不直接連接區域網路；請下載專案並啟動本機閘道。'; ui.settingsDialog.showModal(); });
ui.settingsForm.addEventListener('submit', async (event) => {
  if (event.submitter?.value === 'cancel') return;
  event.preventDefault();
  ui.formMessage.textContent = '正在套用設定…';
  try {
    const response = await fetch('/api/config', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ boardIp: ui.boardIp.value.trim(), rtspPort: Number(ui.rtspPort.value), statusPort: Number(ui.statusPort.value) }) });
    if (!response.ok) throw new Error();
    ui.cameraFeed.removeAttribute('src'); demoStart = performance.now(); ui.formMessage.textContent = '設定已儲存，正在連線。';
    setTimeout(() => ui.settingsDialog.close(), 700);
  } catch (_) { ui.formMessage.textContent = '此頁目前是展示版；請在電腦啟動 gateway/server.py 後再設定。'; }
});

setInterval(() => {
  const now = new Date(); ui.cameraClock.textContent = now.toLocaleTimeString('zh-TW', { hour12: false });
  if (!liveMode) render(demoFrame(performance.now()), 'demo');
}, 100);
setInterval(pollLiveStatus, 350);
loadConfig(); pollLiveStatus(); render(demoFrame(performance.now()), 'demo');
