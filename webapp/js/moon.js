import mqtt from 'https://unpkg.com/mqtt/dist/mqtt.esm.js';

/* ============================================================
   Configuración
   ============================================================ */
const MQTT_BROKER = 'wss://broker.hivemq.com:8884/mqtt';
const TOPIC_PREFIX = 'moon';

/* ============================================================
   Utilidades de color
   ============================================================ */
function hsvToRgb(h, s, v){
  h = ((h % 360) + 360) % 360;
  const c = v * s;
  const x = c * (1 - Math.abs(((h/60) % 2) - 1));
  const m = v - c;
  let r=0,g=0,b=0;
  if(h<60){r=c;g=x;b=0;} else if(h<120){r=x;g=c;b=0;}
  else if(h<180){r=0;g=c;b=x;} else if(h<240){r=0;g=x;b=c;}
  else if(h<300){r=x;g=0;b=c;} else {r=c;g=0;b=x;}
  return [Math.round((r+m)*255), Math.round((g+m)*255), Math.round((b+m)*255)];
}

function rgbToHsv(r,g,b){
  r/=255; g/=255; b/=255;
  const max=Math.max(r,g,b), min=Math.min(r,g,b), d=max-min;
  let h=0;
  if(d!==0){
    if(max===r) h=60*(((g-b)/d)%6);
    else if(max===g) h=60*((b-r)/d+2);
    else h=60*((r-g)/d+4);
  }
  if(h<0) h+=360;
  const s = max===0?0:d/max;
  const v = max;
  return [h,s,v];
}

function rgbToHex(r,g,b){
  return '#'+[r,g,b].map(v=>v.toString(16).padStart(2,'0')).join('');
}

function hexToRgb(hex){
  hex = hex.replace('#','');
  if(hex.length===3) hex = hex.split('').map(c=>c+c).join('');
  const num = parseInt(hex,16);
  return [(num>>16)&255,(num>>8)&255,num&255];
}

function clamp(v,min,max){ return Math.max(min,Math.min(max,v)); }

function throttle(fn, wait){
  let last=0, timer=null, pendingArgs=null;
  return function(...args){
    const now=Date.now();
    const remaining = wait - (now-last);
    if(remaining<=0){
      last=now; fn.apply(this,args);
    } else {
      pendingArgs=args;
      clearTimeout(timer);
      timer=setTimeout(()=>{ last=Date.now(); fn.apply(this,pendingArgs); }, remaining);
    }
  };
}

/* ============================================================
   Almacenamiento local
   ============================================================ */
const Store = {
  getDevices(){ try{ return JSON.parse(localStorage.getItem('moon_devices')||'[]'); }catch(e){ return []; } },
  saveDevices(list){ localStorage.setItem('moon_devices', JSON.stringify(list)); },
  key(prefix, deviceId){ return `moon_${prefix}_${deviceId}`; },
  getFavorites(id){ try{ return JSON.parse(localStorage.getItem(this.key('fav',id))||'[]'); }catch(e){ return []; } },
  saveFavorites(id, list){ localStorage.setItem(this.key('fav',id), JSON.stringify(list)); },
  getScenes(id){ try{ return JSON.parse(localStorage.getItem(this.key('scenes',id))||'[]'); }catch(e){ return []; } },
  saveScenes(id, list){ localStorage.setItem(this.key('scenes',id), JSON.stringify(list)); },
};

/* ============================================================
   Cliente MQTT
   ============================================================ */
class MoonMqttClient {
  constructor(deviceId){
    this.deviceId = deviceId;
    this.client = null;
    this.connected = false;
    this.status = null;
    this.onStatus = null;
    this.onConnect = null;
    this.onDisconnect = null;
  }

  topic(suffix){
    return `${TOPIC_PREFIX}/${this.deviceId}/${suffix}`;
  }

  connect(){
    if(this.client) return;
    const clientId = `moon_web_${Math.random().toString(36).slice(2,10)}`;
    this.client = mqtt.connect(MQTT_BROKER, {
      clientId,
      keepalive: 60,
      reconnectPeriod: 3000,
      connectTimeout: 10000,
      clean: true,
    });

    this.client.on('connect', ()=>{
      this.connected = true;
      this.client.subscribe(this.topic('status'), {qos:1}, (err)=>{
        if(err) console.error('subscribe error', err);
      });
      if(this.onConnect) this.onConnect();
    });

    this.client.on('close', ()=>{
      this.connected = false;
      if(this.onDisconnect) this.onDisconnect();
    });

    this.client.on('error', (err)=>{
      console.error('MQTT error', err);
      this.connected = false;
      if(this.onDisconnect) this.onDisconnect();
    });

    this.client.on('message', (topic, message)=>{
      if(topic === this.topic('status')){
        try{
          this.status = JSON.parse(message.toString());
          if(this.onStatus) this.onStatus(this.status);
        }catch(e){}
      }
    });
  }

