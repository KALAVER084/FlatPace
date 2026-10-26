// Por que cada juego tiene su limite (desplegable de Juegos, desde la 0.63).
//
// Dos registros por juego, que el motor guarda al cerrar cada partida y la ventana muestra:
//  - LimitExplain: como se decidio el limite actual (la regla, sus numeros y la curva de la partida que
//    lo decidio: el porcentaje de los segundos en que el juego alcanzo cada ritmo). No cambia hasta la
//    proxima decision.
//  - SessionExplain: la ultima partida con el limite (segundo a segundo, para la grafica de la
//    comprobacion, y el resultado de cada regla). Cambia en cada partida.
// Solo explican: ninguna decision los lee.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// Ritmo de cada segundo (fps reales), sin los segundos por debajo del rango del VRR (menus y
// cinematicas, que la decision de Mas FPS tampoco cuenta).
inline std::vector<float> PerSecondRates(const std::vector<float>& realMs, double minFps) {
  std::vector<float> rates;
  double acc = 0.0;
  int cnt = 0;
  for (float x : realMs) {
    acc += x;
    ++cnt;
    if (acc >= 1000.0) {
      const double r = cnt * 1000.0 / acc;
      if (r >= minFps) rates.push_back(static_cast<float>(r));
      acc = 0.0;
      cnt = 0;
    }
  }
  return rates;
}

struct LimitExplain {
  // Regla que decidio el limite.
  enum Kind {
    None = -1,
    WholeSession = 0,  // la partida completa (primera decision o tras Deshacer)
    FailMedian = 1,    // regla de fallos: el ritmo superado el 90 % de los segundos
    FailTail = 2,      // regla del 1 % mas lento: un escalon del 13 % hacia abajo
    Raise = 3,         // iba sobrado: un escalon del 13 % hacia arriba
    Revert = 4,        // la subida no se sostuvo: vuelta al limite anterior
    Manual = 5,        // puesto a mano
    Accepted = 6,      // recomendacion aplicada con su boton
  };
  static constexpr int kCurvePoints = 48;

  int kind = None;
  long long when = 0;  // segundos desde 1970
  float from = 0, to = 0;  // limite anterior y nuevo (fps reales)
  float pct = 0;           // reglas de fallo: % de la partida por debajo
  bool fluid = true;       // prioridad con la que se decidio (Mas FPS)
  float minutes = 0;       // minutos de juego de la partida que lo decidio
  float low1 = 0;          // 1 % mas lento de esa partida (fps reales)
  float p90 = 0;           // ritmo alcanzado el 90 % de los segundos
  float curveLo = 0, curveStep = 0;
  // % de los fotogramas (sin tirones) que alcanzan curveLo + i * curveStep fps o mas; con la regla de
  // fallos, % de los segundos (es lo que usa esa regla).
  std::vector<float> curve;
  // La curva cuenta segundos: la regla de fallos, o un registro de la 0.63 (version 1 del formato), que
  // contaba segundos para todas las reglas.
  bool curveSeconds = false;

  void SetCurve(std::vector<float> rates) {
    curve.clear();
    if (rates.size() < 30) return;
    std::sort(rates.begin(), rates.end());
    const double lo = std::floor(rates.front() * 0.95);
    const double hi = std::ceil(rates[static_cast<size_t>(0.995 * (rates.size() - 1))] * 1.02);
    curveLo = static_cast<float>(lo);
    curveStep = static_cast<float>(std::max(0.5, (hi - lo) / (kCurvePoints - 1)));
    for (int i = 0; i < kCurvePoints; ++i) {
      const float x = curveLo + i * curveStep;
      const size_t below = static_cast<size_t>(std::lower_bound(rates.begin(), rates.end(), x) - rates.begin());
      curve.push_back(100.0f * static_cast<float>(rates.size() - below) / static_cast<float>(rates.size()));
    }
  }

