// FlatPace - estadisticas, detector de limites y recomendaciones
// Logica pura, sin dependencias de Windows: se incluye desde main.cpp (en el mismo punto donde
// estaba) y desde las pruebas automaticas (tests/). Requiere las cabeceras estandar ya incluidas, config.h, stats_basic.h y frame_sample.h, y las variables g_vrrMinHz y g_vrrMaxHz.
#pragma once

// ---------------------------------------------------------------------------
// Analisis
// ---------------------------------------------------------------------------
struct WindowStats {
  size_t frames = 0;      // fotogramas reales analizados
  size_t presents = 0;    // todo lo presentado (reales + generados)
  size_t generated = 0;
  int pauses = 0;
  size_t cpuInflated = 0;  // fotogramas con "trabajo de CPU" mayor que su duracion
  // Variacion entre fotogramas consecutivos (como CapFrameX): media de |dt(i) - dt(i-1)| y
  // proporcion de cambios menores de 2 ms. "real" = fotogramas reales; "screen" = todo lo
  // presentado (incluye los generados).
  double jitterMs = 0.0, steady2 = 0.0, screenJitterMs = 0.0, screenSteady2 = 0.0;
  double animErrMs = -1.0;  // media de |error de animacion| (-1 = no disponible)
  double medianMs = 0, p5Ms = 0, p95Ms = 0, p99Ms = 0;
  double cpuBusyMed = -1, cpuBusyP99 = -1, gpuBusyMed = -1, gpuBusyP99 = -1;
  double presentedFps = 0;
  std::string dominant;
  std::vector<double> realSorted;  // tiempos reales de fotograma (sin pausas largas)
  std::vector<double> workSorted;  // trabajo por fotograma: max(CPU, GPU) o tiempo de fotograma
  std::vector<double> gpuSorted;   // trabajo de GPU por fotograma

  // Limite activo detectado (VSync o limite del juego)
  bool capped = false;
  double capFps = 0.0;
  bool capFits = false;  // encaja con los Hz del monitor
  int capDivisor = 0;

  double GeneratedFraction() const {
    return presents ? static_cast<double>(generated) / presents : 0.0;
  }
  // Imagenes en pantalla por fotograma real (1 sin generacion, 2 con DLSS FG, 3-4 multiple).
  int FgFactor() const {
    if (GeneratedFraction() <= 0.05 || presents <= generated) return 1;
    double k = static_cast<double>(presents) / static_cast<double>(presents - generated);
    return std::clamp(static_cast<int>(std::lround(k)), 2, 4);
  }
  bool HasBusy() const { return cpuBusyP99 > 0 || gpuBusyP99 > 0; }

  // Con un limite activo, la espera del juego se cuenta como trabajo de CPU:
  // en ese caso solo la GPU es fiable.
  bool UseGpuOnly() const { return capped && gpuBusyP99 > 0; }

  double WorkP99() const {
    if (UseGpuOnly()) return gpuBusyP99;
    double w = std::max(cpuBusyP99, gpuBusyP99);
    return w > 0 ? w : p99Ms;
  }

  // Proporcion de fotogramas REALES que tardaron menos que un periodo dado.
  double FractionRealUnder(double periodMs) const {
    if (realSorted.empty()) return 0.0;
    auto it = std::lower_bound(realSorted.begin(), realSorted.end(), periodMs);
    return static_cast<double>(it - realSorted.begin()) / realSorted.size();
  }

  // Proporcion de fotogramas REALES que tardaron mas que un periodo dado.
  double FractionRealOver(double periodMs) const {
    if (realSorted.empty()) return 0.0;
    auto it = std::upper_bound(realSorted.begin(), realSorted.end(), periodMs);
    return static_cast<double>(realSorted.end() - it) / realSorted.size();
  }

