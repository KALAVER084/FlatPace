// FlatPace - grafica de ritmo: serie por tramos y escala (dentro de namespace overlay)
// Logica pura, sin dependencias de Windows: se incluye desde main.cpp (en el mismo punto donde
// estaba) y desde las pruebas automaticas (tests/). Requiere las cabeceras estandar ya incluidas, stats_basic.h y pace_basics.h.
#pragma once

// Grafica de ritmo: los fotogramas se agrupan en tramos fijos en el tiempo (no en columnas de
// pantalla), asi cada tramo conserva su valor mientras la grafica se desplaza y no "tiembla".
// Clasificacion de un tramo ya completo: se decide una vez y se recuerda, para que un tramo no
// pase de "dentro" a "fuera" de la tolerancia (ni un tiron aparezca o desaparezca) mientras se
// desplaza, aunque la referencia sin limite se mueva poco a poco.
// Tambien se guarda el valor (la altura): en el borde izquierdo, los fotogramas mas antiguos de un
// tramo salen de la ventana de datos uno a uno, y recalcularlo hacia que un tiron encogiera o un
// tramo cambiara de altura justo antes de desaparecer (parpadeo en el borde, hasta la 0.43.1).
struct PaceClass {
  bool flat;    // dentro de la tolerancia: se dibuja sobre la referencia
  bool spike;   // tiron
  float value;  // altura con la que se dibuja si no es recto
};
struct PaceClassCache {
  std::map<long long, PaceClass> bins;
  double colDur = 0.0;
  unsigned generation = 0;
};

// Grafica de desviaciones: cada tramo vale la mediana de sus fotogramas (o su maximo si contiene
// un tiron); los que quedan dentro de la tolerancia se dibujan sobre la referencia, como una linea
// recta. Misma regla con y sin limite; las metricas siguen contando todos los fotogramas.
// Modo en pantalla (generacion de fotogramas): cada tramo (~25 ms) tiene solo ~3 imagenes, y con
// generacion es normal que alguna imagen dure algo mas o algo menos que otra. Con las reglas de los
// fotogramas reales, un 11 % de los tramos se marcaban como tiron (una sola imagen > 1,5 x) y un 27 %
// se salian de la linea (simulacion del overlay con una partida real de Space Marine 2). En pantalla,
// un tramo vale la duracion media de las imagenes en ~125 ms (5 tramos), mas cercana a lo que percibe
// el ojo, y solo es tiron una imagen de mas de 2,5 x (como los TIRONES del registro): linea recta un
// 94 % del tiempo, sin esconder caidas reales (si la generacion se apaga, la media tambien cambia).
constexpr int kScreenAvgBins = 5;
static std::vector<PacePoint> BuildTimeSeries(const std::vector<std::pair<double, double>>& timeline,
                                              double display, double span, int cols, double ref,
                                              PaceClassCache& cache, unsigned generation, bool screen = false) {
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
  const double spikeAt = ref * (screen ? 2.5 : 1.5);
  const int half = screen ? kScreenAvgBins / 2 : 0;
  for (auto& [k, v] : bins) {
    const double center = (k + 0.5) * colDur;
    const float mx = *std::max_element(v.begin(), v.end());
    float med;
    if (screen) {  // media de las imagenes de los tramos vecinos
      double sum = 0.0;
      int n = 0;
      for (long long j = k - half; j <= k + half; ++j) {
        auto it = bins.find(j);
        if (it == bins.end()) continue;
        for (float x : it->second) {
          sum += x;
          ++n;
        }
      }
      med = static_cast<float>(sum / std::max(1, n));
    } else {
      std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
      med = v[v.size() / 2];
    }
    PaceClass c{!(mx > spikeAt) && std::fabs(med - ref) <= tol, mx > spikeAt, mx > spikeAt ? mx : med};
    // Solo se dibujan los tramos completos (en pantalla, con sus vecinos): un tramo a medias podia
    // aparecer como pico en el borde derecho y volver a la linea al completarse (hasta la 0.43.1).
    const bool complete = (k + 1 + half) * colDur <= display;
    if (!complete) continue;
    {
      auto it = cache.bins.find(k);
      if (it != cache.bins.end()) c = it->second;
      else cache.bins[k] = c;
    }
    const float value = c.flat ? static_cast<float>(ref) : c.value;
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
