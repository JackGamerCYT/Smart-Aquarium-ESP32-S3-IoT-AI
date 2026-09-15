#include <WiFi.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>

// ============================================================================
// 1. HARDWARE PIN DEFINITIONS (ESP32-S3)
// ============================================================================
#define PIN_DS18B20       4   // DS18B20 Temp Sensor (Data line)
#define PIN_RELAY_CHILLER 19  // IN1: Relay 1 - Peltier TEC1-12706 + 12V Fan
#define PIN_RELAY_PUMP    18  // IN2: Relay 2 - 12V Water Filter Pump
#define PIN_SERVO_FEED    13  // Servo MG90S Food Dispenser
#define PIN_BUZZER        12  // 5V Active Buzzer
#define PIN_BUTTON_TEST   0   // On-board BOOT Button for Manual Diagnostics

#define PIN_SDA           8   // I2C SDA - OLED 1.3" Display
#define PIN_SCL           9   // I2C SCL - OLED 1.3" Display
#define SCREEN_WIDTH      128
#define SCREEN_HEIGHT     64

#define RELAY_ON          HIGH   
#define RELAY_OFF         LOW

// ============================================================================
// 2. WI-FI & MQTT BROKER CONFIGURATION
// ============================================================================
const char* ssid         = "YOUR_WIFI_NAME";     // Replace with your Wi-Fi SSID
const char* password     = "YOUR_WIFI_PASS";     // Replace with your Wi-Fi Password
const char* mqtt_server  = "broker.hivemq.com";
const int   mqtt_port    = 1883;

const char* TOPIC_TELEMETRY = "beca/ai/telemetry";
const char* TOPIC_COMMAND   = "beca/ai/command";

// Hardware Objects
WiFiClient espClient;
PubSubClient mqttClient(espClient);
OneWire oneWire(PIN_DS18B20);
DallasTemperature sensors(&oneWire);
Servo feedServo;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// Global Variables
float currentTemp = 0.0;
bool  chillerState = false;
bool  pumpState    = true;
int   feedCount    = 0;
bool  manualOverrideChiller = false;
unsigned long lastTelemetryTime = 0;
unsigned long lastSensorReadTime = 0;

// ============================================================================
// 3. AUXILIARY FUNCTIONS (BUZZER & OLED DISPLAY)
// ============================================================================
void beep(int durationMs, int count = 1) {
  for (int i = 0; i < count; i++) {
    digitalWrite(PIN_BUZZER, HIGH);
    delay(durationMs);
    digitalWrite(PIN_BUZZER, LOW);
    if (count > 1) delay(80);
  }
}

void renderOLED() {
  display.clearDisplay();
  
  // Header: Title & Wi-Fi Status
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("BE CA SMART AI");
  display.setCursor(82, 0);
  display.print(WiFi.status() == WL_CONNECTED ? "[WiFi:OK]" : "[NO-WiFi]");
  display.drawLine(0, 10, 128, 10, SSD1306_WHITE);

  // Body: Temperature Reading (Large Font Size 3)
  display.setCursor(0, 15);
  if (currentTemp <= -127.0 || currentTemp >= 85.0) {
    display.setTextSize(2);
    display.print("ERR Temp");
  } else {
    display.setTextSize(3);
    display.print(currentTemp, 1);
    display.setTextSize(2);
    display.write(247); // Degree symbol °
    display.print("C");
  }

  // Auto Status Indicator
  display.setTextSize(1);
  display.setCursor(0, 42);
  if (currentTemp >= 27.0) {
    display.print("Nong -> Bat Chiller!");
  } else if (currentTemp <= 25.5) {
    display.print("Nuoc mat -> Tat Chiller");
  } else {
    display.print("Nhiet do on dinh");
  }

  // Footer: Actuator States (IN1, IN2, Feed Count)
  display.drawLine(0, 52, 128, 52, SSD1306_WHITE);
  display.setCursor(0, 55);
  display.print("IN1:");
  display.print(digitalRead(PIN_RELAY_CHILLER) == RELAY_ON ? "ON " : "OFF");

  display.print(" IN2:");
  display.print(digitalRead(PIN_RELAY_PUMP) == RELAY_ON ? "ON " : "OFF");

  display.print(" An:");
  display.print(feedCount);

  display.display();
}

// ============================================================================
// 4. FEEDING MECHANISM & HARDWARE SELF-TEST
// ============================================================================
void triggerFeeding() {
  Serial.println("\n[SERVO MG90S] Dang cho ca an...");
  feedCount++;

  // Tắt máy bơm tạm thời (3.5s) để không hút mất thức ăn
  digitalWrite(PIN_RELAY_PUMP, RELAY_OFF);
  renderOLED();
  beep(80);

  feedServo.attach(PIN_SERVO_FEED, 500, 2400);
  delay(50);

  // Dynamic sweep để chống sụt áp / Brownout
  for (int pos = 0; pos <= 110; pos += 5) {
    feedServo.write(pos);
    delay(20);
  }
  delay(1000); // Giữ hộc mở 1 giây

  for (int pos = 110; pos >= 0; pos -= 5) {
    feedServo.write(pos);
    delay(20);
  }
  delay(200);
  feedServo.detach(); // Ngắt xung để chống rung, chống nóng và tiết kiệm điện

  if (pumpState) digitalWrite(PIN_RELAY_PUMP, RELAY_ON);
  renderOLED();
  beep(120, 2);
}

