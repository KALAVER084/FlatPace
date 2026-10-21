// FlatPace - ciclo de decision: verificar, vigilar, bajar tras un fallo y decidir al cerrar
// Logica pura, sin dependencias de Windows: se incluye desde main.cpp y desde las pruebas
// automaticas (tests/). Requiere las cabeceras estandar, config.h, stats_basic.h, analysis.h y
// suggest.h (NextLimitStep).
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
constexpr size_t kCloseTrimSec = 60;  // segundos finales que no cuentan para la regla de fallos
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
  // Copia sin los ultimos n segundos: el cierre del juego (menus, pantallas de salida o un cuelgue que
  // sigue presentando imagenes) no debe contar como un fallo del limite.
  FailureTracker WithoutLast(size_t n, double expectedFps) const {
    FailureTracker t;
    const size_t m = secRates.size() > n ? secRates.size() - n : 0;
    for (size_t i = 0; i < m; ++i) t.Add(secRates[i], expectedFps, 1.0);
    return t;
  }
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

// ---------------------------------------------------------------------------
// Subida automatica: si el juego va sobrado con su limite, subirlo al siguiente escalon
// ---------------------------------------------------------------------------
// Las reglas eligen el limite con una partida sin limite, y medir sin limite exagera lo que el juego
// necesita: con un limite puesto, muchos juegos trabajan menos (Megabonk sostenia 225 fps cuando las
// reglas daban 189-215). Asi que, si el juego va sobrado con su limite, este sube solo al siguiente
// escalon (un 13 % mas, sin pasar del techo de la pantalla), y se comprueba y vigila como cualquier otro.
// Protecciones contra el "ping-pong" (subir, fallar, bajar, volver a subir...):
// - si un limite subido asi falla, se vuelve al anterior (que si se sostenia), no al ritmo superado
//   el 90 % del tiempo, que podria quedar por debajo;
// - el limite que fallo se recuerda como techo durante 30 dias: no se vuelve a subir hasta el;
// - con prioridad Estabilidad hacen falta dos partidas seguidas yendo sobrado; con Fluidez, una.
struct ClimbState {
  double failedCeil = 0.0;  // limite (fps reales) que fallo
  long long failedDay = 0;  // dia en que fallo (dias desde 1970)
  double prevGood = 0.0;    // limite anterior a una subida aun no confirmada (0 = no hay)
  int plentyStreak = 0;     // partidas seguidas yendo sobrado

  static ClimbState Parse(const std::string& s) {
    ClimbState c;
    if (!s.empty()) std::sscanf(s.c_str(), "%lf;%lld;%lf;%d", &c.failedCeil, &c.failedDay, &c.prevGood, &c.plentyStreak);
    return c;
  }
  std::string Serialize() const {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%.3f;%lld;%.3f;%d", failedCeil, failedDay, prevGood, plentyStreak);
    return buf;
  }
};

enum class ClimbAction { None, Raise, RevertToPrev, BlockedByCeiling, AtTop };
struct ClimbResult {
  ClimbAction action = ClimbAction::None;
  double newLimit = 0.0;  // fps reales (Raise y RevertToPrev)
  ClimbState state;
};

constexpr int kCeilingDays = 30;

// failed: la regla de fallos detecto que no se sostuvo; plenty: fue sobrado toda la partida.
static ClimbResult DecideClimb(ClimbState st, bool failed, bool plenty, double limitReal, int fgFactor, bool vrr,
                               double hz, bool fluid, long long today) {
  ClimbResult r;
  if (st.failedCeil > 0 && today - st.failedDay > kCeilingDays) st.failedCeil = 0.0;  // techo caducado
  if (failed) {
    st.failedCeil = limitReal;
    st.failedDay = today;
    st.plentyStreak = 0;
    if (st.prevGood > 0) {
      r.action = ClimbAction::RevertToPrev;
      r.newLimit = st.prevGood;
    }
    st.prevGood = 0.0;
    r.state = st;
    return r;
  }
  if (!plenty) {
    st.plentyStreak = 0;
    st.prevGood = 0.0;  // la subida se sostuvo una partida entera: queda confirmada
    r.state = st;
    return r;
  }
  st.prevGood = 0.0;
  st.plentyStreak = std::min(st.plentyStreak + 1, 9);
  const int need = fluid ? 1 : 2;
  if (st.plentyStreak >= need) {
    const double next = std::floor(NextLimitStep(limitReal, fgFactor, vrr, hz) + 1e-9);
    if (next <= limitReal) {
      r.action = ClimbAction::AtTop;
    } else if (st.failedCeil > 0 && next >= st.failedCeil - 0.5) {
      r.action = ClimbAction::BlockedByCeiling;
    } else {
      r.action = ClimbAction::Raise;
      r.newLimit = next;
      st.prevGood = limitReal;
      st.plentyStreak = 0;
    }
  }
  r.state = st;
  return r;
}

// ---------------------------------------------------------------------------
// Prioridad por juego: de Fluidez a Estabilidad si el juego es inestable con su limite
// ---------------------------------------------------------------------------
// La prioridad global es el punto de partida de cada juego. Con Fluidez, si el 1 % bajo causado por
// el juego se hunde de forma sostenida (la regla de fallos aplicada al 1 % bajo) en dos partidas
// seguidas, el juego pasa a Estabilidad. Solo en esa direccion: con un limite que ya se sostiene, el
// juego parece estable precisamente porque el limite es el correcto, y volver a Fluidez lo subiria
// otra vez (un ping-pong entre prioridades); con Estabilidad, un juego que va sobrado ya sube solo.
// La medida se toma con el limite puesto: sin limite, la inestabilidad se exagera (Megabonk varia
// mucho sin limite y es el juego mas estable con el).
struct AutoPriority {
  bool stability = false;  // la app eligio Estabilidad para este juego
  int unstableStreak = 0;  // partidas seguidas con el 1 % bajo hundido (con Fluidez)

  static AutoPriority Parse(const std::string& s) {
    AutoPriority a;
    int st = 0;
    if (!s.empty()) std::sscanf(s.c_str(), "%d;%d", &st, &a.unstableStreak);
    a.stability = st == 1;
    return a;
  }
  std::string Serialize() const {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d;%d", stability ? 1 : 0, unstableStreak);
    return buf;
  }
};

constexpr int kUnstableSessions = 2;

// tail: el 1 % bajo de la partida (sin el ultimo minuto). Una partida corta (< 3 min) no cuenta.
static AutoPriority UpdateAutoPriority(AutoPriority a, bool fluidNow, const FailureTracker& tail) {
  if (!fluidNow || a.stability) return a;
  if (tail.Failed()) {
    if (++a.unstableStreak >= kUnstableSessions) {
      a.stability = true;
      a.unstableStreak = 0;
    }
  } else if (tail.limPlaySecs >= 180.0) {
    a.unstableStreak = 0;  // una partida larga y limpia corta la racha
  }
  return a;
}
