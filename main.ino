#include <SPI.h>
#include <MFRC522.h>
#include <LiquidCrystal.h>
#include <EEPROM.h>
#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>

#define SS_PIN 10
#define RST_PIN 9

const int rs = 8, en = 7, d4 = 5, d5 = 4, d6 = 3, d7 = 2;
LiquidCrystal lcd(rs, en, d4, d5, d6, d7);

#define ASM_LED_PIN A0

MFRC522 mfrc522(SS_PIN, RST_PIN);
Adafruit_PWMServoDriver pwm = Adafruit_PWMServoDriver();

#define SERVOMIN 150
#define SERVOMAX 600

#define SERVO_CANAL_DOC 0
#define SERVO_CANAL_ENF 1
#define SERVO_CANAL_PAC 2

struct LogEntry {
  byte rol;
};
const int EEPROM_LOG_START = 100;
const int MAX_LOGS = 50;
int currentLogIndex = 0;

void setup() {
  SPI.begin();
  mfrc522.PCD_Init();

  pinMode(ASM_LED_PIN, OUTPUT);
  digitalWrite(ASM_LED_PIN, LOW);

  Wire.begin();
  pwm.begin();
  pwm.setPWMFreq(60);
  
  pwm.setPWM(SERVO_CANAL_DOC, 0, SERVOMIN);
  pwm.setPWM(SERVO_CANAL_ENF, 0, SERVOMIN);
  pwm.setPWM(SERVO_CANAL_PAC, 0, SERVOMIN);

  lcd.begin(16, 2);
  lcd.print("Iniciando...");
  delay(1000);
  lcd.clear();
  
  currentLogIndex = EEPROM.read(99);
  if (currentLogIndex >= MAX_LOGS) currentLogIndex = 0;

  lcd.clear();
  lcd.print("Sistema Listo");
  delay(1000);

  lcd.clear();
}

void asm_blink_led() {
  asm volatile("sbi 0x06, 0");
  delay(100);
  asm volatile("sbi 0x06, 0");
  delay(100);
  
  asm volatile("sbi 0x06, 0");
  delay(100);
  asm volatile("sbi 0x06, 0");
}

void dispensar(int canalServo) {
  asm_blink_led();
  
  int pulsoAbierto = SERVOMAX;
  pwm.setPWM(canalServo, 0, pulsoAbierto);
  delay(1500);
  
  pwm.setPWM(canalServo, 0, SERVOMIN);
  delay(500);
}

void loop() {
  lcd.setCursor(0, 0);
  lcd.print("Dispensador");
  lcd.setCursor(0, 1);
  lcd.print("Acerque Tarjeta ");

  if (!mfrc522.PICC_IsNewCardPresent()) {
    return;
  }
  if (!mfrc522.PICC_ReadCardSerial()) {
    return;
  }

  String content = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    content.concat(String(mfrc522.uid.uidByte[i] < 0x10 ? " 0" : " "));
    content.concat(String(mfrc522.uid.uidByte[i], HEX));
  }
  content.toUpperCase();
  
  int rolDetectado = 0;
  if (content.substring(1) == "1A 5E CA 0B") rolDetectado = 1;
  else if(content.substring(1) == "6B 85 0D 02") rolDetectado = 2;
  else if (content.substring(1) == "CA 97 30 15") rolDetectado = 3;
  else rolDetectado = 0;

  lcd.clear();

  if (rolDetectado > 0) {
    lcd.print("Bienvenido:");
    lcd.setCursor(0, 1);
    
    if (rolDetectado == 1) {
      lcd.print("Doctor");
      delay(500);
      lcd.setCursor(0, 1);
      lcd.print("Dispensando...");
      dispensar(SERVO_CANAL_DOC);
    }
    else if (rolDetectado == 2) {
      lcd.print("Enfermero");
      delay(500);
      lcd.setCursor(0, 1);
      lcd.print("Dispensando...");
      dispensar(SERVO_CANAL_ENF);
    }
    else if (rolDetectado == 3) {
      lcd.print("Paciente");
      delay(500);
      lcd.setCursor(0, 1);
      lcd.print("Dispensando...");
      dispensar(SERVO_CANAL_PAC);
    }

    guardarLog(rolDetectado);

  } else {
    lcd.print("ACCESO DENEGADO");
    lcd.setCursor(0, 1);
    lcd.print("No registrado");
    
    guardarLog(0);
    delay(2000);
  }

  lcd.clear();
}

void guardarLog(byte rol) {
  LogEntry entry;
  entry.rol = rol;

  int addr = EEPROM_LOG_START + (currentLogIndex * sizeof(LogEntry));

  EEPROM.put(addr, entry);

  currentLogIndex++;
  if (currentLogIndex >= MAX_LOGS) currentLogIndex = 0;

  EEPROM.write(99, currentLogIndex);
}