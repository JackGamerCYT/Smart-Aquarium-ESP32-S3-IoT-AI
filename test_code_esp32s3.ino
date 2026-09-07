/*
 * ==============================================================================
 * PROJ: SMART AQUARIUM IOT + AI - HARDWARE TEST CODE (ESP32-S3)
 * FILE: test_code_esp32s3.ino
 * AUTH: Le Ngoc Hoan, Vo Tran Dang Khoa, Huynh Le Thanh Liem
 * DATE: September 2026
 * DESC: Compile-ready test code for verifying all PCB hardware modules on FR4 board.
 *       Includes: DS18B20 (Temp), ST7789 (TFT LCD SPI), MG90S (Servo),
 *                 Active Buzzer, MOSFET Chiller Gate, RTC DS3231 (I2C) & Push Buttons.
 * ==============================================================================
 */

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <ESP32Servo.h>
#include <RTClib.h>

// --- PINOUT DEFINITIONS MATCHING WIRING DIAGRAM ---
#define PIN_DS18B20       4   // OneWire bus with 4.7k resistor to 3.3V
#define PIN_BUZZER       18   // Active buzzer control through S8050 NPN transistor
#define PIN_MOSFET_GATE  19   // PWM control to Peltier & Fan through PC817 Optocoupler
#define PIN_SERVO        13   // MG90S Micro Servo Signal pin
#define PIN_BTN_FEED     15   // Solder buttons connecting pin to GND (INPUT_PULLUP)
#define PIN_BTN_MUTE     16   // Solder buttons connecting pin to GND (INPUT_PULLUP)

// ST7789 TFT SPI Interface Pins
#define TFT_CS           14   // Chip Select
#define TFT_RST          10   // Reset (Optional, can be tied to 3.3V or RST)
#define TFT_DC            9   // Data/Command Control
#define TFT_MOSI         11   // SPI Master Out Slave In (SDA)
#define TFT_SCLK         12   // SPI Clock (SCL)
#define TFT_BACKLIGHT    21   // LED Backlight Control (High = ON)

// I2C Pins for RTC DS3231
#define I2C_SDA          21   // Dedicated I2C SDA
#define I2C_SCL          22   // Dedicated I2C SCL

// --- OBJECT INSTANTIATIONS ---
OneWire oneWire(PIN_DS18B20);
DallasTemperature sensors(&oneWire);
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
Servo feederServo;
RTC_DS3231 rtc;

// --- CONFIGURATION CONSTANTS ---
const int PWM_CHANNEL_CHILLER = 0;
const int PWM_FREQ = 5000;         // 5kHz frequency for quiet MOSFET switching
const int PWM_RES = 8;             // 8-bit resolution (0-255 duty cycle)

void setup() {
  // 1. Initialize Serial monitor
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n[SYSTEM LOG] Starting Smart Aquarium Hardware Test...");

  // 2. Initialize GPIO directions
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_BTN_FEED, INPUT_PULLUP);
  pinMode(PIN_BTN_MUTE, INPUT_PULLUP);
  pinMode(TFT_BACKLIGHT, OUTPUT);
  digitalWrite(TFT_BACKLIGHT, HIGH); // Turn on screen backlight

  digitalWrite(PIN_BUZZER, LOW);     // Ensure buzzer is quiet initially

  // 3. Initialize I2C for RTC DS3231
  Wire.begin(I2C_SDA, I2C_SCL);
  if (!rtc.begin()) {
    Serial.println("[-] Warning: Couldn't find RTC DS3231 module!");
  } else {
    Serial.println("[+] Success: RTC DS3231 detected.");
    if (rtc.lostPower()) {
      Serial.println("[!] Warning: RTC lost power, setting time to compile date.");
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__))); // Sync with compile time
    }
  }

  // 4. Initialize OneWire DS18B20 temperature sensor
  sensors.begin();
  Serial.println("[+] Dallas Temperature DS18B20 initialized.");

  // 5. Initialize MG90S Servo
  ESP32PWM::allocateTimer(0);
  feederServo.setPeriodHertz(50); // Standard 50Hz servo signal
  feederServo.attach(PIN_SERVO, 500, 2400); // Attach with min/max pulse width
  feederServo.write(0);          // Return to home position (0 degrees)
  Serial.println("[+] MG90S Micro Servo initialized and set to 0 deg.");

  // 6. Initialize Chiller Control (PWM on MOSFET Gate)
  ledcSetup(PWM_CHANNEL_CHILLER, PWM_FREQ, PWM_RES);
  ledcAttachPin(PIN_MOSFET_GATE, PWM_CHANNEL_CHILLER);
  ledcWrite(PWM_CHANNEL_CHILLER, 0); // Start with Chiller off
  Serial.println("[+] MOSFET Peltier PWM initialized.");

  // 7. Initialize LCD TFT ST7789 1.54" SPI Screen
  // Since we use custom pins on ESP32-S3, we configure SPI hardware mapping:
  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS); // SCLK, MISO (-1), MOSI, CS
  tft.init(240, 240); // Standard resolution 240x240 pixels
  tft.setRotation(1); // Adjust view orientation
  tft.fillScreen(ST77XX_BLACK);
  
  // Show beautiful boot message
  tft.setTextColor(ST77XX_GREEN);
  tft.setTextSize(2);
  tft.setCursor(10, 30);
  tft.println("SMART AQUARIUM");
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(10, 70);
  tft.println("Team: Hoan - Khoa - Liem");
  tft.setCursor(10, 90);
  tft.println("Supervisor: MEng. H. H. Ha");
  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(10, 130);
  tft.println("[+] Hardware Test Mode");
  
  // Alert buzzer short beep to notify setup complete
  digitalWrite(PIN_BUZZER, HIGH);
  delay(150);
  digitalWrite(PIN_BUZZER, LOW);
  
  delay(2000); // Pause to let users read boot screen
}

