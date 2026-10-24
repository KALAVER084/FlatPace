// Lo que el motor pide al driver para un juego, sin depender del fabricante, y su traduccion a los
// ajustes del perfil de NVIDIA. Cada fabricante (NVIDIA hoy; Intel y AMD en preparacion) traduce la
// misma peticion a lo que ofrece su driver.
#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "../../third_party/nvapi/nvapi_min.h"

struct LimitRequest {
  uint32_t frl = 0;         // valor del limitador de FPS (0 = sin limitador)
  bool vsyncOn = false;     // VSync del driver forzado (con VRR: el tope recomendado)
  int vsyncDivisor = 0;     // > 0: VSync por intervalos (1/n del refresco), sin limitador
  bool maxPerf = false;     // "GPU al maximo"
  bool lowLatency = false;  // cola de render de un fotograma
  bool Empty() const { return frl == 0 && vsyncDivisor == 0; }
};

namespace nv {
inline NvU32 VsyncForDivisor(int n) {
  return n == 1 ? VSYNC_FORCEON : n == 2 ? VSYNC_FLIP2 : n == 3 ? VSYNC_FLIP3 : VSYNC_FLIP4;
}
inline int DivisorForVsync(NvU32 v) {
  return v == VSYNC_FORCEON ? 1 : v == VSYNC_FLIP2 ? 2 : v == VSYNC_FLIP3 ? 3 : v == VSYNC_FLIP4 ? 4 : 0;
}
}  // namespace nv

// Ajustes del perfil de NVIDIA para una peticion, en el mismo orden que hasta la 0.52: el VSync por
// intervalos lleva siempre la cola de un fotograma; "GPU al maximo" y la baja latencia solo acompanan
// a un limite (una peticion vacia no toca nada).
static std::vector<std::pair<uint32_t, uint32_t>> NvDesired(const LimitRequest& r) {
  std::vector<std::pair<uint32_t, uint32_t>> d;
  if (r.vsyncDivisor > 0) {
    d = {{nv::VSYNCMODE_ID, nv::VsyncForDivisor(r.vsyncDivisor)}, {nv::PRERENDERLIMIT_ID, 1}};
  } else if (r.frl > 0) {
    d.push_back({nv::FRL_FPS_ID, r.frl});
    if (r.vsyncOn) d.push_back({nv::VSYNCMODE_ID, nv::VSYNC_FORCEON});
  }
  if (d.empty()) return d;
  if (r.maxPerf) d.push_back({nv::PREFERRED_PSTATE_ID, nv::PSTATE_PREFER_MAX});
  if (r.lowLatency && std::none_of(d.begin(), d.end(), [](const auto& x) { return x.first == nv::PRERENDERLIMIT_ID; }))
    d.push_back({nv::PRERENDERLIMIT_ID, 1});
  return d;
}