  std::string Serialize() const {
    char buf[200];
    std::snprintf(buf, sizeof(buf), "2|%d|%lld|%.1f|%.1f|%.1f|%d|%.2f|%.1f|%.1f|%.2f|%.3f|", kind, when, from, to, pct,
                  fluid ? 1 : 0, minutes, low1, p90, curveLo, curveStep);
    std::string s = buf;
    for (size_t i = 0; i < curve.size(); ++i) {
      std::snprintf(buf, sizeof(buf), "%s%.1f", i ? "," : "", curve[i]);
      s += buf;
    }
    return s;
  }
  static LimitExplain Parse(const std::string& s) {
    LimitExplain e;
    int ver = 0, fl = 1;
    if (s.empty() || std::sscanf(s.c_str(), "%d|", &ver) != 1 || (ver != 1 && ver != 2)) return e;
    if (std::sscanf(s.c_str() + s.find('|') + 1, "%d|%lld|%f|%f|%f|%d|%f|%f|%f|%f|%f|", &e.kind, &e.when, &e.from, &e.to, &e.pct, &fl,
                    &e.minutes, &e.low1, &e.p90, &e.curveLo, &e.curveStep) != 11)
      return LimitExplain();
    e.fluid = fl != 0;
    e.curveSeconds = ver == 1 || e.kind == FailMedian;
    size_t pos = 0;
    for (int i = 0; i < 12 && pos != std::string::npos; ++i) pos = s.find('|', pos + 1);
    if (pos == std::string::npos) return e;
    for (size_t p = pos + 1; p < s.size();) {
      size_t end = s.find(',', p);
      if (end == std::string::npos) end = s.size();
      e.curve.push_back(static_cast<float>(std::atof(s.substr(p, end - p).c_str())));
      p = end + 1;
    }
    return e;
  }
};

struct SessionExplain {
  static constexpr size_t kMaxPoints = 240;

  long long when = 0;
  float minutes = 0;
  float limit = 0;          // fps reales
  bool verified = false;    // comprobado en esta partida
  bool fg = false;          // con generacion de fotogramas: las reglas de ajuste no cuentan
  float failSecs = 0, failPct = 0;  // regla de fallos (sin el ultimo minuto)
  float tailSecs = 0, tailPct = 0;  // regla del 1 % mas lento (sin el ultimo minuto)
  float waitPct = -1;       // % de fotogramas esperando al limitador (-1: no medido)
  float trimSecs = 60;      // segundos finales que no cuentan
  float secsPerPoint = 1;   // segundos que representa cada punto
  std::vector<float> fps, low;  // ritmo y 1 % mas lento (ultimos 20 s); 0 = sin dato

  // Reduce series largas a kMaxPoints (media de cada tramo; el 1 % mas lento, su minimo, para no
  // esconder caidas).
  void SetSeries(const std::vector<float>& f, const std::vector<float>& l) {
    fps.clear();
    low.clear();
    const size_t n = std::min(f.size(), l.size());
    if (n == 0) return;
    const size_t k = (n + kMaxPoints - 1) / kMaxPoints;
    secsPerPoint = static_cast<float>(k);
    for (size_t i = 0; i < n; i += k) {
      double sf = 0;
      int cf = 0;
      float ml = 0;
      for (size_t j = i; j < std::min(n, i + k); ++j) {
        if (f[j] > 0) { sf += f[j]; ++cf; }
        if (l[j] > 0) ml = ml > 0 ? std::min(ml, l[j]) : l[j];
      }
      fps.push_back(cf ? static_cast<float>(sf / cf) : 0.0f);
      low.push_back(ml);
    }
  }

  std::string Serialize() const {
    char buf[220];
    std::snprintf(buf, sizeof(buf), "1|%lld|%.2f|%.1f|%d|%d|%.0f|%.1f|%.0f|%.1f|%.1f|%.0f|%.0f|", when, minutes, limit,
                  verified ? 1 : 0, fg ? 1 : 0, failSecs, failPct, tailSecs, tailPct, waitPct, trimSecs, secsPerPoint);
    std::string s = buf;
    for (size_t i = 0; i < fps.size(); ++i) {
      std::snprintf(buf, sizeof(buf), "%s%.0f:%.0f", i ? "," : "", fps[i], i < low.size() ? low[i] : 0.0f);
      s += buf;
    }
    return s;
  }
  static SessionExplain Parse(const std::string& s) {
    SessionExplain e;
    int ver = 0, v = 0, g = 0;
    if (s.empty() || std::sscanf(s.c_str(), "%d|", &ver) != 1 || ver != 1) return e;
    if (std::sscanf(s.c_str(), "1|%lld|%f|%f|%d|%d|%f|%f|%f|%f|%f|%f|%f|", &e.when, &e.minutes, &e.limit, &v, &g,
                    &e.failSecs, &e.failPct, &e.tailSecs, &e.tailPct, &e.waitPct, &e.trimSecs, &e.secsPerPoint) != 12)
      return SessionExplain();
    e.verified = v != 0;
    e.fg = g != 0;
    size_t pos = 0;
    for (int i = 0; i < 13 && pos != std::string::npos; ++i) pos = s.find('|', pos + 1);
    if (pos == std::string::npos) return e;
    for (size_t p = pos + 1; p < s.size();) {
      size_t end = s.find(',', p);
      if (end == std::string::npos) end = s.size();
      float a = 0, b = 0;
      if (std::sscanf(s.substr(p, end - p).c_str(), "%f:%f", &a, &b) == 2) {
        e.fps.push_back(a);
        e.low.push_back(b);
      }
      p = end + 1;
    }
    return e;
  }
};
