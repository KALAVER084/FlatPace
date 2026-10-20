// FlatPace - ciclo de decision: verificar, vigilar, bajar tras un fallo y decidir al cerrar
// Logica pura, sin dependencias de Windows: se incluye desde main.cpp y desde las pruebas
// automaticas (tests/). Requiere las cabeceras estandar, config.h, stats_basic.h y analysis.h.
#pragma once

// ---------------------------------------------------------------------------
// Verificacion: el limite aplicado esta funcionando ("LIMITE FUNCIONANDO")
// ---------------------------------------------------------------------------
struct VerifyInput {
  double expectedFps = 0.0;    // ritmo real esperado
  int expectFg = 1;            // factor de generacion con el que se aplico
  double fpsNow = 0.0;         // ritmo real medido (1000 / mediana)
  int fgNow = 1;               // factor de generacion medido
  double presentedFps = 0.0;   // ritmo en pantalla medido
  double fractionFast = 0.0;   // fotogramas mas de un 7 % por debajo del periodo esperado
  bool capped = false;         // el detector de limites ve un limite
  double capFps = 0.0;         // ritmo del limite detectado
};

// Tres formas de reconocerlo:
// - con generacion: los ritmos real (+-2 %) y en pantalla (+-4 %) coinciden con los esperados (con
//   generacion la GPU parece siempre ocupada y el detector de limites no sirve);
// - directa: el ritmo coincide (+-1,5 %) y menos del 10 % de los fotogramas va mas de un 7 % por
//   debajo del periodo (el limitador del driver deja pasar algun fotograma algo mas rapido);
// - el detector de limites ve un limite al ritmo esperado (+-2 %).
static bool LimitVerified(const VerifyInput& v) {
  const double exp = v.expectedFps;
  if (exp <= 0 || v.fpsNow <= 0) return false;
  const bool fgMatch = v.expectFg > 1 && v.fgNow > 1 && std::fabs(v.fpsNow - exp) / exp < 0.02 &&
                       std::fabs(v.presentedFps - exp * v.expectFg) / (exp * v.expectFg) < 0.04;
  const bool directMatch = v.expectFg <= 1 && std::fabs(v.fpsNow - exp) / exp < 0.015 && v.fractionFast < 0.10;
  const bool capMatch = v.capped && std::fabs(v.capFps - exp) / exp < 0.02;
  return fgMatch || directMatch || capMatch;
}

// ---------------------------------------------------------------------------
// Regla de fallos: tramos en los que el juego va claramente por debajo de su limite
// ---------------------------------------------------------------------------
// Cada segundo con el limite funcionando se anota el ritmo real. Un segundo cuenta como "por
// debajo" si el ritmo queda mas de un 13 % (la diferencia perceptible) por debajo del limite; solo
// cuentan los tramos de al menos 5 s seguidos (los primeros 5 s se suman al llegar al quinto).
// El limite fallo si esos tramos suman al menos 60 s y al menos el 10 % del tiempo con el limite.
struct FailureTracker {
  double limPlaySecs = 0.0;  // tiempo de juego con el limite funcionando
  double failSecs = 0.0;     // tiempo en tramos por debajo
  int belowRun = 0;          // segundos seguidos por debajo
  std::vector<float> secRates;

  void Add(double rate, double expectedFps, double dt) {
    limPlaySecs += dt;
    secRates.push_back(static_cast<float>(rate));
    if (rate < expectedFps * (1.0 - cfg::kJnd)) {
      ++belowRun;
      if (belowRun == 5) failSecs += 5.0;
      else if (belowRun > 5) failSecs += dt;
    } else {
      belowRun = 0;
    }
  }
  bool Failed() const { return failSecs >= 60.0 && failSecs >= 0.10 * limPlaySecs; }
  double Percent() const { return 100.0 * failSecs / std::max(1.0, limPlaySecs); }
};

// Limite al que se baja tras un fallo: con VRR, el ritmo que el juego supero el 90 % de los
// segundos (percentil 10 de los ritmos por segundo); sin VRR, el divisor siguiente de los Hz.
static Recommendation LowerAfterFailure(std::vector<float> secRates, bool vrr, int fgFactor, double expectedFps,
                                        double hz) {
  Recommendation lower;
  const int k = std::max(1, fgFactor);
  if (vrr) {
    if (secRates.empty()) return lower;
    std::sort(secRates.begin(), secRates.end());
    const double real = std::floor(secRates[secRates.size() / 10]);
    if (real >= cfg::kMinCapFps) {
      lower.valid = lower.vrr = lower.vrrValid = true;
      lower.fgFactor = k;
      lower.realFps = real;
      lower.capFps = real * k;
      lower.vrrCapFps = lower.capFps;
    }
    return lower;
  }
  const int n = std::max(1, static_cast<int>(std::lround(hz / std::max(1.0, expectedFps * k))));
  return DivisorRec(hz, n + 1, k);
}

// ---------------------------------------------------------------------------
// Decision al cerrar el juego, con toda la partida
// ---------------------------------------------------------------------------
struct SessionDecisionInput {
  std::map<int, std::vector<float>> realByFg;  // tiempos reales por factor de generacion
  double gameSecs = 0.0, vrrSecs = 0.0;        // tiempo de juego y con VRR actuando
  double ownCapSecs = 0.0, ownCapRateSum = 0.0;  // juego limitado por si mismo (sin limite nuestro)
  bool fluid = false;                          // prioridad Fluidez
  double hz = 0.0;
  double capReal = 0.0;                        // limite activo nuestro (fps reales), 0 si no hay
};

// Se usa el estado de generacion mas jugado; VRR si actuo mas de la mitad del tiempo. El ritmo
// "demostrado" (casi todos los fotogramas iguales, o el juego limitado por si mismo la mayor parte
// de la partida) nunca se rebaja. Sin al menos 600 fotogramas, no se decide.
static Recommendation DecideFromSession(const SessionDecisionInput& in) {
  Recommendation none;
  int k = 1;
  size_t best = 0;
  for (const auto& [kk, v] : in.realByFg)
    if (v.size() > best) {
      best = v.size();
      k = kk;
    }
  if (best < 600) return none;
  const auto& vf = in.realByFg.at(k);
  std::vector<double> d(vf.begin(), vf.end());
  std::sort(d.begin(), d.end());
  const bool vrr = in.gameSecs > 0 && in.vrrSecs > in.gameSecs * 0.5;
  const double p99 = PercentileSorted(d, 99), p10 = PercentileSorted(d, 10), p90 = PercentileSorted(d, 90);
  const double med = PercentileSorted(d, 50);
  double held = (p10 > 0 && p90 / p10 < 1.06) ? 1000.0 / med : 0.0;
  if (in.gameSecs > 0 && in.ownCapSecs > in.gameSecs * 0.5)
    held = std::max(held, in.ownCapRateSum / in.ownCapSecs);
  if (vrr) {
    Recommendation vr = RecommendVrr(d, p99, in.hz, k, in.fluid, held, in.capReal);
    if (vr.valid) return vr;  // si no (por debajo del rango VRR sin LFC), un divisor
  }
  Recommendation r = in.fluid ? RecommendFluid(d, in.hz, k) : Recommendation{};
  if (!r.valid) r = Recommend(in.hz, p99, cfg::kMarginInitial, k);
  return r;
}
