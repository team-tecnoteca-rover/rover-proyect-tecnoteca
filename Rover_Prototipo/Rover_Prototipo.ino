#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <esp_arduino_version.h>

// Dos módulos L298N: uno por lado, dos motores por módulo.
// Izquierdo: GPIO 17 a ENA/ENB, GPIO 18 a IN1/IN3, GPIO 19 a IN2/IN4.
// Derecho: GPIO 13 a ENA/ENB, GPIO 21 a IN1/IN3, GPIO 22 a IN2/IN4.
// Cada motor usa su propio par OUT. Las salidas OUT no se unen entre sí.
// Servo de cabeza: señal GPIO 4, alimentación externa según modelo y GND común.
// Quitá los jumpers ENA y ENB antes de conectar los GPIO. Compartí GND.

struct Motor {
  uint8_t enable;
  uint8_t in1;
  uint8_t in2;
  uint8_t canalPwm;
  bool invertir;
  int sentido;
  int pwmActual;
};

//                EN   IN1 IN2  PWM  invertir
Motor izquierdo{ 17,  18, 19,  0,   false,    0, 0 };
Motor derecho  { 13,  21, 22,  1,   false,    0, 0 };

constexpr int pwmMaximo = 220;
constexpr int pasoRampa = 25; // Incremento por comando recibido, cada 80 ms desde la web.
constexpr uint8_t servoPin = 4;
constexpr uint8_t servoCanal = 2;
constexpr uint8_t servoResolucion = 14;
constexpr int servoMinUs = 1000; // Calibrar según servo y recorrido mecánico de la cabeza.
constexpr int servoCentroUs = 1500;
constexpr int servoMaxUs = 2000;
constexpr bool invertirCabeza = false;
constexpr unsigned long servoPeriodoMs = 20;
constexpr int servoPasoUs = 40; // Reduce saltos. No sustituye una fuente adecuada.
int servoObjetivoUs = servoCentroUs;
int servoAplicadoUs = servoCentroUs;
unsigned long ultimaActualizacionServo = 0;
int cabezaActual = 0; // -100 a 100, centro 0. Conserva posición al soltar el joystick.

constexpr int zonaMuerta = 8;   // Sobre el rango de -100 a 100.
constexpr unsigned long esperaMaximaMs = 350;

WebServer web(80);
WebSocketsServer control(81);
unsigned long ultimoComando = 0;
bool recibiendo = false;
bool servidorListo = false;

void escribirPwm(const Motor& motor, int pwm);
bool iniciarMotor(Motor& motor);
void moverMotor(Motor& motor, int orden);

void escribirPwm(const Motor& motor, int pwm) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(motor.enable, pwm);
#else
  ledcWrite(motor.canalPwm, pwm);
#endif
}

bool iniciarMotor(Motor& motor) {
  pinMode(motor.enable, OUTPUT);
  digitalWrite(motor.enable, LOW);
  pinMode(motor.in1, OUTPUT);
  pinMode(motor.in2, OUTPUT);
  digitalWrite(motor.in1, LOW);
  digitalWrite(motor.in2, LOW);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  if (!ledcAttachChannel(motor.enable, 1000, 8, motor.canalPwm)) return false;
#else
  if (ledcSetup(motor.canalPwm, 1000, 8) == 0) return false;
  ledcAttachPin(motor.enable, motor.canalPwm);
#endif
  escribirPwm(motor, 0);
  return true;
}

void moverMotor(Motor& motor, int orden) {
  if (motor.invertir) orden = -orden;
  const int sentido = (orden > 0) - (orden < 0);

  if (sentido != motor.sentido) {
    escribirPwm(motor, 0);
    motor.pwmActual = 0;
    digitalWrite(motor.in1, LOW);
    digitalWrite(motor.in2, LOW);
    if (motor.sentido != 0 && sentido != 0) delay(20);
    digitalWrite(motor.in1, sentido > 0 ? HIGH : LOW);
    digitalWrite(motor.in2, sentido < 0 ? HIGH : LOW);
    motor.sentido = sentido;
  }
  const int objetivo = abs(orden) * pwmMaximo / 100;
  if (objetivo == 0) {
    motor.pwmActual = 0;
  } else if (motor.pwmActual < objetivo) {
    motor.pwmActual = min(objetivo, motor.pwmActual + pasoRampa);
  } else {
    motor.pwmActual = objetivo;
  }
  escribirPwm(motor, motor.pwmActual);
}

void escribirCabezaUs(int pulsoUs) {
  const uint32_t duty = uint32_t(pulsoUs) * ((1UL << servoResolucion) - 1) / 20000UL;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(servoPin, duty);
#else
  ledcWrite(servoCanal, duty);
#endif
}

void moverCabeza(int posicion) {
  cabezaActual = constrain(posicion, -100, 100);
  const int orientacion = invertirCabeza ? -cabezaActual : cabezaActual;
  const int nuevoObjetivo = orientacion >= 0
    ? servoCentroUs + orientacion * (servoMaxUs - servoCentroUs) / 100
    : servoCentroUs + orientacion * (servoCentroUs - servoMinUs) / 100;
  if (nuevoObjetivo == servoObjetivoUs) return;
  servoObjetivoUs = nuevoObjetivo;
  Serial.print("[CABEZA] Orden recibida, objetivo en us: ");
  Serial.println(servoObjetivoUs);
}

