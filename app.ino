#include <WiFi.h>
#include <WebServer.h>
#include <EEPROM.h>
#include <ArduinoOTA.h>

#if __has_include("secrets.h")
  #include "secrets.h"
#elif __has_include("secrets.example.h")
  #include "secrets.example.h"
#else
  const char* HTTP_USERNAME = "admin";
  const char* HTTP_PASSWORD = "CHANGE_ME";
  const char* AP_SSID = "PoweRemote-Setup";
  const char* AP_PASSWORD = "CHANGE_ME_AP";
  const char* WIFI_SSID = "YOUR_WIFI_SSID";
  const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
  const char* AMBI_IP = "192.168.0.3";
#endif

#include <Matter.h>
#include <MatterEndpoints/MatterOnOffLight.h>
#include "ping/ping_sock.h"

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
const char* http_username = HTTP_USERNAME;
const char* http_password = HTTP_PASSWORD;

// Modo AP (Access Point)
const char* ap_ssid = AP_SSID;
const char* ap_password = AP_PASSWORD;
bool ap_mode = false;

WebServer server(80);

// Endpoint Matter de tipo On/Off
MatterOnOffLight matter_pc;

// Declaraciones previas de funciones
void powerPress();
void forceOff();
void setupAP();
void registerHTTPRoutes();
void handleAPRoot();
String base64_encode_simple(const uint8_t* data, size_t len);

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
unsigned long last_ap_check = 0;
const unsigned long AP_CHECK_INTERVAL = 10000;  // Revisar red cada 10s en modo AP

// Estado del PC detectado mediante la ESP32 Ambi en la red local.
enum PcPowerState { PC_UNKNOWN, PC_ON, PC_OFF };
PcPowerState pc_power_state = PC_UNKNOWN;
unsigned long last_ambi_ping = 0;
const unsigned long AMBI_PING_INTERVAL = 10000;
const uint8_t AMBI_ON_CONFIRMATIONS = 2;
const uint8_t AMBI_OFF_CONFIRMATIONS = 3;
uint8_t ambi_success_count = 0;
uint8_t ambi_failure_count = 0;
volatile bool ambi_ping_reply = false;
volatile bool ambi_ping_finished = false;
bool ambi_ping_active = false;
esp_ping_handle_t ambi_ping_handle = nullptr;

// ============================================================================
// CALLBACK MATTER (Google Home / Asistentes)
// ============================================================================

bool onMatterChange(bool state) {
  Serial.printf("\n[Matter] Comando recibido desde Google Home: %s\n", state ? "ON" : "OFF");
  
  // Tanto ON como OFF realizan una pulsación corta del botón de encendido (300ms)
  // El PC gestionará el arranque (si está apagado) o el apagado limpio por SO (si está encendido)
  powerPress();
  
  return true;
}

// ============================================================================
void onAmbiPingSuccess(esp_ping_handle_t hdl, void *args) {
  ambi_ping_reply = true;
}

void onAmbiPingEnd(esp_ping_handle_t hdl, void *args) {
  ambi_ping_finished = true;
}

const char* getPcPowerStateLabel() {
  if (pc_power_state == PC_ON) return "ENCENDIDO";
  if (pc_power_state == PC_OFF) return "APAGADO";
  return "VERIFICANDO";
}

void beginAmbiPing() {
  if (WiFi.status() != WL_CONNECTED || ambi_ping_active) return;

  IPAddress ambi_address;
  if (!ambi_address.fromString(AMBI_IP)) {
    Serial.println("[PC] IP de Ambi inválida");
    return;
  }

  ip_addr_t target_addr;
  target_addr.type = IPADDR_TYPE_V4;
  target_addr.u_addr.ip4.addr = (uint32_t)ambi_address;

  esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
  ping_config.target_addr = target_addr;
  ping_config.count = 1;
  ping_config.timeout_ms = 1000;

  esp_ping_callbacks_t callbacks = {};
  callbacks.on_ping_success = onAmbiPingSuccess;
  callbacks.on_ping_end = onAmbiPingEnd;

  ambi_ping_reply = false;
  ambi_ping_finished = false;
  if (esp_ping_new_session(&ping_config, &callbacks, &ambi_ping_handle) != ESP_OK) {
    Serial.println("[PC] No se pudo iniciar ping a Ambi");
    return;
  }

  ambi_ping_active = true;
  esp_ping_start(ambi_ping_handle);
}

