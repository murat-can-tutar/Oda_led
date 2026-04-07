#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <EEPROM.h>
#include <ESP32Servo.h>

const char* apSSID = "RKS";
const char* apPassword = "44AFT748";

WebServer server(80);

const uint8_t RELAY_ON  = LOW;
const uint8_t RELAY_OFF = HIGH;

#define RELAY_LOCK_OPEN   13
#define RELAY_LOCK_CLOSE  14
#define RELAY_START       16
#define RELAY_ALARM       17
#define RELAY_LED         18
#define RELAY_FOG         19
#define RELAY_HEADLIGHT   21
#define RELAY_SEAT        22
#define RELAY_DOOR        23

#define SERVO_RIGHT_PIN   25
#define SERVO_LEFT_PIN    26
#define SERVO_TRIGGER_PIN 27

#define EEPROM_SIZE 64
#define EEPROM_MAGIC 0x4D

struct MirrorConfig {
  uint8_t magic;
  uint8_t rightOpen;
  uint8_t leftOpen;
  uint8_t rightClosed;
  uint8_t leftClosed;
};

MirrorConfig cfg;

Servo servoRight;
Servo servoLeft;

bool motorRunning = false;
bool alarmActive = false;
bool ledState = false;
bool fogState = false;
bool headlightState = false;
bool mirrorsOpen = false;
bool otaOK = false;

String logsBuf[10];
uint8_t logsCount = 0;

