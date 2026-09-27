#include <WiFi.h>
#include <WebServer.h>
#include <EEPROM.h>
#include <ArduinoOTA.h>
#include <Base64.h>

// ============================================================================
// CONFIGURACIÓN SEGURA
// ============================================================================

// Pines
#define RELAY_PIN 18
#define STATUS_LED_PIN 2  // LED indicador de estado WiFi
#define EEPROM_SIZE 512

// Credenciales (usar EEPROM)
String ssid_stored = "";
String password_stored = "";

// Autenticación HTTP (cambiar en producción)
const char* http_username = "admin";
const char* http_password = "CHANGE_ME";

WebServer server(80);

// ============================================================================
// ESTRUCTURA DE ALMACENAMIENTO EN EEPROM
// ============================================================================

struct Config {
  char ssid[32];
  char password[64];
  char username[32];
  char api_password[64];
  uint32_t checksum;
};

Config config;

// ============================================================================
// VARIABLES DE ESTADO
// ============================================================================

unsigned long last_wifi_check = 0;
const unsigned long WIFI_CHECK_INTERVAL = 30000;  // Revisar cada 30s
bool led_state = false;

// ============================================================================
// FUNCIONES DE LED Y ESTADO
// ============================================================================

void updateStatusLED() {
  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(STATUS_LED_PIN, HIGH);
    led_state = true;
  } else {
    // Parpadeo lento si desconectado
    unsigned long now = millis();
    if ((now / 500) % 2 == 0) {
      digitalWrite(STATUS_LED_PIN, HIGH);
    } else {
      digitalWrite(STATUS_LED_PIN, LOW);
    }
  }
}

void printWiFiStatus() {
  Serial.print("[WiFi] Estado: ");
  switch (WiFi.status()) {
    case WL_CONNECTED:
      Serial.print("CONECTADO | IP: ");
      Serial.println(WiFi.localIP());
      break;
    case WL_DISCONNECTED:
      Serial.println("DESCONECTADO");
      break;
    case WL_SCAN_COMPLETED:
      Serial.println("ESCANEO COMPLETADO");
      break;
    case WL_NO_SSID_AVAIL:
      Serial.println("RED NO DISPONIBLE");
      break;
    case WL_CONNECT_FAILED:
      Serial.println("CONEXIÓN FALLÓ");
      break;
    default:
      Serial.println("ESTADO DESCONOCIDO");
  }
}

// ============================================================================
// FUNCIONES DE ALMACENAMIENTO
// ============================================================================

uint32_t calculateChecksum(const Config& cfg) {
  uint32_t sum = 0;
  const uint8_t* ptr = (uint8_t*)&cfg;
  for (int i = 0; i < sizeof(Config) - 4; i++) {
    sum += ptr[i];
  }
  return sum;
}

bool loadConfig() {
  Serial.println("[EEPROM] Leyendo configuración...");
  EEPROM.get(0, config);
  
  if (calculateChecksum(config) != config.checksum) {
    Serial.println("[EEPROM] ⚠️  Configuración inválida o corrupta");
    return false;
  }
  
  ssid_stored = String(config.ssid);
  password_stored = String(config.password);
  Serial.println("[EEPROM] ✓ Configuración cargada");
  return true;
}

void saveConfig(const char* new_ssid, const char* new_password) {
  Serial.println("[EEPROM] Guardando nueva configuración...");
  
  strncpy(config.ssid, new_ssid, sizeof(config.ssid) - 1);
  strncpy(config.password, new_password, sizeof(config.password) - 1);
  strncpy(config.username, http_username, sizeof(config.username) - 1);
  strncpy(config.api_password, http_password, sizeof(config.api_password) - 1);
  
  config.checksum = calculateChecksum(config);
  EEPROM.put(0, config);
  EEPROM.commit();
  
  ssid_stored = String(config.ssid);
  password_stored = String(config.password);
  Serial.println("[EEPROM] ✓ Configuración guardada");
}

// ============================================================================
// AUTENTICACIÓN HTTP
// ============================================================================

bool checkAuth() {
  if (!server.hasHeader("Authorization")) {
    server.sendHeader("WWW-Authenticate", "Basic realm=\"Control Remoto PC\"");
    server.send(401, "text/plain", "Autenticación requerida");
    Serial.println("[AUTH] ❌ Acceso sin autenticación denegado");
    return false;
  }

  String auth = server.header("Authorization");
  
  if (auth.substring(0, 6) != "Basic ") {
    server.send(401, "text/plain", "Método de autenticación inválido");
    return false;
  }

  String credentials = auth.substring(6);
  String expected = String(http_username) + ":" + String(http_password);
  String encoded_expected = "";
  
  // Verificar credenciales (comparación simple)
  if (credentials != base64_encode((uint8_t*)expected.c_str(), expected.length())) {
    server.send(401, "text/plain", "Credenciales inválidas");
    Serial.println("[AUTH] ❌ Credenciales rechazadas");
    return false;
  }

  Serial.println("[AUTH] ✓ Usuario autenticado");
  return true;
}

