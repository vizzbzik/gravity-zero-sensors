/*
  GravityZero 1.1
  ESP32-WROOM-32

  USB Serial: command interface, 115200 baud, JSON lines
  Wi-Fi: telemetry + alarms + web panel

  Sensors:
    DHT22  - GPIO23
    MQ-2   - GPIO34 (ADC1)
    Buzzer - GPIO13
    LED    - GPIO2

  Libraries:
    DHT sensor library (Adafruit)
    Adafruit Unified Sensor
    ArduinoJson 7
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <DHT.h>

// ---------------- PIN CONFIG ----------------

struct PinConfig {
  int dht = 23;
  int mq2 = 34;
  int buzzer = 13;
  int led = 2;
};

PinConfig pins;

// ---------------- DEVICE CONFIG ----------------

struct DeviceConfig {
  int gasThreshold = 1800;
  float tempThreshold = 45.0;
  float humidityThreshold = 85.0;
  bool alarmEnabled = true;
};

DeviceConfig config;

// ---------------- WIFI ----------------

const char* AP_SSID = "GravityZero";
const char* AP_PASSWORD = "GZ-setup-2026";

WebServer server(80);
Preferences prefs;

// ---------------- SENSORS ----------------

DHT* dht = nullptr;

float temperature = NAN;
float humidity = NAN;

int gasRaw = 0;
float gasVoltage = 0;

bool dhtValid = false;
bool gasValid = false;

bool alarmActive = false;
bool alarmMuted = false;

unsigned long lastDhtRead = 0;
unsigned long lastGasRead = 0;
unsigned long lastAlarmLog = 0;
unsigned long lastBuzzerToggle = 0;

bool buzzerState = false;

// ---------------- TIMERS ----------------

const unsigned long DHT_INTERVAL = 2500;
const unsigned long GAS_INTERVAL = 250;
const unsigned long ALARM_LOG_INTERVAL = 10000;

// ---------------- LOGGING ----------------

String lastAlert = "None";
int alertCount = 0;

// ---------------- SERIAL ----------------

String serialBuffer;
const size_t SERIAL_BUFFER_LIMIT = 512;

// =====================================================
// JSON SERIAL RESPONSE
// =====================================================

void serialResponse(JsonDocument& doc) {
  serializeJson(doc, Serial);
  Serial.println();
}

void serialError(const String& message) {
  JsonDocument doc;
  doc["ok"] = false;
  doc["error"] = message;
  serialResponse(doc);
}

void serialSuccess(const String& message) {
  JsonDocument doc;
  doc["ok"] = true;
  doc["message"] = message;
  serialResponse(doc);
}

// =====================================================
// GPIO VALIDATION
// =====================================================

bool isOutputCapablePin(int pin) {
  const int allowed[] = {
    2, 4, 13, 14, 16, 17, 18, 19,
    21, 22, 23, 25, 26, 27, 32, 33
  };

  for (int p : allowed) {
    if (p == pin) return true;
  }

  return false;
}

bool isDhtPinAllowed(int pin) {
  return isOutputCapablePin(pin);
}

bool isMq2PinAllowed(int pin) {
  // ESP32 ADC1 pins. ADC2 may conflict with Wi-Fi.
  return pin >= 32 && pin <= 39;
}

bool isPinUsed(int pin, const String& except) {
  if (except != "dht22" && pins.dht == pin) return true;
  if (except != "mq2" && pins.mq2 == pin) return true;
  if (except != "buzzer" && pins.buzzer == pin) return true;
  if (except != "led" && pins.led == pin) return true;

  return false;
}

// =====================================================
// CONFIGURATION STORAGE
// =====================================================

void saveConfig() {
  prefs.begin("gz-config", false);

  prefs.putInt("dht", pins.dht);
  prefs.putInt("mq2", pins.mq2);
  prefs.putInt("buzzer", pins.buzzer);
  prefs.putInt("led", pins.led);

  prefs.putInt("gas", config.gasThreshold);
  prefs.putFloat("temp", config.tempThreshold);
  prefs.putFloat("humidity", config.humidityThreshold);

  prefs.putBool("alarm", config.alarmEnabled);

  prefs.end();
}

void loadConfig() {
  prefs.begin("gz-config", true);

  pins.dht = prefs.getInt("dht", 23);
  pins.mq2 = prefs.getInt("mq2", 34);
  pins.buzzer = prefs.getInt("buzzer", 13);
  pins.led = prefs.getInt("led", 2);

  config.gasThreshold = prefs.getInt("gas", 1800);
  config.tempThreshold = prefs.getFloat("temp", 45.0);
  config.humidityThreshold = prefs.getFloat("humidity", 85.0);

  config.alarmEnabled = prefs.getBool("alarm", true);

  prefs.end();

  // Validate stored GPIOs to avoid invalid configuration.
  if (!isDhtPinAllowed(pins.dht)) pins.dht = 23;
  if (!isMq2PinAllowed(pins.mq2)) pins.mq2 = 34;
  if (!isOutputCapablePin(pins.buzzer)) pins.buzzer = 13;
  if (!isOutputCapablePin(pins.led)) pins.led = 2;

  if (pins.dht == pins.buzzer || pins.dht == pins.led ||
      pins.mq2 == pins.buzzer || pins.mq2 == pins.led ||
      pins.buzzer == pins.led) {
    pins = PinConfig();
  }

  config.gasThreshold = constrain(config.gasThreshold, 1, 4095);
  config.tempThreshold = constrain(config.tempThreshold, -40.0, 125.0);
  config.humidityThreshold = constrain(config.humidityThreshold, 1.0, 100.0);
}

// =====================================================
// SENSOR INITIALIZATION
// =====================================================

void initDht() {
  if (dht != nullptr) {
    delete dht;
    dht = nullptr;
  }

  dht = new DHT(pins.dht, DHT22);
  dht->begin();

  dhtValid = false;
  temperature = NAN;
  humidity = NAN;
}

void initPins() {
  pinMode(pins.mq2, INPUT);
  pinMode(pins.buzzer, OUTPUT);
  pinMode(pins.led, OUTPUT);

  digitalWrite(pins.buzzer, LOW);
  digitalWrite(pins.led, LOW);

  analogReadResolution(12);
  analogSetPinAttenuation(
    pins.mq2,
    ADC_11db
  );

  initDht();
}

// =====================================================
// LOGGING
// =====================================================

void addLog(const String& message) {
  String line = String(millis()) + " ms | " + message;

  if (LittleFS.begin(false)) {
    File file = LittleFS.open("/alerts.log", FILE_APPEND);

    if (file) {
      file.println(line);
      file.close();
    }
  }

  lastAlert = message;
  alertCount++;

  Serial.print("[EVENT] ");
  Serial.println(message);
}

String readLogs() {
  if (!LittleFS.begin(false)) {
    return "LittleFS unavailable\n";
  }

  File file = LittleFS.open("/alerts.log", FILE_READ);

  if (!file) return "No logs available\n";

  String result = file.readString();
  file.close();

  return result;
}

// =====================================================
// SENSOR READING
// =====================================================

void readDht() {
  if (millis() - lastDhtRead < DHT_INTERVAL) return;

  lastDhtRead = millis();

  if (dht == nullptr) return;

  float t = dht->readTemperature();
  float h = dht->readHumidity();

  if (isnan(t) || isnan(h)) {
    dhtValid = false;
    return;
  }

  temperature = t;
  humidity = h;
  dhtValid = true;
}

void readGas() {
  if (millis() - lastGasRead < GAS_INTERVAL) return;

  lastGasRead = millis();

  gasRaw = analogRead(pins.mq2);
  gasVoltage = (gasRaw / 4095.0) * 3.3;

  gasValid = true;
}

// =====================================================
// ALARM SYSTEM
// =====================================================

bool checkAlarm() {
  bool gasAlarm = gasValid &&
                  gasRaw >= config.gasThreshold;

  bool tempAlarm = dhtValid &&
                   temperature >= config.tempThreshold;

  bool humidityAlarm = dhtValid &&
                       humidity >= config.humidityThreshold;

  return gasAlarm || tempAlarm || humidityAlarm;
}

void updateAlarm() {
  bool newAlarm = checkAlarm() && config.alarmEnabled;

  if (newAlarm && !alarmActive) {
    addLog("ALARM ACTIVATED");
  }

  if (!newAlarm && alarmActive) {
    addLog("Alarm cleared");
  }

  alarmActive = newAlarm;

  if (alarmActive) {
    digitalWrite(pins.led, HIGH);

    if (millis() - lastBuzzerToggle >= 500) {
      lastBuzzerToggle = millis();
      buzzerState = !buzzerState;
      digitalWrite(pins.buzzer, buzzerState ? HIGH : LOW);
    }

    if (millis() - lastAlarmLog >= ALARM_LOG_INTERVAL) {
      lastAlarmLog = millis();
      addLog("ALARM STILL ACTIVE");
    }
  } else {
    digitalWrite(pins.led, LOW);
    digitalWrite(pins.buzzer, LOW);
    buzzerState = false;
  }
}

// =====================================================
// TELEMETRY JSON
// =====================================================

String makeTelemetry() {
  JsonDocument doc;

  doc["device"] = "GravityZero";
  doc["version"] = "1.1.0";

  doc["temperatureValid"] = dhtValid;
  doc["humidityValid"] = dhtValid;

  if (dhtValid) {
    doc["temperature"] = temperature;
    doc["humidity"] = humidity;
  } else {
    doc["temperature"] = nullptr;
    doc["humidity"] = nullptr;
  }

  doc["gasValid"] = gasValid;
  doc["gas"] = gasRaw;
  doc["voltage"] = gasVoltage;

  doc["alarm"] = alarmActive;
  doc["alarmEnabled"] = config.alarmEnabled;

  doc["uptime"] = millis() / 1000;
  doc["freeHeap"] = ESP.getFreeHeap();

  doc["gasThreshold"] = config.gasThreshold;
  doc["tempThreshold"] = config.tempThreshold;
  doc["humidityThreshold"] = config.humidityThreshold;

  doc["lastAlert"] = lastAlert;
  doc["alertCount"] = alertCount;

  String output;
  serializeJson(doc, output);

  return output;
}

// =====================================================
// PIN JSON
// =====================================================

void addPinsToJson(JsonDocument& doc) {
  JsonObject p = doc["pins"].to<JsonObject>();

  p["dht22"] = pins.dht;
  p["mq2"] = pins.mq2;
  p["buzzer"] = pins.buzzer;
  p["led"] = pins.led;
}

// =====================================================
// SERIAL COMMAND HANDLER
// =====================================================

void handleSerialCommand(const String& input) {
  JsonDocument request;

  DeserializationError error = deserializeJson(request, input);

  if (error) {
    serialError("Invalid JSON");
    return;
  }

  String cmd = request["cmd"] | "";

  // STATUS
  if (cmd == "status" || cmd == "sensors") {
    JsonDocument response;
    response["ok"] = true;

    JsonDocument telemetry;
    deserializeJson(telemetry, makeTelemetry());

    response["data"] = telemetry;

    serialResponse(response);
    return;
  }

  // PINS LIST
  if (cmd == "pins_list") {
    JsonDocument response;
    response["ok"] = true;
    addPinsToJson(response);
    serialResponse(response);
    return;
  }

  // CHANGE PIN
  if (cmd == "pins_set") {
    String device = request["device"] | "";
    int newPin = request["pin"] | -1;

    if (newPin < 0 || newPin > 39) {
      serialError("Invalid GPIO");
      return;
    }

    if (isPinUsed(newPin, device)) {
      serialError("GPIO already assigned");
      return;
    }

    if (device == "dht22") {
      if (!isDhtPinAllowed(newPin)) {
        serialError("DHT22 requires an output-capable GPIO");
        return;
      }

      pins.dht = newPin;
      initDht();
    }
    else if (device == "mq2") {
      if (!isMq2PinAllowed(newPin)) {
        serialError("MQ-2 requires an ADC1 GPIO (32-39)");
        return;
      }

      pins.mq2 = newPin;
      pinMode(pins.mq2, INPUT);
      analogSetPinAttenuation(pins.mq2, ADC_11db);
    }
    else if (device == "buzzer") {
      if (!isOutputCapablePin(newPin)) {
        serialError("Invalid buzzer GPIO");
        return;
      }

      digitalWrite(pins.buzzer, LOW);
      pins.buzzer = newPin;
      pinMode(pins.buzzer, OUTPUT);
      digitalWrite(pins.buzzer, LOW);
    }
    else if (device == "led") {
      if (!isOutputCapablePin(newPin)) {
        serialError("Invalid LED GPIO");
        return;
      }

      digitalWrite(pins.led, LOW);
      pins.led = newPin;
      pinMode(pins.led, OUTPUT);
      digitalWrite(pins.led, LOW);
    }
    else {
      serialError("Unknown device");
      return;
    }

    JsonDocument response;
    response["ok"] = true;
    response["message"] = "GPIO changed";
    response["device"] = device;
    response["pin"] = newPin;

    serialResponse(response);
    return;
  }

  // SAVE CONFIG
  if (cmd == "config_save") {
    saveConfig();
    serialSuccess("Configuration saved to NVS");
    return;
  }

  // CONFIG GET
  if (cmd == "config_get") {
    JsonDocument response;

    response["ok"] = true;
    response["gas"] = config.gasThreshold;
    response["temp"] = config.tempThreshold;
    response["humidity"] = config.humidityThreshold;
    response["alarm"] = config.alarmEnabled;

    serialResponse(response);
    return;
  }

  // CONFIG SET
  if (cmd == "config_set") {
    String key = request["key"] | "";
    float value = request["value"] | NAN;

    if (isnan(value)) {
      serialError("Invalid value");
      return;
    }

    if (key == "gas") {
      if (value < 1 || value > 4095) {
        serialError("Gas threshold must be 1-4095");
        return;
      }

      config.gasThreshold = (int)value;
    }
    else if (key == "temp") {
      if (value < -40 || value > 125) {
        serialError("Temperature threshold out of range");
        return;
      }

      config.tempThreshold = value;
    }
    else if (key == "humidity") {
      if (value < 1 || value > 100) {
        serialError("Humidity threshold must be 1-100");
        return;
      }

      config.humidityThreshold = value;
    }
    else {
      serialError("Unknown config key");
      return;
    }

    serialSuccess("Configuration updated. Use config_save.");
    return;
  }

  // ALARM
  if (cmd == "alarm") {
    bool enabled = request["enabled"] | true;

    config.alarmEnabled = enabled;

    if (!enabled) {
      alarmActive = false;
      digitalWrite(pins.buzzer, LOW);
      digitalWrite(pins.led, LOW);
    }

    serialSuccess(enabled ? "Alarm enabled" : "Alarm disabled");
    return;
  }

  // LOGS
  if (cmd == "logs") {
    JsonDocument response;
    response["ok"] = true;
    response["logs"] = readLogs();
    serialResponse(response);
    return;
  }

  // REBOOT
  if (cmd == "reboot") {
    serialSuccess("Rebooting");
    Serial.flush();
    delay(300);
    ESP.restart();
    return;
  }

  serialError("Unknown command");
}

// =====================================================
// WIFI WEB PANEL
// =====================================================

const char WEB_PAGE[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>GravityZero</title>
<style>
*{box-sizing:border-box}
body{
  margin:0;background:#0b1220;color:#e5edf8;
  font-family:Arial,sans-serif;padding:22px
}
main{max-width:850px;margin:auto}
header{padding:18px 0;border-bottom:1px solid #26354c}
h1{margin:0;color:#60a5fa}
small{color:#91a4bd}
.grid{
  display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));
  gap:14px;margin-top:20px
}
.card{
  background:#131f32;border:1px solid #26354c;
  border-radius:14px;padding:18px
}
.value{font-size:30px;font-weight:bold;margin-top:12px}
.status{padding:14px;border-radius:10px;margin-top:16px}
.good{background:#123b30;color:#6ee7b7}
.bad{background:#4b1d29;color:#fda4af}
button{
  background:#2563eb;color:white;border:0;padding:11px 15px;
  border-radius:8px;cursor:pointer;margin:4px
}
input{
  width:100%;padding:10px;background:#0b1220;
  border:1px solid #34445e;color:white;border-radius:7px
}
label{display:block;margin:12px 0 5px;color:#a9bad0}
</style>
</head>
<body>
<main>
<header>
<h1>GravityZero</h1>
<small>Environmental monitoring system · v1.1.0</small>
</header>

<div id="alarm" class="status">Connecting...</div>

<div class="grid">
<div class="card">
<small>TEMPERATURE</small>
<div class="value" id="temp">-- °C</div>
</div>
<div class="card">
<small>HUMIDITY</small>
<div class="value" id="hum">-- %</div>
</div>
<div class="card">
<small>MQ-2 RAW</small>
<div class="value" id="gas">--</div>
</div>
<div class="card">
<small>MQ-2 VOLTAGE</small>
<div class="value" id="voltage">-- V</div>
</div>
<div class="card">
<small>UPTIME</small>
<div class="value" id="uptime">--</div>
</div>
<div class="card">
<small>FREE HEAP</small>
<div class="value" id="heap">--</div>
</div>
</div>

<div class="card" style="margin-top:18px">
<h3>Alarm configuration</h3>

<label>Gas threshold (ADC)</label>
<input id="gasThreshold" type="number">

<label>Temperature threshold (°C)</label>
<input id="tempThreshold" type="number" step="0.1">

<label>Humidity threshold (%)</label>
<input id="humidityThreshold" type="number" step="0.1">

<button onclick="saveConfig()">Save thresholds</button>
<button onclick="alarmToggle(true)">Enable alarm</button>
<button onclick="alarmToggle(false)">Disable alarm</button>
<p id="message"></p>
</div>

<div class="card" style="margin-top:18px">
<h3>Last event</h3>
<p id="lastAlert">None</p>
<p>Events: <span id="count">0</span></p>
</div>
</main>

<script>
async function update(){
 try{
  const r=await fetch('/api');
  const d=await r.json();

  document.getElementById('temp').textContent=
    d.temperatureValid ? d.temperature.toFixed(1)+' °C':'--';

  document.getElementById('hum').textContent=
    d.humidityValid ? d.humidity.toFixed(1)+' %':'--';

  document.getElementById('gas').textContent=
    d.gasValid ? d.gas:'--';

  document.getElementById('voltage').textContent=
    d.voltage.toFixed(2)+' V';

  document.getElementById('uptime').textContent=
    d.uptime+' s';

  document.getElementById('heap').textContent=
    Math.round(d.freeHeap/1024)+' KB';

  document.getElementById('lastAlert').textContent=d.lastAlert;
  document.getElementById('count').textContent=d.alertCount;

  const a=document.getElementById('alarm');
  a.textContent=d.alarm?'ALARM ACTIVE':'SYSTEM NORMAL';
  a.className='status '+(d.alarm?'bad':'good');

  document.getElementById('gasThreshold').value=d.gasThreshold;
  document.getElementById('tempThreshold').value=d.tempThreshold;
  document.getElementById('humidityThreshold').value=d.humidityThreshold;
 }catch(e){
  document.getElementById('alarm').textContent='Connection error';
  document.getElementById('alarm').className='status bad';
 }
}

async function saveConfig(){
 const body=new URLSearchParams();
 body.set('gas',document.getElementById('gasThreshold').value);
 body.set('temp',document.getElementById('tempThreshold').value);
 body.set('humidity',document.getElementById('humidityThreshold').value);

 const r=await fetch('/config',{method:'POST',body});
 document.getElementById('message').textContent=
  r.ok?'Configuration saved':'Configuration error';
 update();
}

async function alarmToggle(enabled){
 const body=new URLSearchParams();
 body.set('enabled',enabled?'1':'0');

 await fetch('/alarm',{method:'POST',body});
 update();
}

update();
setInterval(update,2000);
</script>
</body>
</html>
)HTML";

// =====================================================
// WEB ROUTES
// =====================================================

void setupWebServer() {
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html; charset=utf-8", WEB_PAGE);
  });

  server.on("/api", HTTP_GET, []() {
    server.send(200, "application/json", makeTelemetry());
  });

  server.on("/config", HTTP_GET, []() {
    JsonDocument doc;

    doc["gas"] = config.gasThreshold;
    doc["temp"] = config.tempThreshold;
    doc["humidity"] = config.humidityThreshold;
    doc["alarm"] = config.alarmEnabled;

    String out;
    serializeJson(doc, out);

    server.send(200, "application/json", out);
  });

  server.on("/config", HTTP_POST, []() {
    if (!server.hasArg("gas") ||
        !server.hasArg("temp") ||
        !server.hasArg("humidity")) {
      server.send(400, "text/plain", "Missing fields");
      return;
    }

    int gas = server.arg("gas").toInt();
    float temp = server.arg("temp").toFloat();
    float hum = server.arg("humidity").toFloat();

    if (gas < 1 || gas > 4095 ||
        temp < -40 || temp > 125 ||
        hum < 1 || hum > 100) {
      server.send(400, "text/plain", "Invalid values");
      return;
    }

    config.gasThreshold = gas;
    config.tempThreshold = temp;
    config.humidityThreshold = hum;

    saveConfig();

    server.send(200, "text/plain", "OK");
  });

  server.on("/alarm", HTTP_POST, []() {
    bool enabled = server.arg("enabled") == "1";

    config.alarmEnabled = enabled;
    saveConfig();

    if (!enabled) {
      alarmActive = false;
      digitalWrite(pins.buzzer, LOW);
      digitalWrite(pins.led, LOW);
    }

    server.send(200, "text/plain", "OK");
  });

  server.on("/logs", HTTP_GET, []() {
    server.send(200, "text/plain; charset=utf-8", readLogs());
  });

  server.on("/pins", HTTP_GET, []() {
    JsonDocument doc;
    addPinsToJson(doc);

    String out;
    serializeJson(doc, out);

    server.send(200, "application/json", out);
  });

  server.on("/system/reboot", HTTP_POST, []() {
    server.send(200, "text/plain", "Rebooting");
    delay(300);
    ESP.restart();
  });

  server.begin();
}

// =====================================================
// SERIAL INPUT
// =====================================================

void processSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();

    if (c == '\n') {
      serialBuffer.trim();

      if (serialBuffer.length() > 0) {
        handleSerialCommand(serialBuffer);
      }

      serialBuffer = "";
    }
    else if (c != '\r') {
      if (serialBuffer.length() < SERIAL_BUFFER_LIMIT) {
        serialBuffer += c;
      } else {
        serialBuffer = "";
        serialError("Serial command too long");
      }
    }
  }
}

// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("GravityZero booting...");

  loadConfig();

  if (!LittleFS.begin(false)) {
    Serial.println("[WARN] LittleFS unavailable");
  }

  initPins();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);

  setupWebServer();

  Serial.println("[READY] GravityZero 1.1.0");
  Serial.print("[WIFI] SSID: ");
  Serial.println(AP_SSID);
  Serial.print("[WIFI] IP: ");
  Serial.println(WiFi.softAPIP());

  Serial.println("[SERIAL] JSON protocol ready");
}

// =====================================================
// LOOP
// =====================================================

void loop() {
  processSerial();

  readDht();
  readGas();

  updateAlarm();

  server.handleClient();

  delay(2);
}