void updatePcPowerState() {
  unsigned long now = millis();

  if (ambi_ping_active && ambi_ping_finished) {
    esp_ping_delete_session(ambi_ping_handle);
    ambi_ping_handle = nullptr;
    ambi_ping_active = false;

    if (ambi_ping_reply) {
      if (ambi_success_count < AMBI_ON_CONFIRMATIONS) ambi_success_count++;
      ambi_failure_count = 0;
      if (ambi_success_count >= AMBI_ON_CONFIRMATIONS && pc_power_state != PC_ON) {
        pc_power_state = PC_ON;
        Serial.println("[PC] ✓ ENCENDIDO (Ambi responde en la red)");
      }
    } else {
      if (ambi_failure_count < AMBI_OFF_CONFIRMATIONS) ambi_failure_count++;
      ambi_success_count = 0;
      if (ambi_failure_count >= AMBI_OFF_CONFIRMATIONS && pc_power_state != PC_OFF) {
        pc_power_state = PC_OFF;
        Serial.println("[PC] ○ APAGADO (Ambi no responde)");
      }
    }
  }

  if (!ambi_ping_active && now - last_ambi_ping >= AMBI_PING_INTERVAL) {
    last_ambi_ping = now;
    beginAmbiPing();
  }
}

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
  // En modo AP, permitir acceso sin autenticación
  if (ap_mode) {
    return true;
  }

  if (!server.hasHeader("Authorization")) {
    server.sendHeader("WWW-Authenticate", "Basic realm=\"Control Remoto PC\"");
    server.send(401, "text/plain", "Autenticación requerida");
    Serial.println("[AUTH] ❌ Acceso sin autenticación denegado");
    return false;
  }

  String auth = server.header("Authorization");
  
  // Verificar que comience con "Basic "
  if (auth.substring(0, 6) != "Basic ") {
    server.send(401, "text/plain", "Método de autenticación inválido");
    Serial.println("[AUTH] ❌ Método inválido");
    return false;
  }

  // Obtener credenciales codificadas en base64
  String encoded_credentials = auth.substring(6);
  
  // Crear las credenciales esperadas en formato "usuario:contraseña"
  String expected_credentials = String(http_username) + ":" + String(http_password);
  
  // Codificar manualmente en base64 para comparación
  String encoded_expected = base64_encode_simple((uint8_t*)expected_credentials.c_str(), expected_credentials.length());
  
  if (encoded_credentials != encoded_expected) {
    server.send(401, "text/plain", "Credenciales inválidas");
    Serial.println("[AUTH] ❌ Credenciales rechazadas");
    return false;
  }

  Serial.println("[AUTH] ✓ Usuario autenticado");
  return true;
}

// Función simple de codificación base64
String base64_encode_simple(const uint8_t* data, size_t len) {
  static const char* base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String encoded = "";
  
  for (size_t i = 0; i < len; i += 3) {
    uint8_t b1 = data[i];
    uint8_t b2 = (i + 1 < len) ? data[i + 1] : 0;
    uint8_t b3 = (i + 2 < len) ? data[i + 2] : 0;
    
    uint32_t n = ((uint32_t)b1 << 16) | ((uint32_t)b2 << 8) | ((uint32_t)b3);
    
    encoded += base64_chars[(n >> 18) & 63];
    encoded += base64_chars[(n >> 12) & 63];
    
    if (i + 1 < len) {
      encoded += base64_chars[(n >> 6) & 63];
    } else {
      encoded += '=';
    }
    
    if (i + 2 < len) {
      encoded += base64_chars[n & 63];
    } else {
      encoded += '=';
    }
  }
  
  return encoded;
}

// ============================================================================
// FUNCIONES DE CONTROL DEL RELÉ
// ============================================================================

void powerPress() {
  Serial.println("[RELÉ] Pulsación corta (300ms)");
  digitalWrite(RELAY_PIN, LOW);   // Relé activo (lógica en LOW)
  delay(300);
  digitalWrite(RELAY_PIN, HIGH);  // Relé desactivado
  Serial.println("[RELÉ] ✓ Pulsación completada");
}

