/*
 * Alcohol Detection and Motor Locking System
 * ============================================
 * Platform  : ESP32 (Espressif)
 * IDE       : Arduino IDE 2.x
 * Author    : Rosmia Jose
 * Date      : December 2023
 *
 * Description:
 *   Continuously monitors alcohol concentration via MQ-3 sensor.
 *   Three operating states:
 *     1. SOBER     — full motor speed, no alert
 *     2. WARNING   — reduced speed, buzzer/LED on, GSM SMS alert sent once
 *     3. DRUNK     — motor halted, buzzer/LED on, GSM DRUNK alert sent once
 *
 * Hardware:
 *   - ESP32 Dev Module
 *   - MQ-3 Alcohol Sensor  → GPIO 14 (analog)
 *   - L298N Motor Driver   → Motor A: GPIO 26,27 (EN: GPIO 4)
 *                            Motor B: GPIO 15,19 (EN: GPIO 5)
 *   - A7670C 4G GSM Module → UART2 TX:GPIO17 / RX:GPIO16
 *   - 16x2 I2C LCD Display → I2C address 0x27
 *   - Buzzer               → GPIO 23
 *   - LED                  → GPIO 22
 */

#include <HardwareSerial.h>
#include <LiquidCrystal_I2C.h>

// ── Alcohol thresholds (raw ADC, 0–4095 on ESP32 12-bit ADC) ─────────────────
#define SOBER_THRESHOLD  600    // Below this: sober, full speed
#define DRUNK_THRESHOLD  1000   // Above this: drunk, motor lock

// ── Sensor pin ────────────────────────────────────────────────────────────────
#define MQ3_PIN          14     // Analog input pin for MQ-3 sensor

// ── Alert recipient — REPLACE before flashing ────────────────────────────────
#define RECIPIENT_NUMBER "YOUR_PHONE_NUMBER"  // e.g. "+919XXXXXXXXX"

// ── Peripheral pins ───────────────────────────────────────────────────────────
#define BUZZER_PIN       23
#define LED_PIN          22

// ── Motor A (L298N outputs 1 & 2) ────────────────────────────────────────────
#define MOTOR_A_IN1      26
#define MOTOR_A_IN2      27
#define MOTOR_A_EN       4     // PWM-capable pin for speed control

// ── Motor B (L298N outputs 3 & 4) ────────────────────────────────────────────
#define MOTOR_B_IN3      15
#define MOTOR_B_IN4      19
#define MOTOR_B_EN       5     // PWM-capable pin for speed control

// ── GSM serial (UART2) ───────────────────────────────────────────────────────
HardwareSerial gsmSerial(2);   // RX=16, TX=17

// ── LCD (I2C, 16 columns × 2 rows) ──────────────────────────────────────────
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ── State tracking ───────────────────────────────────────────────────────────
float sensorValue    = 0;

// Tracks which alert state an SMS was last sent for.
// Prevents repeated SMS on every loop iteration while state persists.
// Resets to 0 (SOBER) when driver returns to sober — re-arms for next event.
//   0 = no alert sent (sober)
//   1 = WARNING SMS sent
//   2 = DRUNK SMS sent
int lastAlertState = 0;


// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations
// ─────────────────────────────────────────────────────────────────────────────
void moveForward(int speed);
void stopMotors();
void sendSMS(const char* message);


// ─────────────────────────────────────────────────────────────────────────────
// setup()
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  // Motor driver pins
  pinMode(MOTOR_A_IN1, OUTPUT);
  pinMode(MOTOR_A_IN2, OUTPUT);
  pinMode(MOTOR_A_EN,  OUTPUT);
  pinMode(MOTOR_B_IN3, OUTPUT);
  pinMode(MOTOR_B_IN4, OUTPUT);
  pinMode(MOTOR_B_EN,  OUTPUT);

  // Alert peripherals
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN,    OUTPUT);

  // LCD initialisation
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Initialising...");

  // GSM module initialisation (A7670C, 115200 baud, UART2)
  gsmSerial.begin(115200, SERIAL_8N1, 16, 17);
  delay(2000);
  gsmSerial.println("AT");    // Verify modem is alive
  delay(500);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("System Ready");
  delay(1000);
}


