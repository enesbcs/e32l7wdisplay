// Embedded web UI (English). Single page; sections per menu order:
// Info -> WiFi -> Data Source -> Display -> Dashboard -> Settings -> Firmware -> Reboot
extern "C" const char WEBUI_HTML[] = R"html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<link rel='icon' href='data:image/svg+xml,%3Csvg xmlns=%27http://www.w3.org/2000/svg%27 viewBox=%270 0 32 32%27%3E%3Crect width=%2732%27 height=%2732%27 rx=%277%27 fill=%27%2312161c%27/%3E%3Crect x=%279%27 y=%276%27 width=%274%27 height=%2713%27 fill=%27%23ffbe5a%27/%3E%3Ccircle cx=%2711%27 cy=%2723%27 r=%275%27 fill=%27%23ffbe5a%27/%3E%3Cpolygon points=%2722,8 17,20 27,20%27 fill=%27%236ebeFF%27/%3E%3Ccircle cx=%2722%27 cy=%2720%27 r=%275%27 fill=%27%236ebeFF%27/%3E%3C/svg%3E'>
<title>L7 Display</title>
<style>
  body{margin:0;font-family:system-ui,Segoe UI,Roboto,sans-serif;background:#12161c;color:#e8e8e8}
  header{background:#1b212b;padding:10px 16px;display:flex;align-items:center;gap:10px}
  header h1{font-size:18px;margin:0}
  nav{display:flex;flex-wrap:wrap;gap:6px;padding:10px 16px;background:#0e1218}
  nav button{background:#232b38;color:#fff;border:1px solid #333;padding:8px 12px;border-radius:6px;cursor:pointer;font-size:14px}
  nav button.active{background:#2f6fd0;border-color:#2f6fd0}
  main{padding:16px;max-width:860px}
  .page{display:none}
  .page.active{display:block}
  fieldset{border:1px solid #2a3038;border-radius:8px;margin-bottom:14px;padding:12px}
  legend{color:#9fc0ff;font-size:13px}
  label{display:block;margin:8px 0 2px;font-size:13px;color:#aaa}
  input,select{width:100%;box-sizing:border-box;padding:8px;border-radius:6px;border:1px solid #333;background:#1b212b;color:#fff}
  input[type=checkbox]{width:auto}
  .row{display:flex;gap:10px}
  .row>div{flex:1}
  button.act{background:#2f6fd0;color:#fff;border:0;padding:10px 18px;border-radius:6px;cursor:pointer;margin-top:10px;font-size:14px}
  button.act.warn{background:#b3541e}
  button.act.danger{background:#b3271e}
  table{width:100%;border-collapse:collapse;font-size:13px}
  td,th{border:1px solid #2a3038;padding:6px;text-align:left}
  th{background:#1b212b;color:#9fc0ff}
  td select,td input{width:100%}
  .msg{font-size:13px;margin-top:8px;min-height:16px;color:#9fe08a}
  .msg.err{color:#ff9c8a}
  pre{background:#0c0f14;padding:10px;border-radius:6px;overflow:auto;max-height:300px;font-size:12px}
</style>
</head>
<body>
<header>
  <h1>&#9632; L7 Display</h1>
</header>
<nav id="menu"></nav>
<main>
  <div id="p-info" class="page">
    <fieldset><legend>Device info</legend>
      <table id="infotbl"><tr><td>loading...</td></tr></table>
    </fieldset>
  </div>

  <div id="p-wifi" class="page">
    <fieldset><legend>WiFi Scan</legend>
      <button class="act" onclick="scanWifi()">Scan for networks</button>
      <table id="wifilist" style="margin-top:10px"></table>
    </fieldset>
    <fieldset><legend>Primary network</legend>
      <label>SSID</label><input id="w_ssid1">
      <label>Password</label><input id="w_pass1" type="password" autocomplete="new-password" placeholder="(unchanged)">
      <label>Backup SSID</label><input id="w_ssid2">
      <label>Backup password</label><input id="w_pass2" type="password" autocomplete="new-password" placeholder="(unchanged)">
      <label><input type="checkbox" id="w_dhcp" checked> Use DHCP</label>
      <div class="row">
        <div><label>Static IP</label><input id="w_ip" placeholder="192.168.1.50"></div>
        <div><label>Netmask</label><input id="w_mask" placeholder="255.255.255.0"></div>
        <div><label>Gateway</label><input id="w_gw" placeholder="192.168.1.1"></div>
      </div>
      <label><input type="checkbox" id="w_hotspot" checked> Start hotspot if connection fails</label>
      <button class="act" onclick="saveWifi()">Save and reconnect</button>
    </fieldset>
  </div>

  <div id="p-integration" class="page">
    <fieldset><legend>Home Assistant MQTT integration</legend>
      <label>MQTT broker host (empty = integration off)</label><input id="in_host" placeholder="192.168.1.20">
      <label>MQTT port</label><input id="in_port" type="number" value="1883">
      <label>Username</label><input id="in_user">
      <label>Password</label><input id="in_pass" type="password" autocomplete="new-password" placeholder="(unchanged)">
      <p><small style="color:#7f8ba1">Topic prefix: <span id="in_prefix">-</span> (fixed, = AP name). Shares the Shelly connection when host/port/user/pass match an active Shelly source, else runs its own. Publishes display switch + uptime + MCU temperature with autodiscovery.</small></p>
      <button class="act" onclick="saveIntegration()">Apply integration</button>
      <div class="msg" id="intmsg"></div>
    </fieldset>
  </div>

  <div id="p-source" class="page">
    <fieldset><legend>Data source</legend>
      <label>Source</label>
      <select id="ds_source">
        <option value="off">Off - no source (default)</option>
        <option value="ble">Passive BLE</option>
        <option value="ha">Home Assistant (WebSocket)</option>
        <option value="shelly">Shelly MQTT</option>
      </select>
      <div id="ds_ha">
        <label>WebSocket URL (plain ws:// only)</label><input id="ds_ha_url" placeholder="ws://192.168.1.10:8123/api/websocket">
        <label>Long-lived access token</label><input id="ds_ha_token" type="password" autocomplete="new-password" placeholder="(unchanged)">
      </div>
      <div id="ds_shelly">
        <label>MQTT host</label><input id="ds_mqtt_host" placeholder="192.168.1.20">
        <label>MQTT port</label><input id="ds_mqtt_port" type="number" value="1883">
        <label>Username</label><input id="ds_mqtt_user">
        <label>Password</label><input id="ds_mqtt_pass" type="password" autocomplete="new-password" placeholder="(unchanged)">
      </div>
      <button class="act" onclick="saveSource()">Apply data source</button>
      <button class="act" id="discbtn" onclick="doDiscover()">Start discovering</button>
      <div class="msg" id="srcmsg"></div>
    </fieldset>
    <fieldset><legend>Discovered sensors</legend>
      <div id="srclist"><span style="color:#7f8ba1">No data yet - press Refresh.</span></div>
      <p><button class="act" onclick="loadSourceSensors()">Refresh sensor list</button></p>
    </fieldset>
  </div>

  <div id="p-display" class="page">
    <fieldset><legend>Grid size</legend>
      <div class="row">
        <div><label>Rows (1-3)</label><select id="d_rows"><option>1</option><option selected>2</option><option>3</option></select></div>
        <div><label>Columns (1-4)</label><select id="d_cols"><option>1</option><option>2</option><option selected>3</option><option>4</option></select></div>
      </div>
      <small style="color:#7f8ba1">The dashboard grid shows temperature, humidity, battery and a "last updated" note per sensor. Sensors older than 1 hour get a red background.</small>
      <button class="act" onclick="saveDisplay()">Save grid size</button>
    </fieldset>
  </div>

  <div id="p-dashboard" class="page">
    <fieldset><legend>Sensor assignment</legend>
      <small style="color:#7f8ba1">Assign a sensor to each cell. Discover devices on the Data Source page first.</small>
      <p style="font-size:13px"><a href="/dashboard" style="color:#9fc0ff">Open dashboard view</a></p>
      <button class="act" onclick="loadSensors()">Refresh sensor list</button>
      <div id="grid"></div>
      <button class="act" onclick="saveDashboard()">Save dashboard</button>
    </fieldset>
  </div>

  <div id="p-settings" class="page">
    <fieldset><legend>General</legend>
      <label>NTP server</label><input id="s_ntp" placeholder="pool.ntp.org">
      <label>Time zone (POSIX TZ)</label><input id="s_tz" list="tzlist" placeholder="CET-1CEST,M3.5.0,M10.5.0/3">
      <datalist id="tzlist">
        <option value="UTC0">UTC</option>
        <option value="GMT0BST,M3.5.0/1,M10.5.0">Europe/London</option>
        <option value="CET-1CEST,M3.5.0,M10.5.0/3">Europe/Budapest</option>
        <option value="CET-1CEST,M3.5.0,M10.5.0/3">Europe/Berlin</option>
        <option value="CET-1CEST,M3.5.0,M10.5.0/3">Europe/Paris</option>
        <option value="EET-2EEST,M3.5.0/3,M10.5.0/4">Europe/Helsinki</option>
        <option value="MSK-3">Europe/Moscow</option>
        <option value="EST5EDT,M3.2.0,M11.1.0">America/New_York</option>
        <option value="CST6CDT,M3.2.0,M11.1.0">America/Chicago</option>
        <option value="PST8PDT,M3.2.0,M11.1.0">America/Los_Angeles</option>
        <option value="JST-9">Asia/Tokyo</option>
        <option value="AEST-10AEDT,M10.1.0,M4.1.0/3">Australia/Sydney</option>
      </datalist>
      <label>Web UI username</label><input id="s_user">
      <label>Web UI password</label><input id="s_pass" type="password" autocomplete="new-password" placeholder="(unchanged)">
      <button class="act" onclick="saveSettings()">Save settings</button>
    </fieldset>
    <fieldset><legend>Log</legend><pre id="logbox"></pre><button class="act" onclick="loadLog()">Refresh log</button></fieldset>
  </div>

  <div id="p-firmware" class="page">
    <fieldset><legend id="fw_legend">Firmware update</legend>
      <p style="font-size:13px;color:#aaa">Select the new firmware .bin. The device transparently switches to the safeboot recovery partition, uploads the image there (written to app0/ota_0) and reboots into it.</p>
      <form id="fwform" action="/u2?fsz=" method="post" enctype="multipart/form-data" onsubmit="return doFwUpload(this.querySelector('button'))">
        <input type="file" id="fw_file" name="u2" accept=".bin,application/octet-stream">
        <button class="act" type="submit">Upload and flash</button>
      </form>
      <div class="msg" id="fwmsg"></div>
    </fieldset>
    <fieldset><legend>Recovery</legend>
      <p style="font-size:13px;color:#aaa">Emergency use only: reboots into the Tasmota-compatible safeboot recovery web UI (manual firmware restore).</p>
      <button class="act warn" onclick="doRecovery()">Restart in recovery mode</button>
      <div class="msg" id="rcmsg"></div>
    </fieldset>
  </div>

  <div id="p-reboot" class="page">
    <fieldset><legend>Reboot</legend>
      <p style="font-size:14px">Reboot the display now?</p>
      <button class="act danger" onclick="doReboot()">Reboot device</button>
      <div class="msg" id="rbmsg"></div>
    </fieldset>
  </div>
</main>

<script>
const menu=[["info","Info","p-info"],["wiFi","WiFi","p-wifi"],["int","Integration","p-integration"],["src","Data Source","p-source"],["disp","Display","p-display"],["dash","Dashboard","p-dashboard"],["set","Settings","p-settings"],["fw","Firmware","p-firmware"],["rb","Reboot","p-reboot"]];
let CFG=null, SENS=[];

function show(id){
  document.querySelectorAll('.page').forEach(p=>p.classList.remove('active'));
  document.getElementById(id).classList.add('active');
  const btns=document.querySelectorAll('#menu button');
  btns.forEach(b=>b.classList.remove('active'));
  const i=menu.findIndex(m=>m[2]===id);
  if(i>=0&&btns[i])btns[i].classList.add('active');
}
function buildMenu(){
  const nav=document.getElementById('menu');
  menu.forEach(m=>{
    const b=document.createElement('button');
    b.textContent=m[1];
    b.onclick=()=>{
      document.querySelectorAll('#menu button').forEach(x=>x.classList.remove('active'));
      b.classList.add('active');
      show(m[2]);
      if(m[2]==='p-dashboard'){loadSensors();}
    };
    nav.appendChild(b);
  });
  document.querySelectorAll('#menu button')[0].classList.add('active');
}

async function api(path,opts){
  const r=await fetch(path,opts);
  if(r.status===401){alert('login required');throw new Error('401');}
  return r;
}
// HTML-escape for every device/network-controlled string rendered into the
// page (rogue AP SSIDs, HA sensor names, saved display names). ES2017-safe.
function esc(s){return String(s===undefined||s===null?'':s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;').replace(/'/g,'&#39;');}
function fmtB(b){if(b>=1048576)return (b/1048576).toFixed(1)+' MB';if(b>=1024)return Math.floor(b/1024)+' KB';return b+' B';}
function fmtH(a){return '0x'+a.toString(16).toUpperCase();}
function fmtUp(s){s=Math.floor(s||0);const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);let o='';if(d)o+=d+'d ';if(h||d)o+=h+'h ';o+=m+'m';if(!d&&!h)o+=' '+(s%60)+'s';return o;}
async function loadInfo(){
  const r=await api('/api/info');
  const j=await r.json();
  let h='<tr><th>Item</th><th>Value</th></tr>';
  h+='<tr><td>CPU</td><td>'+j.cpu+'</td></tr>';
  h+='<tr><td>Uptime</td><td>'+fmtUp(j.uptime_s)+'</td></tr>';
  h+='<tr><td>Date/time</td><td>'+(j.time_ok?j.datetime:'not synced yet')+'</td></tr>';
  h+='<tr><td>SRAM</td><td>'+fmtB(j.sram_free)+' free / '+fmtB(j.sram_total)+'</td></tr>';
  h+='<tr><td>PSRAM</td><td>'+fmtB(j.psram_free)+' free / '+fmtB(j.psram_total)+'</td></tr>';
  h+='<tr><td>Flash</td><td>'+fmtB(j.flash_size)+'</td></tr>';
  h+='<tr><td>Firmware</td><td>v'+j.fw_version+'</td></tr>';
  h+='<tr><td>MCU temp</td><td>'+(typeof j.mcu_temp==='number'?j.mcu_temp.toFixed(1)+' C':'-')+'</td></tr>';
  h+='<tr><td>Partitions</td><td><small>'+(j.partitions||[]).map(p=>p.label+' '+fmtH(p.offset)+' +'+fmtB(p.size)).join('<br>')+'</small></td></tr>';
  document.getElementById('infotbl').innerHTML=h;
  return j;
}
async function refreshStatus(){
  try{
    const r=await api('/api/status');
    const j=await r.json();
    if(j.fw_version){document.getElementById('fw_legend').textContent='Firmware update (v'+j.fw_version+')';}
    if(j.ap_name){document.getElementById('in_prefix').textContent=j.ap_name;}
  }catch(e){}
  try{await loadInfo();}catch(e){}
}
async function loadConfig(){
  const r=await api('/api/config');
  CFG=await r.json();
  // Secrets (passwords/token) are NEVER filled in: the server only sends a
  // mask, and an untouched (empty) field is omitted on save so the stored
  // value survives. Only typing into the field replaces it. This keeps
  // masked placeholders, password-manager autofill and stale pages from
  // ever overwriting stored secrets with garbage.
  document.getElementById('w_ssid1').value=CFG.wifi_ssid||'';
  document.getElementById('w_pass1').value='';
  document.getElementById('w_ssid2').value=CFG.wifi_ssid2||'';
  document.getElementById('w_pass2').value='';
  document.getElementById('w_dhcp').checked=CFG.use_dhcp;
  document.getElementById('w_ip').value=CFG.static_ip||'';
  document.getElementById('w_mask').value=CFG.static_netmask||'';
  document.getElementById('w_gw').value=CFG.static_gateway||'';
  document.getElementById('w_hotspot').checked=CFG.hotspot_on_fail;
  // data source
  document.getElementById('ds_source').value=CFG.data_source;
  document.getElementById('ds_ha_url').value=CFG.ha_url||'';
  document.getElementById('ds_ha_token').value='';
  document.getElementById('ds_mqtt_host').value=CFG.mqtt_host||'';
  document.getElementById('ds_mqtt_port').value=CFG.mqtt_port;
  document.getElementById('ds_mqtt_user').value=CFG.mqtt_username||'';
  document.getElementById('ds_mqtt_pass').value='';
  // integration (secrets never prefilled, same rule as everywhere)
  document.getElementById('in_host').value=CFG.mqtt_int_host||'';
  document.getElementById('in_port').value=CFG.mqtt_int_port;
  document.getElementById('in_user').value=CFG.mqtt_int_username||'';
  document.getElementById('in_pass').value='';
  // display
  document.getElementById('d_rows').value=CFG.grid_rows;
  document.getElementById('d_cols').value=CFG.grid_cols;
  // settings
  document.getElementById('s_ntp').value=CFG.ntp_server||'';
  document.getElementById('s_tz').value=CFG.timezone||'';
  document.getElementById('s_user').value=CFG.username||'';
  document.getElementById('s_pass').value='';
  toggleSource();
}
function toggleSource(){
  const s=document.getElementById('ds_source').value;
  document.getElementById('ds_ha').style.display=s==='ha'?'block':'none';
  document.getElementById('ds_shelly').style.display=s==='shelly'?'block':'none';
  // BLE is always discovering; discovery button only for shelly/ha
  document.getElementById('discbtn').style.display=(s==='ha'||s==='shelly')?'inline-block':'none';
}
async function doDiscover(){
  const msg=document.getElementById('srcmsg');
  msg.textContent='Discovery started - watch the sensor list and log...';
  try{
    const r=await api('/api/discover',{method:'POST'});
    const j=await r.json();
    msg.textContent=j.ok?('Discovery started ('+j.source+') - refresh the list in a few seconds.'):'Discovery not available for '+j.source+'.';
  }catch(e){msg.textContent='discover failed';}
}

async function scanWifi(){
  const tbl=document.getElementById('wifilist');
  tbl.innerHTML='<tr><th>SSID</th><th>RSSI</th><th></th></tr>';
  const r=await api('/api/wifi/scan');
  const j=await r.json();
  (j.networks||[]).forEach(n=>{
    // textContent assignment throughout: no HTML parsing of the SSID, and
    // no quote-breakout in the pick() handler (structural XSS fix)
    const tr=document.createElement('tr');
    const tdSsid=document.createElement('td'); tdSsid.textContent=n.ssid;
    const tdRssi=document.createElement('td'); tdRssi.textContent=n.rssi+' dBm';
    const tdBtn=document.createElement('td');
    const btn=document.createElement('button'); btn.textContent='use';
    btn.onclick=function(){pick(n.ssid);};
    tdBtn.appendChild(btn);
    tr.appendChild(tdSsid); tr.appendChild(tdRssi); tr.appendChild(tdBtn);
    tbl.appendChild(tr);
  });
}
function pick(ssid){document.getElementById('w_ssid1').value=ssid;}

async function saveWifi(){
  const body={
    wifi_ssid:document.getElementById('w_ssid1').value,
    wifi_ssid2:document.getElementById('w_ssid2').value,
    use_dhcp:document.getElementById('w_dhcp').checked,
    static_ip:document.getElementById('w_ip').value,
    static_netmask:document.getElementById('w_mask').value,
    static_gateway:document.getElementById('w_gw').value,
    hotspot_on_fail:document.getElementById('w_hotspot').checked,
    reboot:true,
  };
  // secrets are sent ONLY when the user typed something (untouched fields
  // stay empty and are omitted, so the stored value is kept)
  const wp1=document.getElementById('w_pass1').value;
  if(wp1)body.wifi_password=wp1;
  const wp2=document.getElementById('w_pass2').value;
  if(wp2)body.wifi_password2=wp2;
  const r=await api('/api/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
  alert('Saved - device reconnecting...');
}
async function loadSourceSensors(){
  const box=document.getElementById('srclist');
  try{
    const r=await api('/api/sensors');
    const j=await r.json();
    const list=j.sensors||[];
    if(!list.length){box.innerHTML='<span style="color:#7f8ba1">No sensors found. Enable a data source and wait for advertisements.</span>';return;}
    const iT='<span style="display:inline-block;position:relative;width:10px;height:14px;vertical-align:middle">'
      +'<span style="position:absolute;left:4px;top:1px;width:2px;height:9px;background:#ffbe5a;border-radius:1px"></span>'
      +'<span style="position:absolute;left:2px;top:8px;width:6px;height:6px;background:#ffbe5a;border-radius:50%"></span></span> ';
    const iH='<span style="display:inline-block;position:relative;width:10px;height:14px;vertical-align:middle">'
      +'<span style="position:absolute;left:2px;top:2px;width:6px;height:6px;background:#6ebeFF;transform:rotate(45deg)"></span>'
      +'<span style="position:absolute;left:1px;top:6px;width:8px;height:8px;background:#6ebeFF;border-radius:50%"></span></span> ';
    let h='<table><tr><th>Name</th><th>ID</th><th>Temp</th><th>Hum</th><th>Batt</th><th>RSSI</th></tr>';
    list.forEach(s=>{
      const ents=(s.entities||[]).join(', ');
      const tip=s.device?('device: '+s.device+(ents?(' | entities: '+ents):'')):(ents?('entities: '+ents):'');
      h+='<tr><td>'+esc(s.name||s.id)+'</td>'
        +'<td><small title="'+esc(tip)+'">'+esc(s.id)+'</small></td>'
        +'<td>'+(s.temp?iT+esc(s.temp):'-')+'</td><td>'+(s.hum?iH+esc(s.hum):'-')+'</td>'
        +'<td>'+esc(s.batt||'-')+'</td><td>'+esc(s.rssi||'-')+'</td></tr>';
      box.innerHTML='<div style="font-size:13px;color:#9fc0ff;margin-bottom:6px">'+list.length+' sensor'+(list.length===1?'':'s')+'</div>'+h+'</table>';
    });
  }catch(e){box.innerHTML='<span style="color:#f87171">load failed</span>';}
}
async function saveSource(){
  const body={
    data_source:document.getElementById('ds_source').value,
    ha_url:document.getElementById('ds_ha_url').value,
    mqtt_host:document.getElementById('ds_mqtt_host').value,
    mqtt_port:parseInt(document.getElementById('ds_mqtt_port').value||'1883'),
    mqtt_username:document.getElementById('ds_mqtt_user').value,
  };
  const tok=document.getElementById('ds_ha_token').value;
  if(tok)body.ha_token=tok;
  const mp=document.getElementById('ds_mqtt_pass').value;
  if(mp)body.mqtt_password=mp;
  const r=await api('/api/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
  const j=await r.json();
  document.getElementById('srcmsg').textContent=j.ok?'Applied. Discover devices below:':'error';
  await loadConfig();
  loadSensors();
  loadSourceSensors();
}
async function saveIntegration(){
  const body={
    mqtt_int_host:document.getElementById('in_host').value,
    mqtt_int_port:parseInt(document.getElementById('in_port').value||'1883'),
    mqtt_int_username:document.getElementById('in_user').value,
  };
  const pw=document.getElementById('in_pass').value;
  if(pw)body.mqtt_int_password=pw;
  const r=await api('/api/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
  const j=await r.json();
  document.getElementById('intmsg').textContent=j.ok?'Applied. Watch the broker for discovery topics.':'error'+(j.error?(' '+j.error):'');
  await loadConfig();
}
function saveDisplay(){
  const body={
    grid_rows:parseInt(document.getElementById('d_rows').value),
    grid_cols:parseInt(document.getElementById('d_cols').value),
  };
  api('/api/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)}).then(()=>loadConfig());
}
async function loadSensors(){
  const r=await api('/api/sensors');
  SENS=await r.json();
  const grid=document.getElementById('grid');
  // always the full 3x4=12 assignment rows, independent of the current grid
  // size (the display renders the first rows*cols of the stored 12)
  const cols=4;
  const rows=3;
  let opts='<option value="">- empty -</option>';
  const known={};
  (SENS.sensors||[]).forEach(s=>{
    known[s.id]=1;
    opts+='<option value="'+esc(s.id)+'">'+esc(s.name||s.id)+', '+esc(s.id)+((s.rssi)?', '+esc(s.rssi):'')+'</option>';
  });
  function cid(cc){return (typeof cc==='string')?cc:((cc&&typeof cc.id==='string')?cc.id:'');}
  let t='<table><tr><th>#</th><th>Sensor</th><th>Display name</th></tr>';
  const cells=CFG&&CFG.cells?CFG.cells:[];
  for(let i=0;i<rows*cols;i++){
    const cur=cid(cells[i]);
    const nm=cells[i]&&cells[i].name?cells[i].name:'';
    let sel=opts;
    if(cur&&!known[cur]){
      // saved but not currently seen (e.g. registry empty after boot):
      // keep it selectable instead of showing a fake empty row.
      sel+='<option value="'+esc(cur)+'" selected>'+esc(nm||cur)+' (not seen yet)</option>';
    }else{
      // attr-safe match: esc(cur) equals how cur was rendered into opts,
      // so exotic ids simply miss 'selected' instead of breaking markup
      sel=opts.replace('value="'+esc(cur)+'"','value="'+esc(cur)+'" selected');
    }
    t+='<tr><td>'+(i+1)+'</td>'
      +'<td><select data-idx="'+i+'" class="cellsel">'+sel+'</select></td>'
      +'<td><input data-idx="'+i+'" class="cellname" type="text" value="'+esc(nm)+'"></td></tr>';
  }
  grid.innerHTML=t;
}
function cellsTh(){return '';}
async function saveDashboard(){
  const grid=document.getElementById('grid');
  // display grid comes from the Display tab; all 12 assignment rows are sent
  // (entries past rows*cols are stored for later grid growth, not rendered)
  const rows=parseInt(document.getElementById('d_rows').value), cols=parseInt(document.getElementById('d_cols').value);
  const cells=[];
  const sels=grid.querySelectorAll('.cellsel');
  const names=grid.querySelectorAll('.cellname');
  for(let i=0;i<sels.length;i++){
    cells.push({id:sels[i]?sels[i].value:'',name:names[i]?names[i].value:''});
  }
  const body={grid_rows:rows,grid_cols:cols,cells:cells};
  const r=await api('/api/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
  const j=await r.json();
  alert(j.ok?'Saved': 'error');
  // re-render from server truth so the grid never sticks in a stale state
  await loadConfig();
  loadSensors();
}
async function saveSettings(){
  const body={
    ntp_server:document.getElementById('s_ntp').value,
    timezone:document.getElementById('s_tz').value,
    username:document.getElementById('s_user').value,
    reboot:true,
  };
  const sp=document.getElementById('s_pass').value;
  if(sp)body.password=sp;
  const r=await api('/api/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
  alert('Saved - rebooting...');
}
async function loadLog(){
  const r=await api('/api/log');
  document.getElementById('logbox').textContent=await r.text();
}
async function doFwUpload(t){
  const e=document.getElementById('fw_file');
  const msg=document.getElementById('fwmsg');
  if(!e.files||!e.files.length){msg.textContent='select a .bin first';return false;}
  const f=e.files[0];
  const sl=f.slice(0,1);
  const rd=new FileReader();
  rd.onload=function(){
    const bb=new Uint8Array(rd.result);
    if(bb.length===1&&bb[0]===0xE9){fctTries=0;fct(t);}
    else{t.form.action='/u2?fsz='+f.size;t.form.submit();}
  };
  rd.readAsArrayBuffer(sl);
  return false;
}
let fctTries=0;
async function fct(t){
  const e=document.getElementById('fw_file');
  const msg=document.getElementById('fwmsg');
  msg.textContent='switching to safeboot partition...';
  const x=new XMLHttpRequest();
  x.open('GET','/u4?u4=fct&api=',true);
  x.onreadystatechange=function(){
    if(x.readyState===4&&x.status===200){
      const s=x.responseText;
      if(s==='false'){msg.textContent='switching to safeboot partition...';fctTries++;
        if(fctTries<60){setTimeout(function(){fct(t);},5000);}
        else{msg.textContent='Safeboot is not answering. Join the L7-RECOVERY AP (172.218.28.1) or check STA, then open /up manually.';}}
      if(s==='true'){setTimeout(function(){su(t);},1000);}
    }else if(x.readyState===4&&x.status===0){fctTries++;
      if(fctTries<120){setTimeout(function(){fct(t);},2000);}};
  };
  x.send();
}
function su(t){
  const e=document.getElementById('fw_file');
  const msg=document.getElementById('fwmsg');
  msg.textContent='uploading '+e.files[0].name+' ('+(e.files[0].size/1048576).toFixed(1)+' MB)...';
  upRetry(t.form,0);
}
function upRetry(form,tries){
  const e=document.getElementById('fw_file');
  const msg=document.getElementById('fwmsg');
  const url='/u2?fsz='+e.files[0].size;
  fetch(url,{method:'POST',body:new FormData(form)}).then(function(r){
    return r.text().then(function(s){return {st:r.status,body:s};});}).then(function(o){
    if(o.st===200&&o.body.indexOf('Upload successful')>=0){
      msg.textContent='Upload done, rebooting into new firmware...';
      setTimeout(function(){pollAppFw(0);},8000);}
    else if(tries<3){msg.textContent='Upload interrupted, retrying...';
      setTimeout(function(){upRetry(form,tries+1);},3000);}
    else{document.open();document.write(o.body);document.close();}}).catch(function(){
    if(tries<3){msg.textContent='Upload interrupted, retrying...';
      setTimeout(function(){upRetry(form,tries+1);},3000);}
    else{msg.textContent='Upload failed - device may still be reachable, retry manually.';}});
}
function pollAppFw(n){
  const msg=document.getElementById('fwmsg');
  fetch('/api/status',{cache:'no-store'}).then(function(r){return r.text();}).then(function(s){
    let ok=false;try{const j=JSON.parse(s);if(j&&j.wifi_mode)ok=true;}catch(e){}
    if(ok){msg.textContent='New firmware is running.';location.href='/';}
    else if(n<60){setTimeout(function(){pollAppFw(n+1);},3000);}
    else{msg.textContent='Device did not come back - it may have fallen back to safeboot. Open /up there.';}}).catch(function(){
    if(n<60){setTimeout(function(){pollAppFw(n+1);},3000);}
    else{msg.textContent='Device did not come back - it may have fallen back to safeboot. Open /up there.';}});
}
async function doRecovery(){
  const msg=document.getElementById('rcmsg');
  msg.textContent='restarting into recovery mode...';
  const r=await api('/api/ota/flash',{method:'POST'});
  if(!r.ok){msg.textContent='failed';return;}
  msg.textContent='rebooting into recovery...';
  // The device reboots into safeboot on the SAME IP. Wait out the reboot,
  // then poll /u4 until safeboot answers "true" (factory slot) and go
  // straight to its upload page. (Polling too early would hit the still
  // running app, whose /u4 re-triggers a reboot, so the first poll waits.)
  let n=0;
  setTimeout(function poll(){
    n++;
    fetch('/u4?u4=fct&api=',{cache:'no-store'}).then(function(rr){
      if(!rr.ok) throw 0;
      return rr.text();
    }).then(function(s){
      if(s.trim()==='true'){location.href='/up';return;}
      if(n<40){msg.textContent='waiting for recovery...';setTimeout(poll,3000);}
      else{msg.textContent='recovery did not answer - open the device IP manually';}
    }).catch(function(){
      if(n<40){setTimeout(poll,3000);}
      else{msg.textContent='recovery did not answer - open the device IP manually';}
    });
  },12000);
}
async function doReboot(){
  const r=await api('/api/reboot',{method:'POST'});
  document.getElementById('rbmsg').textContent='rebooting...';
}
document.getElementById('ds_source').onchange=toggleSource;

(async function init(){
  buildMenu();
  // default tab: first setup (no stored config) starts on WiFi,
  // an already configured device opens the Info tab
  let info=null;
  try{info=await loadInfo();}catch(e){}
  show(info&&info.configured===false?'p-wifi':'p-info');
  try{await loadConfig();}catch(e){}
  refreshStatus();
  setInterval(refreshStatus,5000);
})();
</script>
</body>
</html>
)html";