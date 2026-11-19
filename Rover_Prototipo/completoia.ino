#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <ESP_I2S.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include "mp3dec.h"  // Decodificador MP3 Helix

// ================================================================
// CONFIGURACIÓN WIFI Y API KEY DE IA
// ================================================================
const char* WIFI_SSID     = "Tecnoteca";
const char* WIFI_PASSWORD = "TecnoTeca";

// Clave de Google AI Studio (empieza con "AQ." o "AIza") o de OpenRouter ("sk-or-v1-...")
const char* AI_API_KEY    = "tu_api_aqui";

// Modelo para Google AI Studio (100% GRATIS: 15 RPM / 500 peticiones por día)
const char* GEMINI_MODEL  = "gemini-3.5-flash-lite";

// Modelo en caso de usar OpenRouter
const char* OPENROUTER_MODEL = "google/gemini-2.5-flash-lite";

// Personalidad del robot (respuestas cortas = mucho más rápido al generar y hablar)
const char* SYSTEM_PROMPT =
  "Eres Robertito, un pequeño robot físico simpático, curioso y con algo de humor. "
  "Escucha el audio del usuario y responde SIEMPRE en español, con una sola oración "
  "corta y directa (máximo 15 palabras), sin emojis, sin asteriscos ni símbolos, "
  "lista para ser leída en voz alta.";

#define USE_MIC 1

// ================================================================
// CONFIGURACIÓN DE PINES (ESP32 de 38 pines)
// ================================================================

// 1. Pantalla OLED I2C (SH1106 / SSD1306)
#define SDA_PIN        21
#define SCL_PIN        22

// 2. Micrófono INMP441 (I2S entrada - Puerto 0)
// L/R del micrófono va a GND
#define MIC_SCK        14   // SCK / BCLK
#define MIC_WS         25   // WS / LRCK
#define MIC_SD         32   // SD (datos de entrada)

// 3. Amplificador MAX98357A (I2S salida - Puerto 1)
#define SPK_BCLK       26   // BCLK (Bit Clock)
#define SPK_LRC        27   // LRC / WS (Word Select)
#define SPK_DOUT       19   // DIN (datos hacia el amplificador)

// ================================================================
// PARÁMETROS DE AUDIO, VELOCIDAD Y VOLUMEN
// ================================================================
#define MIC_SAMPLE_RATE     16000
#define REC_SAMPLE_RATE     8000    // 8 kHz para enviar el audio el doble de rápido por WiFi
#define MAX_REC_SECONDS     4       // Duración máxima de grabación por frase (4s = 64 KB RAM)
#define MIN_REC_MS          450     // Duración mínima de voz para enviarla a la IA
#define SILENCE_STOP_MS     600     // Corta rápido (0.6s) apenas terminas de hablar
#define VAD_START_LEVEL     0.35f   // Nivel para empezar a escuchar
#define VAD_KEEP_LEVEL      0.18f   // Nivel para mantener escucha
#define MIC_GAIN            5       // Ganancia digital del micrófono

#define SPK_SAMPLE_RATE     22050
#define BIP_VOLUMEN         32760   // Volumen máximo digital para bips

// Ganancia de volumen de la voz TTS:
// 2.2f = Fuerte y claro sin distorsionar (puedes probar entre 1.8f y 2.8f según tu parlante)
#define TTS_VOLUME_GAIN     2.2f

// ================================================================
// OBJETOS Y VARIABLES GLOBALES
// ================================================================
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

#if USE_MIC
I2SClass i2sMic(I2S_NUM_0);
#endif
I2SClass i2sSpk(I2S_NUM_1);

// Geometría de los ojos
const int EYE_W = 34;
const int EYE_H = 40;
const int EYE_Y = 12;
const int EYE_LEFT_X = 18;
const int EYE_RIGHT_X = 76;

int lookX = 0;
int targetLookX = 0;
int blink = 0;
bool blinking = false;
bool closing = true;
unsigned long nextBlink = 0;
unsigned long nextLook = 0;

// Nivel de sonido detectado por el micrófono
float nivel = 0;
float ruidoBase = 0;
float dcActual = 0;

// Control de sonido del parlante y anti-eco (feedback)
unsigned long silencioMicHasta = 0;
unsigned long nextSoundReaction = 0;