void forceOff() {
  Serial.println("[RELÉ] Apagado forzado (6s)");
  digitalWrite(RELAY_PIN, LOW);   // Relé activo
  delay(6000);
  digitalWrite(RELAY_PIN, HIGH);  // Relé desactivado
  Serial.println("[RELÉ] ✓ Apagado forzado completado");
}

// ============================================================================
// HANDLERS HTTP
// ============================================================================

void handleRoot() {
  if (!checkAuth()) return;

  String matter_status_html = "";
  if (Matter.isDeviceCommissioned()) {
    matter_status_html = "<div style='color: #4ade80; font-size: 13px; margin-bottom: 10px;'>✓ Matter Vinculado a Google Home</div>"
                         "<form action='/matter-reset' method='post' style='margin-bottom: 15px;'>"
                         "  <button type='submit' style='padding: 8px 16px; font-size: 12px; background: rgba(220, 53, 69, 0.8); border-radius: 6px; color: white; border: none; cursor: pointer;'>🔄 Restablecer Matter / Desvincular</button>"
                         "</form>";
  } else {
    String pairingCode = Matter.getManualPairingCode();
    matter_status_html = "<div style='background: rgba(33, 150, 243, 0.2); padding: 10px; border-radius: 8px; font-size: 13px; margin-bottom: 15px; color: #64b5f6;'>"
                         "📱 <strong>Código Matter Google Home:</strong> " + pairingCode + "</div>";
  }

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
  margin-bottom: 20px;
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

.config-btn {
  background: linear-gradient(135deg, #6f42c1, #5a32a3);
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
)rawliteral";

  html += matter_status_html;

  String pc_color = pc_power_state == PC_ON ? "#4ade80" : (pc_power_state == PC_OFF ? "#f87171" : "#facc15");
  html += "<div style='color: " + pc_color + "; font-size: 14px; margin-bottom: 15px;'>PC: <strong>" + getPcPowerStateLabel() + "</strong></div>";

  html += R"rawliteral(
  <div class="buttons">
    <form action="/power" method="post" style="width: 100%;">
      <button type="submit" class="power-btn">⚡ ENCENDER / APAGAR PC</button>
    </form>
    
    <form action="/forceoff" method="post" style="width: 100%;">
      <button type="submit" class="force-btn">🛑 APAGADO FORZADO (6s)</button>
    </form>

    <a href="/config" style="width: 100%; text-decoration: none;">
      <button type="button" class="config-btn" style="width: 100%;">⚙️ CONFIGURAR WiFi</button>
    </a>
  </div>
  
  <div class="info">
    Interfaz de control segura | Compatible con Matter y Google Home
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
  
  // Sincronizar estado lógico de Matter si está inicializado
  if (Matter.isDeviceCommissioned()) {
    matter_pc.setOnOff(!matter_pc.getOnOff());
  }

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
  <h2>✓ Pulsación enviada (300ms)</h2>
  <p>El botón de encendido del PC ha sido accionado.</p>
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
  
  // Sincronizar estado lógico de Matter si está disponible
  if (Matter.isDeviceCommissioned()) {
    matter_pc.setOnOff(false);
  }

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
  <h2>⚠️ Apagado forzado enviado (6s)</h2>
  <p>El PC se apagará por corte sostenido del botón de encendido.</p>
</div>
<a href="/">← Volver</a>
</body>
</html>
)rawliteral";

  server.send(200, "text/html", response);
  Serial.println("[HTTP] POST /forceoff - Comando ejecutado");
}