  disconnect(){
    if(this.client){
      this.client.end();
      this.client = null;
      this.connected = false;
    }
  }

  publish(topic, payload, opts={}){
    if(!this.connected || !this.client) return false;
    this.client.publish(topic, payload, opts);
    return true;
  }

  sendFrame(frameBytes){
    // frameBytes already includes the 2-byte header [0x01, 0x02, ...]
    this.publish(this.topic('cmd/frame'), frameBytes, {qos:0});
  }

  sendStreamStart(ledCount, fps, loop=false){
    // MQTT payload: led_count(2 BE), fps(2 BE), flags(1)
    // No 2-byte WebSocket header — the MQTT topic already identifies the message type.
    const buf = new Uint8Array(5);
    const view = new DataView(buf.buffer);
    view.setUint16(0, ledCount, false);
    view.setUint16(2, fps, false);
    buf[4] = loop ? 0x01 : 0x00;
    this.publish(this.topic('cmd/stream'), buf, {qos:0});
  }

  sendStop(){
    this.publish(this.topic('cmd/stop'), '', {qos:0});
  }

  sendEffect(effect, speed=128, intensity=128, brightness=255, color1='ff0000', color2='0000ff'){
    const payload = JSON.stringify({effect, speed, intensity, brightness, color1, color2});
    this.publish(this.topic('cmd/effect'), payload, {qos:1});
  }

  sendBrightness(value){
    const payload = JSON.stringify({brightness: value});
    this.publish(this.topic('cmd/brightness'), payload, {qos:1});
  }
}

/* ============================================================
   Iconos SVG
   ============================================================ */
const ICONS = {
  back: '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M15 18l-6-6 6-6"/></svg>',
  plus: '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.3" stroke-linecap="round"><path d="M12 5v14M5 12h14"/></svg>',
  trash: '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 6h18M8 6V4a2 2 0 012-2h4a2 2 0 012 2v2m3 0l-1 14a2 2 0 01-2 2H7a2 2 0 01-2-2L4 6h16z"/></svg>',
  power: '<svg width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 2v9"/><path d="M18.4 6.6a9 9 0 11-12.8 0"/></svg>',
  sun: '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="12" cy="12" r="4.5"/><path d="M12 2.5v2.5M12 19v2.5M4.2 4.2l1.8 1.8M18 18l1.8 1.8M2.5 12H5M19 12h2.5M4.2 19.8L6 18M18 6l1.8-1.8"/></svg>',
  moon: '<svg width="20" height="20" viewBox="0 0 24 24" fill="currentColor"><path d="M20.5 14.7A8.5 8.5 0 019.3 3.5a.6.6 0 00-.7-.8A9.5 9.5 0 1021.3 15.4a.6.6 0 00-.8-.7z"/><path d="M18.5 3.2l.5 1.4 1.4.5-1.4.5-.5 1.4-.5-1.4-1.4-.5 1.4-.5z"/></svg>',
  chevron: '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.3" stroke-linecap="round" stroke-linejoin="round"><path d="M9 6l6 6-6 6"/></svg>',
  device: '<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="4" y="2" width="16" height="20" rx="3"/><path d="M9 18h6"/></svg>',
  wifi_off: '<svg width="34" height="34" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><path d="M1 1l22 22M8.5 16.5a5 5 0 017 0M5 12.5a10 10 0 013.6-2.3M19 12.5a10 10 0 00-2.5-1.9M12 20h.01"/></svg>',
  fx_rainbow: '<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke-linecap="round"><path d="M3 18a9 9 0 0118 0" stroke="#ff5f6d" stroke-width="2"/><path d="M6 18a6 6 0 0112 0" stroke="#ffc371" stroke-width="2"/><path d="M9 18a3 3 0 016 0" stroke="#4bde6e" stroke-width="2"/></svg>',
  fx_breathe: '<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6"><circle cx="12" cy="12" r="3" fill="currentColor" stroke="none"/><circle cx="12" cy="12" r="7" opacity="0.5"/><circle cx="12" cy="12" r="10" opacity="0.25"/></svg>',
  fx_chase: '<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 12h5M9 6l6 6-6 6" opacity="0.4"/><path d="M13 6l6 6-6 6"/></svg>',
  fx_sparkle: '<svg width="20" height="20" viewBox="0 0 24 24" fill="currentColor"><path d="M12 2l1.8 6.2L20 10l-6.2 1.8L12 18l-1.8-6.2L4 10l6.2-1.8z"/></svg>',
  fx_fire: '<svg width="20" height="20" viewBox="0 0 24 24" fill="currentColor"><path d="M12 2c1 3-2 4-2 7a4 4 0 008 0c0-1-.5-2-.5-2 2 1 3.5 3.5 3.5 6a7 7 0 11-14 0c0-4 2-6 3-7.5C10.5 4 11 3 12 2z"/></svg>',
};