const char page[] = R"rawliteral(
<!DOCTYPE html><html lang="tr"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>RKS Panel</title>
<style>
:root{
  --bg:#0b1220;
  --card:#131c2f;
  --card2:#1a2740;
  --line:#2b3a57;
  --txt:#edf2ff;
  --muted:#9fb0d0;
  --red:#d63b3b;
  --green:#1db36b;
  --blue:#2f7df6;
  --orange:#f08b2d;
  --gray:#59657d;
}
*{box-sizing:border-box}
body{
  margin:0;
  font-family:Arial,Helvetica,sans-serif;
  background:linear-gradient(180deg,#08101c 0%,#0d1728 100%);
  color:var(--txt);
  padding:16px;
}
.wrap{max-width:880px;margin:auto}
.top{
  display:grid;
  grid-template-columns:1fr;
  gap:12px;
  margin-bottom:12px;
}
.card{
  background:linear-gradient(180deg,var(--card) 0%,var(--card2) 100%);
  border:1px solid var(--line);
  border-radius:18px;
  padding:16px;
  margin-bottom:12px;
  box-shadow:0 10px 25px rgba(0,0,0,.25);
}
h1,h2,h3{margin:0 0 12px}
h1{font-size:26px}
h3{font-size:18px}
.small{font-size:13px;color:var(--muted)}
.row{
  display:grid;
  grid-template-columns:repeat(auto-fit,minmax(150px,1fr));
  gap:10px;
}
button{
  width:100%;
  min-height:50px;
  border:none;
  border-radius:14px;
  color:#fff;
  font-size:15px;
  font-weight:700;
  cursor:pointer;
  transition:.18s ease;
  box-shadow:0 8px 18px rgba(0,0,0,.18);
}
button:active{transform:scale(.985)}
button:disabled{opacity:.45;cursor:not-allowed}
.red{background:linear-gradient(180deg,#ef5555,#c83030)}
.green{background:linear-gradient(180deg,#24cc7a,#14945a)}
.blue{background:linear-gradient(180deg,#3f96ff,#216ce2)}
.orange{background:linear-gradient(180deg,#f6a246,#dc7821)}
.gray{background:linear-gradient(180deg,#72809a,#556178)}
.sectionTitle{
  display:flex;
  justify-content:space-between;
  align-items:center;
  gap:8px;
  margin-bottom:12px;
}
.statusPill{
  display:inline-flex;
  align-items:center;
  gap:8px;
  padding:10px 14px;
  border-radius:999px;
  background:#0f1727;
  border:1px solid var(--line);
  color:#d9e5ff;
  font-weight:700;
  font-size:14px;
}
.dot{
  width:10px;height:10px;border-radius:50%;background:#20d17a;
  box-shadow:0 0 12px #20d17a;
}
.grid2{
  display:grid;
  grid-template-columns:1fr 1fr;
  gap:12px;
}
@media(max-width:760px){.grid2{grid-template-columns:1fr}}
.sliderWrap{
  background:#0f1727;
  border:1px solid var(--line);
  border-radius:14px;
  padding:12px;
  text-align:left;
}
.sliderHead{
  display:flex;
  justify-content:space-between;
  margin-bottom:8px;
  font-size:14px;
}
input[type=range]{width:100%}
.uploadBox{
  background:#0f1727;
  border:1px solid var(--line);
  border-radius:14px;
  padding:12px;
}
input[type=file]{
  width:100%;
  color:#dbe6ff;
  margin-bottom:10px;
}
#log{
  height:190px;
  overflow:auto;
  text-align:left;
  background:#04070d;
  color:#2aff84;
  border:1px solid var(--line);
  border-radius:14px;
  padding:12px;
  font-family:monospace;
  font-size:13px;
  line-height:1.5;
}
.hero{
  display:flex;
  justify-content:space-between;
  gap:12px;
  align-items:center;
  flex-wrap:wrap;
}
.heroLeft{
  text-align:left;
}
.heroRight{
  display:flex;
  gap:10px;
  flex-wrap:wrap;
}
.badge{
  padding:10px 12px;
  border-radius:12px;
  background:#0f1727;
  border:1px solid var(--line);
  color:#d9e5ff;
  font-size:13px;
  font-weight:700;
}
</style></head><body>
<div class="wrap">

  <div class="top">
    <div class="card">
      <div class="hero">
        <div class="heroLeft">
          <h1>RKS Motor Panel</h1>
          <div class="small">ESP32 Access Point Paneli</div>
        </div>
        <div class="heroRight">
          <div class="badge">Wi-Fi: RKS</div>
          <div class="badge">IP: 192.168.4.1</div>
          <div class="statusPill"><span class="dot"></span><span id="st">Baglaniyor...</span></div>
        </div>
      </div>
    </div>
  </div>

  <div class="card">
    <div class="sectionTitle">
      <h3>Motor Kontrol</h3>
      <div class="small">Start ve stop tek tus mantigi</div>
    </div>
    <div class="row">
      <button id="motorBtn" class="red" onclick="cmd('motor')">Motoru Calistir</button>
    </div>
  </div>

  <div class="grid2">
    <div class="card">
      <div class="sectionTitle">
        <h3>Kumanda</h3>
        <div class="small">Motor acikken kilitlenir</div>
      </div>
      <div class="row">
        <button id="bo" class="gray ctrl" onclick="cmd('lock_open')">Kilit Ac</button>
        <button id="bc" class="gray ctrl" onclick="cmd('lock_close')">Kilit Kapat</button>
        <button id="balarm" class="orange ctrl" onclick="cmd('alarm_toggle')">Alarm Cal</button>
      </div>
    </div>

    <div class="card">
      <div class="sectionTitle">
        <h3>Aydinlatma</h3>
        <div class="small">Far, sis ve led toggle</div>
      </div>
      <div class="row">
        <button id="bfar" class="red ctrl" onclick="cmd('far')">Far</button>
        <button id="bfog" class="red ctrl" onclick="cmd('fog')">Sis</button>
        <button id="bled" class="red ctrl" onclick="cmd('led')">LED</button>
        <button id="bseat" class="orange ctrl" onclick="cmd('seat')">Koltuk</button>
      </div>
    </div>
  </div>

  <div class="card">
    <div class="sectionTitle">
      <h3>Ayna Ayarlari</h3>
      <div class="small">GPIO27 HIGH: acik / LOW: kapali</div>
    </div>

    <div class="grid2">
      <div class="sliderWrap">
        <div class="sliderHead"><span>Sag Acik</span><b id="roV">90</b></div>
        <input id="ro" type="range" min="0" max="180" value="90" oninput="syncV()">
      </div>
      <div class="sliderWrap">
        <div class="sliderHead"><span>Sol Acik</span><b id="loV">90</b></div>
        <input id="lo" type="range" min="0" max="180" value="90" oninput="syncV()">
      </div>
      <div class="sliderWrap">
        <div class="sliderHead"><span>Sag Kapali</span><b id="rcV">0</b></div>
        <input id="rc" type="range" min="0" max="180" value="0" oninput="syncV()">
      </div>
      <div class="sliderWrap">
        <div class="sliderHead"><span>Sol Kapali</span><b id="lcV">0</b></div>
        <input id="lc" type="range" min="0" max="180" value="0" oninput="syncV()">
      </div>
    </div>

    <div style="margin-top:12px" class="row">
      <button class="blue ctrl" onclick="saveMir()">Ayna Ayarlarini Kaydet</button>
    </div>
  </div>

  <div class="grid2">
    <div class="card">
      <div class="sectionTitle">
        <h3>OTA Guncelleme</h3>
        <div class="small">BIN dosyasi sec</div>
      </div>
      <div class="uploadBox">
        <form method="POST" action="/update" enctype="multipart/form-data">
          <input type="file" name="update">
          <button class="green" type="submit">BIN Dosyasi Yukle</button>
        </form>
      </div>
    </div>

    <div class="card">
      <div class="sectionTitle">
        <h3>Bilgi Ekrani</h3>
        <div class="small">Son 10 olay</div>
      </div>
      <div id="log"></div>
    </div>
  </div>

</div>

<script>
let initDone=false;

function syncV(){
  roV.textContent=ro.value;
  loV.textContent=lo.value;
  rcV.textContent=rc.value;
  lcV.textContent=lc.value;
}

function setCtrls(ok){
  document.querySelectorAll('.ctrl').forEach(e=>e.disabled=!ok);
}

function cmd(x){
  fetch('/c?x='+encodeURIComponent(x),{cache:'no-store'})
  .then(()=>setTimeout(upd,150))
  .catch(()=>{
    st.textContent='Baglanti Yok';
    setCtrls(false);
  });
}

function saveMir(){
  fetch('/m?ro='+ro.value+'&lo='+lo.value+'&rc='+rc.value+'&lc='+lc.value,{cache:'no-store'})
  .then(()=>setTimeout(upd,150))
  .catch(()=>{
    st.textContent='Baglanti Yok';
    setCtrls(false);
  });
}

function upd(){
  fetch('/s',{cache:'no-store'})
  .then(r=>r.json())
  .then(d=>{
    st.textContent=d.st;
    setCtrls(true);

    if(!initDone){
      ro.value=d.ro;
      lo.value=d.lo;
      rc.value=d.rc;
      lc.value=d.lc;
      syncV();
      initDone=true;
    }

    motorBtn.textContent=d.m?'Motoru Kapat':'Motoru Calistir';
    motorBtn.className=d.m?'green':'red';

    balarm.textContent=d.a?'Alarm Kapat':'Alarm Cal';
    balarm.className=d.a?'red ctrl':'orange ctrl';

    bfar.className=d.f?'blue ctrl':'red ctrl';
    bfog.className=d.g?'blue ctrl':'red ctrl';
    bled.className=d.l?'green ctrl':'red ctrl';

    bo.disabled=d.m;
    bc.disabled=d.m;
    balarm.disabled=d.m;

    log.innerHTML=d.lg||'';
    log.scrollTop=log.scrollHeight;
  })
  .catch(()=>{
    st.textContent='Baglanti Yok';
    setCtrls(false);
  });
}

syncV();
upd();
setInterval(upd,1000);
</script>
</body></html>
)rawliteral";

void addLog(const String &s) {
  if (logsCount < 10) {
    logsBuf[logsCount++] = s;
  } else {
    for (uint8_t i = 0; i < 9; i++) logsBuf[i] = logsBuf[i + 1];
    logsBuf[9] = s;
  }
}

String getLogs() {
  String out;
  for (uint8_t i = 0; i < logsCount; i++) {
    out += logsBuf[i];
    if (i < logsCount - 1) out += "<br>";
  }
  return out;
}

void relayWrite(uint8_t pin, bool onState) {
  digitalWrite(pin, onState ? RELAY_ON : RELAY_OFF);
}

void pulseRelay(uint8_t pin, uint8_t times = 1, uint16_t onMs = 250, uint16_t offMs = 250) {
  for (uint8_t i = 0; i < times; i++) {
    relayWrite(pin, true);
    delay(onMs);
    relayWrite(pin, false);
    if (i < times - 1) delay(offMs);
  }
}

void lockCloseWithDoor() {
  relayWrite(RELAY_LOCK_CLOSE, true);
  relayWrite(RELAY_DOOR, true);
  delay(250);
  relayWrite(RELAY_LOCK_CLOSE, false);
  delay(1750);
  relayWrite(RELAY_DOOR, false);
}

void loadCfg() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, cfg);

  if (cfg.magic != EEPROM_MAGIC ||
      cfg.rightOpen > 180 || cfg.leftOpen > 180 ||
      cfg.rightClosed > 180 || cfg.leftClosed > 180) {
    cfg.magic = EEPROM_MAGIC;
    cfg.rightOpen = 90;
    cfg.leftOpen = 90;
    cfg.rightClosed = 0;
    cfg.leftClosed = 0;
    EEPROM.put(0, cfg);
    EEPROM.commit();
  }
}

void saveCfg() {
  cfg.magic = EEPROM_MAGIC;
  EEPROM.put(0, cfg);
  EEPROM.commit();
}

void writeMirrors(int r, int l) {
  servoRight.write(constrain(r, 0, 180));
  servoLeft.write(constrain(l, 0, 180));
}

void openMirrors() {
  writeMirrors(cfg.rightOpen, cfg.leftOpen);
  mirrorsOpen = true;
  addLog("Aynalar acildi");
}

void closeMirrors() {
  writeMirrors(cfg.rightClosed, cfg.leftClosed);
  mirrorsOpen = false;
  addLog("Aynalar kapandi");
}

void stopAlarmWithLockOpen(bool logText = true) {
  pulseRelay(RELAY_LOCK_OPEN, 1, 250, 250);
  alarmActive = false;
  if (logText) addLog("Alarm kapatildi");
}

String statusJson() {
  String j = "{";
  j += "\"st\":\"Baglandi (192.168.4.1)\",";
  j += "\"m\":" + String(motorRunning ? "true" : "false") + ",";
  j += "\"a\":" + String(alarmActive ? "true" : "false") + ",";
  j += "\"f\":" + String(headlightState ? "true" : "false") + ",";
  j += "\"g\":" + String(fogState ? "true" : "false") + ",";
  j += "\"l\":" + String(ledState ? "true" : "false") + ",";
  j += "\"ro\":" + String(cfg.rightOpen) + ",";
  j += "\"lo\":" + String(cfg.leftOpen) + ",";
  j += "\"rc\":" + String(cfg.rightClosed) + ",";
  j += "\"lc\":" + String(cfg.leftClosed) + ",";
  j += "\"lg\":\"" + getLogs() + "\"";
  j += "}";
  return j;
}

void handleRoot() {
  server.send(200, "text/html; charset=utf-8", page);
}

void handleStatus() {
  server.send(200, "application/json", statusJson());
}

void handleCmd() {
  String x = server.arg("x");

  if (x == "motor") {
    if (!motorRunning) {
      addLog("Motor start -> Kilit Ac [13]");
      pulseRelay(RELAY_LOCK_OPEN, 1, 250, 250);
      if (alarmActive) {
        alarmActive = false;
        addLog("Alarm kapatildi");
      }
      delay(350);
      addLog("Motor start -> Start x2 [16]");
      pulseRelay(RELAY_START, 2, 250, 300);
      motorRunning = true;
      addLog("Motor calisiyor");
    } else {
      addLog("Motor stop -> Kilit Ac [13]");
      pulseRelay(RELAY_LOCK_OPEN, 1, 250, 250);
      if (alarmActive) {
        alarmActive = false;
        addLog("Alarm kapatildi");
      }
      motorRunning = false;
      addLog("Motor durduruldu");
    }
    server.send(200, "text/plain", "OK");
    return;
  }

  if (motorRunning && (x == "lock_open" || x == "lock_close" || x == "alarm_toggle")) {
    addLog("Komut engellendi: motor acik");
    server.send(200, "text/plain", "LOCKED");
    return;
  }

  if (x == "lock_open") {
    pulseRelay(RELAY_LOCK_OPEN, 1, 250, 250);
    if (alarmActive) {
      alarmActive = false;
      addLog("Alarm kilit ac ile kapatildi");
    }
    addLog("Kilit ac [13]");
  }
  else if (x == "lock_close") {
    lockCloseWithDoor();
    addLog("Kilit kapat [14] + Kapi [23]");
  }
  else if (x == "alarm_toggle") {
    if (!alarmActive) {
      pulseRelay(RELAY_ALARM, 1, 300, 250);
      alarmActive = true;
      addLog("Alarm baslatildi [17]");
    } else {
      stopAlarmWithLockOpen();
    }
  }
  else if (x == "far") {
    headlightState = !headlightState;
    relayWrite(RELAY_HEADLIGHT, headlightState);
    addLog(headlightState ? "Far acildi [21]" : "Far kapandi [21]");
  }
  else if (x == "fog") {
    fogState = !fogState;
    relayWrite(RELAY_FOG, fogState);
    addLog(fogState ? "Sis acildi [19]" : "Sis kapandi [19]");
  }
  else if (x == "led") {
    ledState = !ledState;
    relayWrite(RELAY_LED, ledState);
    addLog(ledState ? "LED acildi [18]" : "LED kapandi [18]");
  }
  else if (x == "seat") {
    addLog("Koltuk pulse [22]");
    pulseRelay(RELAY_SEAT, 1, 1200, 250);
  }

  server.send(200, "text/plain", "OK");
}

void handleMirrorSave() {
  if (!server.hasArg("ro") || !server.hasArg("lo") || !server.hasArg("rc") || !server.hasArg("lc")) {
    server.send(400, "text/plain", "ARGS");
    return;
  }

  cfg.rightOpen   = constrain(server.arg("ro").toInt(), 0, 180);
  cfg.leftOpen    = constrain(server.arg("lo").toInt(), 0, 180);
  cfg.rightClosed = constrain(server.arg("rc").toInt(), 0, 180);
  cfg.leftClosed  = constrain(server.arg("lc").toInt(), 0, 180);

  saveCfg();
  addLog("Ayna ayarlari kaydedildi");

  if (digitalRead(SERVO_TRIGGER_PIN) == HIGH) openMirrors();
  else closeMirrors();

  server.send(200, "text/plain", "OK");
}

void handleUpdateUpload() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    otaOK = Update.begin(UPDATE_SIZE_UNKNOWN);
    addLog(otaOK ? "Web OTA basladi" : "OTA begin hatasi");
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (otaOK && Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      otaOK = false;
      addLog("OTA yazma hatasi");
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (otaOK && Update.end(true)) {
      addLog("Web OTA tamamlandi");
    } else {
      otaOK = false;
      addLog("Web OTA hata");
    }
  }
}

void handleUpdateDone() {
  server.sendHeader("Connection", "close");
  if (otaOK) {
    server.send(200, "text/plain", "GUNCELLEME_BASARILI");
    delay(700);
    ESP.restart();
  } else {
    server.send(500, "text/plain", "GUNCELLEME_HATA");
  }
}

void setup() {
  Serial.begin(115200);

  loadCfg();

  pinMode(RELAY_LOCK_OPEN, OUTPUT);
  pinMode(RELAY_LOCK_CLOSE, OUTPUT);
  pinMode(RELAY_START, OUTPUT);
  pinMode(RELAY_ALARM, OUTPUT);
  pinMode(RELAY_LED, OUTPUT);
  pinMode(RELAY_FOG, OUTPUT);
  pinMode(RELAY_HEADLIGHT, OUTPUT);
  pinMode(RELAY_SEAT, OUTPUT);
  pinMode(RELAY_DOOR, OUTPUT);

  relayWrite(RELAY_LOCK_OPEN, false);
  relayWrite(RELAY_LOCK_CLOSE, false);
  relayWrite(RELAY_START, false);
  relayWrite(RELAY_ALARM, false);
  relayWrite(RELAY_LED, false);
  relayWrite(RELAY_FOG, false);
  relayWrite(RELAY_HEADLIGHT, false);
  relayWrite(RELAY_SEAT, false);
  relayWrite(RELAY_DOOR, false);

  pinMode(SERVO_TRIGGER_PIN, INPUT_PULLDOWN);

  servoRight.setPeriodHertz(50);
  servoLeft.setPeriodHertz(50);
  servoRight.attach(SERVO_RIGHT_PIN, 500, 2400);
  servoLeft.attach(SERVO_LEFT_PIN, 500, 2400);
  writeMirrors(cfg.rightClosed, cfg.leftClosed);

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
  WiFi.softAP(apSSID, apPassword);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/s", HTTP_GET, handleStatus);
  server.on("/c", HTTP_GET, handleCmd);
  server.on("/m", HTTP_GET, handleMirrorSave);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.begin();

  addLog("AP baslatildi");
  addLog("SSID: RKS");
  addLog("IP: 192.168.4.1");
  addLog("Panel hazir");
}

void loop() {
  server.handleClient();

  static bool trigState = false;
  static bool firstRun = true;

  bool trig = digitalRead(SERVO_TRIGGER_PIN);

  if (firstRun) {
    firstRun = false;
    trigState = trig;
    if (trigState) openMirrors();
    else closeMirrors();
  }

  if (!trigState && trig) {
    trigState = true;
    addLog("Servo tetik aktif [27]");
    openMirrors();
  } else if (trigState && !trig) {
    trigState = false;
    addLog("Servo tetik pasif [27]");
    closeMirrors();
  }

  delay(2);
}
