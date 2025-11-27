#include <SPI.h>
#include <MFRC522.h>
#include <LiquidCrystal.h>
#include <EEPROM.h>
#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>
#include <RTClib.h>

// CONFIGURACIÓN DE PINES
#define SS_PIN 10
#define RST_PIN 9

// LCD Paralelo
const int rs = 8, en = 7, d4 = 5, d5 = 4, d6 = 3, d7 = 2;
LiquidCrystal lcd(rs, en, d4, d5, d6, d7);

// LED ASM y Botones
#define ASM_LED_PIN A0
#define BTN_NAVEGAR 6 // Botón Navegar
#define BTN_ENTER A1  // Botón Enter

// OBJETOS
MFRC522 mfrc522(SS_PIN, RST_PIN);
Adafruit_PWMServoDriver pwm = Adafruit_PWMServoDriver();
RTC_DS3231 rtc;

// SERVOS Y VELOCIDAD
#define SERVOMIN 150
#define SERVOMAX 300
#define SERVO_CANAL_MANT 0
#define SERVO_CANAL_DISP1 1
#define SERVO_CANAL_DISP2 2

#define VELOCIDAD_SERVO 1

// LISTA DE MEDICAMENTOS PREDEFINIDOS
const char *LISTA_MEDICAMENTOS[] = {
    "Paracetamol",
    "Ibuprofeno",
    "Amoxicilina",
    "Loratadina",
    "Omeprazol",
    "Metformina",
    "Atorvastatina",
    "Aspirina",
    "Salbutamol",
    "Insulina",
    "Med. Especial"};
const int TOTAL_MEDICAMENTOS = 11;
const int MED_CONTAINER_1 = 0;
const int MED_CONTAINER_2 = 1;

// MAPA DE MEMORIA
const int EEPROM_DB_START = 10;
const int MAX_USUARIOS = 20;
const int USER_RECORD_SIZE = 5;
const int EEPROM_LOG_START = 512;
const int MAX_LOGS = 50;
const int EEPROM_ASIGNACIONES_START = 200;
const int MAX_ASIGNACIONES = 20;
const int ASIGNACION_RECORD_SIZE = 5;
const int EEPROM_LAST_TIMES_START = 400;
const int LAST_TIME_RECORD_SIZE = 4;
const int EEPROM_MED_CONTAINERS = 600;
const int EEPROM_PACIENTE_MEDICAMENTOS = 650;

// ESTRUCTURAS
struct LogEntry
{
  byte rol;
  byte hora;
  byte min;
};
struct ConfigMedicamento
{
  byte medicamentoIndex;
  byte hora;
  byte minuto;
  unsigned long intervalo;
  bool activo;
};

struct ConfigPaciente
{
  ConfigMedicamento med1;
  ConfigMedicamento med2;
};

// Variables Globales
int currentLogIndex = 0;

// PROTOTIPOS
void guardarUsuarioEnDB(int indice, byte *uid, byte rol);
void menuAdministracion();
void menuDoctor();
void menuEnfermero();
void menuPaciente();
void modoMantenimiento();
void asignarEnfermeroAPaciente();
void registrarPacienteConEnfermero();
int buscarIndiceUsuario(byte *uid);
void guardarAsignacionEnfermero(byte *uidPaciente, int indiceEnfermero);
int obtenerEnfermeroAsignado(byte *uidPaciente);
void configurarHoraRTC();
unsigned long obtenerUltimaTomaPaciente(byte *uidPaciente, int numMed);
void guardarUltimaTomaPaciente(byte *uidPaciente, unsigned long tiempo, int numMed);
void guardarEvento(byte rol, byte *uidPaciente = nullptr, int numMed = -1);
bool yaPasoTiempoSeguro(byte *uidPaciente, int numMed);
bool esVentanaCorrecta(byte *uidPaciente, int numMed);
void configurarMedicamentosContenedores();
byte obtenerMedicamentoContenedor(int contenedor);
void guardarMedicamentoContenedor(int contenedor, byte medIndex);
void configurarMedicamentoPaciente();
void mostrarMenuMedicamentos(int &medIndex, int &contenedor);
ConfigPaciente leerConfiguracionPaciente(byte *uidPaciente);
void guardarConfiguracionPaciente(byte *uidPaciente, ConfigPaciente config);
void configurarHorarioIndividual(ConfigMedicamento &config);
void menuSeleccionManual(byte rol);
void moverServo(int canal, int inicio, int fin);

// Función de antirebote que usa el ensamblador
bool botonNavegarPresionado()
{
  static unsigned long ultimoTiempo = 0;
  static bool estadoAnterior = HIGH;

  bool estadoActual = asm_leer_boton();
  unsigned long tiempoActual = millis();
  estadoActual = (estadoActual == 0);
  if (estadoActual != estadoAnterior)
  {
    ultimoTiempo = tiempoActual;
    estadoAnterior = estadoActual;
    return false;
  }
  if ((tiempoActual - ultimoTiempo) > 50 && estadoActual == true)
  {
    ultimoTiempo = tiempoActual;
    return true;
  }

  return false;
}

bool botonEnterPresionado()
{
  static unsigned long ultimoTiempo = 0;
  static bool estadoAnterior = HIGH;
  bool estadoActual = digitalRead(BTN_ENTER);
  unsigned long tiempoActual = millis();

  if (estadoActual != estadoAnterior)
  {
    ultimoTiempo = tiempoActual;
    estadoAnterior = estadoActual;
    return false;
  }

  if ((tiempoActual - ultimoTiempo) > 50 && estadoActual == LOW)
  {
    ultimoTiempo = tiempoActual;
    return true;
  }

  return false;
}

