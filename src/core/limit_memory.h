// Memoria de limites por juego (desde la 0.63).
//
// Hasta la 0.62, cada limite salia de una sola partida. Pero el 1 % mas lento de un mismo juego varia
// de una partida a otra (en Greylock, un 10 %: depende de la zona que se juegue), asi que una sola
// partida da un limite con un error de +-13 % (80 % de confianza), el tamano de un cambio perceptible.
// Con 2 partidas baja a +-9 % y con 3 a +-7 %. Reproduciendo 22 decisiones con las grabaciones de
// Greylock, la memoria dio los mismos FPS que una sola partida, sin saltos de mas de un 13 % (8 con una
// sola partida), y su version prudente redujo a la mitad las partidas que no sostenian el limite.
//
// Cada partida aporta lo que usa cada formula:
//  - Ritmo constante: el 1 % mas lento causado por el juego. La memoria usa una cota prudente (la que
//    el juego sostiene con un 80 % de probabilidad), porque esa prioridad promete sostener el limite
//    incluso en los peores momentos: cuanto mas varian las partidas entre si, mas baja.
//  - Mas FPS: el ritmo que el juego alcanza el 90 % de los segundos. La memoria usa la media, porque
//    esa prioridad acepta caidas a cambio de mas FPS.
// Las partidas recientes y las largas pesan mas: cada partida anterior pierde un 20 % de peso, y cada
// una pesa segun sus minutos (hasta 10).
//
// Datos censurados: si el juego no bajo del limite, la partida solo dice "al menos el limite". Ese
// valor solo puede subir la estimacion, nunca bajarla: tomarlo como exacto haria bajar el limite
// partida a partida (la espiral que ya evitaba la memoria anterior).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

struct LimitMemory {
  static constexpr double kForget = 0.8;        // peso que conserva cada partida anterior
  static constexpr double kPriorSd = 0.08;      // variacion supuesta con pocas partidas (escala log)
  static constexpr double kZ = 0.8416;          // cota del 80 %
  static constexpr double kMaxWeightMin = 10.0;  // una partida pesa como mucho 10 minutos
  static constexpr double kMinMinutes = 1.0;     // partidas mas cortas no cuentan
  static constexpr size_t kMaxPoints = 8;        // partidas que se guardan para la grafica
  static constexpr int kMinSessions = 2;         // con menos, decide la ultima partida
  static constexpr double kMinChange = 0.05;     // solo se cambia un limite si la memoria difiere un 5 %

  // Media ponderada (escala log) con olvido.
  struct Stat {
    double w = 0.0, mean = 0.0, m2 = 0.0;
    int n = 0;
    void Add(double v, double weight) {
      const double x = std::log(v);
      w *= kForget;
      m2 *= kForget;
      const double total = w + weight;
      const double d = x - mean;
      mean += (weight / total) * d;
      m2 += weight * d * (x - mean);
      w = total;
      ++n;
    }
    double Mean() const { return n > 0 ? std::exp(mean) : 0.0; }
    double SdLog() const {
      if (n <= 0 || w <= 0) return kPriorSd;
      return std::sqrt(std::max(0.0, m2 / w) + kPriorSd * kPriorSd / n);
    }
    double Lower() const { return n > 0 ? std::exp(mean - kZ * SdLog()) : 0.0; }
  };

  // Una partida.
  struct Point {
    long long when = 0;   // segundos desde 1970 (para la grafica)
    float minutes = 0;    // minutos de juego
    float low = 0;        // 1 % mas lento causado por el juego (fps reales)
    float p90 = 0;        // ritmo alcanzado el 90 % de los segundos (fps reales)
    float limit = 0;      // limite que tenia (0 = ninguno)
    bool lowCens = false;  // el juego no bajo del limite: low es "al menos"
    bool p90Cens = false;
  };

  Stat low, p90;
  int sessions = 0;
  std::vector<Point> recent;

  void Reset() { *this = LimitMemory(); }

  void Add(const Point& p) {
    if (p.minutes < kMinMinutes) return;
    const double wt = std::min<double>(p.minutes, kMaxWeightMin);
    auto add = [&](Stat& s, double v, bool cens) {
      if (v <= 0) return;
      // Una cota inferior solo puede subir la estimacion.
      if (cens && s.n > 0 && std::log(v) <= s.mean) return;
      s.Add(v, wt);
    };
    add(low, p.low, p.lowCens);
    add(p90, p.p90, p.p90Cens);
    ++sessions;
    recent.push_back(p);
    if (recent.size() > kMaxPoints) recent.erase(recent.begin());
  }

  // Limite (fps reales) que dicta la memoria; 0 si aun no hay partidas suficientes.
  double Limit(bool fluid, double jnd) const {
    if (fluid) return p90.n >= kMinSessions ? std::floor(p90.Mean() + 1e-9) : 0.0;
    return low.n >= kMinSessions ? std::floor((1.0 + jnd) * low.Lower() + 1e-9) : 0.0;
  }

  static bool WorthChanging(double target, double current) {
    return target > 0 && current > 0 && std::fabs(target - current) / current >= kMinChange;
  }