  // Proporcion de fotogramas cuyo trabajo no cabria en un periodo dado.
  double FractionOver(double periodMs) const {
    const auto& v = UseGpuOnly() ? gpuSorted : workSorted;
    if (v.empty()) return 0.0;
    auto it = std::upper_bound(v.begin(), v.end(), periodMs);
    return static_cast<double>(v.end() - it) / v.size();
  }

  std::string Bottleneck() const {
    if (capped) return "ninguno: hay un limite activo (el margen de CPU no se puede medir)";
    if (gpuBusyMed > 0 && gpuBusyMed >= 0.85 * medianMs) return "GPU";
    if (cpuBusyMed > 0 && cpuBusyMed >= 0.85 * medianMs) return "CPU";
    if (!HasBusy()) return "desconocido";
    return "mixto";
  }
};

static WindowStats ComputeStats(const std::deque<FrameSample>& all, double from, double to) {
  WindowStats st;
  std::map<std::string, size_t> counts;
  for (const auto& s : all)
    if (s.t >= from && s.t <= to) counts[s.swapchain]++;
  size_t best = 0;
  for (const auto& [key, count] : counts)
    if (count > best) { best = count; st.dominant = key; }
  if (best == 0) return st;

  std::vector<double> ft, cpu;
  double presentSumMs = 0.0;
  double prevReal = -1.0, prevPresent = -1.0, jSum = 0.0, sjSum = 0.0;
  size_t jN = 0, j2 = 0, sjN = 0, sj2 = 0, aeN = 0;
  double aeSum = 0.0;
  for (const auto& s : all) {
    if (s.t < from || s.t > to || s.swapchain != st.dominant) continue;
    st.presents++;
    if (!std::isnan(s.animErrMs) && s.presentMs <= cfg::kSpikeMs && std::fabs(s.animErrMs) <= cfg::kSpikeMs) {
      aeSum += std::fabs(s.animErrMs);
      aeN++;
    }
    presentSumMs += s.presentMs;
    if (s.presentMs > 0 && s.presentMs <= cfg::kSpikeMs) {
      if (prevPresent > 0) {
        double d = std::fabs(s.presentMs - prevPresent);
        sjSum += d;
        sjN++;
        if (d < 2.0) sj2++;
      }
      prevPresent = s.presentMs;
    } else {
      prevPresent = -1.0;
    }
    if (s.generated) { st.generated++; continue; }
    if (s.realMs <= 0.0) continue;
    if (s.realMs > cfg::kSpikeMs) { st.pauses++; prevReal = -1.0; continue; }
    if (prevReal > 0) {
      double d = std::fabs(s.realMs - prevReal);
      jSum += d;
      jN++;
      if (d < 2.0) j2++;
    }
    prevReal = s.realMs;
    ft.push_back(s.realMs);
    if (s.cpuBusyMs >= 0) cpu.push_back(s.cpuBusyMs);
    if (s.gpuBusyMs >= 0) st.gpuSorted.push_back(s.gpuBusyMs);
    // El "trabajo" no puede superar lo que el fotograma tardo de verdad: si lo supera,
    // la CPU estaba esperando dentro del juego (Reflex, limitador interno) y se cuenta como busy.
    double w = std::max(s.cpuBusyMs, s.gpuBusyMs);
    st.workSorted.push_back(w > 0 ? std::min(w, s.realMs) : s.realMs);
    if (s.cpuBusyMs > s.realMs * 1.05) st.cpuInflated++;
  }
  if (presentSumMs > 0) st.presentedFps = 1000.0 * st.presents / presentSumMs;
  if (jN) { st.jitterMs = jSum / jN; st.steady2 = static_cast<double>(j2) / jN; }
  if (aeN > 50) st.animErrMs = aeSum / aeN;
  if (sjN) { st.screenJitterMs = sjSum / sjN; st.screenSteady2 = static_cast<double>(sj2) / sjN; }
  st.frames = ft.size();
  if (ft.empty()) return st;

  std::sort(ft.begin(), ft.end());
  st.realSorted = ft;
  std::sort(st.workSorted.begin(), st.workSorted.end());
  std::sort(st.gpuSorted.begin(), st.gpuSorted.end());
  st.medianMs = PercentileSorted(ft, 50);
  st.p5Ms = PercentileSorted(ft, 5);
  st.p95Ms = PercentileSorted(ft, 95);
  st.p99Ms = PercentileSorted(ft, 99);
  if (!cpu.empty()) {
    std::sort(cpu.begin(), cpu.end());
    st.cpuBusyMed = PercentileSorted(cpu, 50);
    st.cpuBusyP99 = PercentileSorted(cpu, 99);
  }
  // Si la CPU aparece "ocupada" mas tiempo del que duran los fotogramas, su medida no es
  // fiable: se limita a lo que realmente pudo trabajar.
  if (st.cpuInflated > st.frames / 20) {
    st.cpuBusyP99 = std::min(st.cpuBusyP99, st.p99Ms);
    st.cpuBusyMed = std::min(st.cpuBusyMed, st.medianMs);
  }
  if (!st.gpuSorted.empty()) {
    st.gpuBusyMed = PercentileSorted(st.gpuSorted, 50);
    st.gpuBusyP99 = PercentileSorted(st.gpuSorted, 99);
  }
  return st;
}