void esperarLiberacionBotonNavegar()
{
  while (asm_leer_boton() == 0)
  {
    delay(10);
  }
  delay(50);
}

void esperarLiberacionBotonEnter()
{
  while (digitalRead(BTN_ENTER) == LOW)
  {
    delay(10);
  }
  delay(50);
}

// RUTINAS EN ENSAMBLADOR
int asm_leer_boton()
{
  uint8_t valor;
  asm volatile(
      "in %[resultado], 0x09 \n\t"
      "lsr %[resultado]        \n\t"
      "lsr %[resultado]        \n\t"
      "lsr %[resultado]        \n\t"
      "lsr %[resultado]        \n\t"
      "lsr %[resultado]        \n\t"
      "lsr %[resultado]        \n\t"
      "andi %[resultado], 0x01 \n\t"
      : [resultado] "=r"(valor)
      :
      :);
  return valor;
}

void asm_blink_led()
{
  asm volatile("sbi 0x08, 0");
  delay(50);
  asm volatile("cbi 0x08, 0");
  delay(50);
  asm volatile("sbi 0x08, 0");
  delay(50);
  asm volatile("cbi 0x08, 0");
}

void setup()
{
  lcd.begin(16, 2);
  delay(100);
  lcd.clear();
  lcd.print("Iniciando...");
  delay(1000);

  SPI.begin();
  mfrc522.PCD_Init();

  pinMode(ASM_LED_PIN, OUTPUT);
  digitalWrite(ASM_LED_PIN, LOW);
  pinMode(BTN_NAVEGAR, INPUT_PULLUP);
  pinMode(BTN_ENTER, INPUT_PULLUP);

  Wire.begin();
  pwm.begin();
  pwm.setPWMFreq(60);

  // Posición inicial cerrada
  pwm.setPWM(SERVO_CANAL_MANT, 0, SERVOMIN);
  pwm.setPWM(SERVO_CANAL_DISP1, 0, SERVOMIN);
  pwm.setPWM(SERVO_CANAL_DISP2, 0, SERVOMIN);

  if (!rtc.begin())
  {
    lcd.clear();
    lcd.print("Error RTC");
    while (1)
      ;
  }

  if (rtc.lostPower())
  {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  // AUTO-REGISTRO DOCTOR MAESTRO
  byte primerByte = EEPROM.read(EEPROM_DB_START + 4);
  if (primerByte == 0 || primerByte == 255)
  {
    byte uidDoctor[] = {0x1A, 0x5E, 0xCA, 0x0B};
    guardarUsuarioEnDB(0, uidDoctor, 1);

    lcd.clear();
    lcd.print("Doctor Registrado");
    delay(2000);
  }

  currentLogIndex = EEPROM.read(511);
  if (currentLogIndex >= MAX_LOGS)
    currentLogIndex = 0;

  // Inicializar áreas de memoria
  byte initFlag = EEPROM.read(EEPROM_LAST_TIMES_START - 1);
  if (initFlag != 0xAA)
  {
    for (int i = 0; i < MAX_USUARIOS; i++)
    {
      int addr = EEPROM_LAST_TIMES_START + (i * LAST_TIME_RECORD_SIZE);
      EEPROM.put(addr, (unsigned long)0);
    }
    EEPROM.write(EEPROM_LAST_TIMES_START - 1, 0xAA);
  }

  byte initFlagMed = EEPROM.read(EEPROM_MED_CONTAINERS - 1);
  if (initFlagMed != 0xBB)
  {
    EEPROM.write(EEPROM_MED_CONTAINERS + MED_CONTAINER_1, 0);
    EEPROM.write(EEPROM_MED_CONTAINERS + MED_CONTAINER_2, 1);
    EEPROM.write(EEPROM_MED_CONTAINERS - 1, 0xBB);
  }

  lcd.clear();
  lcd.print("Sistema Listo");
  delay(1000);
  lcd.clear();
}

void moverServo(int canal, int inicio, int fin)
{
  if (inicio < fin)
  {
    for (int pulso = inicio; pulso <= fin; pulso += 5)
    {
      pwm.setPWM(canal, 0, pulso);
      delay(VELOCIDAD_SERVO);
    }
  }
  else
  {
    for (int pulso = inicio; pulso >= fin; pulso -= 5)
    {
      pwm.setPWM(canal, 0, pulso);
      delay(VELOCIDAD_SERVO);
    }
  }
}

void dispensar(int canalServo)
{
  asm_blink_led();
  moverServo(canalServo, SERVOMIN, SERVOMAX);

  delay(800);
  moverServo(canalServo, SERVOMAX, SERVOMIN);

  delay(50);
}

// GESTIÓN DE BASE DE DATOS
byte identificarUsuario(byte *uidLeido)
{
  for (int i = 0; i < MAX_USUARIOS; i++)
  {
    int addr = EEPROM_DB_START + (i * USER_RECORD_SIZE);
    byte rolGuardado = EEPROM.read(addr + 4);
    if (rolGuardado != 0 && rolGuardado != 255)
    {
      bool coincide = true;
      for (int j = 0; j < 4; j++)
      {
        if (EEPROM.read(addr + j) != uidLeido[j])
        {
          coincide = false;
          break;
        }
      }
      if (coincide)
        return rolGuardado;
    }
  }
  return 0;
}

void guardarUsuarioEnDB(int indice, byte *uid, byte rol)
{
  int addr = EEPROM_DB_START + (indice * USER_RECORD_SIZE);
  for (int j = 0; j < 4; j++)
  {
    EEPROM.update(addr + j, uid[j]);
  }
  EEPROM.update(addr + 4, rol);
}

int buscarEspacioLibre()
{
  for (int i = 0; i < MAX_USUARIOS; i++)
  {
    int addr = EEPROM_DB_START + (i * USER_RECORD_SIZE);
    byte rol = EEPROM.read(addr + 4);
    if (rol == 0 || rol == 255)
      return i;
  }
  return -1;
}

int buscarIndiceUsuario(byte *uid)
{
  for (int i = 0; i < MAX_USUARIOS; i++)
  {
    int addr = EEPROM_DB_START + (i * USER_RECORD_SIZE);
    byte rolGuardado = EEPROM.read(addr + 4);
    if (rolGuardado != 0 && rolGuardado != 255)
    {
      bool coincide = true;
      for (int j = 0; j < 4; j++)
      {
        if (EEPROM.read(addr + j) != uid[j])
        {
          coincide = false;
          break;
        }
      }
      if (coincide)
        return i;
    }
  }
  return -1;
}

void guardarAsignacionEnfermero(byte *uidPaciente, int indiceEnfermero)
{
  for (int i = 0; i < MAX_ASIGNACIONES; i++)
  {
    int addr = EEPROM_ASIGNACIONES_START + (i * ASIGNACION_RECORD_SIZE);
    byte primerByte = EEPROM.read(addr);

    if (primerByte == 0 || primerByte == 255)
    {
      for (int j = 0; j < 4; j++)
      {
        EEPROM.update(addr + j, uidPaciente[j]);
      }
      EEPROM.update(addr + 4, indiceEnfermero);
      return;
    }
  }
}

byte obtenerMedicamentoContenedor(int contenedor)
{
  return EEPROM.read(EEPROM_MED_CONTAINERS + contenedor);
}

void guardarMedicamentoContenedor(int contenedor, byte medIndex)
{
  EEPROM.update(EEPROM_MED_CONTAINERS + contenedor, medIndex);
}

void configurarMedicamentosContenedores()
{
  byte medCont1 = obtenerMedicamentoContenedor(MED_CONTAINER_1);
  byte medCont2 = obtenerMedicamentoContenedor(MED_CONTAINER_2);

  int contenedorActual = 0;
  bool editando = true;

  lcd.clear();
  lcd.print("Config Contenedor");

  while (editando)
  {
    lcd.setCursor(0, 1);
    lcd.print("                ");
    lcd.setCursor(0, 1);
    if (contenedorActual == 0)
    {
      lcd.print("C1:");
      lcd.print(LISTA_MEDICAMENTOS[medCont1]);
    }
    else
    {
      lcd.print("C2:");
      lcd.print(LISTA_MEDICAMENTOS[medCont2]);
    }

    if (botonNavegarPresionado())
    {
      if (contenedorActual == 0)
      {
        medCont1 = (medCont1 + 1) % TOTAL_MEDICAMENTOS;
      }
      else
      {
        medCont2 = (medCont2 + 1) % TOTAL_MEDICAMENTOS;
      }
      esperarLiberacionBotonNavegar();
    }

    if (botonEnterPresionado())
    {
      contenedorActual++;
      if (contenedorActual > 1)
      {
        guardarMedicamentoContenedor(MED_CONTAINER_1, medCont1);
        guardarMedicamentoContenedor(MED_CONTAINER_2, medCont2);
        lcd.clear();
        lcd.print("Config Guardada!");
        delay(1500);
        editando = false;
      }
      esperarLiberacionBotonEnter();
    }

    delay(50);
  }
}

ConfigPaciente leerConfiguracionPaciente(byte *uidPaciente)
{
  ConfigPaciente config;
  int indicePaciente = buscarIndiceUsuario(uidPaciente);
  if (indicePaciente != -1)
  {
    int addr = EEPROM_PACIENTE_MEDICAMENTOS + (indicePaciente * sizeof(ConfigPaciente));
    EEPROM.get(addr, config);
  }

  if (config.med1.intervalo == 0)
  {
    config.med1.medicamentoIndex = 0;
    config.med1.hora = 8;
    config.med1.minuto = 0;
    config.med1.intervalo = 3600;
    config.med1.activo = false;

    config.med2.medicamentoIndex = 1;
    config.med2.hora = 20;
    config.med2.minuto = 0;
    config.med2.intervalo = 3600;
    config.med2.activo = false;
  }

  return config;
}

void guardarConfiguracionPaciente(byte *uidPaciente, ConfigPaciente config)
{
  int indicePaciente = buscarIndiceUsuario(uidPaciente);
  if (indicePaciente != -1)
  {
    int addr = EEPROM_PACIENTE_MEDICAMENTOS + (indicePaciente * sizeof(ConfigPaciente));
    EEPROM.put(addr, config);
  }
}

void mostrarMenuMedicamentos(int &medIndex, int &contenedor)
{
  bool seleccionando = true;
  int opcion = 0;

  lcd.clear();
  lcd.print("Selec Contenedor");
  while (seleccionando)
  {
    lcd.setCursor(0, 1);
    lcd.print("                ");
    lcd.setCursor(0, 1);

    if (opcion == 0)
    {
      lcd.print(">C1:");
      byte med = obtenerMedicamentoContenedor(MED_CONTAINER_1);
      lcd.print(LISTA_MEDICAMENTOS[med]);
    }
    else
    {
      lcd.print(">C2:");
      byte med = obtenerMedicamentoContenedor(MED_CONTAINER_2);
      lcd.print(LISTA_MEDICAMENTOS[med]);
    }

    if (botonNavegarPresionado())
    {
      opcion = (opcion + 1) % 2;
      esperarLiberacionBotonNavegar();
    }

    if (botonEnterPresionado())
    {
      if (opcion == 0)
      {
        medIndex = obtenerMedicamentoContenedor(MED_CONTAINER_1);
        contenedor = MED_CONTAINER_1;
      }
      else
      {
        medIndex = obtenerMedicamentoContenedor(MED_CONTAINER_2);
        contenedor = MED_CONTAINER_2;
      }
      seleccionando = false;
      esperarLiberacionBotonEnter();
    }

    delay(50);
  }
}

void configurarHorarioIndividual(ConfigMedicamento &config)
{
  bool editando = true;
  int campo = 0;
  while (editando)
  {
    lcd.setCursor(0, 1);
    lcd.print("                ");
    lcd.setCursor(0, 1);
    lcd.print("H:");
    if (config.hora < 10)
      lcd.print("0");
    lcd.print(config.hora);
    lcd.print(" M:");
    if (config.minuto < 10)
      lcd.print("0");
    lcd.print(config.minuto);
    lcd.print(" I:");
    lcd.print(config.intervalo / 3600);
    lcd.print("h");

    if (botonNavegarPresionado())
    {
      if (campo == 0)
      {
        config.hora = (config.hora + 1) % 24;
      }
      else if (campo == 1)
      {
        config.minuto = (config.minuto + 5) % 60;
      }
      else if (campo == 2)
      {
        config.intervalo = (config.intervalo + 3600) % 86400;
        if (config.intervalo == 0)
          config.intervalo = 3600;
      }
      esperarLiberacionBotonNavegar();
    }

    if (botonEnterPresionado())
    {
      campo++;
      if (campo > 2)
      {
        editando = false;
      }
      esperarLiberacionBotonEnter();
    }

    delay(50);
  }
}

// MEDICAMENTO 2 OPCIONAL
void configurarMedicamentoPaciente()
{
  lcd.clear();
  lcd.print("Pase tarjeta...");

  while (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial())
  {
    if (asm_leer_boton() == 0)
    {
      delay(300);
      return;
    }
    delay(50);
  }

  byte uidPaciente[4];
  for (int i = 0; i < 4; i++)
    uidPaciente[i] = mfrc522.uid.uidByte[i];

  byte rol = identificarUsuario(uidPaciente);
  if (rol != 3)
  {
    lcd.clear();
    lcd.print("No es paciente!");
    delay(2000);
    return;
  }

  ConfigPaciente config = leerConfiguracionPaciente(uidPaciente);

  int opcion = 0;
  bool enMenu = true;

  lcd.clear();
  lcd.print("Config Paciente");
  while (enMenu)
  {
    lcd.setCursor(0, 1);
    lcd.print("                ");
    lcd.setCursor(0, 1);

    switch (opcion)
    {
    case 0:
      lcd.print(">Medicamento 1");
      break;
    case 1:
      lcd.print(">Medicamento 2");
      break;
    case 2:
      lcd.print(">Guardar y Salir");
      break;
    }

    if (botonNavegarPresionado())
    {
      opcion = (opcion + 1) % 3;
      esperarLiberacionBotonNavegar();
    }

    if (botonEnterPresionado())
    {
      esperarLiberacionBotonEnter();
      if (opcion == 0)
      {
        // Configurar Medicamento 1
        lcd.clear();
        lcd.print("Med1: Activar?");
        lcd.setCursor(0, 1);
        lcd.print(">Si   No");

        bool decidido = false;
        bool activar = true;
        while (!decidido)
        {
          if (botonNavegarPresionado())
          {
            activar = !activar;
            lcd.setCursor(0, 1);
            lcd.print("                ");
            lcd.setCursor(0, 1);
            if (activar)
            {
              lcd.print(">Si   No");
            }
            else
            {
              lcd.print(" Si  >No");
            }
            esperarLiberacionBotonNavegar();
          }

          if (botonEnterPresionado())
          {
            decidido = true;
            esperarLiberacionBotonEnter();
          }
          delay(50);
        }

        if (activar)
        {
          int medIndex, contenedor;
          mostrarMenuMedicamentos(medIndex, contenedor);
          config.med1.medicamentoIndex = medIndex;
          config.med1.activo = true;
          lcd.clear();
          lcd.print("Config Horario M1");
          configurarHorarioIndividual(config.med1);
        }
        else
        {
          config.med1.activo = false;
        }
      }
      else if (opcion == 1)
      {
        // Configurar Medicamento 2 (OPCIONAL)
        lcd.clear();
        lcd.print("Med2: Activar?");
        lcd.setCursor(0, 1);
        lcd.print(">Si   No");

        bool decidido = false;
        bool activar = true;
        while (!decidido)
        {
          if (botonNavegarPresionado())
          {
            activar = !activar;
            lcd.setCursor(0, 1);
            lcd.print("                ");
            lcd.setCursor(0, 1);
            if (activar)
            {
              lcd.print(">Si   No");
            }
            else
            {
              lcd.print(" Si  >No");
            }
            esperarLiberacionBotonNavegar();
          }

          if (botonEnterPresionado())
          {
            decidido = true;
            esperarLiberacionBotonEnter();
          }
          delay(50);
        }

        if (activar)
        {
          int medIndex, contenedor;
          mostrarMenuMedicamentos(medIndex, contenedor);
          config.med2.medicamentoIndex = medIndex;
          config.med2.activo = true;
          lcd.clear();
          lcd.print("Config Horario M2");
          configurarHorarioIndividual(config.med2);
        }
        else
        {
          config.med2.activo = false;
        }
      }
      else if (opcion == 2)
      {
        // Guardar y salir
        guardarConfiguracionPaciente(uidPaciente, config);
        lcd.clear();
        lcd.print("Config Guardada!");
        delay(1500);
        enMenu = false;
      }

      if (enMenu)
      {
        lcd.clear();
        lcd.print("Config Paciente");
      }
    }

    delay(50);
  }
}

unsigned long obtenerUltimaTomaPaciente(byte *uidPaciente, int numMed)
{
  int indicePaciente = buscarIndiceUsuario(uidPaciente);
  if (indicePaciente == -1)
    return 0;
  int addr = EEPROM_LAST_TIMES_START + (indicePaciente * LAST_TIME_RECORD_SIZE * 2) + (numMed * LAST_TIME_RECORD_SIZE);
  unsigned long ultimaToma;
  EEPROM.get(addr, ultimaToma);
  return ultimaToma;
}

void guardarUltimaTomaPaciente(byte *uidPaciente, unsigned long tiempo, int numMed)
{
  int indicePaciente = buscarIndiceUsuario(uidPaciente);
  if (indicePaciente == -1)
    return;

  int addr = EEPROM_LAST_TIMES_START + (indicePaciente * LAST_TIME_RECORD_SIZE * 2) + (numMed * LAST_TIME_RECORD_SIZE);
  EEPROM.put(addr, tiempo);
}

void guardarEvento(byte rol, byte *uidPaciente, int numMed)
{
  DateTime now = rtc.now();
  LogEntry entry = {rol, (byte)now.hour(), (byte)now.minute()};
  int addr = EEPROM_LOG_START + (currentLogIndex * sizeof(LogEntry));
  EEPROM.put(addr, entry);
  currentLogIndex++;
  if (currentLogIndex >= MAX_LOGS)
    currentLogIndex = 0;
  EEPROM.write(511, currentLogIndex);
  if (rol == 3 && uidPaciente != nullptr && numMed != -1)
  {
    unsigned long tiempoActual = now.unixtime();
    guardarUltimaTomaPaciente(uidPaciente, tiempoActual, numMed);
  }
}

bool yaPasoTiempoSeguro(byte *uidPaciente, int numMed)
{
  ConfigPaciente configPac = leerConfiguracionPaciente(uidPaciente);
  ConfigMedicamento config;
  if (numMed == 0)
  {
    config = configPac.med1;
  }
  else
  {
    config = configPac.med2;
  }

  unsigned long ultimaToma = obtenerUltimaTomaPaciente(uidPaciente, numMed);
  DateTime now = rtc.now();
  unsigned long tiempoActual = now.unixtime();

  if (ultimaToma == 0 || ultimaToma == 4294967295 || tiempoActual < ultimaToma)
  {
    return true;
  }

  return (tiempoActual - ultimaToma) >= config.intervalo;
}

bool esVentanaCorrecta(byte *uidPaciente, int numMed)
{
  ConfigPaciente configPac = leerConfiguracionPaciente(uidPaciente);
  ConfigMedicamento config;
  if (numMed == 0)
  {
    config = configPac.med1;
  }
  else
  {
    config = configPac.med2;
  }

  if (!config.activo)
    return false;
  DateTime now = rtc.now();
  long minutosConfig = config.hora * 60 + config.minuto;
  long minutosActual = now.hour() * 60 + now.minute();
  return (minutosActual >= minutosConfig) && (minutosActual <= minutosConfig + 30);
}

void configurarHoraRTC()
{
  DateTime now = rtc.now();
  int hora = now.hour();
  int minuto = now.minute();
  int segundo = 0;

  int campo = 0;
  bool editando = true;
  lcd.clear();
  lcd.print("Configurar Hora");

  while (editando)
  {
    lcd.setCursor(0, 1);
    lcd.print("                ");
    lcd.setCursor(0, 1);
    if (hora < 10)
      lcd.print("0");
    lcd.print(hora);
    lcd.print(":");
    if (minuto < 10)
      lcd.print("0");
    lcd.print(minuto);
    lcd.print(":");
    if (segundo < 10)
      lcd.print("0");
    lcd.print(segundo);

    if (botonNavegarPresionado())
    {
      if (campo == 0)
      {
        hora = (hora + 1) % 24;
      }
      else if (campo == 1)
      {
        minuto = (minuto + 1) % 60;
      }
      else if (campo == 2)
      {
        segundo = (segundo + 1) % 60;
      }
      esperarLiberacionBotonNavegar();
    }

    if (botonEnterPresionado())
    {
      campo++;
      if (campo > 2)
      {
        lcd.clear();
        lcd.print("Confirmar?");
        lcd.setCursor(0, 1);
        lcd.print(">Si   No");

        bool confirmando = true;
        bool opcionConfirmar = true;
        while (confirmando)
        {
          lcd.setCursor(0, 1);
          lcd.print("                ");
          lcd.setCursor(0, 1);
          if (opcionConfirmar)
          {
            lcd.print(">Si   No");
          }
          else
          {
            lcd.print(" Si  >No");
          }

          if (botonNavegarPresionado())
          {
            opcionConfirmar = !opcionConfirmar;
            esperarLiberacionBotonNavegar();
          }

          if (botonEnterPresionado())
          {
            if (opcionConfirmar)
            {
              DateTime nuevaHora(now.year(), now.month(), now.day(), hora, minuto, segundo);
              rtc.adjust(nuevaHora);
              lcd.clear();
              lcd.print("Hora actualizada!");
              delay(1500);
            }
            else
            {
              lcd.clear();
              lcd.print("Cancelado");
              delay(1000);
            }
            confirmando = false;
            editando = false;
          }
          delay(50);
        }
      }
      esperarLiberacionBotonEnter();
    }

    delay(50);
  }
}

void registrarPacienteConEnfermero()
{
  lcd.clear();
  lcd.print("Registrar Paciente");
  delay(1000);

  lcd.clear();
  lcd.print("Pase tarjeta...");
  while (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial())
  {
    if (asm_leer_boton() == 0)
    {
      return;
    }
  }

  byte uidPaciente[4];
  for (int i = 0; i < 4; i++)
    uidPaciente[i] = mfrc522.uid.uidByte[i];
  if (identificarUsuario(uidPaciente) != 0)
  {
    lcd.clear();
    lcd.print("Ya registrado!");
    delay(2000);
    return;
  }

  int slot = buscarEspacioLibre();
  if (slot == -1)
  {
    lcd.clear();
    lcd.print("Memoria llena!");
    delay(2000);
    return;
  }

  guardarUsuarioEnDB(slot, uidPaciente, 3);
  lcd.clear();
  lcd.print("Paciente OK");
  delay(1500);
}

void asignarEnfermeroAPaciente()
{
  lcd.clear();
  lcd.print("Asignar Enfermero");
  lcd.setCursor(0, 1);
  lcd.print("Pase tarjeta...");
  while (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial())
  {
    if (asm_leer_boton() == 0)
    {
      return;
    }
  }

  byte uidEnfermero[4];
  for (int i = 0; i < 4; i++)
    uidEnfermero[i] = mfrc522.uid.uidByte[i];
  byte rolEnfermero = identificarUsuario(uidEnfermero);

  if (rolEnfermero == 0)
  {
    lcd.clear();
    lcd.print("No registrado");
    lcd.setCursor(0, 1);
    lcd.print("Registrar?");
    bool decidido = false;
    bool registrar = true;

    while (!decidido)
    {
      lcd.setCursor(0, 1);
      lcd.print("                ");
      lcd.setCursor(0, 1);
      if (registrar)
      {
        lcd.print(">Si   No");
      }
      else
      {
        lcd.print(" Si  >No");
      }

      if (botonNavegarPresionado())
      {
        registrar = !registrar;
        esperarLiberacionBotonNavegar();
      }

      if (botonEnterPresionado())
      {
        if (registrar)
        {
          int slot = buscarEspacioLibre();
          if (slot != -1)
          {
            guardarUsuarioEnDB(slot, uidEnfermero, 2);
            lcd.clear();
            lcd.print("Enfermero OK");
            delay(1000);
          }
        }
        decidido = true;
        esperarLiberacionBotonEnter();
      }
      delay(50);
    }
  }

  lcd.clear();
  lcd.print("Asignacion OK!");
  delay(1500);
}

void registrarNuevoUsuario()
{
  lcd.clear();
  lcd.print("Pase tarjeta...");

  while (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial())
  {
    if (asm_leer_boton() == 0)
    {
      return;
    }
  }

  if (identificarUsuario(mfrc522.uid.uidByte) != 0)
  {
    lcd.clear();
    lcd.print("Ya registrado!");
    delay(2000);
    return;
  }

  byte nuevoUID[4];
  for (int i = 0; i < 4; i++)
    nuevoUID[i] = mfrc522.uid.uidByte[i];
  byte rolElegido = 2;
  bool seleccionando = true;

  lcd.clear();
  lcd.print("Rol: Enfermero");

  while (seleccionando)
  {
    lcd.setCursor(0, 1);
    lcd.print("                ");
    lcd.setCursor(0, 1);
    lcd.print("Sel:OK");
    if (botonNavegarPresionado())
    {
      if (rolElegido == 2)
      {
        rolElegido = 3;
        lcd.clear();
        lcd.print("Rol: Paciente");
      }
      else
      {
        rolElegido = 2;
        lcd.clear();
        lcd.print("Rol: Enfermero");
      }
      esperarLiberacionBotonNavegar();
    }

    if (botonEnterPresionado())
    {
      seleccionando = false;
      esperarLiberacionBotonEnter();
    }
    delay(50);
  }

  int slot = buscarEspacioLibre();
  if (slot != -1)
  {
    guardarUsuarioEnDB(slot, nuevoUID, rolElegido);
    lcd.clear();
    lcd.print("Guardado OK!");
    delay(2000);
  }
  else
  {
    lcd.clear();
    lcd.print("Memoria llena!");
    delay(2000);
  }
}

void menuSeleccionManual(byte rol)
{
  int opcion = 0;
  bool seleccionando = true;

  lcd.clear();
  lcd.print("Selec Disp:");

  while (seleccionando)
  {
    lcd.setCursor(0, 1);
    lcd.print("                ");
    lcd.setCursor(0, 1);

    if (opcion == 0)
    {
      lcd.print(">C1:");
      byte med = obtenerMedicamentoContenedor(MED_CONTAINER_1);
      lcd.print(LISTA_MEDICAMENTOS[med]);
    }
    else
    {
      lcd.print(">C2:");
      byte med = obtenerMedicamentoContenedor(MED_CONTAINER_2);
      lcd.print(LISTA_MEDICAMENTOS[med]);
    }

    if (botonNavegarPresionado())
    {
      opcion = (opcion + 1) % 2;
      esperarLiberacionBotonNavegar();
    }

    if (botonEnterPresionado())
    {
      esperarLiberacionBotonEnter();

      int canal = (opcion == 0) ? SERVO_CANAL_DISP1 : SERVO_CANAL_DISP2;
      int medIndex = (opcion == 0) ? obtenerMedicamentoContenedor(MED_CONTAINER_1) : obtenerMedicamentoContenedor(MED_CONTAINER_2);

      lcd.clear();
      lcd.print("Dispensando...");
      dispensar(canal);
      guardarEvento(rol, nullptr, medIndex);

      lcd.clear();
      lcd.print("Entregado OK");
      delay(1000);
      seleccionando = false;
    }
    delay(50);
  }
}

void menuAdministracion()
{
  lcd.clear();
  lcd.print("MENU ADMIN");
  bool salir = false;
  int subOpcion = 0;

  while (!salir)
  {
    lcd.setCursor(0, 1);
    lcd.print("                ");
    lcd.setCursor(0, 1);
    switch (subOpcion)
    {
    case 0:
      lcd.print(">Alta Usuario");
      break;
    case 1:
      lcd.print(">Reg Paciente");
      break;
    case 2:
      lcd.print(">Asignar Enf");
      break;
    case 3:
      lcd.print(">Config Hora");
      break;
    case 4:
      lcd.print(">Config Cont");
      break;
    case 5:
      lcd.print(">Salir");
      break;
    }

    if (botonNavegarPresionado())
    {
      subOpcion = (subOpcion + 1) % 6;
      esperarLiberacionBotonNavegar();
    }

    if (botonEnterPresionado())
    {
      switch (subOpcion)
      {
      case 0:
        registrarNuevoUsuario();
        break;
      case 1:
        registrarPacienteConEnfermero();
        break;
      case 2:
        asignarEnfermeroAPaciente();
        break;
      case 3:
        configurarHoraRTC();
        break;
      case 4:
        configurarMedicamentosContenedores();
        break;
      case 5:
        salir = true;
        break;
      }
      if (!salir)
      {
        lcd.clear();
        lcd.print("MENU ADMIN");
      }
      esperarLiberacionBotonEnter();
    }
    delay(50);
  }
}

void menuDoctor()
{
  int opcion = 0;
  bool salir = false;

  lcd.clear();
  lcd.print("Hola Doctor");
  while (!salir)
  {
    lcd.setCursor(0, 1);
    lcd.print("                ");
    lcd.setCursor(0, 1);

    switch (opcion)
    {
    case 0:
      lcd.print(">Dispensar");
      break;
    case 1:
      lcd.print(">Admin");
      break;
    case 2:
      lcd.print(">Mantenimiento");
      break;
    case 3:
      lcd.print(">Config Med");
      break;
    case 4:
      lcd.print(">Config Hora");
      break;
    case 5:
      lcd.print(">Config Cont");
      break;
    case 6:
      lcd.print(">Salir");
      break;
    }

    if (botonNavegarPresionado())
    {
      opcion = (opcion + 1) % 7;
      esperarLiberacionBotonNavegar();
    }

    if (botonEnterPresionado())
    {
      switch (opcion)
      {
      case 0:
        menuSeleccionManual(1);
        break;
      case 1:
        menuAdministracion();
        break;
      case 2:
        modoMantenimiento();
        break;
      case 3:
        configurarMedicamentoPaciente();
        break;
      case 4:
        configurarHoraRTC();
        break;
      case 5:
        configurarMedicamentosContenedores();
        break;
      case 6:
        salir = true;
        break;
      }
      if (!salir)
      {
        lcd.clear();
        lcd.print("Hola Doctor");
      }
      esperarLiberacionBotonEnter();
    }
    delay(50);
  }
}

void menuEnfermero()
{
  int opcion = 0;
  bool salir = false;

  lcd.clear();
  lcd.print("Hola Enfermero");
  while (!salir)
  {
    lcd.setCursor(0, 1);
    lcd.print("                ");
    lcd.setCursor(0, 1);

    if (opcion == 0)
      lcd.print(">Dispensar");
    else if (opcion == 1)
      lcd.print(">Config Med");
    else
      lcd.print(">Salir");
    if (botonNavegarPresionado())
    {
      opcion = (opcion + 1) % 3;
      esperarLiberacionBotonNavegar();
    }

    if (botonEnterPresionado())
    {
      if (opcion == 0)
      {
        menuSeleccionManual(2);
      }
      else if (opcion == 1)
      {
        configurarMedicamentoPaciente();
      }
      else
      {
        salir = true;
      }

      if (!salir)
      {
        lcd.clear();
        lcd.print("Hola Enfermero");
      }
      esperarLiberacionBotonEnter();
    }
    delay(50);
  }
}

void menuPaciente()
{
  byte uidPaciente[4];
  for (int i = 0; i < 4; i++)
    uidPaciente[i] = mfrc522.uid.uidByte[i];

  ConfigPaciente config = leerConfiguracionPaciente(uidPaciente);

  lcd.clear();
  lcd.print("Hola Paciente");
  lcd.setCursor(0, 1);
  lcd.print(">Tomar Med");

  long startWait = millis();
  while (millis() - startWait < 5000)
  {
    if (botonEnterPresionado())
    {
      esperarLiberacionBotonEnter();
      bool med1Disponible = config.med1.activo && esVentanaCorrecta(uidPaciente, 0) && yaPasoTiempoSeguro(uidPaciente, 0);
      bool med2Disponible = config.med2.activo && esVentanaCorrecta(uidPaciente, 1) && yaPasoTiempoSeguro(uidPaciente, 1);
      if (med1Disponible && med2Disponible)
      {
        lcd.clear();
        lcd.print("Seleccione Med:");
        lcd.setCursor(0, 1);
        lcd.print("M1");

        bool seleccionando = true;
        int opcionMed = 0;
        while (seleccionando)
        {
          lcd.setCursor(0, 1);
          lcd.print("   ");
          lcd.setCursor(0, 1);
          if (opcionMed == 0)
          {
            lcd.print(">M1");
          }
          else
          {
            lcd.print(">M2");
          }

          if (botonNavegarPresionado())
          {
            opcionMed = (opcionMed + 1) % 2;
            esperarLiberacionBotonNavegar();
          }

          if (botonEnterPresionado())
          {
            if (opcionMed == 0)
            {
              int contenedor = (config.med1.medicamentoIndex == obtenerMedicamentoContenedor(MED_CONTAINER_1)) ? SERVO_CANAL_DISP1 : SERVO_CANAL_DISP2;
              dispensar(contenedor);
              guardarEvento(3, uidPaciente, 0);
            }
            else
            {
              int contenedor = (config.med2.medicamentoIndex == obtenerMedicamentoContenedor(MED_CONTAINER_1)) ? SERVO_CANAL_DISP1 : SERVO_CANAL_DISP2;
              dispensar(contenedor);
              guardarEvento(3, uidPaciente, 1);
            }
            seleccionando = false;
            esperarLiberacionBotonEnter();
          }
          delay(50);
        }
      }
      else if (med1Disponible)
      {
        int contenedor = (config.med1.medicamentoIndex == obtenerMedicamentoContenedor(MED_CONTAINER_1)) ? SERVO_CANAL_DISP1 : SERVO_CANAL_DISP2;
        lcd.clear();
        lcd.print("Dispensando...");
        dispensar(contenedor);
        guardarEvento(3, uidPaciente, 0);
      }
      else if (med2Disponible)
      {
        int contenedor = (config.med2.medicamentoIndex == obtenerMedicamentoContenedor(MED_CONTAINER_1)) ? SERVO_CANAL_DISP1 : SERVO_CANAL_DISP2;
        lcd.clear();
        lcd.print("Dispensando...");
        dispensar(contenedor);
        guardarEvento(3, uidPaciente, 1);
      }
      else
      {
        lcd.clear();
        lcd.print("No disponible");
        delay(2000);
      }
      return;
    }
    if (botonNavegarPresionado())
      return;
    delay(50);
  }
}

void modoMantenimiento()
{
  lcd.clear();
  lcd.print("MODO MANTENIM.");

  moverServo(SERVO_CANAL_MANT, SERVOMIN, SERVOMAX);

  bool enMantenimiento = true;
  int opcion = 0;

  while (enMantenimiento)
  {
    lcd.setCursor(0, 1);
    lcd.print("                ");
    lcd.setCursor(0, 1);

    if (opcion == 0)
      lcd.print(">Abrir Disp 1");
    else if (opcion == 1)
      lcd.print(">Abrir Disp 2");
    else
      lcd.print(">Salir");

    if (botonNavegarPresionado())
    {
      opcion = (opcion + 1) % 3;
      esperarLiberacionBotonNavegar();
    }

    if (botonEnterPresionado())
    {
      esperarLiberacionBotonEnter();

      if (opcion == 0)
      {
        moverServo(SERVO_CANAL_DISP1, SERVOMIN, SERVOMAX);
        lcd.clear();
        lcd.print("D1 Abierto");
        lcd.setCursor(0, 1);
        lcd.print("OK para cerrar");

        while (!botonEnterPresionado())
        {
          delay(50);
        }
        esperarLiberacionBotonEnter();

        moverServo(SERVO_CANAL_DISP1, SERVOMAX, SERVOMIN);
        lcd.clear();
        lcd.print("MODO MANTENIM.");
      }
      else if (opcion == 1)
      {
        moverServo(SERVO_CANAL_DISP2, SERVOMIN, SERVOMAX);
        lcd.clear();
        lcd.print("D2 Abierto");
        lcd.setCursor(0, 1);
        lcd.print("OK para cerrar");

        while (!botonEnterPresionado())
        {
          delay(50);
        }
        esperarLiberacionBotonEnter();

        moverServo(SERVO_CANAL_DISP2, SERVOMAX, SERVOMIN);
        lcd.clear();
        lcd.print("MODO MANTENIM.");
      }
      else
      {
        enMantenimiento = false;
      }
    }
    delay(50);
  }

  moverServo(SERVO_CANAL_MANT, SERVOMAX, SERVOMIN);

  lcd.clear();
  lcd.print("Saliendo...");
  delay(600);
  lcd.clear();
}

void loop()
{
  static long lastTime = 0;
  static bool tarjetaDetectada = false;
  if (millis() - lastTime > 1000)
  {
    DateTime now = rtc.now();
    lcd.setCursor(0, 0);
    if (now.hour() < 10)
      lcd.print("0");
    lcd.print(now.hour());
    lcd.print(":");
    if (now.minute() < 10)
      lcd.print("0");
    lcd.print(now.minute());
    lcd.print(" Esperando   ");
    lcd.setCursor(0, 1);
    lcd.print("Acerque Tarjeta ");
    lastTime = millis();
  }

  if (!tarjetaDetectada && mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial())
  {
    tarjetaDetectada = true;
    byte rol = identificarUsuario(mfrc522.uid.uidByte);
    asm_blink_led();

    lcd.clear();
    if (rol == 1)
    {
      menuDoctor();
    }
    else if (rol == 2)
    {
      menuEnfermero();
    }
    else if (rol == 3)
    {
      menuPaciente();
    }
    else
    {
      lcd.print("No registrado");
      delay(1000);
    }
    lcd.clear();
    delay(1000);
    tarjetaDetectada = false;
  }
}