void handleMatterReset() {
  if (!checkAuth()) return;
  
  Serial.println("\n[Matter] 🔄 Solicitud de restablecimiento recibida...");
  Serial.println("[Matter] Ejecutando Matter.decommission()...");
  Matter.decommission();
  
  String response = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Matter Restablecido</title>
<style>
body { font-family: Arial; text-align: center; margin-top: 50px; background: #f0f0f0; }
.info { background: #2196f3; color: white; padding: 20px; border-radius: 10px; margin: 20px auto; max-width: 400px; }
a { color: #667eea; text-decoration: none; display: block; margin-top: 20px; }
</style>
</head>
<body>
<div class="info">
  <h2>🔄 Matter Restablecido</h2>
  <p>El dispositivo se reiniciará en 3 segundos y volverá a mostrar el código de emparejamiento.</p>
</div>
<a href="/">← Volver</a>
</body>
</html>
)rawliteral";

  server.send(200, "text/html", response);
  delay(3000);
  ESP.restart();
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
  status += "\"uptime\":" + String(millis() / 1000) + ",";
  status += "\"matter_commissioned\":" + String(Matter.isDeviceCommissioned() ? "true" : "false") + ",";
  status += "\"matter_pairing_code\":\"" + Matter.getManualPairingCode() + "\",";
  status += "\"pc_power\":\"" + String(getPcPowerStateLabel()) + "\"";
  status += "}";

  server.send(200, "application/json", status);
  Serial.println("[HTTP] GET /status - JSON enviado");
}

void handleConfig() {
  if (!checkAuth()) return;

  if (server.method() == HTTP_GET) {
    // Mostrar página de configuración
    String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Configuración WiFi</title>

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
  max-width: 600px;
  width: 100%;
}

h1 {
  color: white;
  margin-bottom: 10px;
  font-size: 28px;
}

.subtitle {
  color: rgba(255, 255, 255, 0.7);
  margin-bottom: 30px;
  font-size: 14px;
}

.form-group {
  margin-bottom: 20px;
}

label {
  display: block;
  color: white;
  margin-bottom: 8px;
  font-weight: 500;
}

input {
  width: 100%;
  padding: 12px;
  border: 2px solid rgba(255, 255, 255, 0.2);
  border-radius: 8px;
  background: rgba(255, 255, 255, 0.1);
  color: white;
  font-size: 14px;
  transition: border-color 0.3s;
}

input::placeholder {
  color: rgba(255, 255, 255, 0.5);
}

input:focus {
  outline: none;
  border-color: #4ade80;
  background: rgba(255, 255, 255, 0.15);
}

.button-group {
  display: flex;
  gap: 10px;
  margin-top: 30px;
}

button {
  flex: 1;
  padding: 12px;
  border: none;
  border-radius: 8px;
  font-weight: bold;
  font-size: 14px;
  cursor: pointer;
  transition: transform 0.2s, box-shadow 0.2s;
  color: white;
}

.btn-save {
  background: linear-gradient(135deg, #28a745, #20c997);
}

.btn-cancel {
  background: linear-gradient(135deg, #6c757d, #495057);
}

button:hover {
  transform: translateY(-2px);
  box-shadow: 0 5px 20px rgba(0, 0, 0, 0.3);
}

button:active {
  transform: translateY(0);
}

.warning {
  background: rgba(255, 193, 7, 0.2);
  border: 2px solid rgba(255, 193, 7, 0.5);
  color: #ffc107;
  padding: 12px;
  border-radius: 8px;
  margin-bottom: 20px;
  font-size: 13px;
}

.info {
  background: rgba(33, 150, 243, 0.2);
  border: 2px solid rgba(33, 150, 243, 0.5);
  color: #2196f3;
  padding: 12px;
  border-radius: 8px;
  margin-bottom: 20px;
  font-size: 13px;
}
</style>
</head>

<body>

<div class="container">
  <h1>⚙️ Configuración de Red</h1>
  <p class="subtitle">Cambiar credenciales WiFi de forma segura</p>
  
  <div class="warning">
    ⚠️ Cambiar la red WiFi reiniciará la conexión. Espera a que se reconecte.
  </div>
  
  <div class="info">
    ℹ️ Las credenciales se guardarán en memoria EEPROM de forma persistente.
  </div>

  <form action="/config" method="POST">
    <div class="form-group">
      <label for="ssid">Nombre de Red (SSID)</label>
      <input type="text" id="ssid" name="ssid" placeholder="Ej: MiRed" required>
    </div>

    <div class="form-group">
      <label for="password">Contraseña WiFi</label>
      <input type="password" id="password" name="password" placeholder="Ej: MiPassword123" required>
    </div>

    <div class="button-group">
      <button type="submit" class="btn-save">💾 Guardar</button>
      <button type="button" class="btn-cancel" onclick="window.location='/'">❌ Cancelar</button>
    </div>
  </form>
</div>

</body>
</html>
)rawliteral";

    server.send(200, "text/html", html);
    Serial.println("[HTTP] GET /config - Página de configuración enviada");

  } else if (server.method() == HTTP_POST) {
    // Procesar configuración
    if (!server.hasArg("ssid") || !server.hasArg("password")) {
      server.send(400, "text/plain", "Error: Parámetros faltantes");
      Serial.println("[CONFIG] ❌ Error: Parámetros incompletos");
      return;
    }

    String new_ssid = server.arg("ssid");
    String new_password = server.arg("password");

    // Validar longitudes
    if (new_ssid.length() < 1 || new_ssid.length() > 31) {
      server.send(400, "text/plain", "Error: SSID debe tener 1-31 caracteres");
      Serial.println("[CONFIG] ❌ SSID inválido");
      return;
    }

    if (new_password.length() < 8 || new_password.length() > 63) {
      server.send(400, "text/plain", "Error: Contraseña debe tener 8-63 caracteres");
      Serial.println("[CONFIG] ❌ Contraseña inválida");
      return;
    }

    // Guardar configuración
    saveConfig(new_ssid.c_str(), new_password.c_str());

    String response = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Configuración Guardada</title>
<style>
body { font-family: Arial; text-align: center; margin-top: 50px; background: #f0f0f0; }
.success { background: #28a745; color: white; padding: 20px; border-radius: 10px; margin: 20px auto; max-width: 400px; }
.info { background: #2196f3; color: white; padding: 15px; margin: 20px auto; max-width: 400px; border-radius: 10px; font-size: 13px; line-height: 1.6; }
a { color: #667eea; text-decoration: none; margin-top: 20px; display: block; }
</style>
</head>
<body>
<div class="success">
  <h2>✓ Configuración guardada</h2>
  <p>El dispositivo se reiniciará en 3 segundos...</p>
</div>
<div class="info">
  Estará intentando conectar a la nueva red.<br>
  Si el LED queda fijo, la conexión fue exitosa.
</div>
</body>
</html>
)rawliteral";

    server.send(200, "text/html", response);
    Serial.println("[CONFIG] ✓ Nueva red guardada: " + new_ssid);
    Serial.println("[CONFIG] ⏱️  Reiniciando en 3 segundos...");
    
    // Reiniciar después de 3 segundos
    delay(3000);
    ESP.restart();
  }
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
// SALIR DE MODO AP Y CONECTAR A STA
// ============================================================================

void exitAPMode() {
  Serial.println("\n[AP→STA] Cambiando de modo Access Point a STA...");
  
  ap_mode = false;
  
  // Detener servidor
  server.stop();
  
  // Cambiar a modo STA
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid_stored.c_str(), password_stored.c_str());
  
  // Esperar conexión con timeout
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(300);
    Serial.print(".");
    digitalWrite(STATUS_LED_PIN, millis() % 1000 < 500 ? HIGH : LOW);
  }
  
  Serial.println();
  
  if (WiFi.status() == WL_CONNECTED) {
    printWiFiStatus();
    digitalWrite(STATUS_LED_PIN, HIGH);
    Serial.println("[AP→STA] ✓ Cambiado a modo STA exitosamente");
  } else {
    Serial.println("[AP→STA] ❌ No se pudo conectar, volviendo a AP...");
    setupAP();
    return;
  }
  
  // Reiniciar servidor y reregistrar rutas para STA mode
  server.begin();
  registerHTTPRoutes();
  Serial.println("[HTTP] ✓ Servidor reiniciado en modo STA");
}

// ============================================================================
// VERIFICAR DISPONIBILIDAD DE RED EN MODO AP
// ============================================================================

void checkNetworkInAPMode() {
  unsigned long now = millis();
  
  if (!ap_mode) {
    return;
  }
  
  if (now - last_ap_check < AP_CHECK_INTERVAL) {
    return;
  }
  
  last_ap_check = now;
  
  Serial.println("[AP] Escaneando redes disponibles...");
  
  // Escanear redes WiFi
  int networks = WiFi.scanNetworks();
  
  if (networks == 0) {
    Serial.println("[AP] ℹ️  No se encontró la red configurada");
    return;
  }
  
  // Buscar la red configurada
  for (int i = 0; i < networks; i++) {
    String ssid = WiFi.SSID(i);
    
    if (ssid == ssid_stored) {
      Serial.println("[AP] ✓ Red disponible: " + ssid);
      Serial.println("[AP] → Intentando cambiar a modo normal...");
      exitAPMode();
      WiFi.scanDelete();
      return;
    }
  }
  
  WiFi.scanDelete();
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
// REGISTRAR RUTAS HTTP
// ============================================================================

void registerHTTPRoutes() {
  // Registrar rutas según el modo (se sobrescriben automáticamente)
  server.on("/", HTTP_GET, ap_mode ? handleAPRoot : handleRoot);
  server.on("/power", HTTP_POST, handlePower);
  server.on("/forceoff", HTTP_POST, handleForceOff);
  server.on("/matter-reset", HTTP_POST, handleMatterReset);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/config", HTTP_GET, handleConfig);
  server.on("/config", HTTP_POST, handleConfig);
  server.onNotFound(handleNotFound);
  
  Serial.println("[HTTP] ✓ Rutas registradas para modo " + String(ap_mode ? "AP" : "STA"));
}

// ============================================================================
// MODO AP (ACCESS POINT / PUNTO DE ACCESO)
// ============================================================================

void setupAP() {
  Serial.println("\n[AP] Iniciando modo Access Point...");
  
  ap_mode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_password);
  
  IPAddress ap_ip(192, 168, 4, 1);
  WiFi.softAPConfig(ap_ip, ap_ip, IPAddress(255, 255, 255, 0));
  
  Serial.println("[AP] ✓ Red WiFi creada");
  Serial.println("[AP] SSID: " + String(ap_ssid));
  Serial.println("[AP] Contraseña: " + String(ap_password));
  Serial.println("[AP] IP: " + WiFi.softAPIP().toString());
  Serial.println("[AP] ℹ️  Conectate a esta red y accede a: http://192.168.4.1");
}

void handleAPRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Configuración PoweRemote</title>

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
  max-width: 600px;
  width: 100%;
}

h1 {
  color: white;
  margin-bottom: 10px;
  font-size: 28px;
}

.subtitle {
  color: rgba(255, 255, 255, 0.7);
  margin-bottom: 30px;
  font-size: 14px;
}

.alert {
  background: rgba(255, 193, 7, 0.2);
  border: 2px solid rgba(255, 193, 7, 0.5);
  color: #ffc107;
  padding: 15px;
  border-radius: 8px;
  margin-bottom: 25px;
  font-size: 13px;
  line-height: 1.6;
}

.form-group {
  margin-bottom: 20px;
}

label {
  display: block;
  color: white;
  margin-bottom: 8px;
  font-weight: 500;
}

input {
  width: 100%;
  padding: 12px;
  border: 2px solid rgba(255, 255, 255, 0.2);
  border-radius: 8px;
  background: rgba(255, 255, 255, 0.1);
  color: white;
  font-size: 14px;
  transition: border-color 0.3s;
}

input::placeholder {
  color: rgba(255, 255, 255, 0.5);
}

input:focus {
  outline: none;
  border-color: #4ade80;
  background: rgba(255, 255, 255, 0.15);
}

.button-group {
  display: flex;
  gap: 10px;
  margin-top: 30px;
}

button {
  flex: 1;
  padding: 12px;
  border: none;
  border-radius: 8px;
  font-weight: bold;
  font-size: 14px;
  cursor: pointer;
  transition: transform 0.2s, box-shadow 0.2s;
  color: white;
}

.btn-save {
  background: linear-gradient(135deg, #28a745, #20c997);
}

.btn-cancel {
  background: linear-gradient(135deg, #6c757d, #495057);
}

button:hover {
  transform: translateY(-2px);
  box-shadow: 0 5px 20px rgba(0, 0, 0, 0.3);
}

button:active {
  transform: translateY(0);
}

.info-box {
  background: rgba(33, 150, 243, 0.2);
  border: 2px solid rgba(33, 150, 243, 0.5);
  color: #2196f3;
  padding: 12px;
  border-radius: 8px;
  margin-top: 20px;
  font-size: 13px;
  line-height: 1.6;
}
</style>
</head>

<body>

<div class="container">
  <h1>🔧 Configurar PoweRemote</h1>
  <p class="subtitle">Modo de configuración local</p>
  
  <div class="alert">
    ⚠️ El dispositivo NO pudo conectar a la red WiFi configurada. 
    Configura las credenciales correctas para continuar.
  </div>

  <form action="/config" method="POST">
    <div class="form-group">
      <label for="ssid">Nombre de Red (SSID)</label>
      <input type="text" id="ssid" name="ssid" placeholder="Ej: MiRed" required>
    </div>

    <div class="form-group">
      <label for="password">Contraseña WiFi</label>
      <input type="password" id="password" name="password" placeholder="Mín. 8 caracteres" required>
    </div>

    <div class="button-group">
      <button type="submit" class="btn-save">💾 Guardar y Conectar</button>
    </div>
  </form>

  <div class="info-box">
    <strong>💡 Instrucciones:</strong><br>
    1. Ingresa el nombre y contraseña de tu red WiFi<br>
    2. Haz clic en "Guardar y Conectar"<br>
    3. El dispositivo se reconectará automáticamente<br>
    4. Una vez conectado, accede a la interfaz principal
  </div>
</div>

</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
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
  Serial.println("║          Versión 2.0 + Soporte Matter / Google Home   ║");
  Serial.println("╚═══════════════════════════════════════════════════════╝");
  Serial.println();

  // Inicializar pines
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);    // Relé desactivado (lógica en LOW)
  digitalWrite(STATUS_LED_PIN, LOW); // LED apagado inicialmente

  // Inicializar EEPROM
  EEPROM.begin(EEPROM_SIZE);
  
  // Cargar configuración (si existe) o usar por defecto
  if (!loadConfig()) {
    Serial.println("[CONFIG] Usando valores por defecto");
    ssid_stored = WIFI_SSID;
    password_stored = WIFI_PASSWORD;
  }

  Serial.println("\n[WiFi] Conectando a: " + ssid_stored);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid_stored.c_str(), password_stored.c_str());

  // Esperar conexión inicial con timeout
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(300);
    Serial.print(".");
    digitalWrite(STATUS_LED_PIN, millis() % 1000 < 500 ? HIGH : LOW);
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    printWiFiStatus();
    digitalWrite(STATUS_LED_PIN, HIGH);
  } else {
    Serial.println("[WiFi] ❌ No conectado. Iniciando modo Access Point...");
    digitalWrite(STATUS_LED_PIN, LOW);
    setupAP();
  }

  // Iniciar servidor primero
  server.begin();
  Serial.println("[HTTP] ✓ Servidor web iniciado en puerto 80");
  
  // Registrar rutas HTTP DESPUÉS de iniciar el servidor
  registerHTTPRoutes();

  // Configurar OTA
  setupOTA();

  // Configurar e inicializar Matter
  Serial.println("\n[Matter] Configurando endpoint On/Off...");
  matter_pc.begin();
  matter_pc.onChange(onMatterChange);
  
  Serial.println("[Matter] Iniciando stack Matter...");
  Matter.begin();

  if (!Matter.isDeviceCommissioned()) {
    Serial.println();
    Serial.println("╔═══════════════════════════════════════════════════════╗");
    Serial.println("║     📱 DISPOSITIVO MATTER LISTO PARA EMPAREJAR        ║");
    Serial.println("╠═══════════════════════════════════════════════════════╣");
    Serial.printf("║ Código Manual: %-38s ║\n", Matter.getManualPairingCode().c_str());
    Serial.printf("║ QR Code URL:   %-38s ║\n", Matter.getOnboardingQRCodeUrl().c_str());
    Serial.println("║ Abre Google Home -> Añadir dispositivo -> Matter     ║");
    Serial.println("╚═══════════════════════════════════════════════════════╝\n");
  } else {
    Serial.println("[Matter] ✓ Dispositivo ya emparejado con red Matter (Google Home)");
  }

  Serial.println();
  Serial.println("╔═══════════════════════════════════════════════════════╗");
  Serial.println("║          🚀 SISTEMA LISTO PARA OPERAR 🚀             ║");
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
  
  // Si está en modo AP, verificar disponibilidad de red
  checkNetworkInAPMode();

  // Detectar si el PC está encendido mediante la ESP32 Ambi.
  updatePcPowerState();

  // Actualizar LED de estado
  updateStatusLED();

  // Procesar OTA
  ArduinoOTA.handle();

  delay(10);  // Pequeño delay para evitar saturar el procesador
}