bool wifiConectado = false;

// ================================================================
// DIBUJO Y ANIMACIÓN DE LOS OJOS
// ================================================================
void drawEye(int x, int openH, int pupila) {
  if (openH < 4) {
    u8g2.drawBox(x, EYE_Y + EYE_H / 2 - 1, EYE_W, 3);
    return;
  }
  int y = EYE_Y + (EYE_H - openH) / 2;
  int r = min(8, openH / 2);
  u8g2.drawRBox(x, y, EYE_W, openH, r);
  if (openH > 20) {
    u8g2.setDrawColor(0);
    u8g2.drawDisc(x + EYE_W / 2 + lookX, y + openH / 2, pupila);
    u8g2.setDrawColor(1);
  }
}

void updateAndDrawEyes(float audioLevel, bool thinkingMode = false) {
  unsigned long now = millis();

  // Parpadeo automático
  if (!blinking && now > nextBlink) {
    blinking = true;
    closing = true;
  }
  if (blinking) {
    blink += closing ? 10 : -10;
    if (blink >= EYE_H - 2) closing = false;
    if (blink <= 0) {
      blink = 0;
      blinking = false;
      nextBlink = now + random(2000, 5000);
    }
  }

  // Mirada
  if (now > nextLook) {
    targetLookX = thinkingMode ? random(-9, 10) : random(-8, 9);
    nextLook = now + (thinkingMode ? random(350, 800) : random(1000, 3000));
  }
  if (lookX < targetLookX) lookX++;
  if (lookX > targetLookX) lookX--;

  int baseH = thinkingMode ? (EYE_H - 8) : EYE_H;
  int openH = baseH - blink + (int)(audioLevel * 14);
  int pupila = 5 + (int)(audioLevel * 6);

  u8g2.clearBuffer();
  drawEye(EYE_LEFT_X, openH, pupila);
  drawEye(EYE_RIGHT_X, openH, pupila);
  u8g2.sendBuffer();
}