const EFFECTS = [
  { id:'rainbow', name:'Arcoíris', desc:'Arcoíris animado que se desplaza', icon:ICONS.fx_rainbow, iconBg:'linear-gradient(135deg,#2a1a2e,#1a1e2e)', color1:false, color2:false, intensity:false },
  { id:'breathe', name:'Respiración', desc:'Pulso suave de un color', icon:ICONS.fx_breathe, iconBg:'#20242c', color1:true, color2:false, intensity:false },
  { id:'chase', name:'Persecución', desc:'Cometa que recorre la tira', icon:ICONS.fx_chase, iconBg:'#20242c', color1:true, color2:true, intensity:true, intensityLabel:'Largo de cola' },
  { id:'sparkle', name:'Destellos', desc:'Chispas aleatorias sobre un fondo', icon:ICONS.fx_sparkle, iconBg:'#26241c', color1:true, color2:true, intensity:true, intensityLabel:'Densidad' },
  { id:'fire', name:'Fuego', desc:'Simulación de llamas', icon:ICONS.fx_fire, iconBg:'#2c1c18', color1:false, color2:false, intensity:true, intensityLabel:'Intensidad de llamas' },
];

/* ============================================================
   Estado de la aplicación
   ============================================================ */
const state = {
  route: 'devices',
  devices: Store.getDevices(),
  currentDeviceId: null,
  activeTab: 'home',
  modal: null,
  ledCount: 12,
  hue: 350, sat: 0.85, val: 0.85,
  brightness: 180,
  isOn: true,
  favorites: [],
  scenes: [],
  nightMode: false,
  nightSaved: null,
  openFx: null,
  fxParams: {},
  toast: null,
  mqttStatus: 'connecting',
  deviceStatus: null,
};

let mqttClient = null;
let toastTimer = null;

function currentDevice(){ return state.devices.find(d=>d.id===state.currentDeviceId) || null; }
function currentRgb(){ return hsvToRgb(state.hue, state.sat, state.val); }

/* ============================================================
   MQTT integration
   ============================================================ */
function connectMqtt(deviceId){
  disconnectMqtt();
  mqttClient = new MoonMqttClient(deviceId);
  state.mqttStatus = 'connecting';
  render();

  mqttClient.onConnect = ()=>{
    state.mqttStatus = 'connected';
    render();
  };

  mqttClient.onDisconnect = ()=>{
    state.mqttStatus = 'disconnected';
    render();
  };

  mqttClient.onStatus = (status)=>{
    state.deviceStatus = status;
    if(status && status.led_count){
      state.ledCount = status.led_count;
    }
    if(status && typeof status.brightness === 'number'){
      state.brightness = status.brightness;
    }
    render();
  };

  mqttClient.connect();
}

function disconnectMqtt(){
  if(mqttClient){
    mqttClient.disconnect();
    mqttClient = null;
  }
}

/* ============================================================
   Color / brightness actions
   ============================================================ */
function buildFrame(r,g,b,a=255){
  // MQTT payload: frame_id(4 BE), timestamp_ms(4 BE), pixels(led_count*4 RGBA)
  // No 2-byte WebSocket header — the MQTT topic already identifies the message type.
  const meta = new Uint8Array(8);
  const dv = new DataView(meta.buffer);
  dv.setUint32(0, ++state.frameId || 1, false);
  dv.setUint32(4, Date.now() % 0xFFFFFFFF, false);
  const pixels = new Uint8Array(state.ledCount * 4);
  for(let i=0;i<state.ledCount;i++){
    pixels[i*4]=r; pixels[i*4+1]=g; pixels[i*4+2]=b; pixels[i*4+3]=a;
  }
  const msg = new Uint8Array(meta.length + pixels.length);
  msg.set(meta,0); msg.set(pixels,meta.length);
  return msg;
}

const sendColorThrottled = throttle(()=>{
  if(!mqttClient || !state.isOn) return;
  const [r,g,b] = currentRgb();
  mqttClient.sendFrame(buildFrame(r,g,b,255));
}, 55);

function applyColorFromWheel(){
  state.isOn = true;
  sendColorThrottled();
  render();
}

function setBrightnessRemote(value){
  if(!mqttClient) return;
  mqttClient.sendBrightness(value);
}

const setBrightnessThrottled = throttle((v)=>setBrightnessRemote(v), 150);

function togglePower(){
  state.isOn = !state.isOn;
  if(state.isOn){
    sendColorThrottled();
  } else {
    mqttClient.sendEffect('off');
  }
  render();
}

/* ============================================================
   Favorites / scenes / night mode / effects
   ============================================================ */
function addFavorite(){
  const hex = rgbToHex(...currentRgb());
  if(state.favorites.includes(hex)){ showToast('Ese color ya está guardado'); return; }
  if(state.favorites.length>=8){ showToast('Máximo 8 colores favoritos'); return; }
  state.favorites.push(hex);
  Store.saveFavorites(state.currentDeviceId, state.favorites);
  render();
}

