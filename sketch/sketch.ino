// =====================================================================================
//  FIWARE Smart Lamp - ESP32
//
//  * Lê luminosidade (LDR), temperatura e umidade (DHT22 no Wokwi, DHT11 em sala) e envia
//    ao FIWARE (IoT Agent Ultralight sobre MQTT).
//  * Recebe os comandos "on" e "off". "on": o LED externo pisca e o buzzer apita em sincronia,
//    até chegar "off".
//  * Responde cada comando em /cmdexe (senão o Orion o deixa PENDING para sempre).
//
//  Base: projeto Wokwi "FIWARE Smart Lamp" (Fábio Henrique Cabrini / Lucas Demetrius Augusto).
//  Diferenças: nada usa delay() no loop (o MQTT e os comandos nunca ficam "surdos"), e a lógica
//  que não depende do hardware está em lamp_logic.h.
//
//  Placa: ESP32 DevKit V1 ("DOIT ESP32 DEVKIT V1" na Arduino IDE).
//  Bibliotecas: PubSubClient, DHT sensor library, Adafruit Unified Sensor (ver libraries.txt).
// =====================================================================================
#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>

#include "lamp_logic.h"

// ============================== CONFIGURAÇÃO (edite) ==================================
// --- Wi-Fi ---
const char* WIFI_SSID = "Wokwi-GUEST";  // no Wokwi use sempre esta rede aberta
const char* WIFI_PASSWORD = "";         // ...sem senha

// --- Broker MQTT (o Mosquitto do stack FIWARE) ---
// Um endereço que o ESP32 consiga alcançar. "localhost" NÃO serve: seria o próprio ESP32.
// Veja no README as opções (Wokwi na nuvem x ESP32 real na rede da sala).
const char* BROKER_MQTT = "COLOQUE_AQUI_O_IP_DO_BROKER";
const uint16_t BROKER_PORT = 1883;

// --- Identidade (valores devolvidos por POST /api/lamps: "deviceId" e "apiKey") ---
const char* API_KEY = "TEF";
const char* DEVICE_ID = "lamp001";

// --- Sensores ---
#define DHT_TYPE DHT22  // Wokwi: DHT22  |  sala de aula: DHT11
// LDR: leitura bruta do ADC (0..4095) -> luminosidade 0..100 (100 = claro).
// O LDR do Wokwi entrega valor ALTO no escuro (LDR_INVERTED = true). Muitos módulos reais
// fazem o contrário: se "claro" der número baixo na Serial, troque para false.
const bool LDR_INVERTED = true;
const int LDR_RAW_MIN = 0;     // leitura bruta no extremo (calibre pela Serial)
const int LDR_RAW_MAX = 4095;  // idem

// --- Alarme (comando "on") ---
const bool BUZZER_ENABLED = true;   // false = só o LED (aula silenciosa)
const bool BUZZER_PASSIVE = true;   // true: buzzer passivo/Wokwi (tom por PWM) | false: ativo (liga/desliga)
const int BUZZER_FREQ_HZ = 2000;
const uint32_t BLINK_HALF_PERIOD_MS = 500;  // 500 ms aceso + 500 ms apagado

// --- Tempos ---
const uint32_t TELEMETRY_INTERVAL_MS = 2000;  // DHT22 precisa de >= 2 s (DHT11, >= 1 s)
const uint32_t WIFI_RETRY_MS = 10000;
const uint32_t MQTT_RETRY_MS = 3000;

// ================================== PINOS ============================================
const int PIN_LDR = 34;         // AO do módulo LDR (entrada ADC1; ADC2 não funciona com Wi-Fi)
const int PIN_DHT = 4;          // dados do DHT
const int PIN_LED = 26;         // LED externo do alarme (com resistor de 220 ohms)
const int PIN_BUZZER = 27;      // buzzer
const int PIN_STATUS_LED = 2;   // LED da própria placa: aceso = conectado ao broker, piscando = sem conexão
// ======================================================================================

DHT dht(PIN_DHT, DHT_TYPE);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

char topicAttrs[64];   // /<apikey>/<deviceId>/attrs   (dispositivo -> FIWARE)
char topicCmd[64];     // /<apikey>/<deviceId>/cmd     (FIWARE -> dispositivo)
char topicCmdExe[64];  // /<apikey>/<deviceId>/cmdexe  (resposta ao comando)
char clientId[48];

bool brokerConfigured = false;
bool lampOn = false;           // estado da lâmpada ("on" = alarme ativo)
bool alarmLevel = false;       // fase atual do pisca (LED aceso + buzzer tocando)
uint32_t alarmStartedAt = 0;
uint32_t lastTelemetryAt = 0;
bool wasWifiConnected = false;
lamp::RetryTimer wifiRetry;
lamp::RetryTimer mqttRetry;

