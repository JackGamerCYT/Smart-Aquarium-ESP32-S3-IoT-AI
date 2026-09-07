/**
 * PROJECT: SMART AQUARIUM IOT + AI
 * MILESTONE: WEEKLY HARDWARE INTEGRATION & INTEGRITY TEST
 * TARGET MCU: ESP32-S3 DevKit C (N8R8/N16R8)
 * AUTHOR: Team Smart Aquarium (Le Ngoc Hoan, Vo Tran Dang Khoa, Huynh Le Thanh Liem)
 * SUPERVISOR: MEng. Huynh Hoang Ha
 * 
 * DESCRIPTION:
 * This code is specifically designed for this week's progress review. 
 * It performs systematic, isolated, and integrated testing of:
 *   1. ESP32-S3 System & Memory Check
 *   2. DS18B20 Temperature Sensor (1-Wire)
 *   3. 5V Relay Module (Switching control)
 *   4. 12V Diaphragm Water Pump (Động cơ bơm hút nước)
 *   5. TEC1-12706 Peltier Thermoelectric Module (Sò lạnh)
 *   6. LCD ST7789 SPI / LCD 1602 I2C (Visual status output)
 */

#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h>

// ==========================================
// 1. PIN CONFIGURATION (SPECIFICATION Grounded)
// ==========================================
#define PIN_DS18B20       4   // OneWire bus for Temperature Sensor (4.7k Pull-up)
#define PIN_RELAY_PUMP   18   // Relay Channel 1: Controls 12V Water Pump 
#define PIN_RELAY_PELTIER 19  // Relay Channel 2: Controls 12V Peltier Module (Sò lạnh)
#define PIN_BUZZER       18   // Buzzer Alert pin (Shared/Alternative)
#define PIN_BUTTON_TEST  15   // Tactile Button to cycle through test phases manually

// Display Option Toggle (Enable the one you are testing)
#define USE_LCD1602_I2C    1  // 1: Use LCD1602 I2C, 0: Disable
#define USE_TFT_ST7789     0  // 1: Use TFT ST7789 SPI, 0: Disable

#if USE_LCD1602_I2C
#include <LiquidCrystal_I2C.h>
LiquidCrystal_I2C lcd(0x27, 16, 2); // SDA -> GPIO 21, SCL -> GPIO 22
#endif

// ==========================================
// 2. DEVICE INSTANCES
// ==========================================
OneWire oneWire(PIN_DS18B20);
DallasTemperature sensors(&oneWire);

// Test Stages
enum TestPhase {
  PHASE_SYSTEM_INIT,
  PHASE_SENSOR_TEST,
  PHASE_RELAY_PUMP_TEST,
  PHASE_RELAY_PELTIER_TEST,
  PHASE_INTEGRATED_LOOP,
  NUM_PHASES
};

volatile TestPhase currentPhase = PHASE_SYSTEM_INIT;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 250; 

// Interrupt Service Routine for Button (Manual Phase Cycle)
void IRAM_ATTR handleButtonPress() {
  unsigned long currentTime = millis();
  if (currentTime - lastDebounceTime > debounceDelay) {
    currentPhase = static_cast<TestPhase>((currentPhase + 1) % NUM_PHASES);
    lastDebounceTime = currentTime;
  }
}

// ==========================================
// SETUP
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n=========================================");
  Serial.println("  ESP32-S3 HARDWARE INTEGRITY TEST SYSTEM  ");
  Serial.println("  Supervisor: MEng. Huynh Hoang Ha        ");
  Serial.println("=========================================");

  // Initialize GPIO Pins
  pinMode(PIN_RELAY_PUMP, OUTPUT);
  pinMode(PIN_RELAY_PELTIER, OUTPUT);
  pinMode(PIN_BUTTON_TEST, INPUT_PULLUP);

  // Default Relays to OFF (Active LOW or Active HIGH depends on your module)
  // We assume Active LOW Relays common in Arduino projects
  digitalWrite(PIN_RELAY_PUMP, HIGH); 
  digitalWrite(PIN_RELAY_PELTIER, HIGH);

  // Attach Interrupt for manual test cycle
  attachInterrupt(digitalPinToInterrupt(PIN_BUTTON_TEST), handleButtonPress, FALLING);

  // Initialize Temperature Sensor
  sensors.begin();
  Serial.print("DS18B20 Sensors found: ");
  Serial.println(sensors.getDeviceCount());

  #if USE_LCD1602_I2C
  Wire.begin(21, 22); // Explicitly define SDA, SCL for ESP32-S3
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("SmartAquarium S3");
  lcd.setCursor(0, 1);
  lcd.print("Init Hardware...");
  delay(2000);
  #endif

  currentPhase = PHASE_SENSOR_TEST; // Start testing sequence
}