// ============================================================================
// FUNCIONES DE CONTROL DEL RELÉ
// ============================================================================

void powerPress() {
  Serial.println("[RELÉ] Pulsación corta (150ms)");
  digitalWrite(RELAY_PIN, LOW);
  delay(150);
  digitalWrite(RELAY_PIN, HIGH);
  Serial.println("[RELÉ] ✓ Pulsación completada");
}

void forceOff() {
  Serial.println("[RELÉ] Apagado forzado (6s)");
  digitalWrite(RELAY_PIN, LOW);
  delay(6000);
  digitalWrite(RELAY_PIN, HIGH);
  Serial.println("[RELÉ] ✓ Apagado forzado completado");
}

// ============================================================================
// HANDLERS HTTP
// ============================================================================

void handleRoot() {
  if (!checkAuth()) return;

  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Control PC Remoto</title>

<style>
* {
  margin: 0;
  padding: 0;
  box-sizing: border-box;
}

body {
  font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
  background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
  min-height: 100vh;
  display: flex;
  justify-content: center;
  align-items: center;
  padding: 20px;
}

.container {
  background: rgba(255, 255, 255, 0.1);
  backdrop-filter: blur(10px);
  border-radius: 20px;
  padding: 40px;
  box-shadow: 0 8px 32px rgba(0, 0, 0, 0.3);
  text-align: center;
  max-width: 500px;
  width: 100%;
}

h1 {
  color: white;
  margin-bottom: 10px;
  font-size: 32px;
}

.status {
  color: #4ade80;
  font-size: 14px;
  margin-bottom: 30px;
  padding: 10px;
  background: rgba(0, 0, 0, 0.2);
  border-radius: 10px;
}

.buttons {
  display: flex;
  flex-direction: column;
  gap: 15px;
}

button {
  padding: 15px 30px;
  font-size: 18px;
  border: none;
  border-radius: 10px;
  cursor: pointer;
  font-weight: bold;
  transition: transform 0.2s, box-shadow 0.2s;
  color: white;
}

button:hover {
  transform: translateY(-2px);
  box-shadow: 0 5px 20px rgba(0, 0, 0, 0.3);
}

button:active {
  transform: translateY(0);
}

.power-btn {
  background: linear-gradient(135deg, #28a745, #20c997);
}

.force-btn {
  background: linear-gradient(135deg, #dc3545, #fd7e14);
}

.info {
  color: rgba(255, 255, 255, 0.7);
  font-size: 12px;
  margin-top: 30px;
  padding-top: 20px;
  border-top: 1px solid rgba(255, 255, 255, 0.2);
}
</style>
</head>

<body>

<div class="container">
  <h1>🎮 Control Remoto PC</h1>
  <div class="status">✓ Sistema activo y protegido</div>
  
  <div class="buttons">
    <form action="/power" method="post" style="width: 100%;">
      <button type="submit" class="power-btn">⚡ ENCENDER PC</button>
    </form>
    
    <form action="/forceoff" method="post" style="width: 100%;">
      <button type="submit" class="force-btn">🛑 APAGADO FORZADO</button>
    </form>
  </div>
  
  <div class="info">
    Interfaz de control segura | Autenticación requerida
  </div>
</div>

</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
  Serial.println("[HTTP] GET / - Página principal enviada");
}

void handlePower() {
  if (!checkAuth()) return;
  
  powerPress();
  
  String response = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Control PC</title>
<style>
body { font-family: Arial; text-align: center; margin-top: 50px; background: #f0f0f0; }
.success { background: #28a745; color: white; padding: 20px; border-radius: 10px; margin: 20px auto; max-width: 400px; }
a { color: #667eea; text-decoration: none; }
</style>
</head>
<body>
<div class="success">
  <h2>✓ Pulsación enviada</h2>
  <p>El PC debe encenderse en breves momentos</p>
</div>
<a href="/">← Volver</a>
</body>
</html>
)rawliteral";

  server.send(200, "text/html", response);
  Serial.println("[HTTP] POST /power - Comando ejecutado");
}

void handleForceOff() {
  if (!checkAuth()) return;
  
  forceOff();
  
  String response = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Control PC</title>
<style>
body { font-family: Arial; text-align: center; margin-top: 50px; background: #f0f0f0; }
.warning { background: #dc3545; color: white; padding: 20px; border-radius: 10px; margin: 20px auto; max-width: 400px; }
a { color: #667eea; text-decoration: none; }
</style>
</head>
<body>
<div class="warning">
  <h2>⚠️ Apagado forzado enviado</h2>
  <p>El PC se apagará en 6 segundos</p>
</div>
<a href="/">← Volver</a>
</body>
</html>
)rawliteral";

  server.send(200, "text/html", response);
  Serial.println("[HTTP] POST /forceoff - Comando ejecutado");
}

void handleNotFound() {
  server.send(404, "text/plain", "Error 404: Recurso no encontrado");
  Serial.println("[HTTP] GET " + server.uri() + " - 404 No encontrado");
}

void handleStatus() {
  if (!checkAuth()) return;

  String status = "{";
  status += "\"wifi\":\"" + String(WiFi.status() == WL_CONNECTED ? "CONECTADO" : "DESCONECTADO") + "\",";
  status += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  status += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  status += "\"uptime\":" + String(millis() / 1000) + "";
  status += "}";

  server.send(200, "application/json", status);
  Serial.println("[HTTP] GET /status - JSON enviado");
}

// ============================================================================
// WIFI Y RECONEXIÓN
// ============================================================================

void reconnectWiFi() {
  unsigned long now = millis();
  
  if (now - last_wifi_check < WIFI_CHECK_INTERVAL) {
    return;
  }
  
  last_wifi_check = now;

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] ⚠️  Desconectado, reconectando...");
    
    WiFi.disconnect();
    WiFi.begin(ssid_stored.c_str(), password_stored.c_str());

    unsigned long start = millis();
    int attempts = 0;

    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
      delay(500);
      Serial.print(".");
      attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println();
      printWiFiStatus();
    } else {
      Serial.println();
      Serial.println("[WiFi] ❌ Error en reconexión después de " + String(attempts) + " intentos");
    }
  }
}

// ============================================================================
// ACTUALIZACIÓN OTA (Over-The-Air)
// ============================================================================

void setupOTA() {
  ArduinoOTA.setHostname("PoweRemote");
  ArduinoOTA.setPassword(http_password);
  
  ArduinoOTA.onStart([]() {
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "FIRMWARE" : "FILESYSTEM";
    Serial.println("[OTA] Iniciando actualización: " + type);
    digitalWrite(STATUS_LED_PIN, LOW);  // Apagar LED durante actualización
  });

  ArduinoOTA.onEnd([]() {
    Serial.println();
    Serial.println("[OTA] ✓ Actualización completada");
    digitalWrite(STATUS_LED_PIN, HIGH);
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    static unsigned long last_print = 0;
    if (millis() - last_print > 1000) {
      Serial.printf("[OTA] Progreso: %u%%\r\n", (progress / (total / 100)));
      last_print = millis();
    }
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });

  ArduinoOTA.begin();
  Serial.println("[OTA] ✓ Servicio habilitado");
}

// ============================================================================
// SETUP
// ============================================================================

void setup() {
  Serial.begin(115200);
  delay(100);

  Serial.println("\n\n");
  Serial.println("╔═══════════════════════════════════════════════════════╗");
  Serial.println("║         🎮 CONTROL REMOTO PC - SISTEMA MEJORADO       ║");
  Serial.println("║              Versión 2.0 - Segura & Robusta          ║");
  Serial.println("╚═══════════════════════════════════════════════════════╝");
  Serial.println();

  // Inicializar pines
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);    // Relé desactivado
  digitalWrite(STATUS_LED_PIN, LOW); // LED apagado inicialmente

  // Inicializar EEPROM
  EEPROM.begin(EEPROM_SIZE);
  
  // Cargar configuración (si existe) o usar por defecto
  if (!loadConfig()) {
    Serial.println("[CONFIG] Usando valores por defecto");
    ssid_stored = "YOUR_WIFI_SSID";
    password_stored = "YOUR_WIFI_PASSWORD";
  }

  Serial.println("\n[WiFi] Conectando a: " + ssid_stored);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid_stored.c_str(), password_stored.c_str());

  // Esperar conexión inicial con timeout
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(300);
    Serial.print(".");
    digitalWrite(STATUS_LED_PIN, millis() % 1000 < 500 ? HIGH : LOW);  // Parpadear
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    printWiFiStatus();
    digitalWrite(STATUS_LED_PIN, HIGH);
  } else {
    Serial.println("[WiFi] ❌ No conectado. El sistema continuará intentando reconectar.");
    digitalWrite(STATUS_LED_PIN, LOW);
  }

  // Configurar rutas HTTP
  server.on("/", HTTP_GET, handleRoot);
  server.on("/power", HTTP_POST, handlePower);
  server.on("/forceoff", HTTP_POST, handleForceOff);
  server.on("/status", HTTP_GET, handleStatus);
  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("[HTTP] ✓ Servidor web iniciado en puerto 80");

  // Configurar OTA
  setupOTA();

  Serial.println();
  Serial.println("╔═══════════════════════════════════════════════════════╗");
  Serial.println("║          🚀 SISTEMA LISTO PARA OPERAR 🚀            ║");
  Serial.println("╚═══════════════════════════════════════════════════════╝");
  Serial.println();
}

// ============================================================================
// LOOP PRINCIPAL
// ============================================================================

void loop() {
  // Manejar requests HTTP
  server.handleClient();

  // Reconectar WiFi si es necesario
  reconnectWiFi();

  // Actualizar LED de estado
  updateStatusLED();

  // Procesar OTA
  ArduinoOTA.handle();

  delay(10);  // Pequeño delay para evitar saturar el procesador
}