// Un limite activo se reconoce porque casi ningun fotograma va mas rapido que
// cierto valor, y ese valor coincide con un divisor de los Hz o con una cifra tipica.
static void DetectCap(WindowStats& st, double hz) {
  st.capped = false;
  st.capFits = false;
  st.capDivisor = 0;
  st.capFps = 0.0;
  if (st.frames < 100 || st.medianMs <= 0) return;
  // Un limite real deja la GPU esperando; si trabaja casi todo el tiempo, no hay limite.
  if (st.gpuBusyMed > 0 && st.gpuBusyMed >= 0.90 * st.medianMs) return;
  double fps = 1000.0 / st.medianMs;
  // Los limitadores internos de algunos juegos varian mas: si la cifra coincide casi
  // exacta (0.5 %) con un divisor o un limite tipico, se tolera mas dispersion.
  auto nearExact = [&](double f) { return std::fabs(fps - f) / f < 0.005; };
  bool exactMatch = false;
  for (int n = 1; n <= cfg::kMaxDivisor && !exactMatch; ++n) exactMatch = nearExact(hz / n);
  for (double f : {30.0, 60.0, 90.0, 120.0, 144.0, 165.0, 180.0, 200.0, 240.0, 300.0, 360.0})
    exactMatch = exactMatch || nearExact(f);
  if (st.p5Ms < (exactMatch ? 0.80 : 0.90) * st.medianMs) return;
  for (int n = 1; n <= cfg::kMaxDivisor; ++n) {
    double f = hz / n;
    if (std::fabs(fps - f) / f < 0.01) {
      st.capped = st.capFits = true;
      st.capFps = f;
      st.capDivisor = n;
      return;
    }
  }
  static const double kCommon[] = {30, 40, 45, 50, 60, 72, 75, 90, 100, 120, 144, 150, 160, 165,
                                   170, 175, 180, 200, 240, 280, 300, 360, 480, 500};
  for (double f : kCommon)
    if (std::fabs(fps - f) / f < 0.015) {
      st.capped = true;
      st.capFps = f;
      return;
    }
}

// Techo con VRR (G-SYNC/FreeSync): el juego no debe tocar el maximo de la pantalla, o el
// VRR deja de actuar. Se usa el margen mayor entre la regla clasica de 3 fps (Blur Busters)
// y la formula hz - hz^2/4096 (inspirada en Special K; coincide con el limite automatico de
// Reflex). A frecuencias altas manda la formula (240 Hz -> 225); a bajas, los 3 fps (60 -> 57).
static double VrrCeiling(double hz) { return std::floor(std::min(hz - 3.0, hz - hz * hz / 4096.0)); }

