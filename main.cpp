#include <MAX3010x.h>
#include <Adafruit_SSD1306.h>
#include "filters.h"
#include <Arduino.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "time.h"
#include "apwifieeprommode.h"
#include <EEPROM.h>

// Definición del botón 
#define BUTTON_PIN 25
#define HOLD_TIME_MS 2000  // 2 segundos para pulsación larga

unsigned long pressStart = 0;
bool longPress = false;

// fecha y hora
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -5 * 3600;
const int daylightOffset_sec = 0;

// Definición para la firebas
#define API_KEY "AIzaSyBvLJjVUC9DcJ5xXWC5Evca6tWYkxIPwhs"
#define DATABASE_URL "https://maxesp32-e9124-default-rtdb.firebaseio.com"

FirebaseData fbdo;
FirebaseData userData;
FirebaseAuth auth;
FirebaseConfig config;

// Pnatalla
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET     -1

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Definición del sensor 
MAX30105 sensor;
const auto kSamplingRate = sensor.SAMPLING_RATE_400SPS;
const float kSamplingFrequency = 400.0;

const unsigned long kFingerThreshold = 10000;
const unsigned int kFingerCooldownMs = 500;
const float kEdgeThreshold = -2000.0;

const float kLowPassCutoff = 5.0;
const float kHighPassCutoff = 0.5;
const bool kEnableAveraging = true;
const int kAveragingSamples = 5;
const int kSampleThreshold = 5;

LowPassFilter low_pass_filter_red(kLowPassCutoff, kSamplingFrequency);
LowPassFilter low_pass_filter_ir(kLowPassCutoff, kSamplingFrequency);
HighPassFilter high_pass_filter(kHighPassCutoff, kSamplingFrequency);
Differentiator differentiator(kSamplingFrequency);
MovingAverageFilter<kAveragingSamples> averager_bpm;
MovingAverageFilter<kAveragingSamples> averager_r;
MovingAverageFilter<kAveragingSamples> averager_spo2;
MinMaxAvgStatistic stat_red;
MinMaxAvgStatistic stat_ir;

float kSpO2_A = 1.5958422;
float kSpO2_B = -34.6596622;
float kSpO2_C = 112.6898759;

long last_heartbeat = 0;
long finger_timestamp = 0;
bool finger_detected = false;
float last_diff = NAN;
bool crossed = false;
long crossed_time = 0;
bool display_reset = true;

// Definición de pines para los leds
const int TEMP_LED_RED = 13;
const int TEMP_LED_GREEN = 27;
const int SPO2_LED_RED = 26;
const int SPO2_LED_GREEN = 12;
const int BPM_LED_RED = 14;
const int BPM_LED_GREEN = 15;

volatile int ledBpmState = 0;
volatile int ledSpo2State = 0;
volatile int ledTempState = 0;
// Tarea para el encendido de leds
TaskHandle_t ledTaskHandle = NULL;
//Inicialización de las funciones
void initDrawScreen(void);
void displayMeasuredValues(bool no_finger, int32_t beatAvg, int32_t spo2, float temperature);
String getCurrentTimeKey();
String getCurrentDate();
void actualizarIndicadores(int average_bpm, int average_spo2, float temperature);
void ledControlTask(void *pvParameters);
void guardarLecturas(int average_bpm, int average_spo2, float temperature);

void setup() {
  Serial.begin(115200);

 pinMode(BUTTON_PIN, INPUT_PULLUP);
 

// Detectar si se despertó desde deep sleep
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println(" ESP32 despertado por el botón.");
  }

  // Conexión WiFi desde EEPROM
  intentoconexion("brazalete", "12345678");

  // OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed"));
    while (1);
  }

  // Sensor
  if (sensor.begin() && sensor.setSamplingRate(kSamplingRate)) {
    Serial.println("Sensor initialized");
  } else {
    Serial.println("Sensor not found");
    while (1);
  }

  // Firebase
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  auth.user.email = "jaellanosvelez@gmail.com";
  auth.user.password = "qwerdda236#";
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // Pantalla y hora
  display.clearDisplay();
  initDrawScreen();
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  // LEDs
  pinMode(BPM_LED_GREEN, OUTPUT);
  pinMode(BPM_LED_RED, OUTPUT);
  pinMode(SPO2_LED_GREEN, OUTPUT);
  pinMode(SPO2_LED_RED, OUTPUT);
  pinMode(TEMP_LED_GREEN, OUTPUT);
  pinMode(TEMP_LED_RED, OUTPUT);

  digitalWrite(BPM_LED_GREEN, LOW);
  digitalWrite(BPM_LED_RED, HIGH);
  digitalWrite(SPO2_LED_GREEN, LOW);
  digitalWrite(SPO2_LED_RED, HIGH);
  digitalWrite(TEMP_LED_GREEN, LOW);
  digitalWrite(TEMP_LED_RED, HIGH);

  xTaskCreatePinnedToCore(ledControlTask, "LEDControl", 2000, NULL, 1, &ledTaskHandle, 0);
}