void actualizarCabeza() {
  const unsigned long ahora = millis();
  if (ahora - ultimaActualizacionServo < servoPeriodoMs) return;
  ultimaActualizacionServo = ahora;
  const int diferencia = servoObjetivoUs - servoAplicadoUs;
  if (diferencia == 0) return;
  servoAplicadoUs += constrain(diferencia, -servoPasoUs, servoPasoUs);
  escribirCabezaUs(servoAplicadoUs);
  if (servoAplicadoUs == servoObjetivoUs) {
    Serial.print("[CABEZA] Pulso aplicado en us: ");
    Serial.println(servoAplicadoUs);
    // Confirma señal ordenada. El servo no devuelve posición ni tensión al ESP32.
  }
}

bool iniciarCabeza() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  if (!ledcAttachChannel(servoPin, 50, servoResolucion, servoCanal)) return false;
#else
  if (ledcSetup(servoCanal, 50, servoResolucion) == 0) return false;
  ledcAttachPin(servoPin, servoCanal);
#endif
  servoObjetivoUs = servoAplicadoUs = servoCentroUs;
  cabezaActual = 0;
  escribirCabezaUs(servoCentroUs);
  ultimaActualizacionServo = millis();
  Serial.println("[CABEZA] PWM iniciado en GPIO 4: 50 Hz, centro 1500 us.");
  return true;
}

void parar() {
  moverMotor(izquierdo, 0);
  moverMotor(derecho, 0);
  recibiendo = false;
}

void manejar(int acelerador, int giro) {
  if (abs(acelerador) <= zonaMuerta) acelerador = 0;
  if (abs(giro) <= zonaMuerta) giro = 0;

  const int izq = acelerador + giro;
  const int der = acelerador - giro;
  int escala = abs(izq) > abs(der) ? abs(izq) : abs(der);
  if (escala < 100) escala = 100;
  moverMotor(izquierdo, izq * 100 / escala);
  moverMotor(derecho, der * 100 / escala);
}

void recibirControl(uint8_t, WStype_t tipo, uint8_t* datos, size_t longitud) {
  if (tipo == WStype_DISCONNECTED) {
    parar();
    return;
  }
  // Tres bytes para la web nueva. Dos bytes admiten controles anteriores.
  if (tipo != WStype_BIN || (longitud != 2 && longitud != 3)) return;

  // El navegador codifica cada eje de -100..100 como un byte de 0..200.
  if (datos[0] > 200 || datos[1] > 200 || (longitud == 3 && datos[2] > 200)) return;
  ultimoComando = millis();
  recibiendo = true;
  manejar(int(datos[0]) - 100, int(datos[1]) - 100);
  if (longitud == 3) moverCabeza(int(datos[2]) - 100);
}

