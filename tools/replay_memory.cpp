// Reproduccion de la memoria de limites (0.63) con grabaciones reales: pasa cada grabacion, en orden,
// por el mismo codigo del nucleo que usa la app (SessionMemoryPoint y LimitMemory), e imprime como
// evoluciona el limite. Uso: replay_memory <limite|0> <grabacion.csv> [<limite|0> <grabacion.csv> ...]
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <deque>
#include <fstream>
#include <limits>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>
typedef unsigned long DWORD;  // en Windows lo define windows.h
static std::atomic<int> g_vrrMinHz{0}, g_vrrMaxHz{0};

#include "../src/core/config.h"
#include "../src/core/stats_basic.h"
#include "../src/core/censored.h"
#include "../src/core/frame_sample.h"
#include "../src/core/analysis.h"
#include "../src/core/limit_memory.h"

static bool ReadRecording(const char* path, std::vector<float>& real, std::vector<float>& work) {
  std::ifstream f(path);
  if (!f) return false;
  std::string line;
  std::vector<std::string> cols;
  int iReal = -1, iCpu = -1, iGpu = -1, iGen = -1;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> v;
    std::stringstream ss(line);
    std::string c;
    while (std::getline(ss, c, ',')) v.push_back(c);
    if (iReal < 0) {
      for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == "real_ms") iReal = static_cast<int>(i);
        if (v[i] == "cpu_ms") iCpu = static_cast<int>(i);
        if (v[i] == "gpu_ms") iGpu = static_cast<int>(i);
        if (v[i] == "generado") iGen = static_cast<int>(i);
      }
      continue;
    }
    if (iReal >= static_cast<int>(v.size())) continue;
    if (iGen >= 0 && iGen < static_cast<int>(v.size()) && (v[iGen] == "1" || v[iGen] == "si")) continue;
    const double x = std::atof(v[iReal].c_str());
    if (!(x > 0 && x < 250)) continue;
    const double cpu = iCpu >= 0 ? std::atof(v[iCpu].c_str()) : 0.0, gpu = iGpu >= 0 ? std::atof(v[iGpu].c_str()) : 0.0;
    real.push_back(static_cast<float>(x));
    work.push_back(static_cast<float>(std::max(cpu, gpu)));
  }
  return true;
}

int main(int argc, char** argv) {
  (void)g_vrrMaxHz;
  LimitMemory mem;
  std::printf("%-44s %6s | %6s %5s | %6s %5s | %8s %8s\n", "partida", "limite", "1%bajo", "", "90%", "", "Ritmo c.", "Mas FPS");
  for (int i = 1; i + 1 < argc; i += 2) {
    const double cap = std::atof(argv[i]);
    std::vector<float> real, work;
    if (!ReadRecording(argv[i + 1], real, work)) { std::printf("no puedo leer %s\n", argv[i + 1]); continue; }
    const LimitMemory::Point p = SessionMemoryPoint(real, work, cap, cfg::kVrrMinFps, 0);
    std::string name = argv[i + 1];
    name = name.substr(name.find_last_of('-', name.size() - 12) + 1);
    if (p.minutes <= 0) { std::printf("%-44s %6.0f | (pocos datos: no cuenta)\n", name.c_str(), cap); continue; }
    mem.Add(p);
    const double steady = mem.Limit(false, cfg::kJnd), fluid = mem.Limit(true, cfg::kJnd);
    std::printf("%-44s %6.0f | %6.1f %5s | %6.1f %5s | %8s %8s\n", name.c_str(), cap, p.low, p.lowCens ? "(>=)" : "",
                p.p90, p.p90Cens ? "(>=)" : "", steady > 0 ? std::to_string(static_cast<int>(steady)).c_str() : "-",
                fluid > 0 ? std::to_string(static_cast<int>(fluid)).c_str() : "-");
  }
  std::printf("\nPartidas en la memoria: %d (1 %% mas lento en %d, ritmo del 90 %% en %d)\n", mem.sessions, mem.low.n, mem.p90.n);
  return 0;
}