// ---------------------------------- buzzer ---------------------------------------------
// A API de PWM do ESP32 mudou na versão 3 do core Arduino; este trecho funciona nas duas.
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
void buzzerAttach() { ledcAttach(PIN_BUZZER, BUZZER_FREQ_HZ, 8); }
void buzzerTone(int freq) { ledcWriteTone(PIN_BUZZER, freq); }
#else
const int BUZZER_LEDC_CHANNEL = 0;
void buzzerAttach() {
  ledcSetup(BUZZER_LEDC_CHANNEL, BUZZER_FREQ_HZ, 8);
  ledcAttachPin(PIN_BUZZER, BUZZER_LEDC_CHANNEL);
}
void buzzerTone(int freq) { ledcWriteTone(BUZZER_LEDC_CHANNEL, freq); }
#endif

void buzzerBegin() {
  if (BUZZER_PASSIVE) {
    buzzerAttach();
    buzzerTone(0);
  } else {
    pinMode(PIN_BUZZER, OUTPUT);
    digitalWrite(PIN_BUZZER, LOW);
  }
}

void buzzerSet(bool on) {
  if (!BUZZER_ENABLED) on = false;
  if (BUZZER_PASSIVE) {
    buzzerTone(on ? BUZZER_FREQ_HZ : 0);
  } else {
    digitalWrite(PIN_BUZZER, on ? HIGH : LOW);
  }
}

// ------------------------------- LED + buzzer ------------------------------------------
// Chamada a cada volta do loop: só mexe nos pinos quando a fase do pisca muda.
void updateOutputs(uint32_t now) {
  const bool level = lampOn && lamp::alarmPhaseOn(now, alarmStartedAt, BLINK_HALF_PERIOD_MS);
  if (level == alarmLevel) return;
  alarmLevel = level;
  digitalWrite(PIN_LED, level ? HIGH : LOW);
  buzzerSet(level);
}

void setLamp(bool on, uint32_t now) {
  if (on == lampOn) return;  // "on" repetido (a automação reenvia): não reinicia o pisca
  lampOn = on;
  if (on) alarmStartedAt = now;  // o alarme começa aceso, com som
  updateOutputs(now);
}

// ------------------------------------ MQTT ---------------------------------------------
void publishState() {
  if (!mqtt.connected()) return;
  char payload[16];
  const int n = lamp::buildAttrsPayload(payload, sizeof(payload), lamp::Reading{}, lampOn);
  if (n > 0) mqtt.publish(topicAttrs, payload);  // "s|on" ou "s|off"
}

// Chega aqui cada mensagem de /cmd, por exemplo "lamp001@on|".
void onMqttMessage(char* topic, uint8_t* payload, unsigned int length) {
  (void)topic;
  Serial.print("<- comando: ");
  Serial.write(payload, length);
  Serial.println();

  const lamp::CommandResult result =
      lamp::parseCommand(reinterpret_cast<const char*>(payload), length, DEVICE_ID);
  // A partir daqui NÃO use `payload`: publicar de dentro do callback reaproveita o buffer.

  const uint32_t now = millis();
  if (result == lamp::CommandResult::On) {
    setLamp(true, now);
  } else if (result == lamp::CommandResult::Off) {
    setLamp(false, now);
  } else {
    Serial.println("   (ignorado: não é on/off deste dispositivo)");
    return;
  }

  char reply[48];
  if (lamp::buildCommandReply(reply, sizeof(reply), DEVICE_ID, result) > 0) {
    mqtt.publish(topicCmdExe, reply);  // "lamp001@on|OK"
  }
  publishState();  // o FIWARE passa a saber o novo estado na hora
  Serial.println(lampOn ? "   lâmpada LIGADA (alarme)" : "   lâmpada DESLIGADA");
}

void connectMqtt() {
  Serial.print("Conectando ao broker ");
  Serial.print(BROKER_MQTT);
  Serial.print(":");
  Serial.print(BROKER_PORT);
  Serial.print(" ... ");
  if (mqtt.connect(clientId)) {
    Serial.println("ok");
    mqtt.subscribe(topicCmd);
    mqttRetry.reset();
    publishState();  // depois de reiniciar ou reconectar, o FIWARE sincroniza o estado
  } else {
    Serial.print("falhou (rc=");
    Serial.print(mqtt.state());
    Serial.println("), nova tentativa em instantes");
  }
}