const char pagina[] PROGMEM = R"HTML(
<!doctype html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Rover · Tracción y cabeza</title>
<style>
*{box-sizing:border-box}
body{margin:0;background:#101923;color:#eff6fc;font:1rem/1.5 system-ui,sans-serif}
main{max-width:46rem;margin:auto;padding:1.2rem 1rem;text-align:center}
h1{margin:0;font-size:1.65rem}
h2{font-size:1.1rem;margin:.5rem 0}
p{color:#bacada}
.controles{display:grid;grid-template-columns:1fr 1fr;gap:1rem}
.mando{position:relative;width:100%;max-width:16rem;aspect-ratio:1;margin:1rem auto;
background:#172c3e;border:2px solid #5b8aad;border-radius:50%;touch-action:none;
user-select:none;cursor:grab;display:grid;place-items:center}
.palanca{width:26%;aspect-ratio:1;background:#63d8e7;border-radius:50%;pointer-events:none}
#palancaCabeza{background:#c3a3ff}
button{min-height:3rem;padding:.6rem 1rem;border-radius:.7rem;color:white;font:inherit;
font-weight:650;border:1px solid #7189a2;background:#244259;cursor:pointer}
#parar{width:100%;background:#922d3f;border-color:#e48290;margin-top:1rem}
button:focus-visible{outline:3px solid #63d8e7;outline-offset:4px}
small{color:#bacada}
@media(max-width:380px){main{padding:1rem .6rem}.controles{gap:.6rem}small{font-size:.75rem}}
</style>
</head>
<body>
<main>
<h1>Rover</h1>
<p id="estado" role="status">Conectando…</p>
<div class="controles">
<section>
<h2>Tracción</h2>
<div id="mando" class="mando" aria-label="Joystick táctil de tracción"><div id="palanca" class="palanca"></div></div>
<small>Avance, retroceso y giro. Soltá para parar.</small>
</section>
<section>
<h2>Cabeza</h2>
<div id="mandoCabeza" class="mando" aria-label="Joystick táctil horizontal de cabeza"><div id="palancaCabeza" class="palanca"></div></div>
<small>Izquierda y derecha. Conserva la posición.</small>
<p><button type="button" id="centrar">Centrar cabeza</button></p>
</section>
</div>
<button type="button" id="parar">Detener tracción</button>
</main>
<script>
const estado = document.querySelector('#estado');
let socket, acelerador = 0, giro = 0, cabeza = 0;

function enviar() {
  if (socket?.readyState === WebSocket.OPEN && socket.bufferedAmount === 0) {
    socket.send(new Uint8Array([acelerador + 100, giro + 100, cabeza + 100]));
  }
}

function crearJoystick(id, palancaId, horizontal, alMover) {
  const mando = document.querySelector(id);
  const palanca = document.querySelector(palancaId);
  let puntero = null, x = 0, y = 0;

  function dibujar() {
    const radio = mando.getBoundingClientRect().width * .34;
    palanca.style.transform = 'translate(' + x * radio + 'px,' + y * radio + 'px)';
  }
  function terminar() {
    const anterior = puntero;
    puntero = null;
    if (anterior !== null && mando.hasPointerCapture(anterior)) mando.releasePointerCapture(anterior);
    if (!horizontal) {
      x = y = 0;
      dibujar();
      alMover(0, 0);
    }
  }
  function actualizar(e) {
    if (e.pointerId !== puntero) return;
    const rect = mando.getBoundingClientRect();
    const radio = rect.width * .34;
    x = (e.clientX - rect.left - rect.width / 2) / radio;
    y = horizontal ? 0 : (e.clientY - rect.top - rect.height / 2) / radio;
    const escala = Math.max(1, Math.hypot(x, y));
    x /= escala; y /= escala;
    dibujar();
    alMover(Math.round(x * 100), Math.round(-y * 100));
  }
  mando.addEventListener('pointerdown', e => {
    if (socket?.readyState !== WebSocket.OPEN || puntero !== null || e.button !== 0) return;
    puntero = e.pointerId;
    mando.setPointerCapture(puntero);
    actualizar(e);
  });
  mando.addEventListener('pointermove', actualizar);
  for (const evento of ['pointerup', 'pointercancel', 'lostpointercapture']) {
    mando.addEventListener(evento, e => { if (e.pointerId === puntero) terminar(); });
  }
  window.addEventListener('resize', dibujar);
  return {
    terminar,
    centrar() {
      terminar();
      x = y = 0;
      dibujar();
      alMover(0, 0);
    }
  };
}

const traccion = crearJoystick('#mando', '#palanca', false, (x, y) => {
  giro = x; acelerador = y; enviar();
});
const cabezaJoystick = crearJoystick('#mandoCabeza', '#palancaCabeza', true, x => {
  cabeza = x; enviar();
});
function detener() {
  traccion.terminar();
  cabezaJoystick.terminar();
  enviar();
}
document.querySelector('#parar').addEventListener('click', detener);
document.querySelector('#centrar').addEventListener('click', () => cabezaJoystick.centrar());
window.addEventListener('blur', detener);
window.addEventListener('pagehide', detener);
document.addEventListener('visibilitychange', () => { if (document.hidden) detener(); });

function conectar() {
  socket = new WebSocket('ws://' + location.hostname + ':81/');
  socket.onopen = () => { estado.textContent = 'Conectado'; detener(); };
  socket.onclose = () => {
    detener();
    estado.textContent = 'Sin conexión. Reconectando…';
    setTimeout(conectar, 1000);
  };
}
setInterval(enviar, 80);
conectar();
</script>
</body>
</html>
)HTML";

void setup() {
  Serial.begin(115200);
  const bool listoIzq = iniciarMotor(izquierdo);
  const bool listoDer = iniciarMotor(derecho);
  const bool listaCabeza = iniciarCabeza();
  if (!listoIzq || !listoDer || !listaCabeza) {
    Serial.println("No se pudo iniciar el PWM. Revisá el modelo de ESP32 y los pines.");
    return;
  }

  WiFi.mode(WIFI_AP);
  WiFi.setTxPower(WIFI_POWER_15dBm); // Configuración del prototipo.
  const IPAddress ip(192, 168, 4, 1);
  if (!WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0)) ||
      !WiFi.softAP("Rover-Omni", "rover1234", 1, 0, 1)) {
    Serial.println("No se pudo iniciar Rover-Omni.");
    return;
  }

  web.on("/", HTTP_GET, []() { web.send_P(200, "text/html; charset=utf-8", pagina); });
  web.begin();
  control.begin();
  control.onEvent(recibirControl);
  servidorListo = true;
  Serial.print("Página de control: http://");
  Serial.println(WiFi.softAPIP());
}

void loop() {
  if (!servidorListo) {
    delay(50);
    return;
  }
  if (recibiendo && millis() - ultimoComando >= esperaMaximaMs) parar();
  control.loop();
  actualizarCabeza();
  // Atender la página al estar parado para que una solicitud HTTP lenta
  // no demore el control de los motores.
  if (izquierdo.sentido == 0 && derecho.sentido == 0) web.handleClient();
  delay(1);
}
