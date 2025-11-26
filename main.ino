#include <SPI.h>
#include <MFRC522.h>
#include <LiquidCrystal.h>
#include <EEPROM.h>
#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>
#include <RTClib.h>

// --- CONFIGURACIÓN DE PINES ---
#define SS_PIN 10
#define RST_PIN 9

// LCD Paralelo
const int rs = 8, en = 7, d4 = 5, d5 = 4, d6 = 3, d7 = 2;
LiquidCrystal lcd(rs, en, d4, d5, d6, d7);

// LED ASM y Botones
#define ASM_LED_PIN A0
#define BTN_NAVEGAR 6  // Botón 'Seleccionar' / Navegar
#define BTN_ENTER   A1  // Botón 'Confirmar' / Enter

// --- OBJETOS ---
MFRC522 mfrc522(SS_PIN, RST_PIN);
Adafruit_PWMServoDriver pwm = Adafruit_PWMServoDriver();
RTC_DS3231 rtc;

// --- SERVOS ---
#define SERVOMIN 150
#define SERVOMAX 600
#define SERVO_CANAL_DOC 0
#define SERVO_CANAL_ENF 1
#define SERVO_CANAL_PAC 2

// --- CONFIGURACIÓN MÉDICA ---
const unsigned long INTERVALO_SEG = 10; 
const int HORA_DOSIS = 11;      
const int MINUTO_LIMITE = 15;   

// --- MAPA DE MEMORIA ---
const int EEPROM_ADDR_LAST_TIME = 0;
const int EEPROM_DB_START = 10;
const int MAX_USUARIOS = 20; 
const int USER_RECORD_SIZE = 5; 
const int EEPROM_LOG_START = 512; 
const int MAX_LOGS = 50;

// --- ESTRUCTURAS ---
struct LogEntry {
  byte rol;
  byte hora;
  byte min;
};

// Variables Globales
int currentLogIndex = 0;

// --- PROTOTIPOS ---
void guardarUsuarioEnDB(int indice, byte* uid, byte rol);
void menuAdministracion();
void menuDoctor();
void menuEnfermero();
void menuPaciente();
void modoMantenimiento(); // <-- nueva función

// ---------------------------------------------------------------
// ---------------   RUTINAS EN ENSAMBLADOR   --------------------
// ---------------------------------------------------------------

// Lee el botón BTN_NAVEGAR (pin 6 = PD6) en ASM puro
// Devuelve: 0 = PRESIONADO, 1 = NO presionado
int asm_leer_boton() {
  uint8_t valor;
  asm volatile(
    "in %[resultado], 0x09 \n\t"     // Leer PIND (direccion 0x09)
    "lsr %[resultado]        \n\t"    // desplazar 6 veces para bajar el bit6 a bit0
    "lsr %[resultado]        \n\t"
    "lsr %[resultado]        \n\t"
    "lsr %[resultado]        \n\t"
    "lsr %[resultado]        \n\t"
    "lsr %[resultado]        \n\t"
    "andi %[resultado], 0x01 \n\t"    // quedarnos solo con bit 0 (antes era bit 6)
    : [resultado] "=r" (valor)
    :
    :
  );
  return valor;
}

// Blink LED en ASM
void asm_blink_led() {
  // Prender (Set Bit en PORTC)
  asm volatile("sbi 0x08, 0"); 
  delay(100);
  
  // Apagar (Clear Bit en PORTC)
  asm volatile("cbi 0x08, 0"); 
  delay(100);
  
  asm volatile("sbi 0x08, 0");
  delay(100);
  
  asm volatile("cbi 0x08, 0");
}

// ---------------------------------------------------------------

