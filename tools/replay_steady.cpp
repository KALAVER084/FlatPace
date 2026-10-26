// Reproduccion de la regla del 1 % mas lento de la partida (0.66) con grabaciones reales: pasa cada
// grabacion por el mismo codigo del nucleo que usa la app (SessionGameLowFps, CheckSteady y SessionSank)
// e imprime que decidiria. Uso: replay_steady <limite> <grabacion.csv> [<limite> <grabacion.csv> ...]
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
#include "../src/core/game_memory.h"
#include "../src/core/frame_sample.h"
#include "../src/core/analysis.h"
#include "../src/core/suggest.h"
#include "../src/core/decision.h"

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
  std::printf("%-18s %6s | %12s | %-34s | %s\n", "partida", "limite", "1%% mas lento", "Ritmo constante (1,13 x)", "Mas FPS");
  for (int i = 1; i + 1 < argc; i += 2) {
    const double cap = std::atof(argv[i]);
    std::vector<float> real, work;
    if (!ReadRecording(argv[i + 1], real, work)) continue;
    std::string name = argv[i + 1];
    name = name.substr(name.size() - 19, 15);
    const double low = SessionGameLowFps(real, work, 1000.0 / cap, kCloseTrimSec);
    const SteadyCheck sc = CheckSteady(low, cap);
    std::printf("%-18s %6.0f | %8.1f fps | %-34s | %s\n", name.c_str(), cap, low,
                (sc.holds ? "se sostiene (" + std::to_string((int)sc.target) + ")" : "baja a " + std::to_string((int)sc.target)).c_str(),
                SessionSank(low, cap) ? "hundida" : "bien");
  }
  return 0;
}