// ================================================================
// GENERACIÓN DE TONOS
// ================================================================
void ensureSpkDefaultRate() {
  if (i2sSpk.txSampleRate() != SPK_SAMPLE_RATE ||
      i2sSpk.txDataWidth() != I2S_DATA_BIT_WIDTH_16BIT ||
      i2sSpk.txSlotMode() != I2S_SLOT_MODE_MONO) {
    i2sSpk.configureTX(SPK_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
  }
}

void playChirp(float startFreq, float endFreq, int durMs, int volume = BIP_VOLUMEN) {
  ensureSpkDefaultRate();
  int totalSamples = (durMs * SPK_SAMPLE_RATE) / 1000;
  int16_t buf[64];
  float phase = 0;
  int samplesPlayed = 0;

  while (samplesPlayed < totalSamples) {
    int chunk = min(totalSamples - samplesPlayed, 64);
    for (int i = 0; i < chunk; i++) {
      int s = samplesPlayed + i;
      float progress = (float)s / (float)totalSamples;
      float curFreq = startFreq + (endFreq - startFreq) * progress;
      float phaseInc = (TWO_PI * curFreq) / (float)SPK_SAMPLE_RATE;

      float ramp = 1.0f;
      if (s < 60) ramp = (float)s / 60.0f;
      else if (totalSamples - s < 60) ramp = (float)(totalSamples - s) / 60.0f;

      buf[i] = (int16_t)(sin(phase) * volume * ramp);
      phase += phaseInc;
      if (phase >= TWO_PI) phase -= TWO_PI;
    }
    i2sSpk.write((const uint8_t *)buf, chunk * sizeof(int16_t));
    samplesPlayed += chunk;
  }
}

void flushSilence() {
  int16_t zeroBuf[64] = {0};
  for (int i = 0; i < 4; i++) {
    i2sSpk.write((const uint8_t *)zeroBuf, sizeof(zeroBuf));
  }
}

void soundListeningStart() {
  playChirp(1000, 1600, 35, BIP_VOLUMEN);
  flushSilence();
}

void soundThinkingBlip() {
  playChirp(1400, 1850, 30, BIP_VOLUMEN);
  flushSilence();
}

void soundErrorBlip() {
  playChirp(900, 450, 80, BIP_VOLUMEN);
  flushSilence();
}

// ================================================================
// LECTURA DEL MICRÓFONO (INMP441)
// ================================================================
#if !USE_MIC
float leerNivel() {
  return 0;
}
#else
float leerNivel() {
  static int32_t buf[128];
  size_t n = i2sMic.readBytes((char *)buf, sizeof(buf));
  int muestras = n / sizeof(int32_t);
  if (muestras == 0) return 0;

  double media = 0;
  for (int i = 0; i < muestras; i++) media += (buf[i] >> 8);
  media /= muestras;
  dcActual = media;

  double suma = 0;
  for (int i = 0; i < muestras; i++) suma += fabs((buf[i] >> 8) - media);
  return suma / muestras;
}
#endif

// ================================================================
// CODIFICACIÓN BASE64 Y CABECERA WAV EN STREAMING (Bloques de 1 KB)
// ================================================================
static const char B64_TABLE[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void writeWavHeader(uint8_t *hdr, uint32_t pcmBytes, uint32_t sampleRate) {
  uint32_t fileSize = pcmBytes + 36;
  uint32_t byteRate = sampleRate * 2;
  uint16_t blockAlign = 2;
  uint16_t bitsPerSample = 16;

  memcpy(hdr, "RIFF", 4);
  memcpy(hdr + 4, &fileSize, 4);
  memcpy(hdr + 8, "WAVEfmt ", 8);
  uint32_t fmtSize = 16;
  uint16_t audioFormat = 1;
  uint16_t numChannels = 1;
  memcpy(hdr + 16, &fmtSize, 4);
  memcpy(hdr + 20, &audioFormat, 2);
  memcpy(hdr + 22, &numChannels, 2);
  memcpy(hdr + 24, &sampleRate, 4);
  memcpy(hdr + 28, &byteRate, 4);
  memcpy(hdr + 32, &blockAlign, 2);
  memcpy(hdr + 34, &bitsPerSample, 2);
  memcpy(hdr + 36, "data", 4);
  memcpy(hdr + 40, &pcmBytes, 4);
}

void streamBase64(WiFiClientSecure &client, const uint8_t *data, size_t len, unsigned long &lastEyeUpdate) {
  char out[1028];
  size_t outIdx = 0;
  size_t i = 0;

  while (i + 2 < len) {
    uint32_t triple = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | data[i + 2];
    out[outIdx++] = B64_TABLE[(triple >> 18) & 0x3F];
    out[outIdx++] = B64_TABLE[(triple >> 12) & 0x3F];
    out[outIdx++] = B64_TABLE[(triple >> 6) & 0x3F];
    out[outIdx++] = B64_TABLE[triple & 0x3F];
    i += 3;

    if (outIdx >= 1024) {
      client.write((const uint8_t *)out, outIdx);
      outIdx = 0;
      if (millis() - lastEyeUpdate > 60) {
        updateAndDrawEyes(0.05f, true);
        lastEyeUpdate = millis();
      }
    }
  }

  size_t rem = len - i;
  if (rem == 1) {
    uint32_t triple = ((uint32_t)data[i] << 16);
    out[outIdx++] = B64_TABLE[(triple >> 18) & 0x3F];
    out[outIdx++] = B64_TABLE[(triple >> 12) & 0x3F];
    out[outIdx++] = '=';
    out[outIdx++] = '=';
  } else if (rem == 2) {
    uint32_t triple = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8);
    out[outIdx++] = B64_TABLE[(triple >> 18) & 0x3F];
    out[outIdx++] = B64_TABLE[(triple >> 12) & 0x3F];
    out[outIdx++] = B64_TABLE[(triple >> 6) & 0x3F];
    out[outIdx++] = '=';
  }

  if (outIdx > 0) {
    client.write((const uint8_t *)out, outIdx);
  }
}

String decodeJsonString(const String &body, int startQuote) {
  if (startQuote < 0) return "";
  String respuesta = "";
  bool escaped = false;
  for (int i = startQuote + 1; i < (int)body.length(); i++) {
    char c = body[i];
    if (escaped) {
      if (c == 'n' || c == 'r') respuesta += ' ';
      else if (c == '"' || c == '\\' || c == '/') respuesta += c;
      else if (c == 'u' && i + 4 < (int)body.length()) {
        String hex = body.substring(i + 1, i + 5);
        long code = strtol(hex.c_str(), NULL, 16);
        if (code > 0 && code < 0x80) {
          respuesta += (char)code;
        } else if (code >= 0x80 && code < 0x800) {
          respuesta += (char)(0xC0 | (code >> 6));
          respuesta += (char)(0x80 | (code & 0x3F));
        }
        i += 4;
      }
      escaped = false;
    } else {
      if (c == '\\') escaped = true;
      else if (c == '"') break;
      else respuesta += c;
    }
  }
  respuesta.trim();
  return respuesta;
}

// ================================================================
// CONSULTA A LA IA OPTIMIZADA EN LATENCIA (thinkingLevel: MINIMAL)
// ================================================================
String askAIAudio(const uint8_t *wavData, size_t wavLen) {
  unsigned long tStart = millis();
  bool esOpenRouter = (strncmp(AI_API_KEY, "sk-or-", 6) == 0);
  bool esGoogleAIStudio = !esOpenRouter;
  const char *host = esGoogleAIStudio ? "generativelanguage.googleapis.com" : "openrouter.ai";

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(12);

  unsigned long lastEye = millis();
  updateAndDrawEyes(0.05f, true);

  Serial.printf("[IA] Conectando a %s...\n", host);
  if (!client.connect(host, 443)) {
    Serial.printf("[IA] Error de conexion TLS con %s\n", host);
    return "";
  }
  client.setNoDelay(true); // Envío inmediato de paquetes TCP sin espera Nagle

  String prefix, suffix, path;
  if (esGoogleAIStudio) {
    // thinkingLevel: MINIMAL reduce al mínimo el tiempo de pensamiento interno de Gemini 3.5 Flash Lite
    path = String("/v1beta/models/") + GEMINI_MODEL + ":generateContent?key=" + AI_API_KEY;
    prefix =
      String("{\"system_instruction\":{\"parts\":[{\"text\":\"") + SYSTEM_PROMPT + "\"}]},"
      "\"generationConfig\":{\"maxOutputTokens\":120,\"temperature\":0.7,\"thinkingConfig\":{\"thinkingLevel\":\"MINIMAL\"}},"
      "\"contents\":[{\"parts\":["
        "{\"text\":\"Responde en español en una frase corta:\"},"
        "{\"inline_data\":{\"mime_type\":\"audio/wav\",\"data\":\"";
    suffix = "\"}}]}]}";
  } else {
    path = "/api/v1/chat/completions";
    prefix =
      String("{\"model\":\"") + OPENROUTER_MODEL + "\","
      "\"max_tokens\":100,"
      "\"temperature\":0.7,"
      "\"messages\":["
        "{\"role\":\"system\",\"content\":\"" + SYSTEM_PROMPT + "\"},"
        "{\"role\":\"user\",\"content\":["
          "{\"type\":\"text\",\"text\":\"Responde en español en una frase corta:\"},"
          "{\"type\":\"input_audio\",\"input_audio\":{\"data\":\"";
    suffix = "\",\"format\":\"wav\"}}]}]}";
  }

  size_t b64Len = ((wavLen + 2) / 3) * 4;
  size_t contentLen = prefix.length() + b64Len + suffix.length();

  String headers = String("POST ") + path + " HTTP/1.0\r\n"
                 + "Host: " + host + "\r\n";
  if (!esGoogleAIStudio) {
    headers += String("Authorization: Bearer ") + AI_API_KEY + "\r\n";
  }
  headers += String("Content-Type: application/json\r\n")
           + "Connection: close\r\n"
           + "Content-Length: " + String(contentLen) + "\r\n\r\n"
           + prefix;

  client.print(headers);
  streamBase64(client, wavData, wavLen, lastEye);
  client.print(suffix);

  Serial.println("[IA] Audio enviado, esperando respuesta...");

  unsigned long waitStart = millis();
  while (client.connected() && !client.available()) {
    if (millis() - waitStart > 12000) {
      Serial.println("[IA] Timeout esperando respuesta");
      client.stop();
      return "";
    }
    if (millis() - lastEye > 45) {
      updateAndDrawEyes(0.05f, true);
      lastEye = millis();
    }
    delay(4);
  }

  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n');
    if (line == "\r" || line.length() == 0) break;
  }

  String body = "";
  body.reserve(1536);
  unsigned long readStart = millis();
  while ((client.connected() || client.available()) && (millis() - readStart < 6000)) {
    while (client.available()) {
      char c = client.read();
      if (body.length() < 3800) body += c;
    }
    if (millis() - lastEye > 45) {
      updateAndDrawEyes(0.05f, true);
      lastEye = millis();
    }
    delay(2);
  }
  client.stop();

  int startQuote = -1;
  if (esGoogleAIStudio) {
    int partsIdx = body.indexOf("\"parts\"");
    int textIdx = body.indexOf("\"text\"", (partsIdx >= 0) ? partsIdx : 0);
    if (textIdx >= 0) {
      int colonIdx = body.indexOf(':', textIdx + 6);
      startQuote = body.indexOf('"', colonIdx + 1);
    }
  } else {
    int msgIdx = body.indexOf("\"message\"");
    int contentIdx = body.indexOf("\"content\"", (msgIdx >= 0) ? msgIdx : 0);
    if (contentIdx >= 0) {
      int colonIdx = body.indexOf(':', contentIdx + 9);
      startQuote = body.indexOf('"', colonIdx + 1);
    }
  }

  if (startQuote < 0) {
    Serial.println("[IA] Respuesta inesperada:");
    Serial.println(body);
    return "";
  }

  String respuesta = decodeJsonString(body, startQuote);
  Serial.printf("[IA] Respuesta (%lu ms): %s\n", millis() - tStart, respuesta.c_str());
  return respuesta;
}