void loop() {
  // --- TEST BLOCK 1: READ DS18B20 TEMPERATURE SENSOR ---
  sensors.requestTemperatures();
  float waterTemp = sensors.getTempCByIndex(0);
  
  // Check for error values (-127 or 85)
  bool isTempError = (waterTemp == -127.00 || waterTemp == 85.00);
  
  // --- TEST BLOCK 2: READ RTC DATE & TIME ---
  DateTime now = rtc.now();
  
  // --- TEST BLOCK 3: CONTROL CHILLER PELTIER BY HYSTERESIS ---
  int pcmDutyValue = 0;
  String chillerStatus = "OFF";
  
  if (!isTempError) {
    if (waterTemp >= 27.0) {
      pcmDutyValue = 255; // 100% cooling duty cycle
      chillerStatus = "COOLING (100%)";
    } else if (waterTemp <= 25.5) {
      pcmDutyValue = 0;   // Turn off Peltier cooling
      chillerStatus = "OFF";
    } else {
      // Maintaining current status to prevent rapid oscillations (Hysteresis)
      pcmDutyValue = (pcmDutyValue == 255) ? 255 : 0;
      chillerStatus = (pcmDutyValue == 255) ? "COOLING" : "OFF";
    }
  } else {
    pcmDutyValue = 0; // Emergency off if sensor unplugged
    chillerStatus = "ERR_SENSOR";
  }
  
  ledcWrite(PWM_CHANNEL_CHILLER, pcmDutyValue);

  // --- TEST BLOCK 4: CHECK PUSH BUTTONS & EXECUTE ACTIONS ---
  
  // Button 1: MANUAL FEED TRIGGER
  if (digitalRead(PIN_BTN_FEED) == LOW) {
    Serial.println("[!] Manual feeding button pressed!");
    tft.fillScreen(ST77XX_BLACK);
    tft.setTextColor(ST77XX_MAGENTA);
    tft.setTextSize(2);
    tft.setCursor(20, 100);
    tft.println("FEEDING...");
    
    // Buzzer alert
    digitalWrite(PIN_BUZZER, HIGH);
    delay(100);
    digitalWrite(PIN_BUZZER, LOW);
    
    // Sweep servo to drop food
    feederServo.write(90);  // Rotate to dispenser opening
    delay(1000);            // Wait 1 second
    feederServo.write(0);   // Return home
    delay(200);             // Debounce pause
  }

  // Button 2: MUTE / ALARM TEST
  if (digitalRead(PIN_BTN_MUTE) == LOW) {
    Serial.println("[!] Alarm test button pressed!");
    tft.fillScreen(ST77XX_BLACK);
    tft.setTextColor(ST77XX_RED);
    tft.setTextSize(2);
    tft.setCursor(20, 100);
    tft.println("ALARM TEST");
    
    // Cycle buzzer to test audio output
    for (int i = 0; i < 3; i++) {
      digitalWrite(PIN_BUZZER, HIGH);
      delay(150);
      digitalWrite(PIN_BUZZER, LOW);
      delay(100);
    }
  }

  // --- TEST BLOCK 5: UPDATE LCD TFT ST7789 SCREEN ---
  tft.fillScreen(ST77XX_BLACK);
  
  // Header Info
  tft.setTextColor(ST77XX_GREEN);
  tft.setTextSize(2);
  tft.setCursor(10, 15);
  tft.print("AQUARIUM IoT");
  
  tft.drawFastHLine(0, 40, 240, ST77XX_WHITE);

  // Print Date & Time
  tft.setTextColor(ST77XX_CYAN);
  tft.setTextSize(1);
  tft.setCursor(10, 50);
  char timeStr[32];
  sprintf(timeStr, "Time: %02d/%02d/%04d %02d:%02d:%02d", now.day(), now.month(), now.year(), now.hour(), now.minute(), now.second());
  tft.println(timeStr);

  // Print Water Temperature
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(10, 80);
  tft.setTextSize(2);
  tft.print("Temp: ");
  if (isTempError) {
    tft.setTextColor(ST77XX_RED);
    tft.print("ERROR");
  } else {
    if (waterTemp >= 29.0) tft.setTextColor(ST77XX_RED); // Critical heat
    else if (waterTemp <= 24.0) tft.setTextColor(ST77XX_BLUE); // Critical cold
    else tft.setTextColor(ST77XX_YELLOW); // Safe range
    tft.print(waterTemp, 1);
    tft.print(" C");
  }

  // Print Chiller Status
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(10, 130);
  tft.print("Chiller: ");
  if (pcmDutyValue > 0) {
    tft.setTextColor(ST77XX_BLUE);
  } else {
    tft.setTextColor(ST77XX_WHITE);
  }
  tft.print(chillerStatus);

  // Print Input Button IO states (For debugging)
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(10, 170);
  tft.print("Btn Feed: ");
  tft.print(digitalRead(PIN_BTN_FEED) == LOW ? "ACTIVE" : "HIGH");
  tft.setCursor(10, 190);
  tft.print("Btn Mute: ");
  tft.print(digitalRead(PIN_BTN_MUTE) == LOW ? "ACTIVE" : "HIGH");

  tft.setCursor(10, 220);
  tft.setTextColor(ST77XX_GREEN);
  tft.print("[RUNNING OK]");

  // Update serial telemetry
  Serial.print("[TELEMETRY] Temp: ");
  Serial.print(waterTemp);
  Serial.print("C | Chiller: ");
  Serial.print(chillerStatus);
  Serial.print(" | Time: ");
  Serial.println(timeStr);

  delay(1000); // 1-second update cycle for readability
}
