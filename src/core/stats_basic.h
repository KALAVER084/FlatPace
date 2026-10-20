// FlatPace - percentiles y variacion del ritmo
// Logica pura, sin dependencias de Windows: se incluye desde main.cpp (en el mismo punto donde
// estaba) y desde las pruebas automaticas (tests/). Requiere las cabeceras estandar ya incluidas.
#pragma once

// Percentil sobre un vector YA ordenado (metodo de rango mas cercano).
static double PercentileSorted(const std::vector<double>& v, double p) {
  if (v.empty()) return 0.0;
  size_t idx = static_cast<size_t>(std::ceil(p / 100.0 * v.size()));
  idx = std::clamp<size_t>(idx, 1, v.size());
  return v[idx - 1];
}

// Variacion del ritmo percibido (ms): los fotogramas, en orden, se agrupan en ventanas de
// ~100 ms (unos tres o cuatro fotogramas a 30 fps, unas dos docenas a 240); la variacion es la
// desviacion media de las medianas de esas ventanas respecto a su mediana. Mide las oscilaciones
// lentas (los FPS que suben y bajan con la escena), que son las que se perciben; la variacion de
// un fotograma a otro, imperceptible, queda dentro de cada ventana.
static double PaceSwingMs(const std::vector<double>& ms) {
  std::vector<double> meds, win;
  double acc = 0.0;
  for (double v : ms) {
    if (v <= 0 || v > 250.0) continue;  // pausas y cargas no cuentan
    win.push_back(v);
    acc += v;
    if (acc >= 100.0) {
      std::sort(win.begin(), win.end());
      meds.push_back(win[win.size() / 2]);
      win.clear();
      acc = 0.0;
    }
  }
  if (meds.size() < 10) return -1.0;
  std::vector<double> sorted = meds;
  std::sort(sorted.begin(), sorted.end());
  const double m = sorted[sorted.size() / 2];
  double sum = 0.0;
  for (double v : meds) sum += std::fabs(v - m);
  return sum / meds.size();
}
