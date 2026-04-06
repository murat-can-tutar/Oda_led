#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>

const char* apSSID = "RKS";
const char* apPassword = "44AFT748";

WebServer server(80);

#define TEST_RELAY_PIN 13

bool activeLow = true;
bool testRunning = true;
bool pulseActive = false;
bool otaOK = false;

unsigned long lastModeSwitch = 0;
unsigned long lastPulseTick = 0;
unsigned long pulseStartedAt = 0;

const unsigned long MODE_SWITCH_MS = 10000;
const unsigned long PULSE_EVERY_MS = 1000;
const unsigned long PULSE_ON_MS = 300;

String logsBuf[10];
uint8_t logsCount = 0;

const char page[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="tr"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Tek Röle Test</title>
<style>
body{font-family:Arial;background:#101722;color:#eee;margin:0;padding:16px;text-align:center}
.wrap{max-width:760px;margin:auto}
.card{background:#182234;border:1px solid #2b3a57;border-radius:16px;padding:16px;margin-bottom:12px}
button{width:100%;min-height:50px;border:none;border-radius:12px;color:#fff;font-size:15px;font-weight:700}
.row{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:10px}
.red{background:#d63b3b}.green{background:#1db36b}.blue{background:#2f7df6}.orange{background:#f08b2d}
.badge{display:inline-block;background:#0f1727;border:1px solid #2b3a57;border-radius:12px;padding:10px 12px;margin:4px}
#log{height:220px;overflow:auto;text-align:left;background:#000;color:#2aff84;border:1px solid #2b3a57;border-radius:12px;padding:10px;font-family:monospace;font-size:13px}
input[type=file]{width:100%;margin-bottom:10px;color:#ddd}
.small{font-size:13px;color:#9fb0d0}
</style></head><body>
<div class="wrap">

<div class="card">
  <h2>ESP32 Tek Röle Test</h2>
  <div class="small">Wi-Fi: RKS | IP: 192.168.4.1</div>
  <div style="margin-top:10px">
    <span class="badge" id="modeBadge">Mod: -</span>
    <span class="badge" id="runBadge">Test: -</span>
    <span class="badge">Pin: GPIO13</span>
  </div>
</div>

<div class="card">
  <div class="row">
    <button class="green" onclick="api('/test/start')">Testi Başlat</button>
    <button class="red" onclick="api('/test/stop')">Testi Durdur</button>
    <button class="orange" onclick="api('/mode/toggle')">Modu Değiştir</button>
    <button class="blue" onclick="api('/relay/off')">Röleyi Kapat</button>
  </div>
</div>

<div class="card">
  <h3>OTA Güncelleme</h3>
  <form method="POST" action="/update" enctype="multipart/form-data">
    <input type="file" name="update">
    <button class="blue" type="submit">BIN Dosyası Yükle</button>
  </form>
</div>

<div class="card">
  <h3>Log</h3>
  <div id="log"></div>
</div>

</div>

<script>
function api(path){
  fetch(path,{cache:'no-store'})
  .then(()=>setTimeout(updateStatus,150))
  .catch(()=>{});
}
function updateStatus(){
  fetch('/status',{cache:'no-store'})
  .then(r=>r.json())
  .then(d=>{
    modeBadge.textContent='Mod: ' + d.mode;
    runBadge.textContent='Test: ' + (d.running ? 'Çalışıyor' : 'Durdu');
    log.innerHTML=d.logs || '';
    log.scrollTop=log.scrollHeight;
  })
  .catch(()=>{});
}
updateStatus();
setInterval(updateStatus,1000);
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

void writeRelay(bool active) {
  if (activeLow) {
    digitalWrite(TEST_RELAY_PIN, active ? LOW : HIGH);
  } else {
    digitalWrite(TEST_RELAY_PIN, active ? HIGH : LOW);
  }
}

void relayOffNow() {
  writeRelay(false);
}

String modeText() {
  return activeLow ? "LOW aktif" : "HIGH aktif";
}

String statusJson() {
  String j = "{";
  j += "\"mode\":\"" + modeText() + "\",";
  j += "\"running\":" + String(testRunning ? "true" : "false") + ",";
  j += "\"logs\":\"" + getLogs() + "\"";
  j += "}";
  return j;
}

void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", page);
}

void handleStatus() {
  server.send(200, "application/json", statusJson());
}

void handleStart() {
  testRunning = true;
  addLog("Test baslatildi");
  server.send(200, "text/plain", "OK");
}

void handleStop() {
  testRunning = false;
  pulseActive = false;
  relayOffNow();
  addLog("Test durduruldu");
  server.send(200, "text/plain", "OK");
}

void handleModeToggle() {
  activeLow = !activeLow;
  pulseActive = false;
  relayOffNow();
  addLog("Mod degisti -> " + modeText());
  server.send(200, "text/plain", "OK");
}

void handleRelayOff() {
  pulseActive = false;
  relayOffNow();
  addLog("Role kapatildi");
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
  pinMode(TEST_RELAY_PIN, OUTPUT);

  activeLow = true;
  relayOffNow();

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
  WiFi.softAP(apSSID, apPassword);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/test/start", HTTP_GET, handleStart);
  server.on("/test/stop", HTTP_GET, handleStop);
  server.on("/mode/toggle", HTTP_GET, handleModeToggle);
  server.on("/relay/off", HTTP_GET, handleRelayOff);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.begin();

  addLog("AP baslatildi");
  addLog("SSID: RKS");
  addLog("IP: 192.168.4.1");
  addLog("Pin: GPIO13");
  addLog("Baslangic modu: LOW aktif");
}

void loop() {
  server.handleClient();

  unsigned long now = millis();

  if (testRunning) {
    if (now - lastModeSwitch >= MODE_SWITCH_MS) {
      lastModeSwitch = now;
      activeLow = !activeLow;
      pulseActive = false;
      relayOffNow();
      addLog("Otomatik mod degisti -> " + modeText());
    }

    if (!pulseActive && now - lastPulseTick >= PULSE_EVERY_MS) {
      lastPulseTick = now;
      pulseStartedAt = now;
      pulseActive = true;
      writeRelay(true);
      addLog("Röle ON | " + modeText());
    }

    if (pulseActive && now - pulseStartedAt >= PULSE_ON_MS) {
      writeRelay(false);
      pulseActive = false;
      addLog("Röle OFF");
    }
  }

  delay(2);
}