function selectFavorite(hex){
  const [r,g,b] = hexToRgb(hex);
  const [h,s,v] = rgbToHsv(r,g,b);
  state.hue=h; state.sat=s; state.val=v;
  applyColorFromWheel();
}

function removeFavorite(hex){
  state.favorites = state.favorites.filter(f=>f!==hex);
  Store.saveFavorites(state.currentDeviceId, state.favorites);
  render();
}

function createScene(){
  const name = prompt('Nombre de la escena:', `Escena ${state.scenes.length+1}`);
  if(!name) return;
  const hex = rgbToHex(...currentRgb());
  state.scenes.push({ id:'s'+Date.now(), name, color:hex, brightness: state.brightness });
  Store.saveScenes(state.currentDeviceId, state.scenes);
  render();
}

function applyScene(scene){
  const [r,g,b] = hexToRgb(scene.color);
  const [h,s,v] = rgbToHsv(r,g,b);
  state.hue=h; state.sat=s; state.val=v;
  state.brightness = scene.brightness;
  state.isOn = true;
  sendColorThrottled();
  setBrightnessRemote(scene.brightness);
  render();
  showToast(`Escena "${scene.name}" aplicada`);
}

function removeScene(id){
  state.scenes = state.scenes.filter(s=>s.id!==id);
  Store.saveScenes(state.currentDeviceId, state.scenes);
  render();
}

function toggleNightMode(){
  state.nightMode = !state.nightMode;
  if(state.nightMode){
    state.nightSaved = { hue:state.hue, sat:state.sat, val:state.val, brightness: state.brightness };
    const nightBrightness = Math.round(255*0.05);
    mqttClient.sendEffect('static', 128, 128, nightBrightness, 'ffcc66', '0000ff');
    mqttClient.sendBrightness(nightBrightness);
    const [r,g,b] = hexToRgb('ffcc66');
    const [h,s,v] = rgbToHsv(r,g,b);
    state.hue=h; state.sat=s; state.val=v; state.brightness=nightBrightness;
  } else if(state.nightSaved){
    state.hue=state.nightSaved.hue; state.sat=state.nightSaved.sat; state.val=state.nightSaved.val;
    state.brightness = state.nightSaved.brightness;
    sendColorThrottled();
    setBrightnessRemote(state.brightness);
    state.nightSaved = null;
  }
  render();
}

function fxDefaults(fx){
  return state.fxParams[fx.id] || (state.fxParams[fx.id] = { speed:128, intensity:128, color1:'ff3366', color2:'0000ff' });
}

function toggleFxOpen(id){
  state.openFx = state.openFx===id ? null : id;
  render();
}

function applyFx(fx){
  const p = fxDefaults(fx);
  mqttClient.sendEffect(fx.id, p.speed, p.intensity, state.brightness, p.color1, p.color2);
  showToast(`"${fx.name}" activado`);
}

const applyFxThrottled = throttle((fx)=>applyFx(fx), 300);

/* ============================================================
   Device management
   ============================================================ */
function openDevice(id){
  const device = state.devices.find(d=>d.id===id);
  if(!device) return;
  state.currentDeviceId = id;
  state.route = 'control';
  state.activeTab = 'home';
  state.favorites = Store.getFavorites(id);
  state.scenes = Store.getScenes(id);
  state.isOn = true;
  state.openFx = null;
  state.frameId = 0;
  render();
  connectMqtt(device.id);
}

function backToDevices(){
  disconnectMqtt();
  state.route = 'devices';
  state.currentDeviceId = null;
  state.deviceStatus = null;
  render();
}

function openAddDeviceModal(){ state.modal='add-device'; render(); }
function closeModal(){ state.modal=null; render(); }

function submitAddDevice(name, deviceId){
  deviceId = deviceId.trim().toLowerCase().replace(/:/g,'').replace(/[^a-f0-9]/g,'');
  if(deviceId.length!==12){
    const errEl = document.getElementById('add-device-error');
    if(errEl) errEl.textContent = 'Ingresa una MAC address válida (12 caracteres hex).';
    return;
  }
  let finalName = name.trim();
  if(!finalName) finalName = `Moon ${deviceId.slice(-4).toUpperCase()}`;
  state.devices.push({ id:deviceId, name:finalName });
  Store.saveDevices(state.devices);
  state.modal = null;
  render();
}

function deleteDevice(id){
  const device = state.devices.find(d=>d.id===id);
  if(!device) return;
  if(!confirm(`¿Eliminar "${device.name}" de tus dispositivos?`)) return;
  state.devices = state.devices.filter(d=>d.id!==id);
  Store.saveDevices(state.devices);
  render();
}

/* ============================================================
   Toast
   ============================================================ */
function showToast(msg){
  state.toast = msg;
  render();
  clearTimeout(toastTimer);
  toastTimer = setTimeout(()=>{ state.toast=null; render(); }, 2200);
}