struct Recommendation {
  bool valid = false;
  int divisor = 0;      // refrescos por imagen EN PANTALLA
  double capFps = 0.0;  // ritmo en pantalla
  int fgFactor = 1;     // imagenes en pantalla por cada fotograma real (2 = DLSS FG)
  double realFps = 0.0; // ritmo de fotogramas reales = capFps / fgFactor
  bool tight = false;   // sin margen comodo, pero mejor que no limitar
  bool vrr = false;     // con G-SYNC: cualquier valor estable, sin divisores
  double lateFraction = 0.0;
  bool vrrValid = false;
  double vrrCapFps = 0.0;
};

// Con generacion de fotogramas, el ritmo que se ve es el de pantalla: debe encajar con
// los Hz, y los fotogramas reales (pantalla / factor) deben poder sostenerse.
static Recommendation Recommend(double hz, double workP99Ms, double margin, int fgFactor = 1) {
  Recommendation r;
  r.fgFactor = std::max(1, fgFactor);
  for (int n = 1; n <= cfg::kMaxDivisor * r.fgFactor; ++n) {
    double cap = hz / n;
    double real = cap / r.fgFactor;
    if (real < cfg::kMinCapFps) break;
    if (1000.0 / real >= workP99Ms * (1.0 + margin)) {
      r.valid = true;
      r.divisor = n;
      r.capFps = cap;
      r.realFps = real;
      break;
    }
  }
  double sustainable = 1000.0 / (workP99Ms * (1.0 + margin));
  r.vrrCapFps = std::min(VrrCeiling(hz), sustainable);
  r.vrrValid = r.vrrCapFps >= cfg::kVrrMinFps;
  return r;
}

constexpr double kTightMaxLate = 0.02;  // "ajustado": como mucho 2 % de fotogramas tarde

// Si ningun limite tiene margen comodo, no limitar deja el ritmo irregular, que es peor
// que un limite que falla en muy pocos fotogramas. Se elige el mas alto que cumpla eso.
constexpr double kHoldLateFactor = 1.25;  // "tarde" = mas de un 25 % sobre el periodo
constexpr double kHoldMaxLate = 0.02;     // un limite se sostiene si como mucho el 2 % llega tarde
constexpr double kHoldFailLate = 0.05;    // se considera que no se sostiene si mas del 5 % llega tarde

template <typename Stats>
static Recommendation RecommendFor(const Stats& st, double hz, double margin) {
  int k = st.FgFactor();
  // Con un limite activo, el trabajo de la GPU engana (baja frecuencias y tarda mas en cada
  // imagen). Si el juego cumple su ritmo con pocos fotogramas tarde, ese limite se sostiene.
  if (st.capped && st.capFits && st.capDivisor > 0) {
    double period = 1000.0 / st.capFps;
    if (st.FractionRealOver(period * kHoldLateFactor) <= kHoldMaxLate) {
      Recommendation r = Recommend(hz, st.WorkP99(), margin, k);
      if (!r.valid || r.capFps < st.capFps * k - 0.5) {
        r = Recommendation{};
        r.valid = true;
        r.divisor = st.capDivisor;
        r.capFps = st.capFps;
        r.fgFactor = k;
        r.realFps = st.capFps;
        r.vrrCapFps = VrrCeiling(hz);
        r.vrrValid = true;
      }
      return r;
    }
  }
  Recommendation r = Recommend(hz, st.WorkP99(), margin, k);
  if (r.valid) return r;
  for (int n = 1; n <= cfg::kMaxDivisor * k; ++n) {
    double cap = hz / n, real = cap / k;
    if (real < cfg::kMinCapFps) break;
    double late = st.FractionOver(1000.0 / real);
    if (late <= kTightMaxLate) {
      Recommendation t = r;
      t.valid = true;
      t.tight = true;
      t.divisor = n;
      t.capFps = cap;
      t.realFps = real;
      t.fgFactor = k;
      t.lateFraction = late;
      return t;
    }
  }
  return r;
}