// ================================================================
// TEXTO A VOZ (TTS directo HTTP 1.0 + Limitador Suave de Volumen)
// ================================================================
String urlEncode(const String &str) {
  String encoded = "";
  char hex[4];
  for (size_t i = 0; i < str.length(); i++) {
    uint8_t c = (uint8_t)str[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += (char)c;
    } else if (c == ' ') {
      encoded += '+';
    } else {
      snprintf(hex, sizeof(hex), "%%%02X", c);
      encoded += hex;
    }
  }
  return encoded;
}

// Limitador suave de codo redondo: sube el volumen sin recortar en onda cuadrada
static inline int16_t amplifySampleClean(int16_t s) {
  float x = ((float)s * TTS_VOLUME_GAIN) / 32767.0f;
  // Curva de compresión suave cuando supera el 75% de amplitud
  if (x > 0.75f) {
    x = 0.75f + 0.25f * tanhf((x - 0.75f) / 0.25f);
  } else if (x < -0.75f) {
    x = -0.75f + 0.25f * tanhf((x + 0.75f) / 0.25f);
  }
  return (int16_t)(x * 32760.0f);
}

bool speakChunkTTS(const String &chunkText) {
  if (chunkText.length() == 0) return false;

  Serial.printf("[TTS] Hablando: \"%s\"...\n", chunkText.c_str());

  WiFiClient client;
  if (!client.connect("translate.google.com", 80)) {
    Serial.println("[TTS] Error conectando a translate.google.com:80");
    return false;
  }
  client.setNoDelay(true);

  String req = String("GET /translate_tts?ie=UTF-8&tl=es&client=tw-ob&q=") + urlEncode(chunkText) + " HTTP/1.0\r\n"
             + "Host: translate.google.com\r\n"
             + "User-Agent: Mozilla/5.0\r\n"
             + "Connection: close\r\n\r\n";
  client.print(req);

  unsigned long t0 = millis();
  while (!client.available() && client.connected() && millis() - t0 < 4000) {
    delay(4);
  }

  bool okHeader = false;
  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n');
    if (line.startsWith("HTTP/1.") && line.indexOf("200") > 0) {
      okHeader = true;
    }
    if (line == "\r" || line.length() == 0) break;
  }

  if (!okHeader) {
    Serial.println("[TTS] Servidor no devolvio 200 OK");
    client.stop();
    return false;
  }

  const size_t maxMp3 = 38000;
  uint8_t *mp3Buf = (uint8_t *)malloc(maxMp3);
  if (!mp3Buf) {
    Serial.println("[TTS] Sin memoria para buffer MP3");
    client.stop();
    return false;
  }

  size_t bytesRead = 0;
  t0 = millis();
  while ((client.connected() || client.available()) && bytesRead < maxMp3 && millis() - t0 < 5000) {
    size_t avail = client.available();
    if (avail > 0) {
      size_t toRead = min(avail, maxMp3 - bytesRead);
      int r = client.read(mp3Buf + bytesRead, toRead);
      if (r > 0) bytesRead += r;
      t0 = millis();
    } else {
      delay(1);
    }
  }
  client.stop();

  Serial.printf("[TTS] MP3 descargado: %u bytes\n", (unsigned int)bytesRead);
  if (bytesRead < 256) {
    free(mp3Buf);
    return false;
  }

  HMP3Decoder decoder = MP3InitDecoder();
  if (!decoder) {
    free(mp3Buf);
    return false;
  }

  int16_t *pcmOut = (int16_t *)malloc(MAX_NCHAN * MAX_NGRAN * MAX_NSAMP * sizeof(int16_t));
  if (!pcmOut) {
    MP3FreeDecoder(decoder);
    free(mp3Buf);
    return false;
  }

  const uint8_t *readPtr = mp3Buf;
  int bytesLeft = (int)bytesRead;
  MP3FrameInfo frameInfo;
  uint32_t currentRate = 0;
  unsigned long lastEye = 0;
  int framesDecoded = 0;

  while (bytesLeft > 0) {
    int offset = MP3FindSyncWord((unsigned char *)readPtr, bytesLeft);
    if (offset < 0) break;
    readPtr += offset;
    bytesLeft -= offset;

    int err = MP3Decode(decoder, (unsigned char **)&readPtr, &bytesLeft, pcmOut, 0);
    if (err) {
      if (err == ERR_MP3_INDATA_UNDERFLOW) break;
      readPtr++;
      bytesLeft--;
      continue;
    }

    framesDecoded++;
    MP3GetLastFrameInfo(decoder, &frameInfo);
    if (frameInfo.outputSamps <= 0) continue;

    if ((uint32_t)frameInfo.samprate != currentRate) {
      currentRate = frameInfo.samprate;
      i2sSpk.configureTX(currentRate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    }

    int samples = frameInfo.outputSamps;
    if (frameInfo.nChans == 2) {
      samples /= 2;
      for (int i = 0; i < samples; i++) {
        pcmOut[i] = (int16_t)(((int32_t)pcmOut[2 * i] + (int32_t)pcmOut[2 * i + 1]) / 2);
      }
    }

    // Amplificación limpia con limitador suave (más volumen sin saturación)
    for (int i = 0; i < samples; i++) {
      pcmOut[i] = amplifySampleClean(pcmOut[i]);
    }

    i2sSpk.write((const uint8_t *)pcmOut, samples * sizeof(int16_t));

    // Animar ojos al ritmo de la voz
    if (millis() - lastEye > 35) {
      long sumAbs = 0;
      int nCheck = min(samples, 64);
      for (int i = 0; i < nCheck; i++) sumAbs += abs(pcmOut[i]);
      float voiceLevel = min(1.0f, (float)(sumAbs / max(1, nCheck)) / 10000.0f);
      updateAndDrawEyes(voiceLevel, false);
      lastEye = millis();
    }
  }

  Serial.printf("[TTS] Frames reproducidos: %d\n", framesDecoded);

  free(pcmOut);
  MP3FreeDecoder(decoder);
  free(mp3Buf);

  ensureSpkDefaultRate();
  flushSilence();
  silencioMicHasta = millis() + 500;
  return (framesDecoded > 0);
}