/* ============================================================
   Render
   ============================================================ */
function renderDevicesScreen(){
  const list = state.devices;
  const items = list.map(d=>{
    const status = state.deviceStatus && state.deviceStatus.device_id === d.id ? 'online' : 'offline';
    return `
    <div class="device-card" role="button" tabindex="0" data-action="open-device" data-id="${d.id}">
      <div class="device-avatar"></div>
      <div class="device-info">
        <div class="device-name">${escapeHtml(d.name)} <span class="status-dot ${status}" title="${status}"></span></div>
        <div class="device-ip">${escapeHtml(d.id)}</div>
      </div>
      <button class="device-delete" data-action="delete-device" data-id="${d.id}" aria-label="Eliminar ${escapeHtml(d.name)}">${ICONS.trash}</button>
    </div>`;
  }).join('');
  const empty = `
    <div class="empty-state">
      <div class="empty-icon">${ICONS.device}</div>
      <p>Todavía no agregaste ningún dispositivo.<br>Toca "Agregar dispositivo" para empezar.</p>
    </div>`;
  return `
  <div class="screen">
    <div class="devices-header">
      <div class="wordmark">MOON</div>
      <div class="devices-sub">Selecciona un dispositivo</div>
    </div>
    <div class="scroll">
      <div class="device-list">${list.length ? items : empty}</div>
    </div>
    <button class="add-device-btn" data-action="open-add-device">${ICONS.plus} Agregar dispositivo</button>
    ${state.modal==='add-device' ? renderAddDeviceModal() : ''}
  </div>`;
}

function renderAddDeviceModal(){
  return `
  <div class="modal-overlay" data-action="close-modal-bg">
    <div class="modal-sheet" data-stop>
      <div class="modal-title">Nuevo dispositivo</div>
      <div class="modal-sub">Ingresa la MAC address de tu Moon Speaker.</div>
      <div class="field">
        <label for="dev-name">Nombre (opcional)</label>
        <input id="dev-name" type="text" placeholder="Ej. Sala, Cuarto…" autocomplete="off">
      </div>
      <div class="field">
        <label for="dev-id">MAC address</label>
        <input id="dev-id" type="text" placeholder="aa:bb:cc:dd:ee:ff" autocomplete="off" inputmode="text">
        <div class="field-error" id="add-device-error"></div>
      </div>
      <div class="modal-actions">
        <button class="btn btn-ghost" data-action="close-modal">Cancelar</button>
        <button class="btn btn-primary" data-action="confirm-add-device">Agregar</button>
      </div>
    </div>
  </div>`;
}

function renderControlScreen(){
  const device = currentDevice();
  if(!device){ state.route='devices'; return renderDevicesScreen(); }
  const status = state.deviceStatus || {};
  const isOnline = state.mqttStatus === 'connected';
  return `
  <div class="screen">
    <div class="mqtt-status ${state.mqttStatus}">
      <span class="dot"></span>
      <span>${state.mqttStatus === 'connected' ? 'MQTT' : (state.mqttStatus === 'connecting' ? 'Conectando…' : 'Desconectado')}</span>
    </div>
    <div class="control-header">
      <button class="icon-btn" data-action="back" aria-label="Volver">${ICONS.back}</button>
      <div class="control-title-wrap">
        <div class="control-title">${escapeHtml(device.name)} <span class="status-dot ${isOnline?'online':'offline'}"></span></div>
        <div class="control-sub">${escapeHtml(device.id)} ${status.ip ? '· '+escapeHtml(status.ip) : ''}</div>
      </div>
      <div style="width:38px;"></div>
    </div>
    ${!isOnline ? `<div class="conn-banner">MQTT desconectado. Verifica que el dispositivo esté en línea.</div>` : ''}
    <div class="nav-menu">
      <button class="nav-item ${state.activeTab==='scenes'?'active':''}" data-action="tab" data-tab="scenes">Escenas</button>
      <button class="nav-item ${state.activeTab==='home'?'active':''}" data-action="tab" data-tab="home">Inicio</button>
      <button class="nav-item ${state.activeTab==='animations'?'active':''}" data-action="tab" data-tab="animations">Animaciones</button>
    </div>
    <div class="scroll">
      <div class="tab-content">
        ${state.activeTab==='home' ? renderHomeTab() : ''}
        ${state.activeTab==='scenes' ? renderScenesTab() : ''}
        ${state.activeTab==='animations' ? renderAnimationsTab() : ''}
      </div>
    </div>
    <div class="toast ${state.toast?'show':''}">${state.toast?escapeHtml(state.toast):''}</div>
  </div>`;
}