static Recommendation DivisorRec(double hz, int n, int fgFactor = 1) {
  Recommendation r;
  r.divisor = n;
  r.capFps = hz / n;
  r.fgFactor = std::max(1, fgFactor);
  r.realFps = r.capFps / r.fgFactor;
  r.valid = r.realFps >= cfg::kMinCapFps;
  r.vrrCapFps = VrrCeiling(hz);
  r.vrrValid = true;
  return r;
}

// Fluidez: el limite mas alto que el juego alcanza la mayor parte del tiempo. Usa los
// ultimos minutos de juego: al menos el 70 % de los fotogramas reales llega a tiempo
// (con un 8 % de tolerancia) y menos del 3 % llega muy tarde (mas de un 50 %).
constexpr double kFluidPercentile = 70.0;
constexpr double kFluidTolerance = 1.08;
constexpr double kFluidVeryLate = 1.5;
constexpr double kFluidMaxVeryLate = 0.03;

static bool FluidHolds(const std::vector<double>& sorted, double periodMs) {
  if (sorted.size() < 200) return false;
  double p = PercentileSorted(sorted, kFluidPercentile);
  auto it = std::upper_bound(sorted.begin(), sorted.end(), periodMs * kFluidVeryLate);
  double veryLate = static_cast<double>(sorted.end() - it) / sorted.size();
  return p <= periodMs * kFluidTolerance && veryLate <= kFluidMaxVeryLate;
}

static Recommendation RecommendFluid(const std::vector<double>& sorted, double hz, int k) {
  Recommendation r;
  if (sorted.size() < 200) return r;
  k = std::max(1, k);
  for (int n = 1; n <= cfg::kMaxDivisor * k; ++n) {
    double cap = hz / n, real = cap / k;
    if (real < cfg::kMinCapFps) break;
    double period = 1000.0 / real;
    if (FluidHolds(sorted, period)) {
      r = DivisorRec(hz, n, k);
      r.tight = PercentileSorted(sorted, kFluidPercentile) > period;
      return r;
    }
  }
  return r;
}