// Divide textos largos en frases cortas naturales (máximo ~85 caracteres)
void speakSpanishTTS(const String &text) {
  String clean = text;
  clean.replace("\n", " ");
  clean.replace("*", "");
  clean.replace("#", "");
  clean.trim();
  if (clean.length() == 0) return;

  int start = 0;
  int len = clean.length();
  while (start < len) {
    int end = min(len, start + 85);
    if (end < len) {
      int splitPos = -1;
      for (int i = end; i > start + 25; i--) {
        char c = clean[i];
        if (c == '.' || c == ',' || c == '!' || c == '?' || c == ';') {
          splitPos = i + 1;
          break;
        }
      }
      if (splitPos < 0) {
        for (int i = end; i > start + 25; i--) {
          if (clean[i] == ' ') {
            splitPos = i;
            break;
          }
        }
      }
      if (splitPos > start) end = splitPos;
    }

    String sub = clean.substring(start, end);
    sub.trim();
    if (sub.length() > 0) {
      speakChunkTTS(sub);
    }
    start = end;
  }
}

// ================================================================
// GRABACIÓN AUTOMÁTICA POR VOZ (VAD) CON RECORTE DE SILENCIO FINAL
// ================================================================
void escucharYConversar() {
  const size_t maxSamples = REC_SAMPLE_RATE * MAX_REC_SECONDS;
  const size_t wavBufferSize = 44 + maxSamples * sizeof(int16_t);

  uint8_t *wavBuf = (uint8_t *)malloc(wavBufferSize);
  if (!wavBuf) {
    Serial.println("[MIC] Sin memoria para grabar WAV");
    soundErrorBlip();
    return;
  }

  soundListeningStart();
  delay(25);

  int16_t *pcmPtr = (int16_t *)(wavBuf + 44);
  size_t recordedSamples = 0;
  size_t lastVoiceSampleIdx = 0;

  unsigned long startRec = millis();
  unsigned long lastVoiceTime = millis();
  unsigned long lastEye = 0;
  int32_t rawBuf[128];

  Serial.println("[MIC] Escuchando tu voz...");

  while (recordedSamples < maxSamples) {
    size_t n = i2sMic.readBytes((char *)rawBuf, sizeof(rawBuf));
    int count = n / sizeof(int32_t);
    if (count <= 0) continue;

    double media = 0;
    for (int i = 0; i < count; i++) media += (rawBuf[i] >> 8);
    media /= count;

    double sumaAbs = 0;
    for (int i = 0; i < count; i += 2) {
      if (recordedSamples >= maxSamples) break;
      int32_t v = ((rawBuf[i] >> 8) - (int32_t)media);
      sumaAbs += fabs((double)v);

      int32_t s16 = (v >> 5) * MIC_GAIN;
      if (s16 > 32700) s16 = 32700;
      else if (s16 < -32700) s16 = -32700;
      pcmPtr[recordedSamples++] = (int16_t)s16;
    }

    float crudo = (float)(sumaAbs / (count / 2));
    float sobre = crudo - ruidoBase;
    if (sobre < 0) sobre = 0;
    float nivActual = min(1.0f, sobre / 6000.0f);

    if (nivActual > VAD_KEEP_LEVEL) {
      lastVoiceTime = millis();
      lastVoiceSampleIdx = recordedSamples;
    }

    if (millis() - lastEye > 35) {
      updateAndDrawEyes(nivActual, false);
      lastEye = millis();
    }

    if (millis() - lastVoiceTime > SILENCE_STOP_MS) {
      break;
    }
  }

  unsigned long duracionVoz = lastVoiceTime - startRec;
  // Recortar la cola de silencio final (dejando solo 150ms de margen) para subir menos bytes por WiFi
  size_t marginSamples = (REC_SAMPLE_RATE * 150) / 1000;
  if (lastVoiceSampleIdx + marginSamples < recordedSamples) {
    recordedSamples = lastVoiceSampleIdx + marginSamples;
  }

  Serial.printf("[MIC] Grabado: %u muestras (%lu ms de voz activa)\n",
                (unsigned int)recordedSamples, duracionVoz);

  if (duracionVoz < MIN_REC_MS || recordedSamples < (REC_SAMPLE_RATE / 2)) {
    Serial.println("[MIC] Sonido muy corto, ignorado.");
    free(wavBuf);
    silencioMicHasta = millis() + 250;
    return;
  }

  uint32_t pcmBytes = recordedSamples * sizeof(int16_t);
  writeWavHeader(wavBuf, pcmBytes, REC_SAMPLE_RATE);

  soundThinkingBlip();

  String respuesta = askAIAudio(wavBuf, 44 + pcmBytes);
  free(wavBuf);

  if (respuesta.length() > 0) {
    speakSpanishTTS(respuesta);
  } else {
    soundErrorBlip();
  }

  silencioMicHasta = millis() + 500;
}

