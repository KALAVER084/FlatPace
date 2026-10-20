// FlatPace - grafica de ritmo: serie por tramos y escala (dentro de namespace overlay)
// Logica pura, sin dependencias de Windows: se incluye desde main.cpp (en el mismo punto donde
// estaba) y desde las pruebas automaticas (tests/). Requiere las cabeceras estandar ya incluidas, stats_basic.h y pace_basics.h.
#pragma once

// Grafica de ritmo: los fotogramas se agrupan en tramos fijos en el tiempo (no en columnas de
// pantalla), asi cada tramo conserva su valor mientras la grafica se desplaza y no "tiembla".
// Clasificacion de un tramo ya completo: se decide una vez y se recuerda, para que un tramo no
// pase de "dentro" a "fuera" de la tolerancia (ni un tiron aparezca o desaparezca) mientras se
// desplaza, aunque la referencia sin limite se mueva poco a poco.
struct PaceClass {
  bool flat;   // dentro de la tolerancia: se dibuja sobre la referencia
  bool spike;  // tiron
};
struct PaceClassCache {
  std::map<long long, PaceClass> bins;
  double colDur = 0.0;
  unsigned generation = 0;
};

// Grafica de desviaciones: cada tramo vale la mediana de sus fotogramas (o su maximo si contiene
// un tiron); los que quedan dentro de la tolerancia se dibujan sobre la referencia, como una linea
// recta. Misma regla con y sin limite; las metricas siguen contando todos los fotogramas.
static std::vector<PacePoint> BuildTimeSeries(const std::vector<std::pair<double, double>>& timeline,
                                              double display, double span, int cols, double ref,
                                              PaceClassCache& cache, unsigned generation) {
  std::vector<PacePoint> out;
  if (cols < 2 || timeline.empty()) return out;
  const double colDur = span / cols;
  if (cache.colDur != colDur || cache.generation != generation) {
    cache.bins.clear();
    cache.colDur = colDur;
    cache.generation = generation;
  }
  std::map<long long, std::vector<float>> bins;
  for (const auto& r : timeline) {
    if (r.first > display || r.first < display - span - 2 * colDur) continue;
    bins[static_cast<long long>(std::floor(r.first / colDur))].push_back(static_cast<float>(r.second));
  }
  const double tol = PaceTolerance(ref);
  for (auto& [k, v] : bins) {
    const double center = (k + 0.5) * colDur;
    const float mx = *std::max_element(v.begin(), v.end());
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    const float med = v[v.size() / 2];
    PaceClass c{!(mx > ref * 1.5) && std::fabs(med - ref) <= tol, mx > ref * 1.5};
    const bool complete = (k + 1) * colDur <= display;
    if (complete) {
      auto it = cache.bins.find(k);
      if (it != cache.bins.end()) c = it->second;
      else cache.bins[k] = c;
    }
    const float value = c.spike ? mx : (c.flat ? static_cast<float>(ref) : med);
    out.push_back({static_cast<float>(1.0 - (display - center) / span), value, c.spike});
  }
  // Olvidar los tramos que ya salieron de la grafica
  const long long oldest = static_cast<long long>(std::floor((display - span) / colDur)) - 4;
  cache.bins.erase(cache.bins.begin(), cache.bins.lower_bound(oldest));
  return out;
}

// Variacion del ritmo de los ultimos ~10 s de fotogramas.
static double RecentSwing(const std::vector<std::pair<double, double>>& frames) {
  std::vector<double> ms;
  double sum = 0.0;
  for (size_t i = frames.size(); i-- > 0 && sum < 10000.0;) {
    ms.push_back(frames[i].second);
    sum += frames[i].second;
  }
  std::reverse(ms.begin(), ms.end());
  return PaceSwingMs(ms);
}

// Escala de la grafica: ~3,3 s de historia y zoom vertical alrededor de la referencia
// (+-50 % del tiempo de fotograma, minimo +-2 ms; algo mas de margen por arriba para los tirones),
// para que las diferencias reales de estabilidad se vean. Lo que se sale se dibuja contra el borde.
static constexpr double kPaceSpanSec = 10.0 / 3.0;
struct PaceRange {
  double lo, hi;
};
static PaceRange PaceScale(double ref) {
  const double r = std::max(2.0, 0.5 * ref);
  return {std::max(0.0, ref - r), ref + 1.6 * r};
}
