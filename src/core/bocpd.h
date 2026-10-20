// FlatPace - deteccion de fases de juego (BOCPD)
// Logica pura, sin dependencias de Windows: se incluye desde main.cpp y desde las pruebas
// automaticas (tests/). Requiere las cabeceras estandar ya incluidas.
#pragma once

// Deteccion bayesiana de cambios en linea (Adams y MacKay, 2007). Recibe un dato por segundo
// (el logaritmo de la mediana de los tiempos de fotograma de ese segundo) y mantiene la
// probabilidad de cada "longitud de racha": cuantos segundos lleva la fase actual. Cada fase se
// modela como una normal de media y varianza desconocidas (prior normal-gamma), asi que un cambio
// de nivel claro respecto a la variacion habitual hace que la racha mas probable vuelva a cero.
//
// Uso: Reset() una vez con el primer dato; Push(x) cada segundo; Push devuelve true cuando
// detecta el comienzo de una fase nueva (como mucho una vez por fase).
struct Bocpd {
  double hazard = 1.0 / 60.0;  // probabilidad a priori de cambio por segundo (fases de ~1 min)
  size_t maxRun = 300;         // rachas mas largas se agrupan (5 min)
  double mu0 = 0.0, kappa0 = 0.02, alpha0 = 2.0, beta0 = 0.0;
  std::vector<double> prob, mu, kappa, alpha, beta;
  bool ready = false;
  int lastMap = 0;   // racha mas probable en el paso anterior
  int steps = 0;
  int sinceChange = 0;
  int shortSteps = 0;  // pasos seguidos con racha corta
  int longBefore = 0;  // racha mas probable justo antes de que se acortara

  // priorSd: variacion tipica de un segundo a otro dentro de una fase (escala logaritmica).
  // La creencia inicial sobre el nivel de una fase nueva es amplia (kappa0 pequeno: desviacion de
  // ~priorSd / sqrt(kappa0), unos +-20 % con los valores por defecto), asi una fase nueva lejos del
  // primer dato no se considera imposible. Con kappa0 = 1 el segundo cambio de una partida podia
  // pasar desapercibido.
  void Reset(double first, double priorSd = 0.03) {
    mu0 = first;
    kappa0 = 0.02;
    alpha0 = 2.0;
    beta0 = (alpha0 - 1.0) * priorSd * priorSd;  // varianza esperada = priorSd^2
    prob.assign(1, 1.0);
    mu.assign(1, mu0);
    kappa.assign(1, kappa0);
    alpha.assign(1, alpha0);
    beta.assign(1, beta0);
    ready = true;
    lastMap = 0;
    steps = 0;
    sinceChange = 0;
    shortSteps = 0;
    longBefore = 0;
    filled = 0;
  }

  // Densidad t de Student predictiva de x para la racha r.
  double Predictive(size_t r, double x) const {
    const double nu = 2.0 * alpha[r];
    const double scale2 = beta[r] * (kappa[r] + 1.0) / (alpha[r] * kappa[r]);
    const double z2 = (x - mu[r]) * (x - mu[r]) / scale2;
    const double logc = std::lgamma((nu + 1.0) / 2.0) - std::lgamma(nu / 2.0) - 0.5 * std::log(nu * 3.14159265358979 * scale2);
    return std::exp(logc - (nu + 1.0) / 2.0 * std::log1p(z2 / nu));
  }

  int MapRun() const {
    size_t best = 0;
    for (size_t r = 1; r < prob.size(); ++r)
      if (prob[r] > prob[best]) best = r;
    return static_cast<int>(best);
  }

  // Filtro de mediana de 3 s: un segundo atipico aislado (un tiron, una carga corta) no llega al
  // detector. Sin el, un solo pico hace que el modelo "reinicie" la fase, porque la fase antigua
  // pierde casi toda su probabilidad y no la recupera.
  double last1 = 0.0, last2 = 0.0;
  int filled = 0;