// ================================================================
// CONEXIÓN WIFI (Optimizada para máxima sensibilidad y auto-reconexión)
// ================================================================
void conectarWiFi() {
  if (strlen(WIFI_SSID) == 0 || strcmp(WIFI_SSID, "TU_WIFI_SSID") == 0) {
    Serial.println("[WIFI] WIFI_SSID sin configurar.");
    wifiConectado = false;
    return;
  }

  Serial.printf("[WIFI] Conectando a %s...\n", WIFI_SSID);

  WiFi.disconnect(true);
  delay(100);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  // 1. Desactivar ahorro de energía del módem (evita cortes y baja la latencia de red)
  WiFi.setSleep(false);

  // 2. Ajustar potencia de transmisión al máximo permitido por el chip (+19.5 dBm)
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 14000) {
    updateAndDrawEyes(0.1f, true);
    delay(50);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    wifiConectado = true;
    Serial.printf("[WIFI] Conectado! IP: %s | Señal: %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    wifiConectado = false;
    Serial.println("[WIFI] Tiempo de espera agotado. Se reintentara en segundo plano.");
  }
}

void verificarWiFi() {
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck > 4000) {
    lastCheck = millis();
    if (WiFi.status() == WL_CONNECTED) {
      wifiConectado = true;
    } else {
      wifiConectado = false;
      Serial.println("[WIFI] Conexion perdida, reconectando...");
      WiFi.reconnect();
    }
  }
}

