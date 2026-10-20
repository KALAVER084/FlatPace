// FlatPace - datos censurados: capacidad del juego con un limite puesto (modelo Tobit)
// Logica pura, sin dependencias de Windows: se incluye desde main.cpp y desde las pruebas
// automaticas (tests/). Requiere las cabeceras estandar ya incluidas y stats_basic.h.
#pragma once

// Con un limite activo, los fotogramas que el juego podria hacer mas rapido esperan en el limite:
// de ellos solo se sabe que tardarian "como mucho" el periodo del limite (dato censurado por la
// izquierda). Si se tratan como medidas normales, la capacidad del juego parece igual al limite y
// nunca se sabe si podria dar mas. El modelo Tobit lo resuelve: supone que los tiempos de fotograma
// siguen una distribucion log-normal y la ajusta por maxima verosimilitud usando los fotogramas
// visibles (mas lentos que el limite) como medidas y los que estan en el limite como "<= limite".
// En simulacion, con el 85 % de los fotogramas en el limite, recupera la mediana real con un error
// menor del 1 %. El p99 no se ve afectado mientras los fotogramas lentos queden por encima del limite.

struct CapacityEstimate {
  bool valid = false;
  double censoredFraction = 0.0;  // fotogramas en el limite
  double medianMs = 0.0, p70Ms = 0.0, p99Ms = 0.0;
  double mu = 0.0, sigma = 0.0;   // parametros de la log-normal (sobre ms)
  size_t frames = 0;
};

// log Phi(z), estable tambien para z muy negativos.
static double LogNormalCdf(double z) {
  if (z < -30.0) return -0.5 * z * z - std::log(-z) - 0.91893853320467274;  // aproximacion asintotica
  return std::log(0.5 * std::erfc(-z / 1.4142135623730951));
}

// Minimizacion de Nelder-Mead en dos dimensiones (suficiente para dos parametros).
template <typename F>
static void NelderMead2(F f, double& x0, double& x1, double step0, double step1, int iters = 400) {
  double p[3][2] = {{x0, x1}, {x0 + step0, x1}, {x0, x1 + step1}};
  double v[3] = {f(p[0][0], p[0][1]), f(p[1][0], p[1][1]), f(p[2][0], p[2][1])};
  for (int it = 0; it < iters; ++it) {
    int lo = 0, hi = 0;
    for (int i = 1; i < 3; ++i) {
      if (v[i] < v[lo]) lo = i;
      if (v[i] > v[hi]) hi = i;
    }
    int mid = 3 - lo - hi;
    if (std::fabs(v[hi] - v[lo]) < 1e-10 * (1.0 + std::fabs(v[lo]))) break;
    double c0 = (p[lo][0] + p[mid][0]) / 2, c1 = (p[lo][1] + p[mid][1]) / 2;
    double r0 = c0 + (c0 - p[hi][0]), r1 = c1 + (c1 - p[hi][1]);
    double vr = f(r0, r1);
    if (vr < v[lo]) {
      double e0 = c0 + 2 * (c0 - p[hi][0]), e1 = c1 + 2 * (c1 - p[hi][1]);
      double ve = f(e0, e1);
      if (ve < vr) { p[hi][0] = e0; p[hi][1] = e1; v[hi] = ve; }
      else { p[hi][0] = r0; p[hi][1] = r1; v[hi] = vr; }
    } else if (vr < v[mid]) {
      p[hi][0] = r0; p[hi][1] = r1; v[hi] = vr;
    } else {
      double k0 = c0 + 0.5 * (p[hi][0] - c0), k1 = c1 + 0.5 * (p[hi][1] - c1);
      double vk = f(k0, k1);
      if (vk < v[hi]) { p[hi][0] = k0; p[hi][1] = k1; v[hi] = vk; }
      else {
        for (int i = 0; i < 3; ++i)
          if (i != lo) {
            p[i][0] = p[lo][0] + 0.5 * (p[i][0] - p[lo][0]);
            p[i][1] = p[lo][1] + 0.5 * (p[i][1] - p[lo][1]);
            v[i] = f(p[i][0], p[i][1]);
          }
      }
    }
  }
  int best = 0;
  for (int i = 1; i < 3; ++i) if (v[i] < v[best]) best = i;
  x0 = p[best][0];
  x1 = p[best][1];
}

// Estima la capacidad del juego a partir de tiempos de fotograma reales (ms) medidos con un limite
// de periodo capMs. Se ignoran pausas (> 250 ms) y tirones (> 2,5 x mediana), que no forman parte
// del ritmo normal. Un fotograma cuenta como "en el limite" si no supera el periodo en mas de tol
// (el limitador deja algo de variacion). Devuelve valid = false si no hay censura apreciable
// (entonces basta con las estadisticas normales) o si faltan datos para ajustar el modelo.
static CapacityEstimate EstimateCapacity(const std::vector<double>& framesMs, double capMs, double tol = 0.03) {
  CapacityEstimate e;
  if (capMs <= 0 || framesMs.size() < 200) return e;
  std::vector<double> v;
  v.reserve(framesMs.size());
  for (double x : framesMs)
    if (x > 0 && x <= 250.0) v.push_back(x);
  if (v.size() < 200) return e;
  std::vector<double> sorted = v;
  std::sort(sorted.begin(), sorted.end());
  const double median = PercentileSorted(sorted, 50);
  const double bound = capMs * (1.0 + tol);
  std::vector<double> obs;  // log de los fotogramas visibles
  size_t censored = 0, used = 0;
  for (double x : v) {
    if (x > median * 2.5) continue;  // tiron
    ++used;
    if (x <= bound) ++censored;
    else obs.push_back(std::log(x));
  }
  e.frames = used;
  e.censoredFraction = used ? static_cast<double>(censored) / used : 0.0;
  if (e.censoredFraction < 0.05 || obs.size() < 30) return e;
  const double c = std::log(bound);
  const double nc = static_cast<double>(censored);
  auto nll = [&](double m, double logS) {
    const double s = std::exp(logS);
    double ll = nc * LogNormalCdf((c - m) / s);
    for (double y : obs) {
      const double z = (y - m) / s;
      ll += -0.5 * z * z - logS;
    }
    return -ll;
  };
  double m = c - 0.05, logS = std::log(0.08);
  NelderMead2(nll, m, logS, 0.05, 0.3);
  const double s = std::exp(logS);
  if (!std::isfinite(m) || !std::isfinite(s) || s <= 0 || s > 2.0) return e;
  e.mu = m;
  e.sigma = s;
  e.medianMs = std::exp(m);
  e.p70Ms = std::exp(m + 0.5244005127 * s);
  e.p99Ms = std::exp(m + 2.3263478740 * s);
  e.valid = true;
  return e;
}