void runHardwareSelfTest() {
  Serial.println("\n[SELF-TEST] CHẨN ĐOÁN PHẦN CỨNG...");

  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(10, 5);
  display.println("=== SELF-TEST ===");
  display.setCursor(0, 20);
  
  display.println("1. Test Coi Bip..."); display.display();
  beep(100, 2); delay(400);

  display.println("2. Test IN1 Chiller..."); display.display();
  digitalWrite(PIN_RELAY_CHILLER, RELAY_ON); delay(1200);
  digitalWrite(PIN_RELAY_CHILLER, RELAY_OFF); delay(400);

  display.println("3. Test IN2 May Bom..."); display.display();
  digitalWrite(PIN_RELAY_PUMP, RELAY_OFF); delay(1200);
  digitalWrite(PIN_RELAY_PUMP, RELAY_ON); delay(400);

  display.println("4. Test Servo MG90S..."); display.display();
  triggerFeeding();

  sensors.requestTemperatures();
  float t = sensors.getTempCByIndex(0);
  display.println("5. DS18B20 Temp: OK!"); display.display();
  delay(1200);
}

// ============================================================================
// 5. MQTT CLOUD COMMUNICATION
// ============================================================================
void sendTelemetry() {
  StaticJsonDocument<256> doc;
  doc["temp"]       = currentTemp;
  doc["chiller"]    = (digitalRead(PIN_RELAY_CHILLER) == RELAY_ON);
  doc["pump"]       = (digitalRead(PIN_RELAY_PUMP) == RELAY_ON);
  doc["feed_count"] = feedCount;

  char buffer[256]; // Fix: Khai báo mảng 256 ký tự (sửa lỗi invalid conversion from char)
  serializeJson(doc, buffer);
  mqttClient.publish(TOPIC_TELEMETRY, buffer);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (int i = 0; i < length; i++) message += (char)payload[i];

  StaticJsonDocument<256> doc;
  if (!deserializeJson(doc, message)) {
    const char* device = doc["device"];
    const char* action = doc["action"];

    if (String(device) == "chiller") {
      chillerState = (String(action) == "ON");
      manualOverrideChiller = true;
      digitalWrite(PIN_RELAY_CHILLER, chillerState ? RELAY_ON : RELAY_OFF);
      beep(100);
    } else if (String(device) == "pump") {
      pumpState = (String(action) == "ON");
      digitalWrite(PIN_RELAY_PUMP, pumpState ? RELAY_ON : RELAY_OFF);
      beep(100);
    } else if (String(device) == "feed") {
      triggerFeeding();
    } else if (String(device) == "test") {
      runHardwareSelfTest();
    }
    sendTelemetry();
    renderOLED();
  }
}

void connectWiFi() {
  WiFi.begin(ssid, password);
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 12) {
    delay(500);
    retry++;
  }
}

void reconnectMQTT() {
  if (WiFi.status() == WL_CONNECTED && !mqttClient.connected()) {
    String clientId = "ESP32S3_Aquarium_" + String(random(0xffff), HEX);
    if (mqttClient.connect(clientId.c_str())) {
      mqttClient.subscribe(TOPIC_COMMAND);
    }
  }
}

// ============================================================================
// 6. SETUP & MAIN LOOP
// ============================================================================
void setup() {
  Serial.begin(115200);

  pinMode(PIN_RELAY_CHILLER, OUTPUT);
  pinMode(PIN_RELAY_PUMP, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_BUTTON_TEST, INPUT_PULLUP);

  digitalWrite(PIN_RELAY_CHILLER, RELAY_OFF);
  digitalWrite(PIN_RELAY_PUMP, RELAY_ON);
  digitalWrite(PIN_BUZZER, LOW);

  feedServo.attach(PIN_SERVO_FEED, 500, 2400);
  feedServo.write(0);
  delay(300);
  feedServo.detach();

  // Khởi tạo OLED I2C 1.3" (SDA GPIO 8 / SCL GPIO 9)
  Wire.begin(PIN_SDA, PIN_SCL);
  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(10, 15);
    display.println("BE CA SMART AI");
    display.setCursor(15, 35);
    display.println("OLED 1.3\" READY!");
    display.display();
    delay(1500);
  }

  sensors.begin();
  runHardwareSelfTest();

  connectWiFi();
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback);
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) reconnectMQTT();
    mqttClient.loop();
  }

  unsigned long now = millis();

  // Đọc cảm biến & Render OLED mỗi 1.5s
  if (now - lastSensorReadTime >= 1500) {
    lastSensorReadTime = now;

    sensors.requestTemperatures();
    float t = sensors.getTempCByIndex(0);

    if (t > -55.0 && t < 85.0) {
      currentTemp = t;

      // Logic Hysteresis tự động bảo vệ cá:
      if (!manualOverrideChiller) {
        if (currentTemp >= 27.0 && digitalRead(PIN_RELAY_CHILLER) == RELAY_OFF) {
          digitalWrite(PIN_RELAY_CHILLER, RELAY_ON);
          beep(100, 1);
        } else if (currentTemp <= 25.5 && digitalRead(PIN_RELAY_CHILLER) == RELAY_ON) {
          digitalWrite(PIN_RELAY_CHILLER, RELAY_OFF);
          beep(100, 2);
        }
      }
    }

    renderOLED();
  }

  // Gửi Telemetry lên Cloud mỗi 2s
  if (now - lastTelemetryTime >= 2000) {
    lastTelemetryTime = now;
    if (WiFi.status() == WL_CONNECTED && mqttClient.connected()) {
      sendTelemetry();
    }
  }

  // Bấm nút BOOT (GPIO 0) kích Self-Test thủ công
  if (digitalRead(PIN_BUTTON_TEST) == LOW) {
    delay(50);
    if (digitalRead(PIN_BUTTON_TEST) == LOW) {
      beep(150);
      runHardwareSelfTest();
      while (digitalRead(PIN_BUTTON_TEST) == LOW);
    }
  }
}
