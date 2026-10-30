// lamp_logic.h
// Lógica "pura" da lâmpada: não depende de Arduino nem do ESP32, por isso pode ser testada no PC.
// Fica ao lado do sketch.ino (no Wokwi: use o "+" ao lado das abas para criar este arquivo).
#pragma once

#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace lamp {

// ============================ LDR -> porcentagem ============================
// Converte a leitura bruta do ADC (0..4095 no ESP32) em 0..100 (100 = muito claro).
//   rawMin/rawMax : valores brutos que correspondem aos extremos (calibração)
//   inverted      : true se o módulo entrega valor ALTO no escuro (é o caso do LDR do Wokwi)
inline int ldrToPercent(int raw, int rawMin, int rawMax, bool inverted) {
  if (rawMax <= rawMin) return 0;
  if (raw < rawMin) raw = rawMin;
  if (raw > rawMax) raw = rawMax;
  const long range = rawMax - rawMin;
  const long pct = (static_cast<long>(raw - rawMin) * 100 + range / 2) / range;  // arredondado
  return inverted ? static_cast<int>(100 - pct) : static_cast<int>(pct);
}

// ================================= tempo ====================================
// Tempo decorrido com millis(). A subtração sem sinal continua correta quando o contador
// dá a volta (a cada ~49 dias).
inline bool elapsed(uint32_t now, uint32_t since, uint32_t interval) {
  return static_cast<uint32_t>(now - since) >= interval;
}

// "Tente de novo a cada X ms". A primeira chamada de due() é sempre verdadeira.
struct RetryTimer {
  uint32_t last = 0;
  bool armed = false;

  bool due(uint32_t now, uint32_t interval) {
    if (!armed || elapsed(now, last, interval)) {
      armed = true;
      last = now;
      return true;
    }
    return false;
  }
  void arm(uint32_t now) {  // marca "acabei de tentar"
    armed = true;
    last = now;
  }
  void reset() { armed = false; }  // a próxima due() será verdadeira na hora
};

// Padrão do alarme: começa LIGADO e alterna a cada halfPeriodMs (LED e buzzer juntos).
inline bool alarmPhaseOn(uint32_t now, uint32_t startedAt, uint32_t halfPeriodMs) {
  if (halfPeriodMs == 0) return true;
  return ((static_cast<uint32_t>(now - startedAt) / halfPeriodMs) % 2U) == 0U;
}

// ================================ sensores ==================================
// Faixas aceitas do DHT. Fora delas (ou NaN) a leitura é descartada.
inline bool validTemperature(float t) { return std::isfinite(t) && t >= -40.0f && t <= 80.0f; }
inline bool validHumidity(float h) { return std::isfinite(h) && h >= 0.0f && h <= 100.0f; }

struct Reading {
  bool hasLuminosity = false;
  int luminosity = 0;
  bool hasTemperature = false;
  float temperature = 0;
  bool hasHumidity = false;
  float humidity = 0;
};

// ============================ Ultralight (IoT Agent) ========================
inline bool appendf(char* out, size_t size, size_t& used, const char* fmt, ...) {
  if (used >= size) return false;
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(out + used, size - used, fmt, ap);
  va_end(ap);
  if (n < 0 || static_cast<size_t>(n) >= size - used) return false;
  used += static_cast<size_t>(n);
  return true;
}

// Monta o payload de /attrs, ex.: "l|42|t|23.5|h|60.0|s|off".
// l = luminosidade, t = temperatura, h = umidade, s = estado ("on"/"off").
// Só inclui o que foi lido de fato (uma falha do DHT não derruba a luminosidade).
// Retorna o tamanho do texto, ou -1 se não coube em `out`.
inline int buildAttrsPayload(char* out, size_t size, const Reading& r, bool lampOn) {
  if (out == nullptr || size == 0) return -1;
  size_t used = 0;
  out[0] = '\0';
  bool ok = true;
  if (r.hasLuminosity) ok = ok && appendf(out, size, used, "l|%d|", r.luminosity);
  if (r.hasTemperature) ok = ok && appendf(out, size, used, "t|%.1f|", static_cast<double>(r.temperature));
  if (r.hasHumidity) ok = ok && appendf(out, size, used, "h|%.1f|", static_cast<double>(r.humidity));
  ok = ok && appendf(out, size, used, "s|%s", lampOn ? "on" : "off");
  return ok ? static_cast<int>(used) : -1;
}

// ------------------------------- comandos ----------------------------------
// O IoT Agent envia em /<apikey>/<deviceId>/cmd o texto  "<deviceId>@<comando>|<parâmetros>",
// por exemplo "lamp001@on|".
enum class CommandResult : uint8_t {
  Invalid,      // não segue o formato <id>@<comando>
  OtherDevice,  // é para outro dispositivo
  Unknown,      // é para este dispositivo, mas o comando não existe
  On,
  Off
};

inline bool equalsIgnoreCase(const char* a, size_t aLen, const char* b) {
  if (aLen != std::strlen(b)) return false;
  for (size_t i = 0; i < aLen; ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  }
  return true;
}

// `payload` NÃO precisa terminar em '\0' (é assim que o PubSubClient entrega a mensagem).
inline CommandResult parseCommand(const char* payload, size_t len, const char* deviceId) {
  if (payload == nullptr || deviceId == nullptr) return CommandResult::Invalid;
  while (len > 0 && std::isspace(static_cast<unsigned char>(payload[0]))) { ++payload; --len; }
  while (len > 0 && std::isspace(static_cast<unsigned char>(payload[len - 1]))) --len;

  const char* at = static_cast<const char*>(std::memchr(payload, '@', len));
  if (at == nullptr) return CommandResult::Invalid;

  const size_t targetLen = static_cast<size_t>(at - payload);
  if (targetLen != std::strlen(deviceId) || std::memcmp(payload, deviceId, targetLen) != 0) {
    return CommandResult::OtherDevice;
  }

  const char* name = at + 1;
  const size_t rest = len - targetLen - 1;
  const char* bar = static_cast<const char*>(std::memchr(name, '|', rest));
  const size_t nameLen = bar != nullptr ? static_cast<size_t>(bar - name) : rest;

  if (equalsIgnoreCase(name, nameLen, "on")) return CommandResult::On;
  if (equalsIgnoreCase(name, nameLen, "off")) return CommandResult::Off;
  return CommandResult::Unknown;
}

// Resposta que o IoT Agent espera em /cmdexe: "<deviceId>@on|OK".
// Sem ela o comando fica PENDING para sempre no Orion.
inline int buildCommandReply(char* out, size_t size, const char* deviceId, CommandResult cmd) {
  if (cmd != CommandResult::On && cmd != CommandResult::Off) return 0;
  const int n = std::snprintf(out, size, "%s@%s|OK", deviceId, cmd == CommandResult::On ? "on" : "off");
  return (n < 0 || static_cast<size_t>(n) >= size) ? 0 : n;
}

}  // namespace lamp