// ------------------------------------ Wi-Fi --------------------------------------------
void startWiFi() {
  Serial.print("Conectando ao Wi-Fi ");
  Serial.println(WIFI_SSID);
  WiFi.disconnect();
  if (std::strcmp(WIFI_SSID, "Wokwi-GUEST") == 0) {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD, 6);  // no Wokwi, o canal 6 conecta mais rápido
  } else {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

// Reconecta sem travar o loop: o pisca e os comandos continuam funcionando.
void maintainConnections(uint32_t now) {
  if (WiFi.status() != WL_CONNECTED) {
    wasWifiConnected = false;
    if (wifiRetry.due(now, WIFI_RETRY_MS)) startWiFi();
    return;
  }
  if (!wasWifiConnected) {
    wasWifiConnected = true;
    Serial.print("Wi-Fi conectado, IP: ");
    Serial.println(WiFi.localIP());
  }
  if (brokerConfigured && !mqtt.connected() && mqttRetry.due(now, MQTT_RETRY_MS)) {
    connectMqtt();
  }
}

// --------------------------------- sensores --------------------------------------------
int readLdrRaw() {
  long sum = 0;
  for (int i = 0; i < 8; ++i) sum += analogRead(PIN_LDR);  // média de 8 leituras: menos ruído
  return static_cast<int>(sum / 8);
}

void publishTelemetry() {
  lamp::Reading r;

  const int raw = readLdrRaw();
  r.hasLuminosity = true;
  r.luminosity = lamp::ldrToPercent(raw, LDR_RAW_MIN, LDR_RAW_MAX, LDR_INVERTED);

  const float h = dht.readHumidity();
  const float t = dht.readTemperature();
  if (lamp::validHumidity(h)) {
    r.hasHumidity = true;
    r.humidity = h;
  }
  if (lamp::validTemperature(t)) {
    r.hasTemperature = true;
    r.temperature = t;
  }

  Serial.print("LDR bruto=");
  Serial.print(raw);
  Serial.print(" -> luminosidade ");
  Serial.print(r.luminosity);
  Serial.print("% | ");
  if (r.hasTemperature && r.hasHumidity) {
    Serial.print(t, 1);
    Serial.print(" C | ");
    Serial.print(h, 1);
    Serial.println(" %");
  } else {
    Serial.println("DHT sem leitura (confira a ligação e o DHT_TYPE)");
  }

  if (!mqtt.connected()) return;
  char payload[80];
  const int n = lamp::buildAttrsPayload(payload, sizeof(payload), r, lampOn);
  if (n > 0) mqtt.publish(topicAttrs, payload);  // ex.: "l|42|t|23.5|h|60.0|s|off"
}

// ------------------------------- arranque / loop ---------------------------------------
// Teste rápido ao ligar: o LED acende e o buzzer apita por 150 ms.
void selfTest() {
  digitalWrite(PIN_LED, HIGH);
  buzzerSet(true);
  delay(150);
  digitalWrite(PIN_LED, LOW);
  buzzerSet(false);
}

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("== FIWARE Smart Lamp ==");

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);
  pinMode(PIN_STATUS_LED, OUTPUT);
  pinMode(PIN_LDR, INPUT);
  buzzerBegin();
  dht.begin();

  std::snprintf(topicAttrs, sizeof(topicAttrs), "/%s/%s/attrs", API_KEY, DEVICE_ID);
  std::snprintf(topicCmd, sizeof(topicCmd), "/%s/%s/cmd", API_KEY, DEVICE_ID);
  std::snprintf(topicCmdExe, sizeof(topicCmdExe), "/%s/%s/cmdexe", API_KEY, DEVICE_ID);
  std::snprintf(clientId, sizeof(clientId), "fiware_%s", DEVICE_ID);  // um id MQTT por dispositivo

  selfTest();

  brokerConfigured = std::strncmp(BROKER_MQTT, "COLOQUE", 7) != 0;
  if (brokerConfigured) {
    mqtt.setServer(BROKER_MQTT, BROKER_PORT);
    mqtt.setCallback(onMqttMessage);
    mqtt.setKeepAlive(15);
    mqtt.setSocketTimeout(3);  // uma tentativa de conexão não trava o loop por muito tempo
  } else {
    Serial.println("ATENCAO: configure BROKER_MQTT no topo do sketch. Sem broker, os dados nao saem daqui.");
  }

  Serial.print("Topicos: ");
  Serial.print(topicAttrs);
  Serial.print(" | ");
  Serial.print(topicCmd);
  Serial.print(" | ");
  Serial.println(topicCmdExe);

  startWiFi();
  wifiRetry.arm(millis());
}

void loop() {
  const uint32_t now = millis();

  maintainConnections(now);
  mqtt.loop();  // entrega os comandos recebidos a onMqttMessage()
  updateOutputs(now);

  // LED da placa: aceso = conectado ao broker; piscando = sem conexão
  digitalWrite(PIN_STATUS_LED, (mqtt.connected() || ((now / 250U) % 2U) == 0U) ? HIGH : LOW);

  if (lamp::elapsed(now, lastTelemetryAt, TELEMETRY_INTERVAL_MS)) {
    lastTelemetryAt = now;
    publishTelemetry();
  }
}
