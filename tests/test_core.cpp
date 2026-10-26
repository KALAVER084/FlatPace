// FlatPace - pruebas automaticas de la logica pura (src/core).
// Se compilan en cualquier sistema (no necesitan Windows):
//   g++ -std=c++20 -O1 -Wall -Wextra tests/test_core.cpp -o test_core && ./test_core
// Cada prueba fija un comportamiento que ya se valido con partidas reales o en simulacion,
// para que un cambio futuro no lo rompa sin que se note.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <deque>
#include <limits>
#include <map>
#include <random>
#include <string>
#include <vector>

typedef unsigned long DWORD;  // en Windows lo define windows.h
static std::atomic<int> g_vrrMinHz{0}, g_vrrMaxHz{0};

#include "../src/core/config.h"
#include "../src/core/stats_basic.h"
#include "../src/core/censored.h"
#include "../src/core/bocpd.h"
#include "../src/core/game_memory.h"
#include "../src/core/display_info.h"
#include "../src/core/edid.h"
#include "../src/core/frame_sample.h"
#include "../src/core/analysis.h"
#include "../src/core/suggest.h"
#include "../src/core/decision.h"
#include "../src/core/limit_request.h"
#include "../src/core/log_text.h"
#include "../src/core/limit_memory.h"
#include "../src/core/explain.h"
namespace overlay {
#include "../src/core/pace_basics.h"
#include "../src/core/pace_series.h"
}  // namespace overlay

// ---------------------------------------------------------------------------
// Mini marco de pruebas
// ---------------------------------------------------------------------------
static int g_checks = 0, g_failures = 0;
static const char* g_test = "";

