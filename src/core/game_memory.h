// FlatPace - memoria por juego: capacidad acumulada entre partidas
// Logica pura, sin dependencias de Windows: se incluye desde main.cpp y desde las pruebas
// automaticas (tests/). Requiere las cabeceras estandar ya incluidas.
#pragma once

// Cada partida aporta una estimacion del ritmo que el juego sostiene (en fps, criterio de
// Estabilidad). La memoria las combina en escala logaritmica con una media y varianza ponderadas
// (Welford), dando mas peso a las partidas largas (minutos jugados, hasta 10) y olvidando poco a
// poco las antiguas (cada partida nueva multiplica el peso acumulado por 0,8: un parche o un cambio
// de ajustes se refleja en pocas partidas). La incertidumbre combina la dispersion entre partidas
// con una incertidumbre a priori del 8 % que se reduce con cada partida, asi que con una sola
// partida el limite inferior es prudente y se estrecha a medida que las partidas coinciden.
struct GameMemory {
  int sessions = 0;
  double weight = 0.0, mean = 0.0, m2 = 0.0;  // sobre log(fps)

  static constexpr double kForget = 0.8;
  static constexpr double kPriorSd = 0.08;

  void Add(double fps, double minutes) {
    if (fps <= 0 || minutes <= 0) return;
    const double w = std::min(minutes, 10.0);
    const double x = std::log(fps);
    weight *= kForget;
    m2 *= kForget;
    const double total = weight + w;
    const double delta = x - mean;
    mean += (w / total) * delta;
    m2 += w * delta * (x - mean);
    weight = total;
    ++sessions;
  }

  // Desviacion (escala log) de la capacidad esperada en la proxima partida.
  double SdLog() const {
    if (sessions <= 0 || weight <= 0) return kPriorSd;
    const double spread = m2 / weight;
    return std::sqrt(spread + kPriorSd * kPriorSd / sessions);
  }
  double MeanFps() const { return sessions > 0 ? std::exp(mean) : 0.0; }
  // Valor que la capacidad supera con la probabilidad dada (z = 0,84: 80 %; 1,28: 90 %).
  double LowerFps(double z = 0.8416) const { return sessions > 0 ? std::exp(mean - z * SdLog()) : 0.0; }

  std::string Serialize() const {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%d;%.6f;%.6f;%.8f", sessions, weight, mean, m2);
    return buf;
  }
  static GameMemory Parse(const std::string& s) {
    GameMemory g;
    if (s.empty()) return g;
    int n = 0;
    double w = 0, m = 0, q = 0;
    if (std::sscanf(s.c_str(), "%d;%lf;%lf;%lf", &n, &w, &m, &q) == 4 && n > 0 && w > 0 && q >= 0) {
      g.sessions = n;
      g.weight = w;
      g.mean = m;
      g.m2 = q;
    }
    return g;
  }
};