function renderHomeTab(){
  const [r,g,b] = currentRgb();
  const hex = rgbToHex(r,g,b).toUpperCase();
  const pureHueRgb = hsvToRgb(state.hue,1,1);
  const pureHueHex = rgbToHex(...pureHueRgb);
  const ringAngleRad = (state.hue - 90) * Math.PI/180;
  const ringR = 0.925;
  const ringX = 50 + 50*ringR*Math.cos(ringAngleRad);
  const ringY = 50 + 50*ringR*Math.sin(ringAngleRad);
  const dx = (state.sat-0.5)*2, dy=((1-state.val)-0.5)*2;
  const dist = Math.min(1, Math.sqrt(dx*dx+dy*dy));
  let cdx=dx, cdy=dy;
  if(dist>0.96){ cdx = dx/dist*0.96; cdy = dy/dist*0.96; }
  const discPctX = 50 + cdx*50;
  const discPctY = 50 + cdy*50;
  const favRow = state.favorites.map(hexColor=>`
    <button class="fav-swatch ${hexColor.toUpperCase()===hex?'selected':''}" style="background:${hexColor}"
      data-action="select-fav" data-hex="${hexColor}"
      data-longpress="remove-fav"
      aria-label="Color favorito ${hexColor}"></button>`).join('');
  return `
  <div class="wheel-card ${state.isOn?'':'is-off'}">
    <button class="power-toggle ${state.isOn?'is-on':''}" data-action="toggle-power" aria-label="Encender o apagar">${ICONS.power}</button>
    <div class="wheel-wrap" id="colorWheel" data-ledcount="${state.ledCount}">
      <div class="wheel-ring"></div>
      <div class="wheel-ring-mask"></div>
      <div class="wheel-disc" id="wheelDisc" style="background:
          linear-gradient(to top, #000, transparent),
          linear-gradient(to right, #fff, ${pureHueHex})">
      </div>
      <div class="wheel-knob" id="ringKnob" style="left:${ringX}%; top:${ringY}%; background:${pureHueHex};"></div>
      <div class="wheel-knob-inner" id="discKnob" style="left:${discPctX}%; top:${discPctY}%; background:${hex};"></div>
    </div>
    <div class="hex-readout">${hex}</div>
  </div>
  <div class="section-label">Colores favoritos</div>
  <div class="favorites-row">
    ${favRow}
    <button class="fav-add" data-action="add-fav" aria-label="Guardar color actual">${ICONS.plus}</button>
  </div>
  <div class="section-label">Brillo</div>
  <div class="brightness-row">
    <span class="icon-static">${ICONS.sun}</span>
    <input type="range" id="brightnessRange" min="0" max="255" value="${state.brightness}" data-action="brightness">
    <span class="brightness-value">${Math.round(state.brightness/255*100)}%</span>
  </div>
  <div class="night-card">
    <div class="night-icon">${ICONS.moon}</div>
    <div class="night-body">
      <div class="night-title">Modo noche</div>
      <div class="night-desc">El brillo bajará a un 5% y el color cambiará a un amarillo cálido.</div>
    </div>
    <button class="switch ${state.nightMode?'checked':''}" data-action="toggle-night" aria-label="Activar modo noche" aria-pressed="${state.nightMode}"></button>
  </div>`;
}

function renderScenesTab(){
  const cards = state.scenes.map(s=>`
    <button class="scene-card" data-action="apply-scene" data-id="${s.id}" data-longpress="remove-scene">
      <div class="scene-swatch" style="background:${s.color}"></div>
      <div class="scene-name">${escapeHtml(s.name)}</div>
      <div class="scene-meta">${Math.round(s.brightness/255*100)}% brillo</div>
    </button>`).join('');
  return `
  <div class="section-label" style="margin-top:2px;">Tus escenas</div>
  <div class="scenes-grid">
    ${cards}
    <button class="scene-card scene-card-new" data-action="new-scene">${ICONS.plus} Nueva escena</button>
  </div>
  ${state.scenes.length ? `<div class="control-sub" style="margin-top:16px;text-align:left;">Mantén presionada una escena para eliminarla.</div>` : `<div class="control-sub" style="margin-top:16px;text-align:left;">Ajusta un color en "Inicio" y guárdalo aquí como escena.</div>`}
  `;
}