void setup() {
  SPI.begin();
  mfrc522.PCD_Init();

  pinMode(ASM_LED_PIN, OUTPUT);
  digitalWrite(ASM_LED_PIN, LOW);

  pinMode(BTN_NAVEGAR, INPUT_PULLUP);
  pinMode(BTN_ENTER, INPUT_PULLUP);

  Wire.begin();
  pwm.begin();
  pwm.setPWMFreq(60);
  
  pwm.setPWM(SERVO_CANAL_DOC, 0, SERVOMIN);
  pwm.setPWM(SERVO_CANAL_ENF, 0, SERVOMIN);
  pwm.setPWM(SERVO_CANAL_PAC, 0, SERVOMIN);

  lcd.begin(16, 2);
  
  if (!rtc.begin()) {
    lcd.print("Error RTC");
    while (1); 
  }
  if (rtc.lostPower()) {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  // --- AUTO-REGISTRO DOCTOR ---
  byte primerByte = EEPROM.read(EEPROM_DB_START + 4);
  if (primerByte == 0 || primerByte == 255) {
    byte uidDoctor[] = {0x1A, 0x5E, 0xCA, 0x0B};
    guardarUsuarioEnDB(0, uidDoctor, 1); 
    
    lcd.clear();
    lcd.print("Doctor Maestro");
    lcd.setCursor(0, 1);
    lcd.print("Registrado Auto");
    delay(2000);
  }

  currentLogIndex = EEPROM.read(511); 
  if (currentLogIndex >= MAX_LOGS) currentLogIndex = 0;

  lcd.clear();
  lcd.print("Sistema Listo");
  delay(1000);
  lcd.clear();
}

void dispensar(int canalServo) {
  asm_blink_led();
  int pulsoAbierto = SERVOMAX;
  pwm.setPWM(canalServo, 0, pulsoAbierto);
  delay(1500);
  pwm.setPWM(canalServo, 0, SERVOMIN);
  delay(500);
}

// --- GESTIÓN DE BASE DE DATOS ---
byte identificarUsuario(byte* uidLeido) {
  for (int i = 0; i < MAX_USUARIOS; i++) {
    int addr = EEPROM_DB_START + (i * USER_RECORD_SIZE);
    byte rolGuardado = EEPROM.read(addr + 4);
    if (rolGuardado != 0 && rolGuardado != 255) { 
      bool coincide = true;
      for (int j = 0; j < 4; j++) {
        if (EEPROM.read(addr + j) != uidLeido[j]) {
          coincide = false;
          break;
        }
      }
      if (coincide) return rolGuardado;
    }
  }
  return 0; 
}

void guardarUsuarioEnDB(int indice, byte* uid, byte rol) {
  int addr = EEPROM_DB_START + (indice * USER_RECORD_SIZE);
  for (int j = 0; j < 4; j++) {
    EEPROM.update(addr + j, uid[j]);
  }
  EEPROM.update(addr + 4, rol);
}

int buscarEspacioLibre() {
  for (int i = 0; i < MAX_USUARIOS; i++) {
    int addr = EEPROM_DB_START + (i * USER_RECORD_SIZE);
    byte rol = EEPROM.read(addr + 4);
    if (rol == 0 || rol == 255) return i;
  }
  return -1; 
}

// --- FUNCIONES AUXILIARES ---
void guardarEvento(byte rol) {
  DateTime now = rtc.now();
  LogEntry entry = {rol, (byte)now.hour(), (byte)now.minute()};
  int addr = EEPROM_LOG_START + (currentLogIndex * sizeof(LogEntry));
  EEPROM.put(addr, entry);
  currentLogIndex++;
  if (currentLogIndex >= MAX_LOGS) currentLogIndex = 0;
  EEPROM.write(511, currentLogIndex);
  if (rol == 3) { 
    unsigned long tiempoActual = now.unixtime(); 
    EEPROM.put(EEPROM_ADDR_LAST_TIME, tiempoActual);
  }
}

bool esVentanaCorrecta() { return true; } // Modo Prueba

bool yaPasoTiempoSeguro() {
  unsigned long ultimaToma;
  EEPROM.get(EEPROM_ADDR_LAST_TIME, ultimaToma);
  DateTime now = rtc.now();
  unsigned long tiempoActual = now.unixtime();
  if (ultimaToma == 0 || ultimaToma == 4294967295 || tiempoActual < ultimaToma) return true;
  return (tiempoActual - ultimaToma) >= INTERVALO_SEG;
}

// ============================================================
//                     MENÚ DE ADMIN
// ============================================================

void registrarNuevoUsuario() {
  lcd.clear();
  lcd.print("Pase Nueva Tarj.");
  
  while (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) {
    // Versión ASM del botón para salir
    if (asm_leer_boton() == 0) { 
       delay(300); return; 
    }
  }
  
  if (identificarUsuario(mfrc522.uid.uidByte) != 0) {
    lcd.clear(); lcd.print("Ya registrado!"); delay(2000); return;
  }
  
  byte nuevoUID[4];
  for(int i=0; i<4; i++) nuevoUID[i] = mfrc522.uid.uidByte[i];
  
  byte rolElegido = 2; 
  bool seleccionando = true;
  
  lcd.clear();
  lcd.print("Rol: Enfermero");
  lcd.setCursor(0, 1);
  lcd.print("Btn1:Camb Btn2:OK");
  delay(1000);
  
  while(seleccionando) {
    if (digitalRead(BTN_NAVEGAR) == LOW) { 
      if (rolElegido == 2) {
        rolElegido = 3;
        lcd.setCursor(0, 0); lcd.print("Rol: Paciente ");
      } else {
        rolElegido = 2;
        lcd.setCursor(0, 0); lcd.print("Rol: Enfermero");
      }
      delay(300); 
      while(digitalRead(BTN_NAVEGAR) == LOW); 
    }
    if (digitalRead(BTN_ENTER) == LOW) { 
      seleccionando = false;
      delay(300); 
      while(digitalRead(BTN_ENTER) == LOW); 
    }
  }
  
  int slot = buscarEspacioLibre();
  if (slot != -1) {
    guardarUsuarioEnDB(slot, nuevoUID, rolElegido);
    lcd.clear(); lcd.print("Guardado Exito!"); delay(2000);
  } else {
    lcd.clear(); lcd.print("Memoria Llena!"); delay(2000);
  }
}

void menuAdministracion() {
  lcd.clear();
  lcd.print("MENU ADMIN");
  
  bool salir = false;
  int subOpcion = 0; 
  
  while (digitalRead(BTN_NAVEGAR) == LOW || digitalRead(BTN_ENTER) == LOW) delay(50);
  
  while(!salir) {
    lcd.setCursor(0, 1);
    if (subOpcion == 0) lcd.print("> Alta Usuario ");
    else lcd.print("> Salir        ");
    
    if (digitalRead(BTN_NAVEGAR) == LOW) {
      subOpcion = !subOpcion;
      delay(300); 
      while(digitalRead(BTN_NAVEGAR) == LOW); 
    }
    
    if (digitalRead(BTN_ENTER) == LOW) {
      if (subOpcion == 0) registrarNuevoUsuario();
      else salir = true;
      
      delay(300);
      while(digitalRead(BTN_ENTER) == LOW);
      
      if (!salir) {
         lcd.clear(); lcd.print("MENU ADMIN"); 
      }
    }
  }
}

// ============================================================
//     MENÚS POR ROL
// ============================================================

void menuDoctor() {
    while (digitalRead(BTN_NAVEGAR) == LOW || digitalRead(BTN_ENTER) == LOW) delay(50);

    int opcion = 0; 
    bool salir = false;
    
    long lastActivity = millis(); 
    const long TIMEOUT_DOCTOR = 300000; 

    while (!salir) {
        lcd.clear();
        lcd.print("Hola Doctor");
        lcd.setCursor(0, 1);
        
        if (opcion == 0) lcd.print("> Admin         ");
        if (opcion == 1) lcd.print("> Mantenimiento ");
        if (opcion == 2) lcd.print("> Salir         ");

        if (millis() - lastActivity > TIMEOUT_DOCTOR) {
            salir = true; 
            break;
        }

        // Navegación con botón (usamos digitalRead para compatibilidad, asm_leer_boton está disponible)
        if (digitalRead(BTN_NAVEGAR) == LOW) {
            opcion++;
            if (opcion > 2) opcion = 0;
            delay(300); 
            while(digitalRead(BTN_NAVEGAR) == LOW); 
            lastActivity = millis(); 
        }

        if (digitalRead(BTN_ENTER) == LOW) {
            delay(300);
            while(digitalRead(BTN_ENTER) == LOW);
            lastActivity = millis(); 

            if (opcion == 0) {
                menuAdministracion(); 
            } else if (opcion == 1) {
                // Entra en MODO MANTENIMIENTO y permanece hasta que el doctor presione OK (BTN_ENTER)
                modoMantenimiento();
                // Al salir de modoMantenimiento volvemos al menú del doctor
            } else if (opcion == 2) {
                salir = true; 
            }
        }
        delay(100);
    }
    lcd.clear();
}

void menuEnfermero() {
    while (digitalRead(BTN_NAVEGAR) == LOW || digitalRead(BTN_ENTER) == LOW) delay(50);
    
    lcd.clear();
    lcd.print("Hola Enfermero");
    lcd.setCursor(0, 1);
    lcd.print("> Dispensar");
    
    long startWait = millis();
    while(millis() - startWait < 5000) {
       if (digitalRead(BTN_ENTER) == LOW) {
          delay(300);
          while(digitalRead(BTN_ENTER) == LOW);
          
          lcd.clear(); lcd.print("Dispensando...");
          dispensar(SERVO_CANAL_ENF);
          guardarEvento(2);
          return;
       }
       if (digitalRead(BTN_NAVEGAR) == LOW) return;
    }
}

void menuPaciente() {
    while (digitalRead(BTN_NAVEGAR) == LOW || digitalRead(BTN_ENTER) == LOW) delay(50);
    
    lcd.clear();
    lcd.print("Hola Paciente");
    lcd.setCursor(0, 1);
    lcd.print("> Tomar Med.");
    
    long startWait = millis();
    while(millis() - startWait < 5000) {
       if (digitalRead(BTN_ENTER) == LOW) {
          delay(300);
          while(digitalRead(BTN_ENTER) == LOW);
          
          if (esVentanaCorrecta()) {
             if (yaPasoTiempoSeguro()) {
                lcd.clear(); lcd.print("Autorizado"); lcd.setCursor(0, 1); lcd.print("Dispensando...");
                dispensar(SERVO_CANAL_PAC);
                guardarEvento(3); 
             } else {
                lcd.clear(); lcd.print("Dosis ya tomada"); lcd.setCursor(0, 1); lcd.print("Espere...");
                delay(2000);
             }
          } else {
             lcd.clear(); lcd.print("Horario Error"); delay(2000);
          }
          return;
       }
       if (digitalRead(BTN_NAVEGAR) == LOW) return;
    }
}

// ============================================================
//                MODO MANTENIMIENTO (FINAL)
//   - Mantiene el servo del Doctor ABIERTO.
//   - Ignora BTN_NAVEGAR completamente.
//   - Solo termina cuando se presione BTN_ENTER (OK).
// ============================================================
void modoMantenimiento() {
  lcd.clear();
  lcd.print("MODO MANTENIM.");
  lcd.setCursor(0, 1);
  lcd.print("OK = Salir");
  delay(500);

  // --- ABRIR SERVO DEL DOCTOR ---
  pwm.setPWM(SERVO_CANAL_DOC, 0, SERVOMAX);
  delay(300);

  // Indicador visual de entrada al modo
  asm_blink_led();

  // Bucle principal del modo mantenimiento
  while (true) {
    lcd.setCursor(0, 0);
    lcd.print("MANTENIMIENTO     ");
    lcd.setCursor(0, 1);
    lcd.print("OK p/ Salir       ");

    // Parpadeo ASM solo como indicador
    asm_blink_led();

    // --- EL SERVO SE MANTIENE ABIERTO SIEMPRE ---
    pwm.setPWM(SERVO_CANAL_DOC, 0, SERVOMAX);

    // Verificar salida con botón ENTER
    if (digitalRead(BTN_ENTER) == LOW) {
      delay(200);
      while (digitalRead(BTN_ENTER) == LOW) delay(10);

      // Cerrar servo ANTES de salir
      pwm.setPWM(SERVO_CANAL_DOC, 0, SERVOMIN);

      lcd.clear();
      lcd.print("Saliendo...");
      delay(600);
      lcd.clear();
      return;
    }

    delay(200);
  }
}

// ============================================================
//                     LOOP PRINCIPAL
// ============================================================

void loop() {
  static long lastTime = 0;
  
  if (millis() - lastTime > 1000) {
    DateTime now = rtc.now();
    lcd.setCursor(0, 0);
    if(now.hour() < 10) lcd.print("0"); lcd.print(now.hour()); lcd.print(":");
    if(now.minute() < 10) lcd.print("0"); lcd.print(now.minute());
    lcd.print(" Esperando   ");
    lcd.setCursor(0, 1);
    lcd.print("Acerque Tarjeta ");
    lastTime = millis();
  }

  // --- DETECCIÓN DE TARJETA ---
  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
      byte rol = identificarUsuario(mfrc522.uid.uidByte);
      
      asm_blink_led(); // Indica lectura exitosa

      lcd.clear();
      if (rol == 1) { 
          menuDoctor();
      } 
      else if (rol == 2) { 
          menuEnfermero();
      }
      else if (rol == 3) { 
          menuPaciente();
      } 
      else { 
         lcd.print("ACCESO DENEGADO"); 
         lcd.setCursor(0, 1); 
         lcd.print("No registrado");
         asm_blink_led();
         delay(1000); 
      }
      lcd.clear();
  }
}