  std::string Serialize() const {
    std::string s;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "1|%d|%.6f,%.6f,%.6f,%d|%.6f,%.6f,%.6f,%d|", sessions, low.w, low.mean, low.m2, low.n,
                  p90.w, p90.mean, p90.m2, p90.n);
    s = buf;
    for (size_t i = 0; i < recent.size(); ++i) {
      const Point& p = recent[i];
      std::snprintf(buf, sizeof(buf), "%s%lld,%.2f,%.2f,%.2f,%.2f,%d,%d", i ? ";" : "", p.when, p.minutes, p.low, p.p90,
                    p.limit, p.lowCens ? 1 : 0, p.p90Cens ? 1 : 0);
      s += buf;
    }
    return s;
  }

  static LimitMemory Parse(const std::string& s) {
    LimitMemory m;
    int ver = 0;
    if (s.empty() || std::sscanf(s.c_str(), "%d|", &ver) != 1 || ver != 1) return m;
    size_t a = s.find('|');
    size_t b = s.find('|', a + 1), c = s.find('|', b + 1), d = s.find('|', c + 1);
    if (a == std::string::npos || b == std::string::npos || c == std::string::npos || d == std::string::npos) return m;
    std::sscanf(s.c_str() + a + 1, "%d", &m.sessions);
    std::sscanf(s.c_str() + b + 1, "%lf,%lf,%lf,%d", &m.low.w, &m.low.mean, &m.low.m2, &m.low.n);
    std::sscanf(s.c_str() + c + 1, "%lf,%lf,%lf,%d", &m.p90.w, &m.p90.mean, &m.p90.m2, &m.p90.n);
    size_t pos = d + 1;
    while (pos < s.size()) {
      size_t end = s.find(';', pos);
      if (end == std::string::npos) end = s.size();
      Point p;
      int lc = 0, pc = 0;
      if (std::sscanf(s.substr(pos, end - pos).c_str(), "%lld,%f,%f,%f,%f,%d,%d", &p.when, &p.minutes, &p.low, &p.p90,
                      &p.limit, &lc, &pc) == 7) {
        p.lowCens = lc != 0;
        p.p90Cens = pc != 0;
        m.recent.push_back(p);
      }
      pos = end + 1;
    }
    return m;
  }
};

// Lo que aporta una partida a la memoria, a partir de sus fotogramas reales (sin generacion) y del tiempo
// de trabajo de cada uno (el mayor de CPU y GPU). capFps: el limite que tenia (0 si ninguno). Sin el
// ultimo minuto (el cierre del juego no cuenta) ni los segundos por debajo del rango del VRR (menus y
// cinematicas a 30-40 fps, donde un limite no ayuda). Devuelve minutes = 0 si no hay datos suficientes.
static LimitMemory::Point SessionMemoryPoint(const std::vector<float>& realMs, const std::vector<float>& workMs,
                                             double capFps, double vrrMinFps, long long when) {
  LimitMemory::Point p;
  p.when = when;
  p.limit = static_cast<float>(capFps);
  const size_t n = std::min(realMs.size(), workMs.size());
  if (n < 1000) return p;
  // Sin el ultimo minuto.
  size_t end = n;
  double tail = 0.0;
  while (end > 0 && tail < 60000.0) tail += realMs[--end];
  if (end < 1000) return p;
  std::vector<double> ms(realMs.begin(), realMs.begin() + end), work(workMs.begin(), workMs.begin() + end);
  double total = 0.0;
  for (double x : ms) total += x;
  p.minutes = static_cast<float>(total / 60000.0);
  const double capMs = capFps > 0 ? 1000.0 / capFps : 0.0;
  // 1 % mas lento causado por el juego.
  if (capMs > 0) {
    const double g99 = GameCausedPercentileMs(ms, work, capMs, 99);
    if (g99 <= 0 || g99 <= capMs * 1.015) {
      p.low = static_cast<float>(capFps);  // no bajo del limite: "al menos el limite"
      p.lowCens = true;
    } else {
      p.low = static_cast<float>(1000.0 / g99);
    }
  } else {
    std::vector<double> sorted = ms;
    std::sort(sorted.begin(), sorted.end());
    const double med = PercentileSorted(sorted, 50);
    std::vector<double> body;
    for (double x : sorted)
      if (x <= med * 2.5) body.push_back(x);  // sin tirones
    const double p99 = body.empty() ? 0.0 : PercentileSorted(body, 99);
    if (p99 > 0) p.low = static_cast<float>(1000.0 / p99);
  }
  // Ritmo alcanzado el 90 % de los segundos.
  std::vector<double> rates;
  double acc = 0.0;
  int cnt = 0;
  for (double x : ms) {
    acc += x;
    ++cnt;
    if (acc >= 1000.0) {
      const double r = cnt * 1000.0 / acc;
      if (r >= vrrMinFps) rates.push_back(r);
      acc = 0.0;
      cnt = 0;
    }
  }
  if (rates.size() >= 30) {
    std::sort(rates.begin(), rates.end());
    const double r10 = PercentileSorted(rates, 10);
    if (capFps > 0 && r10 >= capFps * 0.99) {
      p.p90 = static_cast<float>(capFps);
      p.p90Cens = true;
    } else {
      p.p90 = static_cast<float>(r10);
    }
  }
  if (p.low <= 0 || p.p90 <= 0) p.minutes = 0;
  return p;
}