function renderAnimationsTab(){
  const cards = EFFECTS.map(fx=>{
    const p = fxDefaults(fx);
    const open = state.openFx===fx.id;
    return `
    <div class="fx-card ${open?'open':''}">
      <button class="fx-head" data-action="toggle-fx" data-id="${fx.id}">
        <div class="fx-icon" style="background:${fx.iconBg}">${fx.icon}</div>
        <div>
          <div class="fx-name">${fx.name}</div>
          <div class="fx-desc">${fx.desc}</div>
        </div>
        <span class="fx-chevron">${ICONS.chevron}</span>
      </button>
      <div class="fx-body">
        <div class="fx-row">
          <div class="fx-row-label"><span>Velocidad</span><span>${p.speed}</span></div>
          <input type="range" min="0" max="255" value="${p.speed}" data-action="fx-speed" data-id="${fx.id}">
        </div>
        ${fx.intensity ? `
        <div class="fx-row">
          <div class="fx-row-label"><span>${fx.intensityLabel||'Intensidad'}</span><span>${p.intensity}</span></div>
          <input type="range" min="0" max="255" value="${p.intensity}" data-action="fx-intensity" data-id="${fx.id}">
        </div>` : ''}
        ${(fx.color1 || fx.color2) ? `
        <div class="fx-row">
          <div class="fx-row-label"><span>Colores</span><span></span></div>
          <div class="fx-colors">
            ${fx.color1 ? `<label class="fx-color-input"><input type="color" value="#${p.color1}" data-action="fx-color1" data-id="${fx.id}"><span>Color 1</span></label>`:''}
            ${fx.color2 ? `<label class="fx-color-input"><input type="color" value="#${p.color2}" data-action="fx-color2" data-id="${fx.id}"><span>Color 2</span></label>`:''}
          </div>
        </div>` : ''}
        <button class="fx-apply-btn" data-action="apply-fx" data-id="${fx.id}">Aplicar animación</button>
      </div>
    </div>`;
  }).join('');
  return `<div class="section-label" style="margin-top:2px;">Efectos animados</div><div class="fx-list">${cards}</div>`;
}

function escapeHtml(str){
  return String(str).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
}

/* ============================================================
   Render principal y event handlers
   ============================================================ */
const app = document.getElementById('app');
function render(){
  app.innerHTML = state.route==='devices' ? renderDevicesScreen() : renderControlScreen();
  attachDynamicHandlers();
}

function attachDynamicHandlers(){
  const wheel = document.getElementById('colorWheel');
  if(wheel) bindWheel(wheel);

  const brightness = document.getElementById('brightnessRange');
  if(brightness){
    brightness.addEventListener('input', (e)=>{
      state.brightness = parseInt(e.target.value,10);
      const label = brightness.parentElement.querySelector('.brightness-value');
      if(label) label.textContent = Math.round(state.brightness/255*100)+'%';
      setBrightnessThrottled(state.brightness);
    });
    brightness.addEventListener('change', (e)=>{ setBrightnessRemote(parseInt(e.target.value,10)); });
  }

  document.querySelectorAll('[data-action="fx-speed"]').forEach(el=>{
    el.addEventListener('input', e=>{
      const id=el.dataset.id; fxDefaults(EFFECTS.find(f=>f.id===id)).speed = parseInt(e.target.value,10);
      const label = el.parentElement.querySelector('.fx-row-label span:last-child');
      if(label) label.textContent = e.target.value;
    });
  });
  document.querySelectorAll('[data-action="fx-intensity"]').forEach(el=>{
    el.addEventListener('input', e=>{
      const id=el.dataset.id; fxDefaults(EFFECTS.find(f=>f.id===id)).intensity = parseInt(e.target.value,10);
      const label = el.parentElement.querySelector('.fx-row-label span:last-child');
      if(label) label.textContent = e.target.value;
    });
  });
  document.querySelectorAll('[data-action="fx-color1"]').forEach(el=>{
    el.addEventListener('input', e=>{
      const id=el.dataset.id; fxDefaults(EFFECTS.find(f=>f.id===id)).color1 = e.target.value.replace('#','');
    });
  });
  document.querySelectorAll('[data-action="fx-color2"]').forEach(el=>{
    el.addEventListener('input', e=>{
      const id=el.dataset.id; fxDefaults(EFFECTS.find(f=>f.id===id)).color2 = e.target.value.replace('#','');
    });
  });

  document.querySelectorAll('[data-longpress]').forEach(el=> bindLongPress(el));
}

function bindLongPress(el){
  let timer=null;
  const start = ()=>{ timer=setTimeout(()=>{
    const action = el.dataset.longpress;
    if(action==='remove-fav') removeFavorite(el.dataset.hex);
    if(action==='remove-scene'){ if(confirm('¿Eliminar esta escena?')) removeScene(el.dataset.id); }
  }, 550); };
  const cancel = ()=>{ clearTimeout(timer); };
  el.addEventListener('pointerdown', start);
  el.addEventListener('pointerup', cancel);
  el.addEventListener('pointerleave', cancel);
  el.addEventListener('contextmenu', (e)=>{
    e.preventDefault();
    const action = el.dataset.longpress;
    if(action==='remove-fav') removeFavorite(el.dataset.hex);
    if(action==='remove-scene'){ if(confirm('¿Eliminar esta escena?')) removeScene(el.dataset.id); }
  });
}