  bool Push(double raw) {
    if (!ready) {
      Reset(raw);
      last1 = raw;
      filled = 1;
      return false;
    }
    double x = raw;
    if (filled >= 2) {
      const double a = last2, b = last1, c = raw;
      x = std::max(std::min(a, b), std::min(std::max(a, b), c));  // mediana de tres
    }
    last2 = last1;
    last1 = raw;
    filled = std::min(filled + 1, 3);
    const size_t n = prob.size();
    std::vector<double> pred(n);
    for (size_t r = 0; r < n; ++r) pred[r] = Predictive(r, x);
    std::vector<double> next(std::min(n + 1, maxRun + 1), 0.0);
    double change = 0.0;
    for (size_t r = 0; r < n; ++r) {
      const double p = prob[r] * pred[r];
      change += p * hazard;
      const size_t to = std::min(r + 1, maxRun);
      next[to] += p * (1.0 - hazard);
    }
    next[0] = change;
    double total = 0.0;
    for (double p : next) total += p;
    if (!(total > 0) || !std::isfinite(total)) {  // dato imposible para todas las rachas: fase nueva
      Reset(x, std::sqrt(beta0 / (alpha0 - 1.0)));
      return true;
    }
    for (double& p : next) p /= total;
    // Parametros actualizados: la racha 0 empieza con el prior; la r+1 actualiza la r con x.
    std::vector<double> nmu(next.size()), nk(next.size()), na(next.size()), nb(next.size());
    nmu[0] = mu0; nk[0] = kappa0; na[0] = alpha0; nb[0] = beta0;
    for (size_t r = 0; r < n; ++r) {
      const size_t to = std::min(r + 1, maxRun);
      const double k1 = kappa[r] + 1.0;
      const double m1 = (kappa[r] * mu[r] + x) / k1;
      const double b1 = beta[r] + kappa[r] * (x - mu[r]) * (x - mu[r]) / (2.0 * k1);
      if (to == r + 1 || nk[to] == 0.0) {
        nmu[to] = m1; nk[to] = k1; na[to] = alpha[r] + 0.5; nb[to] = b1;
      }
    }
    prob.swap(next);
    mu.swap(nmu);
    kappa.swap(nk);
    alpha.swap(na);
    beta.swap(nb);
    ++steps;
    ++sinceChange;
    // Cambio: la racha mas probable vuelve a ser corta cuando antes era larga, y sigue corta en el
    // en el segundo siguiente.
    const int map = MapRun();
    bool changed = false;
    if (map <= 3) {
      if (shortSteps == 0) longBefore = lastMap;
      ++shortSteps;
    } else {
      shortSteps = 0;
    }
    if (shortSteps == 2 && longBefore >= 8 && sinceChange >= 8) {
      changed = true;
      sinceChange = 0;
    }
    lastMap = map;
    return changed;
  }
};

// Fases de juego a partir de un dato por segundo (la mediana de los tiempos de fotograma de ese
// segundo, en ms). BOCPD propone los cambios; una fase nueva solo se confirma si, tras confirmSec
// segundos, su ritmo difiere al menos minChange (8 %) del de la fase anterior. Asi las variaciones
// pequenas de la escena no cuentan como fases.
struct PhaseTracker {
  struct Phase {
    double start = 0.0, end = 0.0, medianMs = 0.0;
  };
  // Fase confirmada en este paso (si la hay)
  struct Event {
    bool confirmed = false;
    double at = 0.0, beforeMs = 0.0, afterMs = 0.0, beforeSec = 0.0;
  };

  double confirmSec = 10.0, minChange = 0.08, priorSd = 0.03;
  Bocpd bocpd;
  std::vector<Phase> phases;   // fases cerradas
  double nowStart = -1.0;      // fase en curso
  std::vector<double> nowSecs;
  double pendStart = -1.0;     // posible fase nueva
  std::vector<double> pendSecs;

  static double Median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
  }

  Event Push(double t, double medianMs) {
    Event ev;
    if (medianMs <= 0) return ev;
    if (nowStart < 0) {
      bocpd.Reset(std::log(medianMs), priorSd);
      nowStart = t;
      nowSecs.assign(1, medianMs);
      return ev;
    }
    if (bocpd.Push(std::log(medianMs))) {
      pendStart = t;
      pendSecs.clear();
    }
    if (pendStart < 0) {
      nowSecs.push_back(medianMs);
      return ev;
    }
    pendSecs.push_back(medianMs);
    if (t - pendStart < confirmSec) return ev;
    const double before = Median(nowSecs), after = Median(pendSecs);
    if (before > 0 && std::fabs(after - before) / before >= minChange) {
      phases.push_back({nowStart, pendStart, before});
      ev = {true, pendStart, before, after, pendStart - nowStart};
      nowStart = pendStart;
      nowSecs = pendSecs;
    } else {
      nowSecs.insert(nowSecs.end(), pendSecs.begin(), pendSecs.end());
    }
    pendStart = -1.0;
    pendSecs.clear();
    return ev;
  }

  // Todas las fases, incluida la que esta en curso.
  std::vector<Phase> All(double t) const {
    std::vector<Phase> all = phases;
    if (nowStart >= 0 && !nowSecs.empty()) all.push_back({nowStart, t, Median(nowSecs)});
    return all;
  }
};
