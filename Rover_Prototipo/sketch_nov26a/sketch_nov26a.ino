#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <esp_arduino_version.h>

// ESP32 DevKit-C. Quitar los jumpers EN de los canales controlados.
// Compartir GND entre ESP32, drivers y fuente externa del servo.
struct Motor {
  uint8_t enable, in1, in2, canalPwm;
  bool invertir;
  int sentido, pwmActual;
};

Motor izquierdo{17, 18, 19, 0, false, 0, 0};
Motor derecho{13, 21, 22, 1, false, 0, 0};
constexpr uint8_t pinServo = 4, canalServo = 2, bitsServo = 14;
constexpr int pulsoMinUs = 1000, pulsoMaxUs = 2000;
constexpr int pwmMaximo = 220, pasoRampa = 25, zonaMuerta = 8;
constexpr unsigned long esperaMaximaMs = 350;

WebServer web(80);
WebSocketsServer control(81);
unsigned long ultimoComando = 0;
bool recibiendo = false, servidorListo = false;

bool iniciarMotor(Motor& motor);
void moverMotor(Motor& motor, int orden);
void escribirPwm(const Motor& motor, int pwm);

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
  if (!ledcSetup(motor.canalPwm, 1000, 8)) return false;
  ledcAttachPin(motor.enable, motor.canalPwm);
#endif
  escribirPwm(motor, 0);
  return true;
}

void posicionarServo(uint8_t porcentaje) {
  const uint32_t pulsoUs = map(porcentaje, 0, 100, pulsoMinUs, pulsoMaxUs);
  const uint32_t duty = (pulsoUs * (1UL << bitsServo) + 10000UL) / 20000UL;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(pinServo, duty);
#else
  ledcWrite(canalServo, duty);
#endif
}

bool iniciarServo() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  if (!ledcAttachChannel(pinServo, 50, bitsServo, canalServo)) return false;
#else
  if (!ledcSetup(canalServo, 50, bitsServo)) return false;
  ledcAttachPin(pinServo, canalServo);
#endif
  posicionarServo(50);
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
    if (motor.sentido && sentido) delay(20);
    digitalWrite(motor.in1, sentido > 0 ? HIGH : LOW);
    digitalWrite(motor.in2, sentido < 0 ? HIGH : LOW);
    motor.sentido = sentido;
  }
  const int objetivo = abs(orden) * pwmMaximo / 100;
  motor.pwmActual = min(objetivo, motor.pwmActual + pasoRampa);
  escribirPwm(motor, motor.pwmActual);
}

void parar() {
  moverMotor(izquierdo, 0);
  moverMotor(derecho, 0);
  recibiendo = false;
}

void manejar(int acelerador, int giro) {
  if (abs(acelerador) <= zonaMuerta) acelerador = 0;
  if (abs(giro) <= zonaMuerta) giro = 0;
  const int izq = acelerador + giro, der = acelerador - giro;
  const int escala = max(100, max(abs(izq), abs(der)));
  moverMotor(izquierdo, izq * 100 / escala);
  moverMotor(derecho, der * 100 / escala);
}

void recibirControl(uint8_t cliente, WStype_t tipo, uint8_t* datos, size_t longitud) {
  if (tipo == WStype_DISCONNECTED) {
    parar();
    return;
  }
  if (tipo != WStype_BIN) return;
  if (longitud == 1) {
    if (datos[0] <= 100) posicionarServo(datos[0]);
    return;
  }
  if (longitud != 2 || datos[0] > 200 || datos[1] > 200) return;
  ultimoComando = millis();
  recibiendo = true;
  manejar(int(datos[0]) - 100, int(datos[1]) - 100);
}

