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
#include "../src/core/display_info.h"
#include "../src/core/edid.h"
#include "../src/core/frame_sample.h"
#include "../src/core/analysis.h"
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
  CHECK_NEAR(overlay::PaceTolerance(33.3), 2.0, 1e-9);   // maximo
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

int main() {
  std::printf("Pruebas de FlatPace (src/core)\n");
  RUN(Percentiles);
  RUN(PaceSwing);
  RUN(Consistency);
  RUN(DetectCapAtDivisor);
  RUN(DetectCapNotWhenGpuBound);
  RUN(VrrCeilingValues);
  RUN(DivisorRecommendation);
  RUN(FluidRecommendation);
  RUN(VrrRecommendation);
  RUN(VrrRangeCeiling);
  RUN(VrrBelowRangeWithoutLfc);
  RUN(EdidRangeAndName);
  RUN(EdidFreeSyncBlock);
  RUN(PaceToleranceAndScale);
  RUN(SeriesFlatWithinTolerance);
  RUN(SeriesMarksStutter);
  RUN(SeriesStableWhileScrolling);
  RUN(RecentSwingUsesLastTenSeconds);
  std::printf("%d comprobaciones, %d fallos\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