void loop() {
  //ApWifi
  loopAP();

  //lectura del sensor
  auto sample = sensor.readSample(1000);
  float temperature = sensor.readTemperature();
  float current_value_red = sample.red;
  float current_value_ir = sample.ir;

  if (sample.red > kFingerThreshold) {
    if (millis() - finger_timestamp > kFingerCooldownMs) finger_detected = true;
  } else {
    differentiator.reset();
    averager_bpm.reset();
    averager_r.reset();
    averager_spo2.reset();
    low_pass_filter_red.reset();
    low_pass_filter_ir.reset();
    high_pass_filter.reset();
    stat_red.reset();
    stat_ir.reset();
    finger_detected = false;
    finger_timestamp = millis();
  }

  if (finger_detected) {
    displayMeasuredValues(false, 0, 0, temperature);
    current_value_red = low_pass_filter_red.process(current_value_red);
    current_value_ir = low_pass_filter_ir.process(current_value_ir);

    stat_red.process(current_value_red);
    stat_ir.process(current_value_ir);

    float current_value = high_pass_filter.process(current_value_red);
    float current_diff = differentiator.process(current_value);

    if (!isnan(current_diff) && !isnan(last_diff)) {
      if (last_diff > 0 && current_diff < 0) {
        crossed = true;
        crossed_time = millis();
      }
      if (current_diff > 0) crossed = false;

      if (crossed && current_diff < kEdgeThreshold) {
        if (last_heartbeat != 0 && crossed_time - last_heartbeat > 300) {
          int bpm = 60000 / (crossed_time - last_heartbeat);
          float rred = (stat_red.maximum() - stat_red.minimum()) / stat_red.average();
          float rir = (stat_ir.maximum() - stat_ir.minimum()) / stat_ir.average();
          float r = rred / rir;
          float spo2 = kSpO2_A * r * r + kSpO2_B * r + kSpO2_C;

          if (bpm > 50 && bpm < 250) {
            if (kEnableAveraging) {
              int average_bpm = averager_bpm.process(bpm);
              int average_spo2 = averager_spo2.process(spo2);
              if (averager_bpm.count() >= kSampleThreshold) {
                displayMeasuredValues(false, average_bpm, average_spo2, temperature);
                actualizarIndicadores(average_bpm, average_spo2, temperature);
                guardarLecturas(average_bpm, average_spo2, temperature);
              }
            } else {
              displayMeasuredValues(false, bpm, spo2, temperature);
            }
          }
          stat_red.reset();
          stat_ir.reset();
        }
        crossed = false;
        last_heartbeat = crossed_time;
      }
    }
    last_diff = current_diff;
  } else {
    displayMeasuredValues(true, 0, 0, temperature);
  }

 // Lógica del botón de control

bool buttonPressed = digitalRead(BUTTON_PIN) == LOW;

if (buttonPressed) {
  if (pressStart == 0) pressStart = millis();

  if (!longPress && (millis() - pressStart > HOLD_TIME_MS)) {
    longPress = true;
    Serial.println("Pulsación larga detectada → Reiniciando...");
    delay(100);
    ESP.restart();
  }

} else {
  if (pressStart > 0 && !longPress) {
    Serial.println("Pulsación corta → entrando en deep sleep...");
    delay(100);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_PIN, 0);  // GPIO 25 es válido
    esp_deep_sleep_start();
  }

  // Reset de variables
  pressStart = 0;
  longPress = false;
}
}

void initDrawScreen(void) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.println(F("    Taste The Code"));
  display.setCursor(5, display.getCursorY() + 5);
  display.setTextSize(2);
  display.println(F("BPM  SpO2  Temp"));
  display.display();
}