const char pagina[] PROGMEM = R"HTML(
<!doctype html>
<html lang="es"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Rover Omni</title>
<style>
*{box-sizing:border-box}
body{margin:0;background:#101923;color:#eff6fc;font:1rem/1.5 system-ui,sans-serif}
main{max-width:26rem;margin:auto;padding:1.5rem 1rem;text-align:center}
h1{margin:0} p{color:#bacada}
#mando{width:min(68vw,15rem);aspect-ratio:1;margin:1.25rem auto;background:#172c3e;
border:2px solid #5b8aad;border-radius:50%;touch-action:none;user-select:none;
cursor:grab;display:grid;place-items:center}
#palanca{width:26%;aspect-ratio:1;background:#63d8e7;border-radius:50%;pointer-events:none}
button{min-height:3.4rem;width:100%;background:#922d3f;color:white;border:1px solid #e48290;
border-radius:.7rem;font:inherit;font-weight:700;cursor:pointer}
input{width:100%;accent-color:#63d8e7}
</style></head><body><main>
<h1>Rover Omni</h1>
<p id="estado" role="status">Conectando…</p>
<div id="mando" aria-label="Joystick táctil"><div id="palanca"></div></div>
<p>Arriba y abajo para avanzar o retroceder. A los lados para girar. Soltá para parar.</p>
<button type="button" id="parar">Detener</button>
<p><label for="servo">Posición del servo del rover</label>
<input id="servo" type="range" min="0" max="100" value="50" disabled></p>
</main><script>
const mando = document.querySelector('#mando');
const palanca = document.querySelector('#palanca');
const estado = document.querySelector('#estado');
const servo = document.querySelector('#servo');
let socket, puntero = null, acelerador = 0, giro = 0;

function enviarServo() {
  if (socket?.readyState === WebSocket.OPEN)
    socket.send(new Uint8Array([Number(servo.value)]));
}
servo.addEventListener('input', enviarServo);

function enviar() {
  if (socket?.readyState === WebSocket.OPEN && socket.bufferedAmount === 0)
    socket.send(new Uint8Array([acelerador + 100, giro + 100]));
}

function neutral() {
  const anterior = puntero;
  puntero = null;
  if (anterior !== null && mando.hasPointerCapture(anterior))
    mando.releasePointerCapture(anterior);
  acelerador = giro = 0;
  palanca.style.transform = '';
  enviar();
}

function actualizar(e) {
  if (e.pointerId !== puntero) return;
  const rect = mando.getBoundingClientRect();
  const radio = rect.width * .34;
  let x = (e.clientX - rect.left - rect.width / 2) / radio;
  let y = (e.clientY - rect.top - rect.height / 2) / radio;
  const escala = Math.max(1, Math.hypot(x, y));
  x /= escala;
  y /= escala;
  giro = Math.round(x * 100);
  acelerador = Math.round(-y * 100);
  palanca.style.transform = 'translate(' + x * radio + 'px,' + y * radio + 'px)';
  enviar();
}

mando.addEventListener('pointerdown', e => {
  if (socket?.readyState !== WebSocket.OPEN || puntero !== null || e.button !== 0) return;
  puntero = e.pointerId;
  mando.setPointerCapture(puntero);
  actualizar(e);
});
mando.addEventListener('pointermove', actualizar);
for (const evento of ['pointerup','pointercancel','lostpointercapture'])
  mando.addEventListener(evento, e => { if (e.pointerId === puntero) neutral(); });
document.querySelector('#parar').addEventListener('click', neutral);
window.addEventListener('blur', neutral);
window.addEventListener('pagehide', neutral);
document.addEventListener('visibilitychange', () => { if (document.hidden) neutral(); });

function conectar() {
  socket = new WebSocket('ws://' + location.hostname + ':81/');
  socket.onopen = () => {
    estado.textContent = 'Conectado';
    neutral();
    servo.disabled = false;
    enviarServo();
  };
  socket.onclose = () => {
    servo.disabled = true;
    neutral();
    estado.textContent = 'Sin conexión. Reconectando…';
    setTimeout(conectar, 1000);
  };
}
setInterval(enviar, 80);
conectar();
</script></body></html>
)HTML";

void setup() {
  Serial.begin(115200);
  const bool listoIzq = iniciarMotor(izquierdo);
  const bool listoDer = iniciarMotor(derecho);
  if (!listoIzq || !listoDer || !iniciarServo()) {
    Serial.println("No se pudo iniciar el PWM. Revisá la placa y los pines.");
    return;
  }
  WiFi.mode(WIFI_AP);
  WiFi.setTxPower(WIFI_POWER_15dBm);
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
  if (izquierdo.sentido == 0 && derecho.sentido == 0) web.handleClient();
  delay(1);
}