function bindWheel(wheel){
  const disc = wheel.querySelector('.wheel-disc');
  let mode = null;

  function rectInfo(){
    const r = wheel.getBoundingClientRect();
    return { cx: r.left + r.width/2, cy: r.top + r.height/2, radius: r.width/2 };
  }

  function updateHueFromPoint(x,y){
    const { cx, cy } = rectInfo();
    const ang = Math.atan2(y-cy, x-cx) * 180/Math.PI;
    state.hue = ((ang + 90) % 360 + 360) % 360;
  }

  function updateSatValFromPoint(x,y){
    const { cx, cy, radius } = rectInfo();
    const discRadius = radius * 0.85;
    let dx = (x-cx)/discRadius, dy = (y-cy)/discRadius;
    const dist = Math.sqrt(dx*dx+dy*dy);
    if(dist>1){ dx/=dist; dy/=dist; }
    state.sat = clamp((dx+1)/2, 0, 1);
    state.val = clamp(1-((dy+1)/2), 0, 1);
  }

  function paintLive(){
    const [r,g,b] = currentRgb();
    const hex = rgbToHex(r,g,b);
    const pureHue = rgbToHex(...hsvToRgb(state.hue,1,1));
    const ringKnob = document.getElementById('ringKnob');
    const discKnob = document.getElementById('discKnob');
    const hexReadout = wheel.parentElement.querySelector('.hex-readout');
    if(ringKnob){
      const ang = (state.hue-90)*Math.PI/180;
      ringKnob.style.left = (50+50*0.925*Math.cos(ang))+'%';
      ringKnob.style.top = (50+50*0.925*Math.sin(ang))+'%';
      ringKnob.style.background = pureHue;
    }
    if(discKnob){
      const dx=(state.sat-0.5)*2, dy=((1-state.val)-0.5)*2;
      discKnob.style.left = (50+dx*50)+'%';
      discKnob.style.top = (50+dy*50)+'%';
      discKnob.style.background = hex;
    }
    if(disc) disc.style.background = `linear-gradient(to top, #000, transparent), linear-gradient(to right, #fff, ${pureHue})`;
    if(hexReadout) hexReadout.textContent = hex.toUpperCase();
    const favSelected = wheel.parentElement.parentElement.querySelectorAll('.fav-swatch');
    favSelected.forEach(f=>f.classList.toggle('selected', f.dataset.hex && f.dataset.hex.toLowerCase()===hex.toLowerCase()));
  }

  function pointerDown(e){
    const { cx, cy, radius } = rectInfo();
    const dist = Math.hypot(e.clientX-cx, e.clientY-cy);
    if(dist <= radius*0.85){ mode='sat'; updateSatValFromPoint(e.clientX,e.clientY); }
    else if(dist <= radius){ mode='hue'; updateHueFromPoint(e.clientX,e.clientY); }
    else { return; }
    wheel.setPointerCapture(e.pointerId);
    paintLive();
    applyColorFromWheelLive();
    e.preventDefault();
  }

  function pointerMove(e){
    if(!mode) return;
    if(mode==='hue') updateHueFromPoint(e.clientX,e.clientY);
    else updateSatValFromPoint(e.clientX,e.clientY);
    paintLive();
    applyColorFromWheelLive();
  }

  function pointerUp(){
    if(!mode) return;
    mode=null;
  }

  function applyColorFromWheelLive(){
    state.isOn = true;
    sendColorThrottled();
  }

  wheel.addEventListener('pointerdown', pointerDown);
  wheel.addEventListener('pointermove', pointerMove);
  wheel.addEventListener('pointerup', pointerUp);
  wheel.addEventListener('pointercancel', pointerUp);
}

app.addEventListener('click', (e)=>{
  const target = e.target.closest('[data-action]');
  if(!target) return;
  const action = target.dataset.action;
  switch(action){
    case 'open-device': openDevice(target.dataset.id); break;
    case 'delete-device': e.stopPropagation(); deleteDevice(target.dataset.id); break;
    case 'open-add-device': openAddDeviceModal(); break;
    case 'close-modal': closeModal(); break;
    case 'close-modal-bg': if(!e.target.closest('[data-stop]')) closeModal(); break;
    case 'confirm-add-device': {
      const name = document.getElementById('dev-name').value;
      const id = document.getElementById('dev-id').value;
      submitAddDevice(name, id);
      break;
    }
    case 'back': backToDevices(); break;
    case 'tab': state.activeTab = target.dataset.tab; state.openFx=null; render(); break;
    case 'toggle-power': togglePower(); break;
    case 'add-fav': addFavorite(); break;
    case 'select-fav': selectFavorite(target.dataset.hex); break;
    case 'toggle-night': toggleNightMode(); break;
    case 'new-scene': createScene(); break;
    case 'apply-scene': {
      const scene = state.scenes.find(s=>s.id===target.dataset.id);
      if(scene) applyScene(scene);
      break;
    }
    case 'toggle-fx': toggleFxOpen(target.dataset.id); break;
    case 'apply-fx': {
      const fx = EFFECTS.find(f=>f.id===target.dataset.id);
      if(fx) applyFx(fx);
      break;
    }
  }
});

window.addEventListener('beforeunload', ()=>{ disconnectMqtt(); });

render();