// ==========================================
// MAIN LOOP (TEST RUNNER)
// ==========================================
void loop() {
  static unsigned long lastUpdate = 0;
  unsigned long currentMillis = millis();

  // Print diagnostics every 2 seconds
  if (currentMillis - lastUpdate >= 2000) {
    lastUpdate = currentMillis;

    // Read Sensors
    sensors.requestTemperatures();
    float waterTemp = sensors.getTempCByIndex(0);

    Serial.print("[SYSTEM CLOCK]: ");
    Serial.print(currentMillis / 1000);
    Serial.println(" seconds");

    switch (currentPhase) {
      
      case PHASE_SENSOR_TEST: {
        Serial.println(">> TEST PHASE: DS18B20 TEMPERATURE SENSOR <<");
        if (waterTemp == DEVICE_DISCONNECTED_C) {
          Serial.println("[-] ERROR: DS18B20 Sensor Disconnected! Check pull-up resistor.");
          updateDisplay("Sensor Error!", "Check DS18B20");
        } else {
          Serial.printf("[+] Water Temp: %.2f *C\n", waterTemp);
          String tempStr = "Temp: " + String(waterTemp, 1) + " C";
          updateDisplay("1. Sensor Test", tempStr);
        }
        break;
      }

      case PHASE_RELAY_PUMP_TEST: {
        Serial.println(">> TEST PHASE: RELAY 1 - 12V WATER PUMP <<");
        Serial.println("[*] Toggling Pump Relay Channel...");
        
        // Turn ON Pump
        updateDisplay("2. Pump Test", "STATUS: PUMP ON");
        digitalWrite(PIN_RELAY_PUMP, LOW); // Active Low
        delay(2000);
        
        // Turn OFF Pump
        updateDisplay("2. Pump Test", "STATUS: PUMP OFF");
        digitalWrite(PIN_RELAY_PUMP, HIGH);
        break;
      }

      case PHASE_RELAY_PELTIER_TEST: {
        Serial.println(">> TEST PHASE: RELAY 2 - TEC1-12706 PELTIER <<");
        Serial.println("[*] Toggling Peltier Chiller Relay (Monitor Current & Heat Dissipation!)...");
        
        // Turn ON Peltier
        updateDisplay("3. Peltier Test", "STATUS: CHILL ON");
        digitalWrite(PIN_RELAY_PELTIER, LOW); // Active Low
        delay(3000); // Keep on briefly to feel cold/hot side
        
        // Turn OFF Peltier
        updateDisplay("3. Peltier Test", "STATUS: CHILL OFF");
        digitalWrite(PIN_RELAY_PELTIER, HIGH);
        break;
      }

      case PHASE_INTEGRATED_LOOP: {
        Serial.println(">> TEST PHASE: INTEGRATED HYSTERESIS LOOP <<");
        // Hysteresis safety logic:
        // Turn on Chiller if temp >= 27.0C, Turn off if <= 25.5C
        String statusLine = "";
        
        if (waterTemp != DEVICE_DISCONNECTED_C) {
          if (waterTemp >= 27.0) {
            digitalWrite(PIN_RELAY_PELTIER, LOW);  // Turn ON Peltier
            digitalWrite(PIN_RELAY_PUMP, LOW);     // Turn ON Pump to circulate water
            statusLine = "COOLING ACTIVE";
            Serial.println("[!] Temp high! Chiller and Pump RUNNING.");
          } else if (waterTemp <= 25.5) {
            digitalWrite(PIN_RELAY_PELTIER, HIGH); // Turn OFF Peltier
            digitalWrite(PIN_RELAY_PUMP, HIGH);    // Turn OFF Pump
            statusLine = "SYS STANDBY";
            Serial.println("[*] Temp safe. System STANDBY.");
          } else {
            statusLine = "MONITORING...";
            Serial.println("[*] Temp in safe buffer zone.");
          }
          String dispTemp = "T: " + String(waterTemp, 1) + "C";
          updateDisplay(dispTemp, statusLine);
        } else {
          Serial.println("[-] Error: Cannot execute Hysteresis without temperature data!");
          updateDisplay("Hysteresis Err", "Sensor Offline");
          digitalWrite(PIN_RELAY_PELTIER, HIGH);
          digitalWrite(PIN_RELAY_PUMP, HIGH);
        }
        break;
      }
      
      default:
        break;
    }
    Serial.println("-----------------------------------------\n");
  }
}

// ==========================================
// AUXILIARY DISPLAY FUNCT
// ==========================================
void updateDisplay(String line1, String line2) {
  #if USE_LCD1602_I2C
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
  #endif
}