void displayMeasuredValues(bool no_finger, int32_t beatAvg, int32_t spo2, float temperature) {
  display.clearDisplay();
  display.setTextColor(WHITE);

  if (no_finger) {
    display.setTextSize(2);
    display.setCursor(5, 10);
    display.println(F("NO Signal"));
    display.display();
    display_reset = true;
  } else if (beatAvg < 30 && display_reset) {
    display.setTextSize(2);
    display.setCursor(5, 10);
    display.println(F("Pls. Wait"));
    display.display();
    display_reset = false;
  } else if (beatAvg >= 30) {
    display.setTextSize(2);
    display.setCursor(0, 0);
    display.print(F("BPM: "));
    display.println(beatAvg);

    display.setCursor(0, 20);
    display.print(F("SpO2: "));
    if (spo2 >= 20 && spo2 <= 100) display.print(spo2);
    else display.print(F("--"));
    display.println(F("%"));

    display.setCursor(0, 40);
    display.print(F("Tmp: "));
    display.print(temperature, 1);
    display.println(F("C"));

    display.display();
  }
}

String getCurrentTimeKey() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "000000";
  char timeStr[10];
  sprintf(timeStr, "%02d%02d%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  return String(timeStr);
}

String getCurrentDate() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "1970-01-01";
  char dateStr[11];
  sprintf(dateStr, "%04d-%02d-%02d", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
  return String(dateStr);
}

void actualizarIndicadores(int average_bpm, int average_spo2, float temperature) {
  ledBpmState = (average_bpm >= 60 && average_bpm <= 100) ? 1 : 2;
  ledSpo2State = (average_spo2 >= 95 && average_spo2 <= 100) ? 1 : 2;
  ledTempState = (temperature >= 36.1 && temperature <= 37.2) ? 1 : 2;
}

void ledControlTask(void *pvParameters) {
  while (1) {
    digitalWrite(BPM_LED_GREEN, ledBpmState == 1 ? LOW : HIGH);
    digitalWrite(BPM_LED_RED, ledBpmState == 2 ? LOW : HIGH);
    digitalWrite(SPO2_LED_GREEN, ledSpo2State == 1 ? LOW : HIGH);
    digitalWrite(SPO2_LED_RED, ledSpo2State == 2 ? LOW : HIGH);
    digitalWrite(TEMP_LED_GREEN, ledTempState == 1 ? LOW : HIGH);
    digitalWrite(TEMP_LED_RED, ledTempState == 2 ? LOW : HIGH);
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

void guardarLecturas(int average_bpm, int average_spo2, float temperature) {
  // Obtener solo las claves (IDs de usuarios), sin cargar todos los datos
  if (Firebase.RTDB.getShallowData(&userData, "/users")) {
    FirebaseJson& json = userData.jsonObject();
    size_t len = json.iteratorBegin();
    String activeUser = "";

    for (size_t i = 0; i < len; i++) {
      int typeInt;
      String key, dummy;
      json.iteratorGet(i, typeInt, key, dummy);  // obtenemos el nombre de cada usuario

      // Ahora hacemos una consulta más liviana a /active por cada usuario
      if (Firebase.RTDB.getBool(&fbdo, "/users/" + key + "/active") && fbdo.boolData()) {
        activeUser = key;
        break;
      }
    }
    json.iteratorEnd();

    if (activeUser != "") {
      String path = "/users/" + activeUser + "/lecturas/" + getCurrentDate() + "/" + getCurrentTimeKey();
      time_t now = time(nullptr); // timestamp real sincronizado con NTP

      if (!Firebase.RTDB.setInt(&fbdo, path + "/bpm", average_bpm))
        Serial.println("Error BPM: " + fbdo.errorReason());
      if (!Firebase.RTDB.setInt(&fbdo, path + "/spo2", average_spo2))
        Serial.println("Error SpO2: " + fbdo.errorReason());
      if (!Firebase.RTDB.setFloat(&fbdo, path + "/temperatura", temperature))
        Serial.println("Error Temp: " + fbdo.errorReason());
      if (!Firebase.RTDB.setInt(&fbdo, path + "/timestamp", now))
        Serial.println("Error Timestamp: " + fbdo.errorReason());

      Serial.println("Datos guardados para usuario: " + activeUser);
    } else {
      Serial.println("No se encontró ningún usuario activo.");
    }
  } else {
    Serial.println("Error al acceder a /users: " + userData.errorReason());
  }
}
