
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>

// ================= KONFIGURASI WI-FI =================
const char* ssid = "Kosan Vama";
const char* password = "Maira120413";

// ================= KONFIGURASI MQTT ==================
const char* mqttServer = "broker.hivemq.com";
const int mqttPort = 1883;
const char* clientId = "ESP8266_LabIoT_25782106_Safety";

// ================= TOPIK MQTT ========================
const char* topicPublish =
  "kampus/labiot/25782106/telemetri";
const char* topicSubscribe =
  "kampus/labiot/25782106/kendali";
const char* topicAlert =
  "kampus/labiot/25782106/alert";
const char* topicStatus =
  "kampus/labiot/25782106/status";

// ================= PERANGKAT KERAS ===================
const byte dhtPin = 2;  // D4 / GPIO 2
const byte ledPin = 5;  // D1 / GPIO 5
#define DHTTYPE DHT22

DHT dht(dhtPin, DHTTYPE);
WiFiClient espClient;
PubSubClient client(espClient);

// ================= PENGATURAN WAKTU ===================
const unsigned long telemetryInterval = 4000;
const unsigned long wifiRetryInterval = 5000;
const unsigned long mqttRetryInterval = 4000;

unsigned long lastTelemetryTime = 0;
unsigned long lastWiFiAttempt = 0;
unsigned long lastMqttAttempt = 0;

// ================= STATUS KESELAMATAN ================
bool overheat = false;
bool alertSent = false;
bool temperatureKnown = false;
int manualLed = LOW;

// ================= PESAN LAST WILL ===================
const char* willMessage =
  "{\"status\":\"OFFLINE\","
  "\"keterangan\":\"Perangkat Terputus Tak Terduga\"}";

// Mengutamakan LED otomatis ketika suhu berbahaya
void updateLed() {
  if (overheat) {
    digitalWrite(ledPin, HIGH);
  } else {
    digitalWrite(ledPin, manualLed);
  }
}

// ================= CALLBACK KENDALI ==================
void callback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, topicSubscribe) != 0) {
    return;
  }

  StaticJsonDocument<256> doc;
  DeserializationError err =
    deserializeJson(doc, payload, length);

  if (err) {
    Serial.println("[ERROR] JSON kendali tidak valid");
    return;
  }

  if (!doc.containsKey("led")) {
    return;
  }

  int val = doc["led"];

  if (val != 0 && val != 1) {
    Serial.println("[ERROR] Nilai LED harus 0 atau 1");
    return;
  }

  if (overheat) {
    digitalWrite(ledPin, HIGH);
    Serial.println(
      "[SAFETY] Perintah manual diabaikan: suhu PANAS"
    );
    return;
  }

  manualLed = (val == 1) ? HIGH : LOW;
  updateLed();

  Serial.printf(
    "[KONTROL] LED %s\n",
    val == 1 ? "MENYALA" : "PADAM"
  );
}

// ================= KONEKSI WI-FI =====================
void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  unsigned long now = millis();

  if (now - lastWiFiAttempt >= wifiRetryInterval) {
    lastWiFiAttempt = now;
    Serial.println("[Wi-Fi] Mencoba menghubungkan ulang...");
    WiFi.begin(ssid, password);
  }
}

// ================= KONEKSI MQTT + LWT ================
void maintainMQTT() {
  if (WiFi.status() != WL_CONNECTED ||
      client.connected()) {
    return;
  }

  unsigned long now = millis();

  if (now - lastMqttAttempt < mqttRetryInterval) {
    return;
  }

  lastMqttAttempt = now;

  Serial.println("[MQTT] Mencoba menghubungkan broker...");

  bool connected = client.connect(
    clientId,
    topicStatus,
    1,
    true,
    willMessage
  );

  if (connected) {
    Serial.println("[MQTT] Terhubung!");

    client.subscribe(topicSubscribe);

    StaticJsonDocument<128> statusDoc;
    statusDoc["status"] = "ONLINE";
    statusDoc["keterangan"] =
      "Perangkat Beroperasi Normal";

    char statusBuffer[128];
    serializeJson(statusDoc, statusBuffer);

    client.publish(topicStatus, statusBuffer, true);

    Serial.println("[STATUS] ONLINE");
    Serial.printf("[SUBSCRIBE] %s\n", topicSubscribe);
  } else {
    Serial.printf(
      "[MQTT] Gagal, kode: %d\n",
      client.state()
    );
  }
}

// ================= SETUP =============================
void setup() {
  Serial.begin(115200);

  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);

  dht.begin();

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  client.setServer(mqttServer, mqttPort);
  client.setCallback(callback);
  client.setBufferSize(512);

  Serial.println("================================");
  Serial.println(" SISTEM KESELAMATAN MQTT");
  Serial.println("================================");
}

// ================= LOOP UTAMA ========================
void loop() {
  maintainWiFi();
  maintainMQTT();

  if (client.connected()) {
    client.loop();
  }

  unsigned long now = millis();

  if (now - lastTelemetryTime < telemetryInterval) {
    return;
  }

  lastTelemetryTime = now;

  if (WiFi.status() != WL_CONNECTED ||
      !client.connected()) {
    return;
  }

  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (isnan(t) || isnan(h)) {
    Serial.println("[ERROR] Pembacaan DHT gagal");
    return;
  }

  temperatureKnown = true;
  overheat = (t > 31.0);

  // ===== RULE ENGINE DAN ALERT BAHAYA ================
  if (overheat) {
    updateLed();

    if (!alertSent) {
      StaticJsonDocument<192> alertDoc;
      alertDoc["status"] = "BAHAYA";
      alertDoc["parameter"] = "OVERHEAT";
      alertDoc["nilai_suhu"] = t;

      char alertBuffer[192];
      serializeJson(alertDoc, alertBuffer);

      if (client.publish(topicAlert, alertBuffer)) {
        Serial.println("[ALERT] PERINGATAN BAHAYA");
        Serial.println(alertBuffer);
        alertSent = true;
      } else {
        Serial.println("[ERROR] Alert gagal dikirim");
      }
    }
  } else {
    alertSent = false;
    updateLed();
  }

  // ===== MEMBUAT DATA TELEMETRI ======================
  StaticJsonDocument<384> docOut;

  docOut["node_id"] = clientId;
  docOut["suhu"] = t;
  docOut["kelembapan"] = h;
  docOut["status_led"] = digitalRead(ledPin);
  docOut["uptime_detik"] = millis() / 1000;
  docOut["ip_lokal"] = WiFi.localIP().toString();
  docOut["kondisi_suhu"] =
    overheat ? "PANAS" : "NORMAL";

  char buffer[384];
  serializeJson(docOut, buffer);

  // ===== KIRIM TELEMETRI: OUTPUT DUA BARIS ============
  if (client.publish(topicPublish, buffer)) {
    Serial.println("[TELEMETRI TERKIRIM]");
    Serial.println(buffer);
  } else {
    Serial.println("[ERROR] Telemetri gagal dikirim");
  }
}