#define CHECK(cond)                                                                       \
  do {                                                                                    \
    ++g_checks;                                                                           \
    if (!(cond)) {                                                                        \
      ++g_failures;                                                                       \
      std::printf("  FALLO [%s] %s:%d: %s\n", g_test, __FILE__, __LINE__, #cond);        \
    }                                                                                     \
  } while (0)

#define CHECK_NEAR(a, b, eps)                                                             \
  do {                                                                                    \
    ++g_checks;                                                                           \
    double va_ = (a), vb_ = (b);                                                          \
    if (!(std::fabs(va_ - vb_) <= (eps))) {                                               \
      ++g_failures;                                                                       \
      std::printf("  FALLO [%s] %s:%d: %s = %.4f, se esperaba %.4f (+-%.4f)\n", g_test,   \
                  __FILE__, __LINE__, #a, va_, vb_, static_cast<double>(eps));            \
    }                                                                                     \
  } while (0)

#define TEST(name) static void name()
#define RUN(name)        \
  do {                   \
    g_test = #name;      \
    name();              \
  } while (0)

// Fotogramas sinteticos: tiempos con variacion gaussiana y algun tiron.
static std::deque<FrameSample> MakeFrames(double meanMs, double sdMs, int count, double gpuBusyMs,
                                          unsigned seed = 1, double floorMs = 0.0) {
  std::mt19937 rng(seed);
  std::normal_distribution<double> nd(meanMs, sdMs);
  std::deque<FrameSample> out;
  double t = 0.0;
  for (int i = 0; i < count; ++i) {
    FrameSample s;
    double ms = std::max(floorMs > 0 ? floorMs : 0.5, nd(rng));
    t += ms / 1000.0;
    s.t = t;
    s.presentMs = ms;
    s.realMs = ms;
    s.presentRealMs = ms;
    s.gpuBusyMs = gpuBusyMs;
    s.cpuBusyMs = gpuBusyMs * 0.8;
    s.swapchain = "1:0x1";
    s.pid = 1;
    out.push_back(s);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Estadisticas basicas
// ---------------------------------------------------------------------------
TEST(Percentiles) {
  std::vector<double> v = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  CHECK_NEAR(PercentileSorted(v, 50), 5, 1e-9);
  CHECK_NEAR(PercentileSorted(v, 99), 10, 1e-9);
  CHECK_NEAR(PercentileSorted(v, 1), 1, 1e-9);
  CHECK_NEAR(PercentileSorted({}, 50), 0, 1e-9);
}

TEST(PaceSwing) {
  // Ritmo constante: sin variacion.
  std::vector<double> flat(3000, 4.44);
  CHECK_NEAR(PaceSwingMs(flat), 0.0, 1e-9);
  // Ritmo que sube y baja con la escena: variacion clara.
  std::vector<double> wavy;
  for (int i = 0; i < 3000; ++i) wavy.push_back(5.0 + 0.8 * std::sin(i / 60.0));
  CHECK(PaceSwingMs(wavy) > 0.3);
  // Pocos datos: no disponible.
  CHECK(PaceSwingMs(std::vector<double>(20, 4.0)) < 0);
}

TEST(Consistency) {
  auto steady = MakeFrames(4.44, 0.05, 2000, 2.0);
  WindowStats st = ComputeStats(steady, 0, 1e9);
  CHECK(st.frames > 1900);
  CHECK_NEAR(st.medianMs, 4.44, 0.02);
  CHECK(st.steady2 > 0.99);
  auto rough = MakeFrames(8.0, 2.5, 2000, 7.0, 2);
  WindowStats rs = ComputeStats(rough, 0, 1e9);
  CHECK(rs.steady2 < st.steady2);
  CHECK(rs.jitterMs > st.jitterMs);
}

// ---------------------------------------------------------------------------
// Detector de limites
// ---------------------------------------------------------------------------
TEST(DetectCapAtDivisor) {
  // 120 fps clavados en una pantalla de 240 Hz, con la GPU a medio gas: limite activo, 240/2.
  auto f = MakeFrames(1000.0 / 120.0, 0.03, 2000, 4.0, 3);
  WindowStats st = ComputeStats(f, 0, 1e9);
  DetectCap(st, 240.0);
  CHECK(st.capped);
  CHECK(st.capFits);
  CHECK(st.capDivisor == 2);
}

TEST(DetectCapNotWhenGpuBound) {
  // GPU ocupada todo el tiempo: no es un limite, es el maximo que da el equipo.
  auto f = MakeFrames(1000.0 / 120.0, 0.03, 2000, 8.3, 4);
  WindowStats st = ComputeStats(f, 0, 1e9);
  DetectCap(st, 240.0);
  CHECK(!st.capped);
}

// ---------------------------------------------------------------------------
// Recomendaciones
// ---------------------------------------------------------------------------
TEST(GameCausedLowIgnoresLimiterDelays) {
  // Limite de 195 (5,13 ms). El temporizador del limitador retrasa un 7 % de los fotogramas
  // (+0,5 ms, compensado en el siguiente) con un trabajo del juego de ~2,2 ms: el 1 % bajo medido
  // cae por debajo del limite, pero el causado por el juego es el limite (caso Megabonk real).
  const double cap = 1000.0 / 195.0;
  std::mt19937 rng(41);
  std::uniform_real_distribution<double> u(0.0, 1.0);
  std::vector<double> r, w;
  for (int i = 0; i < 20000; ++i) {
    const bool late = u(rng) < 0.07;
    r.push_back(late ? cap + 0.5 : cap - (u(rng) < 0.07 ? 0.4 : 0.0));
    w.push_back(2.2);
  }
  std::vector<double> sorted = r;
  std::sort(sorted.begin(), sorted.end());
  CHECK(1000.0 / PercentileSorted(sorted, 99) < 185);                       // medido: por debajo
  CHECK_NEAR(GameCausedPercentileMs(r, w, cap, 99), cap, 1e-9);            // del juego: el limite
}

TEST(GameCausedLowCountsHeavyScenes) {
  // Limite de 90 (11,11 ms) y un 3 % de fotogramas en escenas pesadas de verdad (13 ms de trabajo):
  // esos si cuentan (caso Greylock real).
  const double cap = 1000.0 / 90.0;
  std::vector<double> r, w;
  for (int i = 0; i < 10000; ++i) {
    const bool heavy = i % 33 == 0;
    r.push_back(heavy ? 13.0 : cap);
    w.push_back(heavy ? 12.8 : 8.0);
  }
  CHECK_NEAR(GameCausedPercentileMs(r, w, cap, 99), 13.0, 1e-9);
  CHECK_NEAR(GameCausedPercentileMs(r, w, 0.0, 99), 13.0, 1e-9);           // sin limite: percentil normal
}

TEST(VrrCeilingValues) {
  CHECK_NEAR(VrrCeiling(60), 57, 1e-9);
  CHECK_NEAR(VrrCeiling(120), 116, 1e-9);
  CHECK_NEAR(VrrCeiling(144), 138, 1e-9);
  CHECK_NEAR(VrrCeiling(240), 225, 1e-9);
  CHECK_NEAR(VrrCeiling(360), 328, 1e-9);
}

TEST(DivisorRecommendation) {
  // Sin VRR a 240 Hz, trabajo p99 de 4 ms (+5 %): 240 no cabe (4,17 ms), 120 si.
  Recommendation r = Recommend(240.0, 4.0, cfg::kMarginInitial);
  CHECK(r.valid);
  CHECK(r.divisor == 2);
  CHECK_NEAR(r.capFps, 120.0, 1e-6);
}

TEST(FluidRecommendation) {
  // Fluidez sin VRR a 240 Hz: el 70 % de los fotogramas cabe en 8,33 ms (120 fps) pero no en
  // 6,25 ms (160): recomienda 120, el divisor mas alto que el juego alcanza la mayor parte del tiempo.
  std::vector<double> d;
  for (int i = 0; i < 3000; ++i) d.push_back(i % 10 < 8 ? 7.0 : 9.5);
  std::sort(d.begin(), d.end());
  Recommendation r = RecommendFluid(d, 240.0, 1);
  CHECK(r.valid);
  CHECK_NEAR(r.capFps, 120.0, 1e-6);
}

TEST(VrrRecommendation) {
  g_vrrMinHz = 0;
  g_vrrMaxHz = 0;
  std::vector<double> d(3000, 4.0);
  // Estabilidad con G-SYNC a 240 Hz y trabajo p99 de 4 ms: el techo manda (225).
  Recommendation r = RecommendVrr(d, 4.0, 240.0, 1, false);
  CHECK(r.valid);
  CHECK(r.vrr);
  CHECK_NEAR(r.capFps, 225, 1e-9);
  // Con generacion x2: el ritmo en pantalla es multiplo de 2.
  Recommendation g = RecommendVrr(d, 15.0, 240.0, 2, false);
  CHECK(g.valid);
  CHECK(static_cast<int>(g.capFps) % 2 == 0);
  CHECK_NEAR(g.realFps * 2, g.capFps, 1e-9);
}

TEST(VrrRulesUsePerceptibleThreshold) {
  // Distribucion como la de Greylock sin limite: mediana ~6 ms, cola hasta ~11 ms, y un 1 % de tirones.
  std::vector<double> d;
  std::mt19937 rng(51);
  std::lognormal_distribution<double> ln(std::log(6.2), 0.22);
  for (int i = 0; i < 20000; ++i) d.push_back(i % 100 == 0 ? 60.0 : ln(rng));
  std::sort(d.begin(), d.end());
  std::vector<double> body;
  for (double x : d)
    if (x <= PercentileSorted(d, 50) * 2.5) body.push_back(x);
  Recommendation st = RecommendVrr(d, 0.0, 240.0, 1, false);
  Recommendation fl = RecommendVrr(d, 0.0, 240.0, 1, true);
  CHECK(st.valid && fl.valid);
  // Estabilidad: 1,13 x 1000 / p99 sin tirones (los tirones de 60 ms no cuentan)
  CHECK_NEAR(st.capFps, std::floor(1.13 * 1000.0 / PercentileSorted(body, 99)), 1.0);
  // Fluidez: el ritmo alcanzado el 90 % del tiempo
  CHECK_NEAR(fl.capFps, std::floor(1000.0 / PercentileSorted(body, 90)), 1.0);
  CHECK(fl.capFps > st.capFps);
}

TEST(VrrNeverRaisesAboveActiveLimit) {
  // Con un limite activo (100 fps reales) y datos censurados en el limite, no se recomienda subir.
  std::vector<double> d(5000, 10.0);
  Recommendation r = RecommendVrr(d, 0.0, 240.0, 1, false, 0.0, 100.0);
  CHECK(r.valid);
  CHECK(r.capFps <= 100.0);
}

TEST(VrrRangeCeiling) {
  // Monitor de 144 Hz cuyo VRR solo llega a 120: el techo es el del rango VRR (116).
  g_vrrMinHz = 48;
  g_vrrMaxHz = 120;
  std::vector<double> d(3000, 4.0);
  Recommendation r = RecommendVrr(d, 4.0, 144.0, 1, false);
  CHECK(r.valid);
  CHECK_NEAR(r.capFps, 116, 1e-9);
  g_vrrMinHz = 0;
  g_vrrMaxHz = 0;
}

TEST(VrrBelowRangeWithoutLfc) {
  // Juego a ~38 fps, por debajo de un rango 48-90 Hz sin LFC: la recomendacion VRR se descarta.
  std::vector<double> d(3000, 25.0);
  g_vrrMinHz = 48;
  g_vrrMaxHz = 90;
  CHECK(!RecommendVrr(d, 25.0, 90.0, 1, false).valid);
  // El mismo juego con un rango 48-144 (con LFC): se mantiene.
  g_vrrMaxHz = 144;
  CHECK(RecommendVrr(d, 25.0, 144.0, 1, false).valid);
  g_vrrMinHz = 0;
  g_vrrMaxHz = 0;
}

// ---------------------------------------------------------------------------
// EDID
// ---------------------------------------------------------------------------
TEST(EdidRangeAndName) {
  std::vector<uint8_t> e(128, 0);
  e[8] = 0x09;  // fabricante "BOE": B=2, O=15, E=5 -> 0x09E5
  e[9] = 0xE5;
  // Descriptor 2 (offset 72): limites de rango 60-240 Hz
  uint8_t* r = &e[72];
  r[3] = 0xFD;
  r[5] = 60;
  r[6] = 240;
  // Descriptor 3 (offset 90): nombre
  uint8_t* n = &e[90];
  n[3] = 0xFC;
  const char* name = "TESTPANEL";
  for (int i = 0; name[i]; ++i) n[5 + i] = static_cast<uint8_t>(name[i]);
  n[5 + 9] = 0x0A;
  DisplayPathInfo info;
  ParseEdid(e, info);
  CHECK(info.rangeMin == 60);
  CHECK(info.rangeMax == 240);
  CHECK(info.edidName == L"TESTPANEL");
  CHECK(info.freeSyncMin == 0);
}

TEST(EdidFreeSyncBlock) {
  std::vector<uint8_t> e(256, 0);
  e[8] = 0x09;
  e[9] = 0xE5;
  uint8_t* x = &e[128];  // extension CTA-861
  x[0] = 0x02;
  x[2] = 20;             // los descriptores de detalle empiezan en 20
  uint8_t* b = &x[4];    // bloque especifico de fabricante (tag 3), 8 bytes de datos
  b[0] = static_cast<uint8_t>((3 << 5) | 8);
  b[1] = 0x1A;  // OUI de AMD 00-00-1A
  b[2] = 0x00;
  b[3] = 0x00;
  b[6] = 48;
  b[7] = 144;
  DisplayPathInfo info;
  ParseEdid(e, info);
  CHECK(info.freeSyncMin == 48);
  CHECK(info.freeSyncMax == 144);
}

// ---------------------------------------------------------------------------
// Grafica de ritmo
// ---------------------------------------------------------------------------
TEST(PaceToleranceAndScale) {
  CHECK_NEAR(overlay::PaceTolerance(8.33), 0.13 * 8.33, 1e-9);
  CHECK_NEAR(overlay::PaceTolerance(2.0), 0.5, 1e-9);    // minimo
  CHECK_NEAR(overlay::PaceTolerance(27.8), 0.13 * 27.8, 1e-9);  // 36 fps: el 13 %, sin el maximo de 2 ms
  CHECK_NEAR(overlay::PaceTolerance(60.0), 6.0, 1e-9);           // maximo (por debajo de ~22 fps)
  // Margen r = max(2 ms, 50 % del objetivo): a 4,44 ms, r = 2,22; a 2 ms, el minimo de 2 ms.
  auto s = overlay::PaceScale(4.44);
  CHECK_NEAR(s.lo, 4.44 - 2.22, 1e-9);
  CHECK_NEAR(s.hi, 4.44 + 1.6 * 2.22, 1e-9);
  auto t = overlay::PaceScale(2.0);
  CHECK_NEAR(t.lo, 0.0, 1e-9);
  CHECK_NEAR(t.hi, 2.0 + 3.2, 1e-9);
}

static std::vector<std::pair<double, double>> Timeline(const std::deque<FrameSample>& f) {
  std::vector<std::pair<double, double>> tl;
  for (const auto& s : f) tl.push_back({s.t, s.presentRealMs});
  return tl;
}

TEST(SeriesFlatWithinTolerance) {
  // Ritmo dentro de la tolerancia: todos los tramos sobre la referencia (linea recta).
  auto f = MakeFrames(4.44, 0.08, 3000, 2.0, 5);
  auto tl = Timeline(f);
  overlay::PaceClassCache cache;
  double display = tl.back().first;
  auto pts = overlay::BuildTimeSeries(tl, display, overlay::kPaceSpanSec, 130, 4.44, cache, 1);
  CHECK(pts.size() > 100);
  int off = 0;
  for (const auto& p : pts) off += std::fabs(p.ms - 4.44) > 1e-6;
  CHECK(off == 0);
}

TEST(SeriesMarksStutter) {
  auto f = MakeFrames(4.44, 0.08, 3000, 2.0, 6);
  f[2900].presentRealMs = 30.0;  // un tiron
  auto tl = Timeline(f);
  overlay::PaceClassCache cache;
  auto pts = overlay::BuildTimeSeries(tl, tl.back().first, overlay::kPaceSpanSec, 130, 4.44, cache, 1);
  int spikes = 0;
  for (const auto& p : pts) spikes += p.spike;
  CHECK(spikes == 1);
}

TEST(SeriesStableWhileScrolling) {
  // Un tramo ya completo conserva su clasificacion aunque la referencia cambie despues: no
  // aparecen ni desaparecen picos mientras la grafica se desplaza.
  auto f = MakeFrames(5.0, 0.6, 4000, 3.0, 7);
  auto tl = Timeline(f);
  overlay::PaceClassCache cache;
  const double display = tl[3500].first;
  auto a = overlay::BuildTimeSeries(tl, display, overlay::kPaceSpanSec, 130, 5.0, cache, 1);
  auto b = overlay::BuildTimeSeries(tl, display + 0.02, overlay::kPaceSpanSec, 130, 5.3, cache, 1);
  const double colDur = overlay::kPaceSpanSec / 130;
  int compared = 0, changed = 0;
  for (const auto& pa : a)
    for (const auto& pb : b) {
      double ca = display - (1.0 - pa.x) * overlay::kPaceSpanSec;
      double cb = display + 0.02 - (1.0 - pb.x) * overlay::kPaceSpanSec;
      if (std::fabs(ca - cb) < colDur * 0.1 && ca < display - colDur) {
        ++compared;
        bool flatA = std::fabs(pa.ms - 5.0) < 1e-6, flatB = std::fabs(pb.ms - 5.3) < 1e-6;
        if (flatA != flatB || pa.spike != pb.spike) ++changed;
      }
    }
  CHECK(compared > 50);
  CHECK(changed == 0);
}

TEST(RecentSwingUsesLastTenSeconds) {
  auto f = MakeFrames(4.44, 0.05, 3000, 2.0, 8);
  CHECK(overlay::RecentSwing(Timeline(f)) >= 0);
  CHECK(overlay::RecentSwing(Timeline(f)) < 0.1);
}

// ---------------------------------------------------------------------------
// Datos censurados (Tobit)
// ---------------------------------------------------------------------------
// Tiempos de un juego capaz de mas que el limite, medidos con el limite puesto.
static std::vector<double> CappedFrames(double trueMedianMs, double sigma, double capMs, int n, unsigned seed,
                                        double stutterShare = 0.0) {
  std::mt19937 rng(seed);
  std::lognormal_distribution<double> ln(std::log(trueMedianMs), sigma);
  std::normal_distribution<double> jitter(0.0, 0.03);
  std::uniform_real_distribution<double> u(0.0, 1.0);
  std::vector<double> v;
  for (int i = 0; i < n; ++i) {
    double x = std::max(ln(rng), capMs) + jitter(rng);
    if (u(rng) < stutterShare) x = 30.0;
    v.push_back(x);
  }
  return v;
}

TEST(TobitRecoversCapacity) {
  // Juego capaz de 250 fps (4,0 ms) con un limite de 225 (4,44 ms): ~85 % de fotogramas en el limite.
  auto v = CappedFrames(4.0, 0.10, 1000.0 / 225.0, 20000, 11);
  CapacityEstimate e = EstimateCapacity(v, 1000.0 / 225.0);
  CHECK(e.valid);
  CHECK(e.censoredFraction > 0.75 && e.censoredFraction < 0.92);
  CHECK_NEAR(e.medianMs, 4.0, 4.0 * 0.02);
  CHECK_NEAR(e.p99Ms, 4.0 * std::exp(2.3263 * 0.10), 0.15);
}

TEST(TobitIgnoresStutters) {
  // Un 1 % de tirones de 30 ms no debe arrastrar la estimacion.
  auto v = CappedFrames(4.0, 0.10, 1000.0 / 225.0, 20000, 12, 0.01);
  CapacityEstimate e = EstimateCapacity(v, 1000.0 / 225.0);
  CHECK(e.valid);
  CHECK_NEAR(e.medianMs, 4.0, 4.0 * 0.03);
}

TEST(TobitNotNeededWithoutCensoring) {
  // El juego no llega al limite: no hay censura y se usan las estadisticas normales.
  auto v = CappedFrames(6.0, 0.10, 4.0, 5000, 13);
  CHECK(!EstimateCapacity(v, 4.0).valid);
}

TEST(TobitSecondGameAtSixty) {
  // Otro caso: capaz de ~75 fps (13,3 ms) con limite de 60 (16,7 ms) y mas variacion.
  auto v = CappedFrames(13.3, 0.15, 1000.0 / 60.0, 6000, 14);
  CapacityEstimate e = EstimateCapacity(v, 1000.0 / 60.0);
  CHECK(e.valid);
  CHECK_NEAR(e.medianMs, 13.3, 13.3 * 0.05);
}

// Como mide FlatPace con el limitador del driver: los tiempos quedan repartidos de forma simetrica
// alrededor del limite (variacion jitterMs), y solo los fotogramas que el juego tarda de verdad mas
// que el limite quedan por encima. Reproduce lo visto en partidas reales (0.37.1).
static std::vector<double> DriverLimited(double trueMedianMs, double sigma, double capMs, double jitterMs, int n,
                                         unsigned seed) {
  std::mt19937 rng(seed);
  std::lognormal_distribution<double> ln(std::log(trueMedianMs), sigma);
  std::normal_distribution<double> jitter(0.0, jitterMs);
  std::vector<double> v;
  for (int i = 0; i < n; ++i) {
    const double x = ln(rng);
    v.push_back(x > capMs ? x + jitter(rng) * 0.3 : capMs + jitter(rng));
  }
  return v;
}

TEST(TobitNotFooledByLimiterJitter) {
  // Caso Megabonk real: capaz de ~300 fps (3,35 ms) con un limite de 195 (5,13 ms) y el limitador
  // variando +-0,16 ms. Antes (umbral fijo del 3 %) estimaba ~205 fps; ahora: va sobrado.
  auto v = DriverLimited(3.35, 0.12, 1000.0 / 195.0, 0.16, 60000, 15);
  CapacityEstimate e = EstimateCapacity(v, 1000.0 / 195.0);
  CHECK(e.censoredFraction > 0.98);
  CHECK(!(e.valid && e.medianReliable && 1000.0 / e.medianMs < 250.0));
}

TEST(TobitMedianWhenGameIsTight) {
  // Juego justo en el limite: capaz de ~185 fps (5,40 ms) con limite de 195 (5,13 ms): censura
  // moderada; la mediana es fiable.
  auto v = DriverLimited(5.40, 0.08, 1000.0 / 195.0, 0.16, 30000, 16);
  CapacityEstimate e = EstimateCapacity(v, 1000.0 / 195.0);
  CHECK(e.valid);
  CHECK(e.medianReliable);
  CHECK_NEAR(e.medianMs, 5.40, 5.40 * 0.05);
}

TEST(TobitP99WithHeavyCensoring) {
  // Caso Greylock real: capaz de mas que el limite casi siempre, pero con escenas pesadas mas
  // lentas que el limite: la mediana no es fiable, el p99 si.
  auto v = DriverLimited(7.4, 0.25, 1000.0 / 90.0, 0.38, 20000, 17);
  CapacityEstimate e = EstimateCapacity(v, 1000.0 / 90.0);
  std::vector<double> sorted = v;
  std::sort(sorted.begin(), sorted.end());
  CHECK(e.valid);
  CHECK(!e.medianReliable);
  CHECK_NEAR(e.p99Ms, 7.4 * std::exp(2.3263 * 0.25), 7.4 * std::exp(2.3263 * 0.25) * 0.08);
}

// ---------------------------------------------------------------------------
// Deteccion de fases (BOCPD)
// ---------------------------------------------------------------------------
static std::vector<int> RunBocpd(const std::vector<double>& xs) {
  Bocpd b;
  std::vector<int> at;
  for (size_t i = 0; i < xs.size(); ++i)
    if (b.Push(xs[i])) at.push_back(static_cast<int>(i));
  return at;
}

TEST(BocpdDetectsPhaseChanges) {
  // Zona ligera (240 fps), zona pesada (185 fps) y zona media (215 fps), con ruido y algun pico.
  std::mt19937 rng(21);
  std::normal_distribution<double> noise(0.0, 0.015);
  std::uniform_real_distribution<double> u(0.0, 1.0);
  std::vector<double> xs;
  auto seg = [&](double ms, int secs) {
    for (int i = 0; i < secs; ++i) xs.push_back(std::log(ms) + noise(rng) + (u(rng) < 0.02 ? 0.08 : 0.0));
  };
  seg(4.17, 60);
  seg(5.40, 40);
  seg(4.65, 60);
  auto at = RunBocpd(xs);
  CHECK(at.size() == 2);
  if (at.size() == 2) {
    CHECK(at[0] >= 60 && at[0] <= 66);
    CHECK(at[1] >= 100 && at[1] <= 106);
  }
}

TEST(BocpdQuietWhenStable) {
  // Cinco minutos de ritmo estable con ruido: como mucho una falsa alarma.
  std::mt19937 rng(22);
  std::normal_distribution<double> noise(0.0, 0.02);
  std::vector<double> xs;
  for (int i = 0; i < 300; ++i) xs.push_back(std::log(4.44) + noise(rng));
  CHECK(RunBocpd(xs).size() <= 1);
}

TEST(BocpdIgnoresIsolatedSpikes) {
  // Ritmo estable con segundos atipicos sueltos (+10 %). El detector, con su filtro de mediana de
  // 3 s, ignora los picos de un segundo; el seguimiento de fases ignora ademas rafagas cortas (dos
  // segundos seguidos), porque exige 10 s y un 8 % de diferencia. Sin el filtro, un solo pico hacia
  // que el detector reiniciara la fase.
  std::mt19937 rng(23);
  std::normal_distribution<double> noise(0.0, 0.015);
  std::uniform_real_distribution<double> u(0.0, 1.0);
  std::vector<double> isolated, withBursts;
  bool prevSpike = false;
  for (int i = 0; i < 300; ++i) {
    const double base = std::log(4.65) + noise(rng);
    const bool spike = u(rng) < 0.04;
    withBursts.push_back(base + (spike ? 0.10 : 0.0));
    isolated.push_back(base + (spike && !prevSpike ? 0.10 : 0.0));
    prevSpike = spike;
  }
  CHECK(RunBocpd(isolated).empty());
  PhaseTracker pt;
  int confirmed = 0;
  for (size_t i = 0; i < withBursts.size(); ++i)
    confirmed += pt.Push(static_cast<double>(i), std::exp(withBursts[i])).confirmed;
  CHECK(confirmed == 0);
}

// ---------------------------------------------------------------------------
// Memoria por juego
// ---------------------------------------------------------------------------
TEST(MemoryConvergesAndIsCautious) {
  GameMemory g;
  g.Add(200, 8);
  CHECK(g.sessions == 1);
  CHECK_NEAR(g.MeanFps(), 200, 1e-6);
  CHECK(g.LowerFps() < 190 && g.LowerFps() > 180);  // prudente con una sola partida
  for (int i = 0; i < 4; ++i) g.Add(200, 8);
  CHECK(g.LowerFps() > 192);                         // mas seguro tras partidas que coinciden
}

TEST(MemoryResistsAtypicalSession) {
  GameMemory g;
  for (int i = 0; i < 4; ++i) g.Add(200, 8);
  g.Add(260, 3);                                     // una partida corta en una zona ligera
  CHECK(g.MeanFps() < 220);
  CHECK(g.LowerFps() < 205);
}

TEST(MemoryAdaptsToChanges) {
  GameMemory g;
  for (int i = 0; i < 5; ++i) g.Add(200, 8);
  for (int i = 0; i < 5; ++i) g.Add(160, 8);         // un parche o ajustes mas exigentes
  CHECK(g.MeanFps() < 170);
}

TEST(MemorySerializes) {
  GameMemory g;
  g.Add(188, 6);
  g.Add(176, 9);
  GameMemory h = GameMemory::Parse(g.Serialize());
  CHECK(h.sessions == 2);
  CHECK_NEAR(h.MeanFps(), g.MeanFps(), 1e-3);
  CHECK_NEAR(h.LowerFps(), g.LowerFps(), 1e-3);
  CHECK(GameMemory::Parse("basura").sessions == 0);
}

TEST(PhaseTrackerConfirmsRealChanges) {
  // Zona ligera, zona pesada (-23 %) y una variacion pequena (6,5 %) que no debe contar como fase.
  std::mt19937 rng(31);
  std::normal_distribution<double> noise(0.0, 0.015);
  PhaseTracker pt;
  int confirmed = 0;
  double t = 0;
  auto seg = [&](double ms, int secs) {
    for (int i = 0; i < secs; ++i, t += 1.0)
      if (pt.Push(t, ms * std::exp(noise(rng))).confirmed) ++confirmed;
  };
  seg(4.17, 60);
  seg(5.40, 50);
  seg(5.75, 50);  // +6,5 %: el detector lo propone, pero no llega al 8 %: misma fase
  CHECK(confirmed == 1);
  auto all = pt.All(t);
  CHECK(all.size() == 2);
  if (all.size() == 2) {
    CHECK_NEAR(all[0].medianMs, 4.17, 0.05);
    CHECK(all[1].start >= 60 && all[1].start <= 66);
  }
}

TEST(SuggestRaiseRules) {
  GameMemory none;
  // Con VRR a 240 Hz, limite de 188: siguiente escalon +13 % (212). Capacidad 220: se sugiere.
  CHECK_NEAR(NextLimitStep(188, 1, true, 240), 188 * 1.13, 1e-9);
  CHECK(SuggestRaise(220, false, none, 188, 1, true, 240) > 0);
  CHECK(SuggestRaise(200, false, none, 188, 1, true, 240) == 0);   // no llega al escalon
  // Ya en el techo con VRR (225 a 240 Hz): nada que sugerir, aunque vaya sobrado.
  CHECK(SuggestRaise(300, true, none, 225, 1, true, 240) == 0);
  // Sin VRR a 240 Hz, limite de 120 (240/2): el siguiente divisor es 240.
  CHECK_NEAR(NextLimitStep(120, 1, false, 240), 240, 1e-9);
  // La memoria del juego frena una sugerencia basada en una partida atipica.
  GameMemory mem;
  for (int i = 0; i < 4; ++i) mem.Add(190, 8);
  CHECK(SuggestRaise(220, false, mem, 188, 1, true, 240) == 0);
  // Con generacion x2 (122 en pantalla = 61 reales): el escalon se expresa en fotogramas reales.
  CHECK(NextLimitStep(61, 2, true, 240) <= VrrCeiling(240) / 2 + 1e-9);
}

// ---------------------------------------------------------------------------
// Ciclo de decision: verificacion, regla de fallos, bajada y decision al cerrar
// ---------------------------------------------------------------------------
TEST(VerifyDirectMatch) {
  VerifyInput v;
  v.expectedFps = 100;
  v.fpsNow = 100.4;        // +0,4 %
  v.fractionFast = 0.03;   // pocos fotogramas rapidos
  CHECK(LimitVerified(v));
  v.fpsNow = 98.0;         // -2 %: no es el ritmo esperado
  CHECK(!LimitVerified(v));
  v.fpsNow = 100.0;
  v.fractionFast = 0.15;   // demasiados fotogramas rapidos: el limite no actua
  CHECK(!LimitVerified(v));
}

TEST(VerifyWithFrameGeneration) {
  VerifyInput v;
  v.expectedFps = 61;
  v.expectFg = 2;
  v.fgNow = 2;
  v.fpsNow = 61.5;
  v.presentedFps = 122.5;
  CHECK(LimitVerified(v));
  v.presentedFps = 110;    // en pantalla no coincide
  CHECK(!LimitVerified(v));
}

TEST(VerifyByDetectedCap) {
  VerifyInput v;
  v.expectedFps = 225;
  v.fpsNow = 221;          // fuera del +-1,5 %
  v.capped = true;
  v.capFps = 224;          // pero el detector ve el limite
  CHECK(LimitVerified(v));
}

TEST(FailureRuleCountsOnlySustainedDrops) {
  FailureTracker f;
  // 10 min con el limite de 100: bajadas de 4 s (no cuentan) y bajadas del 10 % (no perceptibles)
  for (int s = 0; s < 600; ++s) {
    double rate = 100;
    if (s % 30 < 4) rate = 70;          // 4 s seguidos muy por debajo
    else if (s % 30 < 20) rate = 90;    // -10 %: no perceptible
    f.Add(rate, 100, 1.0);
  }
  CHECK(f.failSecs == 0);
  CHECK(!f.Failed());
}

TEST(FailureRuleNeedsSixtySecondsAndTenPercent) {
  FailureTracker a;
  for (int s = 0; s < 600; ++s) a.Add(s < 70 ? 80 : 100, 100, 1.0);  // 70 s seguidos a -20 %
  CHECK_NEAR(a.failSecs, 70, 1e-9);
  CHECK(a.Failed());                                                  // >= 60 s y 11,7 %
  FailureTracker b;
  for (int s = 0; s < 900; ++s) b.Add(s < 70 ? 80 : 100, 100, 1.0);  // los mismos 70 s, en 15 min
  CHECK(!b.Failed());                                                 // solo el 7,8 %
  FailureTracker c;
  for (int s = 0; s < 300; ++s) c.Add(s < 50 ? 80 : 100, 100, 1.0);  // 50 s: menos de un minuto
  CHECK(!c.Failed());
}

TEST(LowerAfterFailureVrrAndDivisors) {
  // Con VRR: el ritmo superado el 90 % de los segundos
  // Ritmos por segundo repartidos de 70 a 99,7: el percentil 10 es 73 (la mediana seria 85).
  std::vector<float> rates;
  for (int i = 0; i < 100; ++i) rates.push_back(static_cast<float>(70 + i * 0.3));
  Recommendation v = LowerAfterFailure(rates, true, 1, 100, 240);
  CHECK(v.valid && v.vrr);
  CHECK_NEAR(v.realFps, 73, 1e-9);
  // Sin VRR, a 240 Hz con 120 (240/2): el divisor siguiente, 80 (240/3)
  Recommendation d = LowerAfterFailure({}, false, 1, 120, 240);
  CHECK(d.valid);
  CHECK(d.divisor == 3);
  CHECK_NEAR(d.capFps, 80, 1e-9);
}

static SessionDecisionInput SessionFrom(const std::vector<double>& ms, bool vrr, bool fluid, double capReal = 0) {
  SessionDecisionInput in;
  for (double x : ms) in.realByFg[1].push_back(static_cast<float>(x));
  in.gameSecs = 300;
  in.vrrSecs = vrr ? 300 : 0;
  in.fluid = fluid;
  in.hz = 240;
  in.capReal = capReal;
  return in;
}

TEST(DecideFromSessionBasics) {
  std::mt19937 rng(61);
  std::lognormal_distribution<double> ln(std::log(6.0), 0.2);
  std::vector<double> ms;
  for (int i = 0; i < 30000; ++i) ms.push_back(ln(rng));
  Recommendation st = DecideFromSession(SessionFrom(ms, true, false));
  Recommendation fl = DecideFromSession(SessionFrom(ms, true, true));
  CHECK(st.valid && st.vrr && fl.valid && fl.vrr);
  CHECK(fl.capFps > st.capFps);
  // Pocos fotogramas: no se decide
  CHECK(!DecideFromSession(SessionFrom(std::vector<double>(500, 6.0), true, false)).valid);
  // Ritmo demostrado: el juego clavado a 144 por si mismo no se rebaja
  Recommendation h = DecideFromSession(SessionFrom(std::vector<double>(5000, 1000.0 / 144.0), true, false));
  CHECK_NEAR(h.capFps, 144, 1.0);
  // Con un limite activo nunca sube
  Recommendation c = DecideFromSession(SessionFrom(ms, true, true, 120));
  CHECK(c.capFps <= 120);
}

// Una partida con el limite puesto: el juego da lo que puede (capacidad) sin pasar del limite, y el
// limitador del driver deja su temblor simetrico alrededor del limite (+-4 % del periodo).
static std::vector<double> PlayCapped(double capacityMedianMs, double sigma, double limitFps, unsigned seed,
                                      int n = 20000) {
  std::mt19937 rng(seed);
  std::lognormal_distribution<double> ln(std::log(capacityMedianMs), sigma);
  std::normal_distribution<double> jitter(0.0, 0.04 * 1000.0 / limitFps);
  std::vector<double> ms;
  for (int i = 0; i < n; ++i) {
    const double cap = 1000.0 / limitFps;
    const double x = ln(rng);
    ms.push_back(x > cap ? x : cap + jitter(rng));
  }
  return ms;
}

TEST(NoDownwardSpiralAcrossSessions) {
  // Un juego capaz de mucho mas que su limite: decidir una y otra vez con partidas jugadas con el
  // limite puesto no debe hacerlo bajar (la espiral 231 -> 226 -> 222 de versiones antiguas).
  for (bool fluid : {false, true}) {
    double limit = 150;
    for (int session = 0; session < 10; ++session) {
      std::vector<double> ms = PlayCapped(4.5, 0.12, limit, 70 + session);
      Recommendation r = DecideFromSession(SessionFrom(ms, true, fluid, limit));
      CHECK(r.valid);
      if (r.valid) limit = r.realFps;
    }
    CHECK_NEAR(limit, 150, 1e-9);
  }
}

TEST(FailureCycleLowersOnceThenHolds) {
  // El juego tiene un limite aceptado de 120, pero tras un parche solo da ~95-100: la regla de fallos
  // lo baja una vez al ritmo superado el 90 % del tiempo, y con el nuevo limite ya no falla.
  std::mt19937 rng(81);
  std::normal_distribution<double> noise(0.0, 3.0);
  double limit = 120;
  int lowered = 0;
  for (int session = 0; session < 5; ++session) {
    FailureTracker f;
    for (int s = 0; s < 600; ++s) {
      const double capacity = 98 + noise(rng);
      f.Add(std::min(capacity, limit), limit, 1.0);
    }
    if (f.Failed()) {
      Recommendation lower = LowerAfterFailure(f.secRates, true, 1, limit, 240);
      CHECK(lower.valid);
      limit = lower.realFps;
      ++lowered;
    }
  }
  CHECK(lowered == 1);
  CHECK(limit >= 90 && limit <= 95);  // el ritmo superado el 90 % del tiempo (~94), no la mediana (~98)
}

TEST(ScreenModeAveragesGeneratedImages) {
  // Imagenes en pantalla con generacion a 112 fps (8,93 ms de media), alternando 6 y 12 ms con alguna
  // imagen de 14 ms (> 1,5 x): en pantalla la linea queda recta y sin tirones; un tiron real (40 ms,
  // > 2,5 x) si se marca. Con las reglas de los fotogramas reales, esas imagenes se marcaban.
  const double ref = 1000.0 / 112.0;
  std::vector<std::pair<double, double>> tl;
  double t = 0.0;
  for (int i = 0; i < 1000; ++i) {
    double ms = (i % 2 == 0) ? 6.0 : 11.86;
    if (i % 37 == 0) ms = 14.0;
    if (i == 700) ms = 40.0;
    t += ms / 1000.0;
    tl.push_back({t, ms});
  }
  const double display = t;
  overlay::PaceClassCache cs, cr;
  auto screen = overlay::BuildTimeSeries(tl, display, overlay::kPaceSpanSec, 133, ref, cs, 1, true);
  auto real = overlay::BuildTimeSeries(tl, display, overlay::kPaceSpanSec, 133, ref, cr, 1, false);
  int sSpikes = 0, sFlat = 0, rSpikes = 0;
  for (auto& p : screen) {
    sSpikes += p.spike;
    sFlat += !p.spike && std::fabs(p.ms - ref) < 1e-4;
  }
  for (auto& p : real) rSpikes += p.spike;
  CHECK(sSpikes == 1);                                 // solo el tiron real
  CHECK(sFlat >= static_cast<int>(screen.size()) - 8); // casi todo recto (el tiron y sus vecinos)
  CHECK(rSpikes >= 8);                                 // con las reglas de fotogramas reales, muchas marcas
}

TEST(LeftEdgeValuesStayPut) {
  // Un tramo con un tiron y otro fuera de la tolerancia deben dibujarse a la misma altura mientras se
  // desplazan hasta salir por la izquierda, aunque sus fotogramas mas antiguos salgan antes de la
  // ventana de datos (hasta la 0.43.1, el valor se recalculaba y en el borde izquierdo "parpadeaba").
  const double ref = 10.0;
  std::vector<std::pair<double, double>> tl;
  double t = 0.0;
  for (int i = 0; i < 2000; ++i) {
    double ms = 10.0;
    if (i >= 300 && i < 303) ms = (i == 301) ? 40.0 : 13.0;  // tramo con un tiron (40 ms) y lentos
    if (i >= 600 && i < 606) ms = 13.0;                        // tramo fuera de la tolerancia
    t += ms / 1000.0;
    tl.push_back({t, ms});
  }
  for (bool screen : {false, true}) {
    overlay::PaceClassCache cache;
    std::map<long long, float> firstValue;
    int changed = 0;
    const double colDur = overlay::kPaceSpanSec / 133;
    for (double display = 3.5; display < 9.5; display += 1.0 / 60.0) {
      auto pts = overlay::BuildTimeSeries(tl, display, overlay::kPaceSpanSec, 133, ref, cache, 1, screen);
      for (auto& p : pts) {
        const long long k = std::llround((display - (1.0 - p.x) * overlay::kPaceSpanSec) / colDur - 0.5);
        if ((k + 1 + (screen ? overlay::kScreenAvgBins / 2 : 0)) * colDur > display) continue;  // aun no completo
        auto it = firstValue.find(k);
        if (it == firstValue.end()) firstValue[k] = p.ms;
        else if (std::fabs(it->second - p.ms) > 1e-4) ++changed;
      }
    }
    CHECK(changed == 0);
  }
}

TEST(RightEdgeNeverRedraws) {
  // Lo que se dibuja en el borde derecho no debe cambiar despues: un tramo solo se dibuja completo
  // (y en pantalla, con sus vecinos), asi un pico nunca aparece para borrarse en el dibujo siguiente.
  const double ref = 10.0;
  std::vector<std::pair<double, double>> tl;
  double t = 0.0;
  for (int i = 0; i < 1500; ++i) {
    const double ms = (i % 7 == 0) ? 13.5 : 9.5;  // variacion que, con pocos fotogramas, parece un pico
    t += ms / 1000.0;
    tl.push_back({t, ms});
  }
  for (bool screen : {false, true}) {
    overlay::PaceClassCache cache;
    std::map<long long, std::pair<float, bool>> seen;
    int changed = 0;
    const double colDur = overlay::kPaceSpanSec / 133;
    for (double display = 3.5; display < 10.0; display += 1.0 / 60.0) {
      std::vector<std::pair<double, double>> arrived;
      for (auto& r : tl)
        if (r.first <= display + 0.05) arrived.push_back(r);  // datos hasta un poco despues del reloj
      auto pts = overlay::BuildTimeSeries(arrived, display, overlay::kPaceSpanSec, 133, ref, cache, 1, screen);
      for (auto& p : pts) {
        const long long k = std::llround((display - (1.0 - p.x) * overlay::kPaceSpanSec) / colDur - 0.5);
        auto it = seen.find(k);
        if (it == seen.end()) seen[k] = {p.ms, p.spike};
        else if (std::fabs(it->second.first - p.ms) > 1e-4 || it->second.second != p.spike) ++changed;
      }
    }
    CHECK(changed == 0);
  }
}

TEST(ClimbLadderToCeiling) {
  // Megabonk: empieza en 189 (Estabilidad) y va sobrado; con Fluidez sube cada partida hasta el
  // techo de la pantalla (225 a 240 Hz) y ahi se detiene.
  ClimbState st;
  double limit = 189;
  std::vector<double> path{limit};
  for (int session = 0; session < 6; ++session) {
    ClimbResult r = DecideClimb(st, false, true, limit, 1, true, 240.0, true, 1000 + session);
    st = r.state;
    if (r.action == ClimbAction::Raise) limit = r.newLimit;
    path.push_back(limit);
  }
  CHECK_NEAR(path[1], 213, 1e-9);  // 189 x 1,13
  CHECK_NEAR(limit, 225, 1e-9);    // techo de la pantalla
  // Con Estabilidad hacen falta dos partidas seguidas yendo sobrado.
  ClimbState s2;
  ClimbResult a = DecideClimb(s2, false, true, 189, 1, true, 240.0, false, 1000);
  CHECK(a.action == ClimbAction::None);
  ClimbResult b = DecideClimb(a.state, false, true, 189, 1, true, 240.0, false, 1001);
  CHECK(b.action == ClimbAction::Raise);
}

TEST(ClimbRevertsAndRemembersCeiling) {
  // Greylock: 100 va sobrado en la zona ligera; sube a 113; en la zona pesada falla: vuelve a 100
  // (no al ritmo superado el 90 % del tiempo) y no vuelve a intentarlo durante 30 dias.
  ClimbState st;
  ClimbResult r = DecideClimb(st, false, true, 100, 1, true, 240.0, true, 2000);
  CHECK(r.action == ClimbAction::Raise);
  CHECK_NEAR(r.newLimit, 113, 1e-9);
  r = DecideClimb(r.state, true, false, 113, 1, true, 240.0, true, 2001);
  CHECK(r.action == ClimbAction::RevertToPrev);
  CHECK_NEAR(r.newLimit, 100, 1e-9);
  r = DecideClimb(r.state, false, true, 100, 1, true, 240.0, true, 2002);
  CHECK(r.action == ClimbAction::BlockedByCeiling);
  // Pasados 30 dias, el techo caduca y se puede volver a intentar.
  r = DecideClimb(r.state, false, true, 100, 1, true, 240.0, true, 2040);
  CHECK(r.action == ClimbAction::Raise);
  // Un fallo sin subida previa no vuelve a nada (lo resuelve la regla de fallos), pero anota el techo.
  ClimbState s3;
  ClimbResult f = DecideClimb(s3, true, false, 150, 1, true, 240.0, true, 3000);
  CHECK(f.action == ClimbAction::None);
  CHECK_NEAR(f.state.failedCeil, 150, 1e-9);
  // El estado se guarda y se recupera igual.
  ClimbState back = ClimbState::Parse(r.state.Serialize());
  CHECK(back.prevGood == r.state.prevGood && back.failedDay == r.state.failedDay);
}

TEST(ClimbNoPingPongAcrossSessions) {
  // Un juego que sostiene 105 fps en sus zonas pesadas, jugado alternando zonas ligeras (va sobrado) y
  // pesadas: como mucho una partida fallida en 60 partidas (dos meses), sin bucle subir-bajar.
  ClimbState st;
  double limit = 100;
  int failures = 0, raises = 0;
  for (int session = 0; session < 60; ++session) {
    const bool heavy = session % 2 == 1;
    const bool failed = heavy && limit > 105 * 1.13;  // en la zona pesada no llega
    const bool plenty = !heavy && !failed;
    ClimbResult r = DecideClimb(st, failed, plenty, limit, 1, true, 240.0, true, 5000 + session);
    st = r.state;
    if (failed) ++failures;
    if (r.action == ClimbAction::Raise) { limit = r.newLimit; ++raises; }
    if (r.action == ClimbAction::RevertToPrev) limit = r.newLimit;
  }
  CHECK(failures <= 2);           // una por cada caducidad del techo (30 dias)
  CHECK(raises <= 4);
  CHECK(limit >= 100 && limit <= 105 * 1.13);
}

TEST(FailureRuleIgnoresClosingMinute) {
  // 10 min a 110 fps y, al final, 70 s colgado presentando a ~52 fps (Greylock): sin quitar el cierre,
  // seria un fallo. Desde la 0.65 se quitan los ultimos 15 s y el tramo final muy por debajo del
  // limite (menos del 70 %): el cuelgue entero.
  FailureTracker f;
  for (int s = 0; s < 600; ++s) f.Add(110, 110, 1.0);
  for (int s = 0; s < 70; ++s) f.Add(52, 110, 1.0);
  CHECK(f.Failed());
  const FailureTracker t = f.WithoutClosing(110);
  CHECK(!t.Failed());
  CHECK_NEAR(t.failSecs, 0, 1e-9);
  CHECK(t.trimmed == 70);
  // Un fallo de verdad, en mitad de la partida, se sigue detectando.
  FailureTracker g;
  for (int s = 0; s < 600; ++s) g.Add(s >= 200 && s < 300 ? 80 : 110, 110, 1.0);
  CHECK(g.WithoutClosing(110).Failed());
  // Salida normal: solo se pierden 15 s, no un minuto.
  FailureTracker h;
  for (int s = 0; s < 300; ++s) h.Add(110, 110, 1.0);
  CHECK(h.WithoutClosing(110).trimmed == 15);
  // Una zona pesada (algo por debajo del limite) hasta el cierre si cuenta: es juego, no el cierre.
  FailureTracker z;
  for (int s = 0; s < 400; ++s) z.Add(110, 110, 1.0);
  for (int s = 0; s < 120; ++s) z.Add(90, 110, 1.0);  // 82 % del limite
  const FailureTracker zt = z.WithoutClosing(110);
  CHECK(zt.trimmed == 15);
  CHECK(zt.Failed());  // 105 s por debajo, el 20 % de la partida
}

TEST(AutoPriorityMovesUnstableGamesToStability) {
  auto Session = [](int secs, int sunkSecs) {
    FailureTracker t;
    for (int s = 0; s < secs; ++s) t.Add(s < sunkSecs ? 90 : 130, 130, 1.0);
    return t;
  };
  // Greylock con Fluidez: el 1 % bajo hundido en dos partidas seguidas -> Estabilidad.
  AutoPriority a;
  a = UpdateAutoPriority(a, true, Session(600, 200));
  CHECK(!a.stability && a.unstableStreak == 1);
  a = UpdateAutoPriority(a, true, Session(600, 200));
  CHECK(a.stability);
  // Una partida larga y limpia entre medias corta la racha; una corta (< 3 min) no cuenta.
  AutoPriority b;
  b = UpdateAutoPriority(b, true, Session(600, 200));
  b = UpdateAutoPriority(b, true, Session(120, 0));
  CHECK(b.unstableStreak == 1);
  b = UpdateAutoPriority(b, true, Session(600, 0));
  CHECK(b.unstableStreak == 0 && !b.stability);
  // Con Estabilidad (global) no hay nada que decidir, y una vez elegida no vuelve atras sola.
  AutoPriority c;
  c = UpdateAutoPriority(c, false, Session(600, 200));
  CHECK(!c.stability && c.unstableStreak == 0);
  AutoPriority d;
  d.stability = true;
  d = UpdateAutoPriority(d, true, Session(600, 0));
  CHECK(d.stability);
  CHECK(AutoPriority::Parse(a.Serialize()).stability);
}

// La traduccion a NVIDIA debe dar exactamente las listas que construia el motor hasta la 0.52.
TEST(NvDesiredMatchesPreviousEngine) {
  using V = std::vector<std::pair<uint32_t, uint32_t>>;
  for (int mp = 0; mp <= 1; ++mp)
    for (int ll = 0; ll <= 1; ++ll) {
      auto old = [&](V d) {  // el codigo anterior: extras solo si hay algo, sin duplicar la cola
        if (mp && !d.empty()) d.push_back({nv::PREFERRED_PSTATE_ID, nv::PSTATE_PREFER_MAX});
        if (ll && !d.empty() &&
            std::none_of(d.begin(), d.end(), [](const auto& x) { return x.first == nv::PRERENDERLIMIT_ID; }))
          d.push_back({nv::PRERENDERLIMIT_ID, 1});
        return d;
      };
      LimitRequest r;
      r.maxPerf = mp;
      r.lowLatency = ll;
      // G-SYNC: limitador + VSync forzado
      r.frl = 114;
      r.vsyncOn = true;
      CHECK(NvDesired(r) == old({{nv::FRL_FPS_ID, 114}, {nv::VSYNCMODE_ID, nv::VSYNC_FORCEON}}));
      // Sin VRR, limite entero o con generacion: solo el limitador
      r.vsyncOn = false;
      CHECK(NvDesired(r) == old({{nv::FRL_FPS_ID, 114}}));
      // VSync por intervalos (1/2, 1/3, 1/4): sin limitador, con la cola de un fotograma
      for (int n = 1; n <= 4; ++n) {
        LimitRequest v = r;
        v.frl = 0;
        v.vsyncDivisor = n;
        CHECK(NvDesired(v) == old({{nv::VSYNCMODE_ID, nv::VsyncForDivisor(n)}, {nv::PRERENDERLIMIT_ID, 1}}));
      }
      // Sin nada que aplicar: no se toca nada, ni siquiera los extras
      LimitRequest e;
      e.maxPerf = mp;
      e.lowLatency = ll;
      CHECK(NvDesired(e).empty() && e.Empty());
    }
  CHECK(nv::DivisorForVsync(nv::VsyncForDivisor(3)) == 3);
}

// Cada traduccion del registro debe tener los mismos huecos que su original y en el mismo orden (un
// %s donde habia un %d, o uno de menos, romperia el mensaje o la app). "%.0s" equivale a "%s".
static std::vector<std::string> FormatSlots(const char* f) {
  std::vector<std::string> out;
  for (const char* p = f; *p; ++p) {
    if (*p != '%') continue;
    ++p;
    if (*p == '%') continue;
    while (*p && std::strchr("-+ #0123456789.", *p)) ++p;
    std::string len;
    while (*p && std::strchr("hlzjtL", *p)) len += *p++;
    if (*p) out.push_back(len + *p);
  }
  return out;
}
TEST(LogTranslationsKeepFormat) {
  CHECK(LogEnglishTable().size() > 150);
  for (const auto& [es, en] : LogEnglishTable()) {
    const bool same = FormatSlots(es) == FormatSlots(en);
    if (!same) std::printf("  traduccion con huecos distintos: %s\n", es);
    CHECK(same);
  }
  CHECK(std::string(LogEnglish("[%5.0f s] Pausa larga ignorada (carga o menu)\n")) == "[%5.0f s] Long pause ignored (loading or menu)\n");
  CHECK(std::string(LogEnglish("mensaje sin traducir %d")) == "mensaje sin traducir %d");
}


// ---- Memoria de limites (0.63) ----
static LimitMemory::Point MemPt(double low, double p90, double minutes, double limit = 0, bool lc = false, bool pc = false) {
  LimitMemory::Point p;
  p.low = static_cast<float>(low);
  p.p90 = static_cast<float>(p90);
  p.minutes = static_cast<float>(minutes);
  p.limit = static_cast<float>(limit);
  p.lowCens = lc;
  p.p90Cens = pc;
  return p;
}
TEST(LimitMemoryNeedsTwoSessions) {
  LimitMemory m;
  m.Add(MemPt(100, 120, 5));
  CHECK(m.Limit(false, 0.13) == 0.0);
  CHECK(m.Limit(true, 0.13) == 0.0);
  m.Add(MemPt(100, 120, 5));
  CHECK(m.Limit(false, 0.13) > 0.0);
  CHECK(m.Limit(true, 0.13) == 120.0);
}
TEST(LimitMemoryPrudentForSteadyMeanForMoreFps) {
  LimitMemory m;
  m.Add(MemPt(101, 125, 5.0));
  m.Add(MemPt(100, 122, 3.1));
  m.Add(MemPt(100, 123, 3.6));
  // Cota prudente por debajo de la media; Ritmo constante = 1,13 x cota.
  CHECK(m.low.Lower() < m.low.Mean());
  CHECK(m.Limit(false, 0.13) == std::floor(1.13 * m.low.Lower() + 1e-9));
  CHECK(std::fabs(m.Limit(false, 0.13) - 109.0) < 1.5);  // el caso de Greylock de la maqueta
  CHECK(m.Limit(true, 0.13) >= 122.0 && m.Limit(true, 0.13) <= 124.0);
}
TEST(LimitMemoryMoreSpreadIsMoreCautious) {
  LimitMemory calm, rough;
  for (double v : {100.0, 101.0, 100.0, 99.0}) calm.Add(MemPt(v, 120, 5));
  for (double v : {85.0, 115.0, 90.0, 110.0}) rough.Add(MemPt(v, 120, 5));
  CHECK(rough.low.Lower() < calm.low.Lower());
}
TEST(LimitMemoryCensoredOnlyRaises) {
  LimitMemory m;
  m.Add(MemPt(100, 120, 5));
  m.Add(MemPt(100, 120, 5));
  const double before = m.low.Mean();
  // El juego no bajo de un limite de 90: "al menos 90" no dice nada nuevo y no baja la estimacion.
  m.Add(MemPt(90, 90, 5, 90, true, true));
  CHECK(std::fabs(m.low.Mean() - before) < 1e-9);
  CHECK(m.sessions == 3);
  // "Al menos 114", por encima de la estimacion: si la sube.
  m.Add(MemPt(114, 114, 5, 114, true, true));
  CHECK(m.low.Mean() > before);
}
TEST(LimitMemoryRecentWeighsMore) {
  LimitMemory m;
  for (int i = 0; i < 5; ++i) m.Add(MemPt(80, 100, 10));
  for (int i = 0; i < 5; ++i) m.Add(MemPt(120, 140, 10));
  CHECK(m.low.Mean() > 105.0);  // va hacia las recientes
}
TEST(LimitMemoryShortSessionsIgnored) {
  LimitMemory m;
  m.Add(MemPt(100, 120, 0.5));
  CHECK(m.sessions == 0);
}
TEST(LimitMemoryKeepsLastEightPoints) {
  LimitMemory m;
  for (int i = 0; i < 12; ++i) m.Add(MemPt(100 + i, 120, 5));
  CHECK(m.recent.size() == 8);
  CHECK(std::fabs(m.recent.back().low - 111.0f) < 1e-3);
}
TEST(LimitMemoryRoundTrip) {
  LimitMemory m;
  m.Add(MemPt(101, 125, 5.0, 129));
  m.Add(MemPt(114, 114, 3.6, 114, true, true));
  const LimitMemory r = LimitMemory::Parse(m.Serialize());
  CHECK(r.sessions == m.sessions);
  CHECK(r.low.n == m.low.n && r.p90.n == m.p90.n);
  CHECK(std::fabs(r.low.mean - m.low.mean) < 1e-5);
  CHECK(r.recent.size() == 2 && r.recent[1].lowCens && r.recent[1].limit == 114.0f);
  CHECK(LimitMemory::Parse("").sessions == 0);
  CHECK(LimitMemory::Parse("basura").sessions == 0);
}
TEST(LimitMemoryChangeThreshold) {
  CHECK(!LimitMemory::WorthChanging(109, 114));  // un 4,4 %: dentro del margen de error
  CHECK(LimitMemory::WorthChanging(105, 114));
  CHECK(LimitMemory::WorthChanging(120, 114));
  CHECK(!LimitMemory::WorthChanging(0, 114));
}
TEST(SessionMemoryPointFromFrames) {
  // Sin limite: 8 ms, con un 2 % de fotogramas a 12 ms, durante unos 3 minutos.
  std::vector<float> real, work;
  for (int i = 0; i < 25000; ++i) {
    const float x = (i % 50 == 0) ? 12.0f : 8.0f;
    real.push_back(x);
    work.push_back(x);
  }
  LimitMemory::Point p = SessionMemoryPoint(real, work, 0, 48, 0);
  CHECK(p.minutes > 2.0f);
  CHECK(std::fabs(p.low - 1000.0f / 12.0f) < 2.0f);
  CHECK(!p.lowCens && !p.p90Cens);
  // Con un limite de 100 que el juego sostiene siempre (trabajo de 5 ms): censurado.
  std::vector<float> r2(20000, 10.0f), w2(20000, 5.0f);
  LimitMemory::Point q = SessionMemoryPoint(r2, w2, 100, 48, 0);
  CHECK(q.lowCens && q.p90Cens);
  CHECK(q.low == 100.0f && q.p90 == 100.0f);
  // Pocos datos: no cuenta.
  std::vector<float> few(500, 8.0f);
  CHECK(SessionMemoryPoint(few, few, 0, 48, 0).minutes == 0.0f);
}


// ---- Explicacion de cada limite (desplegable de Juegos, 0.63) ----
TEST(ExplainCurveCrossesNinetyAtP10) {
  std::vector<float> rates;
  for (int i = 0; i < 1000; ++i) rates.push_back(60.0f + 0.1f * i);  // 60..160
  LimitExplain e;
  e.SetCurve(rates);
  CHECK(e.curve.size() == static_cast<size_t>(LimitExplain::kCurvePoints));
  for (size_t i = 1; i < e.curve.size(); ++i) CHECK(e.curve[i] <= e.curve[i - 1]);  // nunca sube
  CHECK(e.curve.front() >= 99.0f);
  // Donde la curva cruza el 90 %: en torno al percentil 10 (70 fps).
  float cross = 0;
  for (size_t i = 0; i < e.curve.size(); ++i)
    if (e.curve[i] < 90.0f) { cross = e.curveLo + i * e.curveStep; break; }
  CHECK(cross > 66.0f && cross < 74.0f);
  LimitExplain few;
  few.SetCurve(std::vector<float>(10, 100.0f));
  CHECK(few.curve.empty());
}
TEST(ExplainPerSecondRatesSkipMenus) {
  std::vector<float> ms;
  for (int i = 0; i < 1200; ++i) ms.push_back(10.0f);   // 12 s a 100 fps
  for (int i = 0; i < 300; ++i) ms.push_back(33.4f);    // ~10 s a 30 fps (menu)
  const std::vector<float> r = PerSecondRates(ms, 48.0);
  CHECK(r.size() >= 11 && r.size() <= 12);
  for (float x : r) CHECK(x >= 48.0f);
}
TEST(ExplainRoundTrip) {
  LimitExplain e;
  e.kind = LimitExplain::FailTail;
  e.when = 1792957000;
  e.from = 129;
  e.to = 114;
  e.pct = 60;
  e.fluid = false;
  e.minutes = 5.2f;
  e.low1 = 87.0f;
  e.p90 = 108.6f;
  std::vector<float> rates;
  for (int i = 0; i < 300; ++i) rates.push_back(90.0f + (i % 40));
  e.SetCurve(rates);
  const LimitExplain r = LimitExplain::Parse(e.Serialize());
  CHECK(r.kind == LimitExplain::FailTail && r.when == e.when && r.to == 114.0f && !r.fluid);
  CHECK(std::fabs(r.low1 - 87.0f) < 0.06f && std::fabs(r.pct - 60.0f) < 0.06f);
  CHECK(r.curve.size() == e.curve.size());
  CHECK(std::fabs(r.curveStep - e.curveStep) < 0.001f);
  CHECK(LimitExplain::Parse("").kind == LimitExplain::None);
  CHECK(LimitExplain::Parse("x|y").kind == LimitExplain::None);
  CHECK(!r.curveSeconds);  // version 2 y regla del 1 %: la curva es de fotogramas
  LimitExplain w;
  w.kind = LimitExplain::WholeSession;
  w.to = 118;
  std::string v1 = w.Serialize();
  v1[0] = '1';  // un registro guardado con la 0.63
  const LimitExplain old = LimitExplain::Parse(v1);
  CHECK(old.kind == LimitExplain::WholeSession && old.to == 118.0f && old.curveSeconds);
  w.kind = LimitExplain::FailMedian;
  CHECK(LimitExplain::Parse(w.Serialize()).curveSeconds);
  CHECK(LimitExplain::Parse(w.Serialize()).version == 3);

}
TEST(ExplainSessionSeriesKeepDips) {
  std::vector<float> f(1000, 114.0f), l(1000, 110.0f);
  l[503] = 80.0f;  // una caida de un segundo
  SessionExplain s;
  s.SetSeries(f, l);
  CHECK(s.fps.size() <= SessionExplain::kMaxPoints);
  CHECK(s.secsPerPoint >= 4.0f);
  float minLow = 1000;
  for (float x : s.low) minLow = std::min(minLow, x);
  CHECK(minLow == 80.0f);  // la reduccion no la esconde
  s.when = 1792957000;
  s.limit = 114;
  s.verified = true;
  s.tailSecs = 1;
  s.tailPct = 0.6f;
  s.waitPct = 97;
  const SessionExplain r = SessionExplain::Parse(s.Serialize());
  CHECK(r.verified && !r.fg && r.limit == 114.0f && r.fps.size() == s.fps.size());
  CHECK(std::fabs(r.waitPct - 97.0f) < 0.06f && std::fabs(r.secsPerPoint - s.secsPerPoint) < 0.01f);
  CHECK(SessionExplain::Parse("").limit == 0.0f);
  // 0.66: guarda el 1 % mas lento de la partida; los registros de la 0.63-0.65 (version 1) se siguen leyendo.
  s.sessionLow = 96.2f;
  const SessionExplain r2 = SessionExplain::Parse(s.Serialize());
  CHECK(std::fabs(r2.sessionLow - 96.2f) < 0.06f && r2.fps.size() == s.fps.size());
  const std::string v1 = "1|1792957000|3.50|114.0|1|0|0|0.0|1|0.6|97.0|15|1|114:110,114:80";
  const SessionExplain old1 = SessionExplain::Parse(v1);
  CHECK(old1.limit == 114.0f && old1.sessionLow == 0.0f && old1.fps.size() == 2 && old1.low[1] == 80.0f);
}


// 0.65: la serie de la grafica, mas barata, debe ser identica a la de la 0.64, dibujo a dibujo.
TEST(PaceSeriesFastMatchesReference) {
  for (int screen = 0; screen <= 1; ++screen) {
    overlay::PaceClassCache ca, cb;
    std::vector<std::pair<double, double>> tl;
    double t = 0.0;
    unsigned seed = 12345;
    auto rnd = [&]() { seed = seed * 1103515245u + 12345u; return (seed >> 8) % 1000 / 1000.0; };
    bool same = true;
    size_t points = 0;
    for (int frame = 0; frame < 2000; ++frame) {
      // ~8,3 ms con ruido, tirones de vez en cuando y una zona mas lenta
      double ms = (frame > 900 && frame < 1300 ? 10.5 : 8.3) + (rnd() - 0.5) * 1.2;
      if (frame % 173 == 0) ms *= 3.0;
      t += ms / 1000.0;
      tl.push_back({t, ms});
      if (tl.size() > 1400) tl.erase(tl.begin());
      if (frame % 3 == 0) {  // un dibujo cada pocos fotogramas, con el reloj un poco por detras
        const double display = t - 0.05;
        const auto a = overlay::BuildTimeSeriesMap(tl, display, overlay::kPaceSpanSec, 190, 8.3, ca, 1, screen != 0);
        const auto b = overlay::BuildTimeSeries(tl, display, overlay::kPaceSpanSec, 190, 8.3, cb, 1, screen != 0);
        if (a.size() != b.size()) same = false;
        for (size_t i = 0; same && i < a.size(); ++i)
          if (a[i].x != b[i].x || a[i].ms != b[i].ms || a[i].spike != b[i].spike) same = false;
        points += b.size();
      }
    }
    CHECK(same);
    CHECK(points > 1000);
  }
}


// ---- El 1 % mas lento de la partida (0.66) ----
TEST(SteadyChecksWithSessionLow) {
  // Greylock con 107: el 1 % mas lento de la partida fue ~97 -> la formula da 109: se sostiene.
  SteadyCheck a = CheckSteady(96.9, 107);
  CHECK(a.holds);
  CHECK(a.target == 109.0);
  // Con 121: ~90 -> 102: no se sostiene, y baja a 102 (no un escalon a ciegas).
  SteadyCheck b = CheckSteady(90.0, 121);
  CHECK(!b.holds);
  CHECK(b.target == 101.0 || b.target == 102.0);
  // Diferencias pequenas (dentro del 5 %) no bajan el limite.
  CHECK(CheckSteady(92.0, 107).holds);   // 103 frente a 107: un 3,7 %
  CHECK(!CheckSteady(88.0, 107).holds);  // 99 frente a 107: un 7,5 %
  CHECK(CheckSteady(0.0, 107).holds);    // sin datos, no se toca
}
TEST(MoreFpsSankWithSessionLow) {
  CHECK(SessionSank(90.0, 121));    // 25 % por debajo
  CHECK(!SessionSank(110.0, 121));  // 9 % por debajo
  CHECK(!SessionSank(0.0, 121));
  AutoPriority p;
  p = UpdateAutoPriority(p, true, true, 300);
  CHECK(!p.stability && p.unstableStreak == 1);
  p = UpdateAutoPriority(p, true, false, 300);  // partida larga y limpia: corta la racha
  CHECK(p.unstableStreak == 0);
  p = UpdateAutoPriority(p, true, true, 300);
  p = UpdateAutoPriority(p, true, true, 300);
  CHECK(p.stability);
}
TEST(SessionGameLowIgnoresShortClosingAndStutters) {
  // 8 min a ~107 fps (9,35 ms) con un 0,5 % de fotogramas lentos del juego (11 ms) y tirones (60 ms)
  std::vector<float> r, w;
  for (int i = 0; i < 50000; ++i) {
    float x = 9.35f, wk = 6.0f;
    if (i % 200 == 0) { x = 11.0f; wk = 10.8f; }
    if (i % 997 == 0) { x = 60.0f; wk = 59.0f; }
    r.push_back(x);
    w.push_back(wk);
  }
  const double low = SessionGameLowFps(r, w, 1000.0 / 107, 15);
  CHECK(low > 100.0 && low <= 107.5);  // el 0,5 % lento no llega al 1 %; los tirones no cuentan
  // Un 3 % de fotogramas a 13 ms causados por el juego: el 1 % mas lento cae a ~77.
  std::vector<float> r2(r), w2(w);
  for (int i = 0; i < 50000; i += 33) { r2[i] = 13.0f; w2[i] = 12.8f; }
  const double low2 = SessionGameLowFps(r2, w2, 1000.0 / 107, 15);
  CHECK(low2 > 70.0 && low2 < 80.0);
  CHECK(SessionGameLowFps(std::vector<float>(500, 9.0f), std::vector<float>(500, 5.0f), 9.35, 15) == 0.0);
}

int main() {
  std::printf("Pruebas de FlatPace (src/core)\n");
  RUN(Percentiles);
  RUN(PaceSwing);
  RUN(Consistency);
  RUN(DetectCapAtDivisor);
  RUN(DetectCapNotWhenGpuBound);
  RUN(GameCausedLowIgnoresLimiterDelays);
  RUN(GameCausedLowCountsHeavyScenes);
  RUN(VrrCeilingValues);
  RUN(DivisorRecommendation);
  RUN(FluidRecommendation);
  RUN(VrrRecommendation);
  RUN(VrrRulesUsePerceptibleThreshold);
  RUN(VrrNeverRaisesAboveActiveLimit);
  RUN(VrrRangeCeiling);
  RUN(VrrBelowRangeWithoutLfc);
  RUN(EdidRangeAndName);
  RUN(EdidFreeSyncBlock);
  RUN(PaceToleranceAndScale);
  RUN(SeriesFlatWithinTolerance);
  RUN(SeriesMarksStutter);
  RUN(SeriesStableWhileScrolling);
  RUN(RecentSwingUsesLastTenSeconds);
  RUN(TobitRecoversCapacity);
  RUN(TobitIgnoresStutters);
  RUN(TobitNotNeededWithoutCensoring);
  RUN(TobitSecondGameAtSixty);
  RUN(TobitNotFooledByLimiterJitter);
  RUN(TobitMedianWhenGameIsTight);
  RUN(TobitP99WithHeavyCensoring);
  RUN(BocpdDetectsPhaseChanges);
  RUN(BocpdQuietWhenStable);
  RUN(BocpdIgnoresIsolatedSpikes);
  RUN(MemoryConvergesAndIsCautious);
  RUN(MemoryResistsAtypicalSession);
  RUN(MemoryAdaptsToChanges);
  RUN(MemorySerializes);
  RUN(PhaseTrackerConfirmsRealChanges);
  RUN(SuggestRaiseRules);
  RUN(ScreenModeAveragesGeneratedImages);
  RUN(LeftEdgeValuesStayPut);
  RUN(RightEdgeNeverRedraws);
  RUN(FailureRuleIgnoresClosingMinute);
  RUN(NvDesiredMatchesPreviousEngine);
  RUN(LogTranslationsKeepFormat);
  RUN(LimitMemoryNeedsTwoSessions);
  RUN(LimitMemoryPrudentForSteadyMeanForMoreFps);
  RUN(LimitMemoryMoreSpreadIsMoreCautious);
  RUN(LimitMemoryCensoredOnlyRaises);
  RUN(LimitMemoryRecentWeighsMore);
  RUN(LimitMemoryShortSessionsIgnored);
  RUN(LimitMemoryKeepsLastEightPoints);
  RUN(LimitMemoryRoundTrip);
  RUN(LimitMemoryChangeThreshold);
  RUN(SessionMemoryPointFromFrames);
  RUN(ExplainCurveCrossesNinetyAtP10);
  RUN(ExplainPerSecondRatesSkipMenus);
  RUN(ExplainRoundTrip);
  RUN(ExplainSessionSeriesKeepDips);
  RUN(PaceSeriesFastMatchesReference);
  RUN(SteadyChecksWithSessionLow);
  RUN(MoreFpsSankWithSessionLow);
  RUN(SessionGameLowIgnoresShortClosingAndStutters);
  RUN(AutoPriorityMovesUnstableGamesToStability);
  RUN(ClimbLadderToCeiling);
  RUN(ClimbRevertsAndRemembersCeiling);
  RUN(ClimbNoPingPongAcrossSessions);
  RUN(VerifyDirectMatch);
  RUN(VerifyWithFrameGeneration);
  RUN(VerifyByDetectedCap);
  RUN(FailureRuleCountsOnlySustainedDrops);
  RUN(FailureRuleNeedsSixtySecondsAndTenPercent);
  RUN(LowerAfterFailureVrrAndDivisors);
  RUN(DecideFromSessionBasics);
  RUN(NoDownwardSpiralAcrossSessions);
  RUN(FailureCycleLowersOnceThenHolds);
  std::printf("%d comprobaciones, %d fallos\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
