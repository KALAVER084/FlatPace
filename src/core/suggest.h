// FlatPace - sugerencia de subir un limite confirmado
// Logica pura, sin dependencias de Windows: se incluye desde main.cpp y desde las pruebas
// automaticas (tests/). Requiere las cabeceras estandar, analysis.h (VrrCeiling) y game_memory.h.
#pragma once

// Siguiente escalon por encima del limite actual (fps reales), o 0 si ya esta en el maximo:
// con VRR, un 13 % mas (la diferencia perceptible) sin pasar del techo; sin VRR, el divisor
// siguiente de los Hz.
static double NextLimitStep(double limitRealFps, int fgFactor, bool vrr, double hz) {
  const int k = std::max(1, fgFactor);
  const double screen = limitRealFps * k;
  if (vrr) return screen < VrrCeiling(hz) - 0.5 ? std::min(limitRealFps * 1.13, VrrCeiling(hz) / k) : 0.0;
  const int n = static_cast<int>(std::lround(hz / screen));
  return n > 1 ? hz / (n - 1) / k : 0.0;
}

// Se sugiere subir si el juego va sobrado (casi todo en el limite) o si su capacidad estimada en
// esta partida alcanza el siguiente escalon y la memoria del juego no lo contradice (con dos o mas
// partidas, su limite inferior al 80 % debe llegar al 95 % del escalon). Devuelve el escalon o 0.
static double SuggestRaise(double stableFps, bool plenty, const GameMemory& mem, double limitRealFps, int fgFactor,
                           bool vrr, double hz) {
  const double next = NextLimitStep(limitRealFps, fgFactor, vrr, hz);
  if (next <= 0) return 0.0;
  if (plenty) return next;
  const bool memoryAgrees = mem.sessions < 2 || mem.LowerFps() >= next * 0.95;
  return stableFps >= next && memoryAgrees ? next : 0.0;
}
