# PoweRemote

Firmware para ESP32 que permite encender o forzar el apagado de un PC mediante un relé, desde una interfaz web protegida. Si no logra conectarse a la red Wi‑Fi guardada, el dispositivo abre su propia red de configuración.

## Características

- Pulsación corta del botón de encendido del PC.
- Apagado forzado mediante una pulsación de 6 segundos.
- Panel web con autenticación HTTP Basic.
- Configuración de la red Wi‑Fi desde el navegador.
- Modo punto de acceso (AP) como respaldo cuando no hay conexión Wi‑Fi.
- Almacenamiento de configuración en EEPROM.
- Actualización inalámbrica mediante Arduino OTA.
- LED de estado de conexión.

## Hardware necesario

- ESP32.
- Módulo relé compatible con 3.3 V o transistor/optoacoplador equivalente.
- Cables Dupont.
- Acceso a los dos pines del botón de encendido de la placa base del PC.

## Conexiones

| ESP32 | Conectar a |
| --- | --- |
| GPIO 18 | Entrada de control del relé |
| GPIO 2 | LED de estado (integrado en muchas placas ESP32) |
| GND | GND del módulo de relé |
| Contactos `COM` y `NO` del relé | Los dos pines `POWER SW` de la placa base |

> El relé debe funcionar como un contacto momentáneo en paralelo al botón físico. No conectes el ESP32 directamente a los pines del botón de la placa base.

## Instalación

1. Instala el IDE de Arduino.
2. En el Gestor de placas instala **esp32 by Espressif Systems**.
3. Selecciona tu modelo de ESP32 y el puerto serie correcto.
4. Abre `app.ino` y revisa los valores de configuración inicial.
5. Compila y carga el programa al ESP32 por USB.

### Dependencias

Las bibliotecas incluidas con el paquete de ESP32 son:

- `WiFi`
- `WebServer`
- `EEPROM`
- `ArduinoOTA`

## Primera configuración

El firmware no contiene credenciales reales. Antes de cargarlo, copia `secrets.example.h` como `secrets.h` y cambia sus marcadores:

```cpp
const char* http_username = "admin";
const char* http_password = "CHANGE_ME";

const char* ap_ssid = "PoweRemote-Setup";
const char* ap_password = "CHANGE_ME_AP";

ssid_stored = "YOUR_WIFI_SSID";
password_stored = "YOUR_WIFI_PASSWORD";
```

Usa contraseñas robustas. No publiques `secrets.h`: está incluido en `.gitignore` y queda sólo en tu copia local.

Al arrancar, el ESP32 intenta unirse a la red configurada. Si tiene éxito, consulta la dirección IP mostrada en el monitor serial y abre:

```
http://DIRECCION_IP_DEL_ESP32/
```

El navegador pedirá el usuario y la contraseña configurados.

## Modo de configuración AP

Si el ESP32 no puede conectarse a la red Wi‑Fi, crea un punto de acceso:

- Red: `PoweRemote-Setup` (o el valor definido en `ap_ssid`)
- Contraseña: el valor definido en `ap_password`
- Dirección: `http://192.168.4.1`

Conéctate a esa red, abre la dirección indicada y guarda el SSID y la contraseña de tu Wi‑Fi. El dispositivo se reiniciará e intentará conectarse a la nueva red.

## Uso

En la página principal encontrarás estas acciones:

- **Encender PC**: cierra el relé durante 150 ms, igual que una pulsación breve del botón físico.
- **Apagado forzado**: mantiene cerrado el relé durante 6 segundos. Úsalo sólo cuando el sistema operativo no responda, porque puede causar pérdida de datos.
- **Configuración**: permite cambiar la red Wi‑Fi desde el navegador.

También está disponible el estado en:

```
GET /status
```

La respuesta incluye el estado Wi‑Fi, IP, potencia de señal y tiempo de actividad.

## Detección de estado del PC

PoweRemote consulta por ICMP cada 10 segundos la ESP32 Ambi configurada en `AMBI_IP`. Como Ambi se apaga junto con el PC, dos respuestas consecutivas marcan el PC como **encendido** y tres fallos consecutivos lo marcan como **apagado**.

La dirección se define sólo en `secrets.h`:

```cpp
const char* AMBI_IP = "192.168.0.3";
```

El estado aparece en la interfaz web, en `GET /status` como `pc_power` y en el monitor serial. Ambi no requiere ningún cambio.

## Actualización OTA

Una vez que el ESP32 esté conectado a la misma red que tu computador, aparecerá como un puerto de red en el IDE de Arduino. Selecciónalo, conserva la misma placa y carga el firmware. La clave OTA es la misma definida en `http_password`.

## Seguridad

- Cambia todas las contraseñas de ejemplo antes de usar el dispositivo.
- El acceso HTTP Basic no cifra el tráfico; úsalo sólo en una red local de confianza.
- No expongas el ESP32 directamente a Internet. Para acceso remoto, utiliza una VPN.
- El relé debe estar aislado correctamente y no debe manejar tensión de red eléctrica para esta función.

## Historial

El repositorio conserva dos versiones del firmware:

- **Versión inicial**: control básico de relé, interfaz web, EEPROM y OTA.
- **Versión actual**: añade configuración Wi‑Fi desde el panel y modo AP de respaldo.