// Con G-SYNC la pantalla muestra cada imagen al llegar: el limite no necesita ser un
// divisor de los Hz. Se elige el ritmo que el juego sostiene (segun la prioridad), en
// numeros enteros y siempre unos fps por debajo del maximo de la pantalla.
// heldReal: ritmo real que el juego esta sosteniendo bajo un limite (el suyo o el nuestro),
// o 0. Con un limite activo ningun fotograma va mas rapido que el limite pero algunos van
// mas lentos, asi que los percentiles quedan sesgados hacia abajo: lo demostrado manda.
// Con VRR (0.39, validado con partidas reales de Greylock y Megabonk), sobre los tiempos de
// fotograma reales sin tirones (> 2,5 x mediana: ningun limite los arregla, e inflaban el p99):
// - Fluidez: el ritmo que el juego alcanza el 90 % del tiempo (antes, el 70 %: el juego quedaba por
//   debajo del limite un 22-34 % de los segundos, y un 29 % de forma perceptible).
// - Estabilidad: su 1 % mas lento puede llegar como mucho un 13 % tarde (la diferencia perceptible):
//   1,13 x 1000 / p99 (antes 1000 / (p99 x 1,05) con tirones: Greylock 78-85 fps; ahora ~91-109,
//   sin caidas perceptibles).
// Con un limite activo (capReal), los datos estan censurados: nunca se recomienda subir (eso queda
// para la sugerencia, que exige que el juego vaya sobrado), y solo se baja si el juego queda por
// debajo de forma perceptible (> 13 %), como en la regla de fallos. Sin esa histeresis, el temblor
// del limitador hacia bajar el limite 1-2 fps en cada partida: 150 -> 87 en diez partidas (detectado
// por las pruebas del ciclo completo).
// Con un ritmo demostrado (heldReal: el juego clavado por su propio limite o VSync), su capacidad
// real es desconocida: se usa ese ritmo, ni mas ni menos.
constexpr double kVrrFluidPercentile = 90.0;
static Recommendation RecommendVrr(const std::vector<double>& longSorted, double workP99, double hz, int k,
                                   bool fluid, double heldReal = 0.0, double capReal = 0.0) {
  Recommendation r;
  k = std::max(1, k);
  double realRate = 0.0;
  if (longSorted.size() >= 200) {
    const double med = PercentileSorted(longSorted, 50);
    std::vector<double> body;
    body.reserve(longSorted.size());
    for (double x : longSorted)
      if (x <= med * 2.5) body.push_back(x);  // ya ordenado: sigue ordenado
    if (fluid) realRate = 1000.0 / PercentileSorted(body, kVrrFluidPercentile);
    else realRate = (1.0 + cfg::kJnd) * 1000.0 / PercentileSorted(body, 99);
  } else if (workP99 > 0) {
    realRate = 1000.0 / (workP99 * (1.0 + cfg::kMarginInitial));
  }
  if (capReal > 0) realRate = realRate >= capReal * (1.0 - cfg::kJnd) ? capReal : realRate;
  double maxScreen = VrrCeiling(hz);
  const int vmin = g_vrrMinHz, vmax = g_vrrMaxHz;
  // Si el VRR del monitor no llega a su frecuencia maxima, el techo es el de su rango VRR.
  if (vmax > 0 && vmax < hz - 1.0) maxScreen = std::min(maxScreen, VrrCeiling(vmax));
  if (heldReal > 0) {
    realRate = heldReal;
    // Si lo que limita es el maximo de la pantalla, el juego puede al menos eso.
    if (heldReal * k >= maxScreen) realRate = std::max(realRate, maxScreen / k);
  }
  double screen = std::min(maxScreen, std::floor(realRate * k));
  screen = std::floor(screen / k) * k;  // que los reales tambien sean un numero entero
  if (screen / k < cfg::kMinCapFps) return r;
  // Por debajo del rango VRR sin LFC, el monitor no puede seguir el ritmo: mejor un divisor.
  if (vmin > 0 && screen < vmin && vmax > 0 && vmax < 2 * vmin) return r;
  r.valid = true;
  r.vrr = true;
  r.capFps = screen;
  r.fgFactor = k;
  r.realFps = screen / k;
  r.vrrValid = true;
  r.vrrCapFps = screen;
  return r;
}

// Percentil de los tiempos de fotograma reales causado por el juego con un limite activo (capMs).
// Un fotograma mas lento que el limite cuenta como tal solo si su trabajo de CPU/GPU supero el 95 %
// del limite; si no, el retraso fue del temporizador del limitador (que suele compensarlo adelantando
// el siguiente fotograma), no del juego. Sin limite (capMs <= 0) es el percentil normal.
// Validado con partidas reales (0.37.2): Megabonk con 195 fps, 1 % bajo medido 177 -> 195 causado por
// el juego (el 97 % de los fotogramas tardios tenian ~2,2 ms de trabajo); Greylock con 90, 76 -> 84
// (ahi si hay escenas pesadas en las que el juego no llega).
static double GameCausedPercentileMs(const std::vector<double>& realMs, const std::vector<double>& workMs, double capMs,
                                     double p) {
  std::vector<double> v;
  v.reserve(realMs.size());
  for (size_t i = 0; i < realMs.size(); ++i) {
    double x = realMs[i];
    if (x <= 0 || x > 250.0) continue;  // pausas y cargas
    const double w = i < workMs.size() ? workMs[i] : -1.0;
    if (capMs > 0 && x > capMs && w >= 0 && w < capMs * 0.95) x = capMs;
    v.push_back(x);
  }
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  return PercentileSorted(v, p);
}