// ─────────────────────────────────────────────────────────────────────────────
// loop()
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  sensorValue = analogRead(MQ3_PIN);

  // Update LCD with live reading
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Sensor Value:");
  lcd.setCursor(0, 1);
  lcd.print(sensorValue);

  Serial.print("Sensor Value: ");
  Serial.print(sensorValue);

  if (sensorValue < SOBER_THRESHOLD) {
    // ── State 1: SOBER ───────────────────────────────────────────────────────
    Serial.println(" | Status: Sober");
    digitalWrite(BUZZER_PIN, LOW);
    digitalWrite(LED_PIN,    LOW);
    moveForward(255);           // Full speed

    // Reset alert state — next WARNING or DRUNK event will re-trigger SMS
    lastAlertState = 0;

  } else if (sensorValue >= SOBER_THRESHOLD && sensorValue < DRUNK_THRESHOLD) {
    // ── State 2: WARNING ─────────────────────────────────────────────────────
    Serial.println(" | Status: Drinking but within legal limits");
    moveForward(127);           // Reduce to ~50% speed
    digitalWrite(BUZZER_PIN, HIGH);
    digitalWrite(LED_PIN,    HIGH);

    // Send SMS only once per WARNING event (not every second)
    if (lastAlertState != 1) {
      sendSMS("ALERT!! Drinking suspected within legal limits!!");
      lastAlertState = 1;
    }

  } else {
    // ── State 3: DRUNK ───────────────────────────────────────────────────────
    Serial.println(" | Status: DRUNK");
    digitalWrite(BUZZER_PIN, HIGH);
    digitalWrite(LED_PIN,    HIGH);
    moveForward(50);            // Brief ramp-down before full stop
    delay(500);
    stopMotors();               // Lock motors

    // Send SMS only once per DRUNK event (not every second)
    if (lastAlertState != 2) {
      sendSMS("DRUNK AND DRIVE!!");
      lastAlertState = 2;
    }
  }

  delay(1000);  // 1-second polling interval
}


// ─────────────────────────────────────────────────────────────────────────────
// moveForward(speed)
//   speed : 0–255 PWM duty cycle
// ─────────────────────────────────────────────────────────────────────────────
void moveForward(int speed) {
  analogWrite(MOTOR_A_EN, speed);
  digitalWrite(MOTOR_A_IN1, HIGH);
  digitalWrite(MOTOR_A_IN2, LOW);

  analogWrite(MOTOR_B_EN, speed);
  digitalWrite(MOTOR_B_IN3, HIGH);
  digitalWrite(MOTOR_B_IN4, LOW);
}


// ─────────────────────────────────────────────────────────────────────────────
// stopMotors()
//   Cuts PWM to both motor channels — vehicle locks.
// ─────────────────────────────────────────────────────────────────────────────
void stopMotors() {
  analogWrite(MOTOR_A_EN, 0);
  analogWrite(MOTOR_B_EN, 0);
  digitalWrite(MOTOR_A_IN1, LOW);
  digitalWrite(MOTOR_A_IN2, LOW);
  digitalWrite(MOTOR_B_IN3, LOW);
  digitalWrite(MOTOR_B_IN4, LOW);
}


// ─────────────────────────────────────────────────────────────────────────────
// sendSMS(message)
//   Sends a single SMS via AT commands to RECIPIENT_NUMBER.
//   A7670C uses standard GSM 07.07 AT command set.
// ─────────────────────────────────────────────────────────────────────────────
void sendSMS(const char* message) {
  gsmSerial.println("AT+CMGF=1");        // Set SMS to text mode
  delay(1000);
  gsmSerial.print("AT+CMGS=\"");
  gsmSerial.print(RECIPIENT_NUMBER);
  gsmSerial.println("\"");
  delay(1000);
  gsmSerial.println(message);            // SMS body
  delay(1000);
  gsmSerial.println((char)26);           // Ctrl+Z → trigger send
  delay(1000);

  // Drain GSM response from buffer to Serial monitor
  while (gsmSerial.available()) {
    Serial.write(gsmSerial.read());
  }
}