// ================================================================
// SETUP
// ================================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  // 1. Pantalla OLED
  Wire.begin(SDA_PIN, SCL_PIN);
  u8g2.begin();
  u8g2.setBusClock(400000);

#if USE_MIC
  // 2. Micrófono INMP441 (I2S RX - Puerto 0)
  i2sMic.setPins(MIC_SCK, MIC_WS, -1, MIC_SD, -1);
  if (!i2sMic.begin(I2S_MODE_STD, MIC_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT,
                    I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
    Serial.println("Error iniciando microfono I2S (puerto 0)");
  }
#endif

  // 3. Amplificador MAX98357A (I2S TX - Puerto 1)
  i2sSpk.setPins(SPK_BCLK, SPK_LRC, SPK_DOUT, -1, -1);
  if (!i2sSpk.begin(I2S_MODE_STD, SPK_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                    I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
    Serial.println("Error iniciando parlante I2S (puerto 1)");
  }

  // 4. Calibración de ruido ambiente
  randomSeed(micros());
  nextBlink = millis() + 2000;
  nextLook = millis() + 1500;

  double acc = 0;
  int veces = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < 1000) {
    acc += leerNivel();
    veces++;
    updateAndDrawEyes(0.0f, false);
  }
  ruidoBase = veces ? acc / veces : 0;
  Serial.print("Ruido base calibrado: ");
  Serial.println(ruidoBase);

  // 5. Conectar a WiFi
  conectarWiFi();

  // 6. Sonido de encendido
  playChirp(800, 1800, 90, BIP_VOLUMEN);
  flushSilence();

  // 7. Saludo inicial hablado por TTS
  if (wifiConectado) {
    speakSpanishTTS("Hola, ya estoy listo.");
  }

  nextSoundReaction = millis() + 600;
}

// ================================================================
// LOOP PRINCIPAL
// ================================================================
void loop() {
  unsigned long now = millis();

  verificarWiFi();

  // --- LECTURA DEL MICRÓFONO CON ANTI-FEEDBACK ---
  float crudo = leerNivel();
  float sobre = 0;

  if (now > silencioMicHasta) {
    sobre = crudo - ruidoBase;
    if (sobre < 0) sobre = 0;
  }

  float objetivo = sobre / 6000.0f;
  if (objetivo > 1.0f) objetivo = 1.0f;
  nivel = (objetivo > nivel) ? objetivo : (nivel * 0.90f + objetivo * 0.10f);

#if USE_MIC
  // --- DETECCIÓN DE VOZ Y CONVERSACIÓN CON IA ---
  if (nivel > VAD_START_LEVEL && now > nextSoundReaction && now > silencioMicHasta) {
    if (WiFi.status() == WL_CONNECTED && strlen(AI_API_KEY) > 10) {
      escucharYConversar();
    } else {
      soundErrorBlip();
    }
    nivel = 0;
    nextSoundReaction = millis() + 600;
  }
#endif

  // --- DIBUJAR OJOS ANIMADOS EN PANTALLA ---
  updateAndDrawEyes(nivel, false);
}
