// FlatPace
// --------
// Mide cada juego con PresentMon (sin inyectarse en el juego), decide el limite de FPS que
// lo hace ir lo mas estable posible (divisores de los Hz o G-SYNC/FreeSync), lo aplica en el
// perfil del driver de NVIDIA, comprueba que funciona y lo mantiene. Todo es reversible.
//
// Uso:
//   FlatPace.exe                          -> ventana normal
//   FlatPace.exe --deshacer               -> quita todos los limites aplicados (sin ventana)
//   FlatPace.exe --presentmon ruta.exe    -> usa otra copia de PresentMon

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <evntrace.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <d3d11_1.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <timeapi.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "version.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#endif

using Microsoft::WRL::ComPtr;

#include "src/core/config.h"

// ---------------------------------------------------------------------------
// Utilidades
// ---------------------------------------------------------------------------
static double QpcFreq() {
  static const double f = [] {
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    return static_cast<double>(li.QuadPart);
  }();
  return f;
}

static double NowSec() {
  LARGE_INTEGER li;
  QueryPerformanceCounter(&li);
  return static_cast<double>(li.QuadPart) / QpcFreq();
}

static std::string Narrow(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                              nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(),
                      n, nullptr, nullptr);
  return s;
}

static std::string Lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

static std::string Trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\"");
  if (a == std::string::npos) return {};
  size_t b = s.find_last_not_of(" \t\"");
  return s.substr(a, b - a + 1);
}

#include "src/core/stats_basic.h"
#include "src/core/censored.h"
#include "src/core/bocpd.h"
#include "src/core/game_memory.h"

static bool IsElevated() {
  HANDLE token = nullptr;
  bool elevated = false;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    TOKEN_ELEVATION e{};
    DWORD size = 0;
    if (GetTokenInformation(token, TokenElevation, &e, sizeof(e), &size))
      elevated = e.TokenIsElevated != 0;
    CloseHandle(token);
  }
  return elevated;
}

static std::atomic<bool> g_quit{false};

// Idioma de la interfaz: 0 = espanol, 1 = ingles. El registro tecnico queda en espanol.
static std::atomic<int> g_lang{0};
static const char* T(const char* es, const char* en) { return g_lang == 1 ? en : es; }

static constexpr const char* kVersion = "0.38";

// Carpeta donde esta este .exe.
static std::wstring ExeDirectory() {
  wchar_t buf[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  std::wstring path(buf, n);
  size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

// Busca PresentMon*.exe junto a este programa (el paquete lo incluye).
static std::wstring FindPresentMon() {
  std::wstring dir = ExeDirectory();
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW((dir + L"\\PresentMon*.exe").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return {};
  std::wstring found = dir + L"\\" + fd.cFileName;
  FindClose(h);
  return found;
}

// ---------------------------------------------------------------------------
// Hardware: monitores (DisplayConfig) y GPUs (DXGI)
// ---------------------------------------------------------------------------
#include "src/core/display_info.h"

// Lee el EDID del monitor desde el registro a partir de su ruta de dispositivo
// ("\\?\DISPLAY#BOE0B7D#5&...#{guid}" -> "SYSTEM\CurrentControlSet\Enum\DISPLAY\BOE0B7D\5&...").
static std::vector<uint8_t> ReadEdid(const std::wstring& devicePath) {
  std::vector<uint8_t> edid;
  std::wstring p = devicePath;
  if (p.rfind(L"\\\\?\\", 0) == 0) p = p.substr(4);
  size_t g = p.find(L"#{");
  if (g != std::wstring::npos) p = p.substr(0, g);
  for (auto& c : p) if (c == L'#') c = L'\\';
  std::wstring key = L"SYSTEM\\CurrentControlSet\\Enum\\" + p + L"\\Device Parameters";
  DWORD size = 0;
  if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"EDID", RRF_RT_REG_BINARY, nullptr, nullptr, &size) !=
          ERROR_SUCCESS || size < 128)
    return edid;
  edid.resize(size);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"EDID", RRF_RT_REG_BINARY, nullptr, edid.data(), &size) !=
      ERROR_SUCCESS)
    edid.clear();
  return edid;
}

#include "src/core/edid.h"

static std::vector<DisplayPathInfo> QueryDisplayPaths() {
  std::vector<DisplayPathInfo> result;
  UINT32 numPaths = 0, numModes = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &numPaths, &numModes) != ERROR_SUCCESS)
    return result;
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(numPaths);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(numModes);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &numPaths, paths.data(), &numModes,
                         modes.data(), nullptr) != ERROR_SUCCESS)
    return result;
  paths.resize(numPaths);

  for (const auto& p : paths) {
    DisplayPathInfo info;

    DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {};
    src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    src.header.size = sizeof(src);
    src.header.adapterId = p.sourceInfo.adapterId;
    src.header.id = p.sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&src.header) == ERROR_SUCCESS)
      info.gdiName = src.viewGdiDeviceName;

    DISPLAYCONFIG_TARGET_DEVICE_NAME tgt = {};
    tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    tgt.header.size = sizeof(tgt);
    tgt.header.adapterId = p.targetInfo.adapterId;
    tgt.header.id = p.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&tgt.header) == ERROR_SUCCESS) {
      info.friendlyName = tgt.monitorFriendlyDeviceName;
      ParseEdid(ReadEdid(tgt.monitorDevicePath), info);
    }

    if (p.targetInfo.refreshRate.Denominator != 0)
      info.nominalHz = static_cast<double>(p.targetInfo.refreshRate.Numerator) /
                       p.targetInfo.refreshRate.Denominator;
    UINT32 idx = p.targetInfo.modeInfoIdx;
    if (idx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID && idx < modes.size() &&
        modes[idx].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET) {
      const auto& vs = modes[idx].targetMode.targetVideoSignalInfo.vSyncFreq;
      if (vs.Denominator != 0) info.signalHz = static_cast<double>(vs.Numerator) / vs.Denominator;
    }
    result.push_back(info);
  }
  return result;
}

struct AdapterInfo {
  std::wstring name;
  size_t vramMB = 0;
  bool integrated = false;  // heuristica: poca VRAM dedicada
  int outputs = 0;
};

struct OutputInfo {
  ComPtr<IDXGIOutput> output;
  std::wstring gdiName;
  size_t adapterIndex = 0;
  bool primary = false;
  bool hdrOn = false;
  float maxNits = 0.0f;
  long width = 0, height = 0;
};

static void EnumerateDxgi(std::vector<AdapterInfo>& adapters, std::vector<OutputInfo>& outputs) {
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return;

  ComPtr<IDXGIAdapter1> adapter;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 d{};
    adapter->GetDesc1(&d);
    if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

    AdapterInfo a;
    a.name = d.Description;
    a.vramMB = static_cast<size_t>(d.DedicatedVideoMemory / (1024 * 1024));
    a.integrated = a.vramMB < 512;
    size_t adapterIndex = adapters.size();

    ComPtr<IDXGIOutput> out;
    for (UINT j = 0; adapter->EnumOutputs(j, &out) != DXGI_ERROR_NOT_FOUND; ++j) {
      DXGI_OUTPUT_DESC od{};
      out->GetDesc(&od);
      OutputInfo o;
      o.output = out;
      o.gdiName = od.DeviceName;
      o.adapterIndex = adapterIndex;
      o.primary = od.DesktopCoordinates.left == 0 && od.DesktopCoordinates.top == 0;
      o.width = od.DesktopCoordinates.right - od.DesktopCoordinates.left;
      o.height = od.DesktopCoordinates.bottom - od.DesktopCoordinates.top;

      ComPtr<IDXGIOutput6> o6;
      if (SUCCEEDED(out.As(&o6))) {
        DXGI_OUTPUT_DESC1 d1{};
        if (SUCCEEDED(o6->GetDesc1(&d1))) {
          o.hdrOn = d1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
          o.maxNits = d1.MaxLuminance;
        }
      }
      outputs.push_back(o);
      a.outputs++;
    }
    adapters.push_back(a);
  }
}

// ---------------------------------------------------------------------------
// Frecuencia real: marcas de tiempo de vblank + regresion lineal
// ---------------------------------------------------------------------------
struct RefreshResult {
  bool ok = false;
  double hz = 0.0;
  int missed = 0;
  size_t vblanks = 0;
  double jitterUs = 0.0;
};

static RefreshResult MeasureRefresh(IDXGIOutput* out, double seconds) {
  RefreshResult r;
  HANDLE thread = GetCurrentThread();
  int oldPriority = GetThreadPriority(thread);
  SetThreadPriority(thread, THREAD_PRIORITY_TIME_CRITICAL);

  std::vector<double> t;
  t.reserve(8192);
  if (SUCCEEDED(out->WaitForVBlank())) {
    double end = NowSec() + seconds;
    while (NowSec() < end) {
      if (FAILED(out->WaitForVBlank())) break;
      t.push_back(NowSec());
    }
  }
  SetThreadPriority(thread, oldPriority);
  if (t.size() < 50) return r;

  // Intervalo tipico (mediana) para detectar vblanks perdidos.
  std::vector<double> d;
  for (size_t i = 1; i < t.size(); ++i) d.push_back(t[i] - t[i - 1]);
  std::vector<double> ds = d;
  std::sort(ds.begin(), ds.end());
  double med = ds[ds.size() / 2];
  if (med <= 0.0) return r;

  // Indice acumulado de ciclos: un hueco doble cuenta como 2 ciclos.
  std::vector<double> x(t.size(), 0.0);
  double k = 0.0;
  for (size_t i = 1; i < t.size(); ++i) {
    long long c = std::llround(d[i - 1] / med);
    if (c < 1) c = 1;
    if (c > 1) r.missed += static_cast<int>(c - 1);
    k += static_cast<double>(c);
    x[i] = k;
  }

  // Regresion lineal t = a + b*x  ->  periodo = b
  double n = static_cast<double>(t.size()), sx = 0, sy = 0;
  for (size_t i = 0; i < t.size(); ++i) { sx += x[i]; sy += t[i]; }
  double mx = sx / n, my = sy / n, sxy = 0, sxx = 0;
  for (size_t i = 0; i < t.size(); ++i) {
    sxy += (x[i] - mx) * (t[i] - my);
    sxx += (x[i] - mx) * (x[i] - mx);
  }
  if (sxx <= 0.0) return r;
  double b = sxy / sxx, a = my - b * mx;
  double res = 0.0;
  for (size_t i = 0; i < t.size(); ++i) {
    double e = t[i] - (a + b * x[i]);
    res += e * e;
  }
  r.ok = b > 0.0;
  r.hz = 1.0 / b;
  r.vblanks = t.size();
  r.jitterUs = std::sqrt(res / n) * 1e6;
  return r;
}


static bool HasSystemBattery() {
  SYSTEM_POWER_STATUS ps{};
  if (!GetSystemPowerStatus(&ps)) return false;
  return ps.BatteryFlag != 128 && ps.BatteryFlag != 255;
}

// ---------------------------------------------------------------------------
// Lectura de PresentMon (proceso hijo, salida CSV por stdout)
// ---------------------------------------------------------------------------
#include "src/core/frame_sample.h"

namespace overlay { static void Push(const FrameSample& s); }

// Cierra una sesion de captura de Windows (ETW) por su nombre. Si PresentMon termina de
// golpe, su sesion puede quedar abierta e interferir con la siguiente.
static void StopEtwSession(const wchar_t* name) {
  const size_t size = sizeof(EVENT_TRACE_PROPERTIES) + 2 * 1024 * sizeof(wchar_t);
  std::vector<unsigned char> buf(size, 0);
  auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buf.data());
  props->Wnode.BufferSize = static_cast<ULONG>(size);
  props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
  props->LogFileNameOffset = sizeof(EVENT_TRACE_PROPERTIES) + 1024 * sizeof(wchar_t);
  ControlTraceW(0, name, props, EVENT_TRACE_CONTROL_STOP);
}

static const wchar_t* kEtwSession = L"FlatPace";

class PresentMonReader {
 public:
  ~PresentMonReader() { Stop(); }

  bool Start(const std::wstring& exe, const std::wstring& processName) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE writeEnd = nullptr;
    if (!CreatePipe(&read_, &writeEnd, &sa, 0)) return false;
    SetHandleInformation(read_, HANDLE_FLAG_INHERIT, 0);

    job_ = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &li, sizeof(li));

    // --track_frame_type: marca que fotogramas son reales y cuales generados.
    // Sin nombre: vigila todos los procesos (deteccion automatica del juego).
    std::wstring cmd = L"\"" + exe + L"\"";
    if (!processName.empty()) cmd += L" --process_name \"" + processName + L"\"";
    cmd += L" --output_stdout --stop_existing_session"
           L" --session_name FlatPace --track_frame_type --track_pc_latency";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);
    CloseHandle(writeEnd);
    if (!ok) {
      CloseHandle(read_);
      read_ = nullptr;
      return false;
    }
    AssignProcessToJobObject(job_, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    running_ = true;
    thread_ = std::thread([this] { ReadLoop(); });
    return true;
  }

  void Stop() {
    if (job_) TerminateJobObject(job_, 0);
    StopEtwSession(kEtwSession);  // no dejar la sesion de captura abierta
    if (thread_.joinable()) thread_.join();
    if (process_) { CloseHandle(process_); process_ = nullptr; }
    if (read_) { CloseHandle(read_); read_ = nullptr; }
    if (job_) { CloseHandle(job_); job_ = nullptr; }
  }

  bool Running() const { return running_; }

  std::vector<FrameSample> DrainSamples() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<FrameSample> out;
    out.swap(samples_);
    return out;
  }

  std::vector<std::string> DrainMessages() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    out.swap(messages_);
    return out;
  }

 private:
  void ReadLoop() {
    std::string buffer;
    char chunk[4096];
    DWORD n = 0;
    while (ReadFile(read_, chunk, sizeof(chunk), &n, nullptr) && n > 0) {
      buffer.append(chunk, n);
      size_t pos;
      while ((pos = buffer.find('\n')) != std::string::npos) {
        std::string line = buffer.substr(0, pos);
        buffer.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        HandleLine(line);
      }
    }
    running_ = false;
  }

  static std::vector<std::string> SplitCsv(const std::string& line) {
    std::vector<std::string> cols;
    size_t start = 0;
    while (true) {
      size_t comma = line.find(',', start);
      cols.push_back(line.substr(start, comma == std::string::npos ? std::string::npos
                                                                     : comma - start));
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
    return cols;
  }

  static int FindColumn(const std::vector<std::string>& cols,
                        std::initializer_list<const char*> candidates) {
    for (const char* cand : candidates)
      for (size_t i = 0; i < cols.size(); ++i)
        if (Lower(Trim(cols[i])) == cand) return static_cast<int>(i);
    return -1;
  }

  static bool ParseNumber(const std::string& s, double& out) {
    std::string v = Trim(s);
    if (v.empty() || v == "NA") return false;
    char* end = nullptr;
    out = std::strtod(v.c_str(), &end);
    return end != v.c_str() && std::isfinite(out);
  }

  static bool IsRealFrameType(const std::string& lowerType) {
    return lowerType.empty() || lowerType == "na" || lowerType == "notset" ||
           lowerType == "application";
  }

  bool HasCol(int c, const std::vector<std::string>& cols) const {
    return c >= 0 && c < static_cast<int>(cols.size());
  }

  void HandleLine(const std::string& line) {
    if (line.empty()) return;
    std::vector<std::string> cols = SplitCsv(line);

    if (!headerParsed_) {
      if (line.find("Application") != std::string::npos && cols.size() > 3) {
        colFrame_ = FindColumn(cols, {"msbetweenpresents", "frametime", "msbetweenappstart"});
        colCpu_ = FindColumn(cols, {"cpubusy", "mscpubusy"});
        colGpu_ = FindColumn(cols, {"gpubusy", "msgpubusy"});
        colSwap_ = FindColumn(cols, {"swapchainaddress"});
        colApp_ = FindColumn(cols, {"application"});
        colPid_ = FindColumn(cols, {"processid"});
        colSim_ = FindColumn(cols, {"msbetweensimulationstart"});
        colMode_ = FindColumn(cols, {"presentmode"});
        colType_ = FindColumn(cols, {"frametype"});
        colDisplay_ = FindColumn(cols, {"msbetweendisplaychange", "displayedtime"});
        colAnim_ = FindColumn(cols, {"msanimationerror", "animationerror"});
        headerParsed_ = true;
        auto name = [&](int c) { return c >= 0 ? Trim(cols[c]) : std::string("no disponible"); };
        Message("Columnas de PresentMon: fotograma=" +
                (colFrame_ >= 0 ? Trim(cols[colFrame_]) : std::string("NO ENCONTRADA")) +
                ", CPU=" + name(colCpu_) + ", GPU=" + name(colGpu_) +
                ", tipo=" + name(colType_) + ", simulacion=" + name(colSim_) +
                ", animacion=" + name(colAnim_));
      } else if (line.find("already running") == std::string::npos &&
                 line.find("--session_name") == std::string::npos) {
        Message("[PresentMon] " + line);
      }
      return;
    }

    if (!HasCol(colFrame_, cols)) return;
    FrameSample s;
    s.t = NowSec();
    if (!ParseNumber(cols[colFrame_], s.presentMs) || s.presentMs <= 0.0) return;
    double v = 0.0;
    if (HasCol(colCpu_, cols) && ParseNumber(cols[colCpu_], v)) s.cpuBusyMs = v;
    if (HasCol(colGpu_, cols) && ParseNumber(cols[colGpu_], v)) s.gpuBusyMs = v;
    if (HasCol(colApp_, cols)) s.app = Lower(Trim(cols[colApp_]));
    if (HasCol(colMode_, cols)) s.presentMode = Trim(cols[colMode_]);
    if (HasCol(colDisplay_, cols) && ParseNumber(cols[colDisplay_], v) && v > 0) s.displayMs = v;
    if (HasCol(colAnim_, cols) && ParseNumber(cols[colAnim_], v)) s.animErrMs = v;
    if (HasCol(colPid_, cols) && ParseNumber(cols[colPid_], v)) s.pid = static_cast<DWORD>(v);
    s.swapchain = std::to_string(s.pid) + ":" + (HasCol(colSwap_, cols) ? Trim(cols[colSwap_]) : "");
    if (HasCol(colType_, cols)) {
      std::string type = Lower(Trim(cols[colType_]));
      s.generated = !IsRealFrameType(type);
      if (seenTypes_.insert(type).second && !type.empty())
        Message("Tipo de fotograma visto: " + Trim(cols[colType_]) +
                (s.generated ? " (generado)" : " (real)"));
    }

    // Marcadores de simulacion (NVIDIA Reflex / PC Latency): cada fotograma real
    // tiene uno; los generados no. Si el juego los emite, son la mejor referencia.
    double sim = 0.0;
    bool simValid = HasCol(colSim_, cols) && ParseNumber(cols[colSim_], sim) && sim > 0.0;
    int& simCount = simSeen_[s.pid];
    if (simValid) {
      if (++simCount == kSimTrustFrames)
        Message("Marcadores de simulacion (Reflex) disponibles en este juego: los uso para\n"
                "  separar fotogramas reales y generados.");
    }
    bool simTrusted = simCount >= kSimTrustFrames;
    if (simTrusted && !simValid) s.generated = true;

    // El intervalo real suma todas las presentaciones desde el ultimo fotograma real.
    // El trabajo de GPU de los fotogramas generados tambien se suma al real que los
    // acompana: PresentMon lo reparte entre ambos, pero el coste es del ciclo completo.
    double& acc = accum_[s.swapchain];
    double& accGpu = accumGpu_[s.swapchain];
    acc += s.presentMs;
    if (s.generated) {
      if (s.gpuBusyMs > 0) accGpu += s.gpuBusyMs;
    } else {
      s.realMs = (simTrusted && simValid) ? sim : acc;
      s.presentRealMs = acc;
      if (s.gpuBusyMs >= 0) s.gpuBusyMs += accGpu;
      acc = 0.0;
      accGpu = 0.0;
    }

    overlay::Push(s);
    std::lock_guard<std::mutex> lock(mutex_);
    samples_.push_back(s);
  }

  void Message(const std::string& m) {
    std::lock_guard<std::mutex> lock(mutex_);
    messages_.push_back(m);
  }

  HANDLE read_ = nullptr, job_ = nullptr, process_ = nullptr;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::mutex mutex_;
  std::vector<FrameSample> samples_;
  std::vector<std::string> messages_;
  std::atomic<bool> headerParsed_{false};

 public:
  bool HeaderSeen() const { return headerParsed_; }

 private:
  int colFrame_ = -1, colCpu_ = -1, colGpu_ = -1, colSwap_ = -1, colType_ = -1, colDisplay_ = -1, colAnim_ = -1;
  int colApp_ = -1, colPid_ = -1, colSim_ = -1, colMode_ = -1;
  static constexpr int kSimTrustFrames = 60;
  std::map<DWORD, int> simSeen_;
  std::map<std::string, double> accum_;
  std::map<std::string, double> accumGpu_;
  std::set<std::string> seenTypes_;
};


// ---------------------------------------------------------------------------
// Estado compartido entre el motor (hilo de medicion) y la ventana
// ---------------------------------------------------------------------------
enum class UiStatus { Starting, Searching, Measuring, Applied, Restart, Paused, Error };

struct UiChange {
  std::string exe, label, desc, warning;
  int value = 0;    // fps en pantalla
  int fg = 1;       // factor de generacion de fotogramas
  bool manual = false;
};

struct UiState {
  std::mutex m;
  UiStatus status = UiStatus::Starting;
  std::string statusText = "Iniciando…", statusDetail;
  std::string gpu, monitor, tip, driver;
  bool driverOk = false;
  bool haveGame = false;
  std::string game, exe;
  double realFps = 0, lowFps = 0, screenFps = 0, steadyPct = 0, jitterMs = 0;
  int hitches = 0;
  std::string capText, bottleneck, recText, appliedText, notice, presentText;
  std::vector<UiChange> changes;
  std::map<std::string, std::string> warnings;  // .exe -> motivo para revisar su limite
  std::string vrrRange;     // rango de frecuencias declarado por el monitor
  bool vrrPossible = false, vrrNvKnown = false;
  int vrrState = 0;         // 0 sin datos, 1 no actua, 2 actua, 3 actua pero desactivado por el usuario
  std::deque<std::string> log;
  std::string partial;
  std::wstring logPath;
  double hz = 0.0;          // frecuencia real usada por la app
};
static UiState g_ui;
static std::atomic<bool> g_applyMode{true};
static std::atomic<int> g_priority{0};  // 0 = estabilidad (por defecto), 1 = fluidez
// Usar G-SYNC / FreeSync cuando actuen (automatico). Ajuste avanzado, sin interfaz:
// "divisores-siempre=si" en equipo.txt fuerza los divisores de los Hz aunque haya VRR, para
// pantallas que parpadean con VRR (algunas OLED y VA).
static std::atomic<bool> g_useVrr{true};
// Con cada limite, pedir al driver de NVIDIA que no baje las frecuencias de la GPU (avanzado:
// rendimiento-maximo=no en equipo.txt lo desactiva).
static std::atomic<bool> g_maxPerf{true};
// Rango de VRR del monitor segun su EDID (0 = desconocido): el rango FreeSync si lo declara, si no
// el rango de frecuencias. Por encima del maximo el VRR deja de actuar; por debajo del minimo, el
// monitor solo sigue el ritmo si puede duplicar fotogramas (LFC, cuando maximo >= 2 x minimo).
static std::atomic<int> g_vrrMinHz{0}, g_vrrMaxHz{0};
// Grabar los datos de cada fotograma en un CSV por partida (para analisis; desactivado por defecto).
static std::atomic<bool> g_recordFrames{false};
static std::atomic<bool> g_undoRequested{false};
static std::atomic<bool> g_applyNowRequest{false};
// Limites manuales pedidos desde la ventana: (.exe, fps en pantalla).
static std::mutex g_manualLimitMutex;
static std::vector<std::pair<std::string, int>> g_manualLimitReq;
static std::mutex g_undoOneMutex;
static std::vector<std::string> g_undoOne;  // .exe cuyos cambios hay que deshacer
static std::mutex g_manualMutex;
static std::string g_manualExe;  // vacio = deteccion automatica

static void UiAppendLog(const std::string& text) {
  std::lock_guard<std::mutex> lock(g_ui.m);
  g_ui.partial += text;
  size_t pos;
  while ((pos = g_ui.partial.find('\n')) != std::string::npos) {
    g_ui.log.push_back(g_ui.partial.substr(0, pos));
    g_ui.partial.erase(0, pos + 1);
    while (g_ui.log.size() > 600) g_ui.log.pop_front();
  }
}

// ---------------------------------------------------------------------------
// Registro: todo lo que se muestra tambien se guarda en un archivo de texto
// ---------------------------------------------------------------------------
static FILE* g_log = nullptr;

static void UiAppendLog(const std::string& text);

static void Log(const char* fmt, ...) {
  char buf[2048];
  va_list a;
  va_start(a, fmt);
  int n = std::vsnprintf(buf, sizeof(buf), fmt, a);
  va_end(a);
  if (n < 0) return;
  std::string text(buf, std::min<size_t>(static_cast<size_t>(n), sizeof(buf) - 1));
  if (g_log) {
    std::fputs(text.c_str(), g_log);
    std::fflush(g_log);
  }
  UiAppendLog(text);
}

// %LOCALAPPDATA%\FlatPace: datos que deben sobrevivir a cambiar de version o carpeta.
static std::wstring DataDirectory() {
  wchar_t buf[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
  std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(buf, n) + L"\\FlatPace" : ExeDirectory();
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

static std::wstring Pad2(int v) {
  return (v < 10 ? L"0" : L"") + std::to_wstring(v);
}

static std::wstring OpenLog() {
  SYSTEMTIME t;
  GetLocalTime(&t);
  std::wstring path = ExeDirectory() + L"\\registro-" + std::to_wstring(t.wYear) + Pad2(t.wMonth) +
                      Pad2(t.wDay) + L"-" + Pad2(t.wHour) + Pad2(t.wMinute) + Pad2(t.wSecond) +
                      L".txt";
  g_log = _wfopen(path.c_str(), L"w");
  return g_log ? path : std::wstring();
}

#include "src/core/analysis.h"
#include "src/core/suggest.h"

static void PrintAdvice(const WindowStats& st) {
  std::string b = st.Bottleneck();
  if (b == "GPU")
    Log("  Sugerencia: el freno es la GPU. Baja la resolucion o los ajustes mas\n"
        "  pesados (trazado de rayos, sombras) o usa DLSS/FSR en un modo mas agresivo.\n");
  else if (b == "CPU")
    Log("  Sugerencia: el freno es el procesador. Baja ajustes que cargan la CPU\n"
        "  (multitudes, distancia de dibujado) y cierra programas en segundo plano.\n");
  else
    Log("  Sugerencia: baja los ajustes graficos hasta pasar de %.0f fps estables.\n",
        cfg::kVrrMinFps);
}

static void PrintRecommendation(const Recommendation& r, double hz, const WindowStats& st) {
  if (r.vrr) {
    if (r.fgFactor > 1)
      Log("  Con G-SYNC: limite %.0f fps en pantalla = %.0f reales (sin necesidad de divisores).\n",
          r.capFps, r.realFps);
    else
      Log("  Con G-SYNC: limite %.0f fps (sin necesidad de divisores; maximo %.0f).\n", r.capFps,
          VrrCeiling(hz));
    return;
  }
  if (!r.valid) {
    Log("  Sin VRR: el juego no sostiene ni %.0f fps estables.\n", cfg::kMinCapFps);
  } else {
    if (r.tight)
      Log("  Ajustado: no hay un limite con margen comodo, pero este solo falla en el %.2f%%\n"
          "  de los fotogramas, y sin limite el ritmo seria irregular.\n", 100.0 * r.lateFraction);
    if (r.fgFactor > 1)
      Log("  Con generacion x%d: %.3f fps en pantalla (%.3f Hz / %d) = %.3f reales - seguro: %.2f%% tarde\n",
          r.fgFactor, r.capFps, hz, r.divisor, r.realFps, 100.0 * st.FractionOver(1000.0 / r.realFps));
    else
      Log("  Sin VRR: limite %.3f fps (%.3f Hz / %d) - seguro: %.2f%% de fotogramas tarde\n",
          r.capFps, hz, r.divisor, 100.0 * st.FractionOver(1000.0 / r.capFps));
    if (r.divisor > 1) {
      double higher = hz / (r.divisor - 1);
      Log("  Opcion mas alta: %.3f fps en pantalla - justo: %.2f%% de fotogramas llegarian tarde\n",
          higher, 100.0 * st.FractionOver(1000.0 * r.fgFactor / higher));
    }
  }
  if (r.vrrValid)
    Log("  Con VRR (G-Sync/FreeSync): limite %.1f fps\n", r.vrrCapFps);
  else
    Log("  Con VRR: el juego va por debajo del rango habitual (~%.0f Hz); ahi el VRR\n"
        "  no ayuda.\n", cfg::kVrrMinFps);
  if (st.UseGpuOnly())
    Log("  Ojo: calculado solo con la GPU; el limite activo oculta el margen de CPU.\n"
        "  Para confirmarlo, quita el limite del juego un momento.\n");
  if (!r.valid || !r.vrrValid) PrintAdvice(st);
}

static void PrintCap(const WindowStats& st, double hz) {
  if (!st.capped) return;
  if (st.capFits && st.capDivisor == 0)
    Log("  Limite activo detectado: ~%.0f fps. Encaja: G-SYNC/FreeSync actua dentro de su rango.\n", st.capFps);
  else if (st.capFits)
    Log("  Limite activo detectado: ~%.0f fps. Encaja con tu monitor (%.3f Hz / %d)%s.\n",
        st.capFps, hz, st.capDivisor, st.capDivisor == 1 ? ", probablemente VSync" : "");
  else
    Log("  Limite activo detectado: ~%.0f fps. NO encaja con tu monitor de %.0f Hz:\n"
        "  el ritmo sera irregular (y con tearing si el VSync esta apagado).\n"
        "  FlatPace lo corregira aplicando un limite que encaje.\n",
        st.capFps, hz);
}

static void PrintStats(const WindowStats& st, double hz) {
  Log("  Fotogramas reales: mediana %.2f ms (~%.0f fps) | p99 %.2f ms (1%% bajo ~ %.0f fps)\n",
      st.medianMs, 1000.0 / st.medianMs, st.p99Ms, 1000.0 / st.p99Ms);
  if (st.GeneratedFraction() > cfg::kFgOnFraction) {
    Log("  Generacion de fotogramas activa: ~%.0f fps en pantalla (%.0f%% generados)\n",
        st.presentedFps, 100.0 * st.GeneratedFraction());
    if (1000.0 / st.medianMs < cfg::kFgMinBaseFps)
      Log("  Aviso: la base real (~%.0f fps) es baja para generar fotogramas. El juego\n"
          "  responde a ese ritmo, con mas latencia, y los fotogramas generados muestran\n"
          "  mas defectos. Suele ser mejor subir la base bajando ajustes graficos.\n",
          1000.0 / st.medianMs);
  }
  if (st.HasBusy())
    Log("  Trabajo real p99: CPU %.2f ms%s | GPU %.2f ms\n", std::max(0.0, st.cpuBusyP99),
        st.UseGpuOnly() ? " (no fiable con limite activo)" : "", std::max(0.0, st.gpuBusyP99));
  PrintCap(st, hz);
  Log("  Cuello de botella: %s\n", st.Bottleneck().c_str());
}

// ---------------------------------------------------------------------------
// Deteccion del juego: procesos y ventanas
// ---------------------------------------------------------------------------

static DWORD ForegroundPid(HWND* outWnd) {
  HWND h = GetForegroundWindow();
  if (outWnd) *outWnd = h;
  DWORD pid = 0;
  if (h) GetWindowThreadProcessId(h, &pid);
  return pid;
}

static bool WindowCoversMonitor(HWND h) {
  if (!h) return false;
  RECT wr;
  if (!GetWindowRect(h, &wr)) return false;
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) return false;
  return wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top &&
         wr.right >= mi.rcMonitor.right && wr.bottom >= mi.rcMonitor.bottom;
}

static std::wstring ProcessImagePath(DWORD pid) {
  HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!p) return {};
  wchar_t buf[1024];
  DWORD n = 1024;
  std::wstring r;
  if (QueryFullProcessImageNameW(p, 0, buf, &n)) r.assign(buf, n);
  CloseHandle(p);
  return r;
}

static bool ProcessAlive(DWORD pid) {
  HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  // Algunos anticheats niegan el acceso: eso no significa que el juego se cerro.
  if (!p) return GetLastError() == ERROR_ACCESS_DENIED;
  DWORD code = 0;
  bool alive = GetExitCodeProcess(p, &code) && code == STILL_ACTIVE;
  CloseHandle(p);
  return alive;
}

// Carpetas tipicas de tiendas y lanzadores de juegos.
static bool IsLibraryPath(const std::wstring& path) {
  std::string l = Lower(Narrow(path));
  for (const char* k : {"steamapps\\common", "epic games", "gog galaxy\\games", "gog games",
                        "xboxgames", "ea games", "ubisoft game launcher",
                        "battle.net", "riot games", "itch\\apps"})
    if (l.find(k) != std::string::npos) return true;
  return false;
}

// Programas que dibujan en pantalla pero nunca son el juego.
static bool IsExcludedApp(const std::string& exe) {
  if (exe.empty() || exe.rfind("presentmon", 0) == 0 || exe.rfind("flatpace", 0) == 0)
    return true;
  static const char* kExcluded[] = {
      "dwm.exe", "explorer.exe", "chrome.exe", "msedge.exe", "firefox.exe", "opera.exe",
      "opera_gx.exe", "brave.exe", "vivaldi.exe", "discord.exe", "steam.exe",
      "steamwebhelper.exe", "epicgameslauncher.exe", "galaxyclient.exe", "eadesktop.exe",
      "battle.net.exe", "obs64.exe", "obs32.exe", "spotify.exe", "code.exe",
      "nvidia overlay.exe", "nvidia app.exe", "applicationframehost.exe", "searchhost.exe",
      "shellexperiencehost.exe", "startmenuexperiencehost.exe", "textinputhost.exe",
      "lockapp.exe", "vlc.exe", "mpc-hc64.exe", "mpc-be64.exe", "wmplayer.exe",
      "microsoft.media.player.exe", "ms-teams.exe", "teams.exe", "slack.exe", "whatsapp.exe",
      "telegram.exe", "windowsterminal.exe", "conhost.exe", "cmd.exe", "powershell.exe",
      "msiafterburner.exe", "rtss.exe", "lossless scaling.exe", "shaderbeam.exe",
      "shaderglass.exe", "armoury crate.exe", "armourycrate.exe", "gameoverlayui.exe",
      "riotclientservices.exe", "riotclientux.exe", "riotclientuxrender.exe", "ubisoftconnect.exe",
      "upc.exe", "uplaywebcore.exe", "galaxyclient helper.exe", "epicwebhelper.exe",
      "crashreportclient.exe", "unrealcefsubprocess.exe", "cefsharp.browsersubprocess.exe",
      "easyanticheat.exe", "easyanticheat_eos.exe", "beservice.exe", "vgtray.exe",
      "eabackgroundservice.exe", "eaconnect_microsoft.exe", "agent.exe",
      // programas de control de fabricantes de portatiles
      "predatorsense.exe", "nitrosense.exe", "acerquickaccess.exe", "lenovovantage.exe",
      "lenovo vantage.exe", "legionspace.exe", "legion space.exe", "omen command center.exe",
      "omencommandcenter.exe", "msi center.exe", "msi.central.exe", "dragon center.exe",
      "alienware command center.exe", "awcc.exe", "gaming center.exe", "controlcenter.exe",
      "nvidia share.exe", "razer synapse.exe", "razercentralservice.exe"};
  for (const char* k : kExcluded)
    if (exe == k) return true;
  return false;
}

static double WindowArea(HWND h) {
  RECT r;
  if (!GetWindowRect(h, &r)) return 0.0;
  return static_cast<double>(r.right - r.left) * static_cast<double>(r.bottom - r.top);
}

static double MonitorAreaOf(HWND h) {
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) return 0.0;
  return static_cast<double>(mi.rcMonitor.right - mi.rcMonitor.left) *
         static_cast<double>(mi.rcMonitor.bottom - mi.rcMonitor.top);
}

// Ventanas principales visibles de todos los procesos: pid -> ventana mas grande.
static std::map<DWORD, HWND> TopWindowsByPid() {
  std::map<DWORD, HWND> out;
  EnumWindows(
      [](HWND w, LPARAM lp) -> BOOL {
        if (!IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
        if (GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        auto& m = *reinterpret_cast<std::map<DWORD, HWND>*>(lp);
        auto it = m.find(pid);
        if (it == m.end() || WindowArea(w) > WindowArea(it->second)) m[pid] = w;
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&out));
  return out;
}

static std::string ExeNameFromPath(const std::wstring& path) {
  size_t slash = path.find_last_of(L"\\/");
  return Lower(Narrow(slash == std::wstring::npos ? path : path.substr(slash + 1)));
}

// Juego abierto aunque no este en primer plano: primero los que dibujan fotogramas,
// despues ventanas grandes de programas instalados en carpetas de juegos.
static DWORD FindBackgroundGame(const std::map<DWORD, int>& perPid,
                                const std::map<DWORD, std::string>& pidApp, HWND* wndOut,
                                std::string* exeOut) {
  static std::map<DWORD, std::wstring> pathCache;
  auto pathOf = [&](DWORD pid) -> const std::wstring& {
    auto it = pathCache.find(pid);
    if (it == pathCache.end()) it = pathCache.emplace(pid, ProcessImagePath(pid)).first;
    return it->second;
  };
  std::map<DWORD, HWND> windows = TopWindowsByPid();
  DWORD best = 0;
  double bestScore = 0.0;
  for (const auto& [pid, n] : perPid) {
    if (n < 10) continue;
    auto app = pidApp.find(pid);
    if (app == pidApp.end() || IsExcludedApp(app->second)) continue;
    auto w = windows.find(pid);
    HWND wnd = w != windows.end() ? w->second : nullptr;
    bool lib = IsLibraryPath(pathOf(pid));
    if (!lib && !(wnd && WindowCoversMonitor(wnd))) continue;
    double score = n + (lib ? 1000.0 : 0.0);
    if (score > bestScore) {
      bestScore = score;
      best = pid;
      *wndOut = wnd;
      *exeOut = app->second;
    }
  }
  if (best) return best;
  for (const auto& [pid, wnd] : windows) {
    const std::wstring& path = pathOf(pid);
    if (!IsLibraryPath(path)) continue;
    std::string exe = ExeNameFromPath(path);
    if (IsExcludedApp(exe)) continue;
    double mon = MonitorAreaOf(wnd);
    if (mon <= 0 || WindowArea(wnd) < 0.5 * mon) continue;
    *wndOut = wnd;
    *exeOut = exe;
    return pid;
  }
  return 0;
}

static std::wstring Hex4(unsigned v) {
  const wchar_t* d = L"0123456789abcdef";
  std::wstring s(4, L'0');
  for (int i = 3; i >= 0; --i) { s[i] = d[v & 0xF]; v >>= 4; }
  return s;
}

// Lee ProductName o FileDescription de la informacion interna del .exe.
static std::wstring VersionString(const std::wstring& path, const wchar_t* key) {
  if (path.empty()) return {};
  DWORD dummy = 0;
  DWORD size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
  if (!size) return {};
  std::vector<BYTE> data(size);
  if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return {};
  struct LangCp { WORD lang, cp; };
  LangCp* tr = nullptr;
  UINT len = 0;
  if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<LPVOID*>(&tr),
                      &len) || len < sizeof(LangCp))
    return {};
  std::wstring sub = L"\\StringFileInfo\\" + Hex4(tr[0].lang) + Hex4(tr[0].cp) + L"\\" + key;
  wchar_t* val = nullptr;
  if (!VerQueryValueW(data.data(), sub.c_str(), reinterpret_cast<LPVOID*>(&val), &len) || !val)
    return {};
  return std::wstring(val);
}

static std::wstring TrimW(const std::wstring& s) {
  size_t a = s.find_first_not_of(L" \t\r\n");
  if (a == std::wstring::npos) return {};
  size_t b = s.find_last_not_of(L" \t\r\n");
  return s.substr(a, b - a + 1);
}

// Nombre legible: titulo de la ventana, nombre del producto, descripcion o .exe.
static std::string GameLabel(HWND wnd, const std::wstring& path, const std::string& exe) {
  if (wnd) {
    wchar_t buf[256];
    int n = GetWindowTextW(wnd, buf, 256);
    std::wstring t = TrimW(std::wstring(buf, n > 0 ? n : 0));
    if (!t.empty()) return Narrow(t);
  }
  for (const wchar_t* key : {L"ProductName", L"FileDescription"}) {
    std::wstring v = TrimW(VersionString(path, key));
    if (!v.empty()) return Narrow(v);
  }
  std::string name = exe;
  if (name.size() > 4 && name.substr(name.size() - 4) == ".exe") name.resize(name.size() - 4);
  return name.empty() ? std::string("(juego sin nombre)") : name;
}

// ---------------------------------------------------------------------------
// Driver de NVIDIA (NVAPI): limite de FPS en el perfil de cada juego
// Estructuras copiadas de los encabezados oficiales de NVAPI (licencia MIT).
// Las funciones se obtienen de nvapi64.dll, que instala el propio driver.
// ---------------------------------------------------------------------------
namespace nv {
using NvU32 = uint32_t;
using NvU16 = uint16_t;
using NvU8 = uint8_t;
using NvU64 = uint64_t;
using Status = int;
constexpr int kStringMax = 2048;
using UnicodeString = NvU16[kStringMax];
struct SessionH__ { int unused; };
using Session = SessionH__*;
struct ProfileH__ { int unused; };
using Profile = ProfileH__*;

constexpr Status OK = 0;
constexpr Status SETTING_NOT_FOUND = -160;
constexpr Status PROFILE_NOT_FOUND = -163;
constexpr Status EXECUTABLE_NOT_FOUND = -166;
constexpr Status INVALID_USER_PRIVILEGE = -137;
constexpr Status NVIDIA_DEVICE_NOT_FOUND = -6;
constexpr NvU32 FRL_FPS_ID = 0x10835002;  // "Frame Rate Limiter" (Velocidad maxima de fotogramas)
constexpr NvU32 VSYNCMODE_ID = 0x00A879CF;       // "Vertical Sync"
constexpr NvU32 PRERENDERLIMIT_ID = 0x007BA09E;  // "Maximum pre-rendered frames"
// "Power management mode" (Modo de administracion de energia): 1 = preferir rendimiento maximo.
// Con un limite, el driver baja las frecuencias de la GPU hasta que va justa y tarda en subirlas en
// las escenas pesadas, que se pasan del limite (Greylock con 90 fps: el doble de trabajo por fotograma
// con el 85 % de uso). Es la combinacion que recomienda NVIDIA para su limitador; la GPU sigue
// protegiendose sola por temperatura y potencia.
constexpr NvU32 PREFERRED_PSTATE_ID = 0x1057EB71;
constexpr NvU32 PSTATE_PREFER_MAX = 1;
constexpr NvU32 VSYNC_FORCEON = 0x47814940;
constexpr NvU32 VSYNC_FLIP2 = 0x32610244;        // VSync a 1/2 del refresco
constexpr NvU32 VSYNC_FLIP3 = 0x71271021;        // 1/3
constexpr NvU32 VSYNC_FLIP4 = 0x13245256;        // 1/4
inline NvU32 VsyncForDivisor(int n) {
  return n == 1 ? VSYNC_FORCEON : n == 2 ? VSYNC_FLIP2 : n == 3 ? VSYNC_FLIP3 : VSYNC_FLIP4;
}
inline int DivisorForVsync(NvU32 v) {
  return v == VSYNC_FORCEON ? 1 : v == VSYNC_FLIP2 ? 2 : v == VSYNC_FLIP3 ? 3 : v == VSYNC_FLIP4 ? 4 : 0;
}
constexpr NvU32 FRL_FPS_MAX = 0x3ff;
constexpr int DWORD_TYPE = 0;
constexpr int CURRENT_PROFILE_LOCATION = 0;

struct BinarySetting {
  NvU32 valueLength;
  NvU8 valueData[4096];
};

#pragma pack(push, 4)
struct Setting {
  NvU32 version;
  UnicodeString settingName;
  NvU32 settingId;
  int settingType;
  int settingLocation;
  NvU32 isCurrentPredefined;
  NvU32 isPredefinedValid;
  union {
    NvU32 u32PredefinedValue;
    BinarySetting binaryPredefinedValue;
    UnicodeString wszPredefinedValue;
    NvU64 u64PredefinedValue;
  };
  union {
    NvU32 u32CurrentValue;
    BinarySetting binaryCurrentValue;
    UnicodeString wszCurrentValue;
    NvU64 u64CurrentValue;
  };
};
#pragma pack(pop)

struct Application {  // NVDRS_APPLICATION_V4
  NvU32 version;
  NvU32 isPredefined;
  UnicodeString appName;
  UnicodeString userFriendlyName;
  UnicodeString launcher;
  UnicodeString fileInFolder;
  NvU32 isMetro : 1;
  NvU32 isCommandLine : 1;
  NvU32 reserved : 30;
  UnicodeString commandLine;
};

struct ProfileInfo {  // NVDRS_PROFILE_V1
  NvU32 version;
  UnicodeString profileName;
  NvU32 gpuSupport;
  NvU32 isPredefined;
  NvU32 numOfApps;
  NvU32 numOfSettings;
};

static_assert(sizeof(Setting) == 12320, "NVDRS_SETTING_V1");
static_assert(sizeof(Application) == 20492, "NVDRS_APPLICATION_V4");
static_assert(sizeof(ProfileInfo) == 4116, "NVDRS_PROFILE_V1");
constexpr NvU32 Ver(size_t size, NvU32 v) { return static_cast<NvU32>(size) | (v << 16); }
constexpr NvU32 SETTING_VER = Ver(sizeof(Setting), 1);
constexpr NvU32 APPLICATION_VER = Ver(sizeof(Application), 4);
constexpr NvU32 PROFILE_VER = Ver(sizeof(ProfileInfo), 1);

using QueryInterfaceFn = void*(__cdecl*)(unsigned int);
using InitializeFn = Status(__cdecl*)();
using CreateSessionFn = Status(__cdecl*)(Session*);
using SessionFn = Status(__cdecl*)(Session);
using FindAppFn = Status(__cdecl*)(Session, NvU16*, Profile*, Application*);
using CreateProfileFn = Status(__cdecl*)(Session, ProfileInfo*, Profile*);
using FindProfileFn = Status(__cdecl*)(Session, NvU16*, Profile*);
using CreateAppFn = Status(__cdecl*)(Session, Profile, Application*);
using GetSettingFn = Status(__cdecl*)(Session, Profile, NvU32, Setting*);
struct VrrInfo {
  NvU32 version;
  NvU32 flags;  // bit 0 activo, 1 posible, 2 pedido, 3 indicador, 4 pantalla en modo VRR
  NvU32 reservedEx[4];
};
using GetDisplayIdFn = Status(__cdecl*)(const char*, NvU32*);
using GetVrrInfoFn = Status(__cdecl*)(NvU32, VrrInfo*);
using SetSettingFn = Status(__cdecl*)(Session, Profile, Setting*);
using DeleteSettingFn = Status(__cdecl*)(Session, Profile, NvU32);
using DeleteProfileFn = Status(__cdecl*)(Session, Profile);

static void Copy(NvU16* dst, const std::wstring& s) {
  std::memset(dst, 0, sizeof(NvU16) * kStringMax);
  size_t n = std::min<size_t>(s.size(), kStringMax - 1);
  std::memcpy(dst, s.c_str(), n * sizeof(NvU16));
}
}  // namespace nv

static std::wstring Widen(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

// Un cambio hecho por la app, con lo necesario para deshacerlo exactamente.
struct SavedSetting {
  uint32_t id = 0;
  bool had = false;   // el perfil ya tenia un valor propio
  uint32_t old = 0;   // ese valor, para restaurarlo
};

enum class Method { FpsLimit = 0, DriverVsync = 1 };

struct AppliedChange {
  std::string exe;              // nombre del .exe en minusculas
  bool createdProfile = false;  // el perfil lo creo la app
  Method method = Method::FpsLimit;
  int divisor = 0;              // refrescos por imagen (VSync del driver)
  uint32_t newValue = 0;        // fps aplicados (limite) o aproximados (VSync); en pantalla si hay generacion
  std::vector<SavedSetting> saved;  // valores originales de cada ajuste tocado
  int fgFactor = 1;             // >1: aplicado para un juego con generacion de fotogramas
  uint32_t frlValue = 0;        // valor puesto en el limitador del driver
  int displayHz = 0;            // frecuencia de pantalla para este juego (0 = la normal)
  bool verified = false;        // se comprobo funcionando al menos una vez
  uint32_t basisReal = 0;       // fps reales (mediana) cuando se decidio el limite
  bool vrr = false;             // decidido con G-SYNC actuando (sin divisores)
  bool manual = false;          // puesto a mano por el usuario: la app nunca lo cambia
};

static std::string DescribeChangeBase(const AppliedChange& c);
static bool HasMaxPerf(const AppliedChange& c) {
  for (const auto& sv : c.saved)
    if (sv.id == 0x1057EB71u) return true;
  return false;
}

static std::string DescribeChange(const AppliedChange& c) {
  std::string d = DescribeChangeBase(c);
  if (c.vrr) d += " · G-SYNC";
  if (c.manual) d += T(" · manual", " · manual");
  if (HasMaxPerf(c)) d += T(" · GPU al máximo", " · GPU at max");
  if (c.displayHz > 0) d += std::string(T(" · pantalla a ", " · display at ")) + std::to_string(c.displayHz) + " Hz";
  return d;
}
static std::string DescribeChangeBase(const AppliedChange& c) {
  if (c.fgFactor > 1)
    return std::to_string(c.newValue) + T(" fps en pantalla (", " fps on screen (") +
           std::to_string(c.newValue / static_cast<uint32_t>(c.fgFactor)) +
           T(" reales) · límite del driver", " real) · driver limit");
  if (c.method == Method::DriverVsync)
    return std::to_string(c.newValue) + T(" fps · VSync del driver (1/", " fps · driver VSync (1/") +
           std::to_string(c.divisor) + T(" del refresco)", " of refresh)");
  return std::to_string(c.newValue) + T(" fps · límite del driver", " fps · driver limit");
}

class NvDriver {
 public:
  ~NvDriver() { if (dll_) FreeLibrary(dll_); }

  bool Init() {
    dll_ = LoadLibraryW(L"nvapi64.dll");
    if (!dll_) { error = "no hay driver de NVIDIA (nvapi64.dll)"; return false; }
    auto qi = reinterpret_cast<nv::QueryInterfaceFn>(
        reinterpret_cast<void*>(GetProcAddress(dll_, "nvapi_QueryInterface")));
    if (!qi) { error = "NVAPI no disponible"; return false; }
    auto get = [&](unsigned id) { return qi(id); };
    init_ = reinterpret_cast<nv::InitializeFn>(get(0x0150e828));
    createSession_ = reinterpret_cast<nv::CreateSessionFn>(get(0x0694d52e));
    destroySession_ = reinterpret_cast<nv::SessionFn>(get(0xdad9cff8));
    load_ = reinterpret_cast<nv::SessionFn>(get(0x375dbd6b));
    save_ = reinterpret_cast<nv::SessionFn>(get(0xfcbc7e14));
    findApp_ = reinterpret_cast<nv::FindAppFn>(get(0xeee566b2));
    createProfile_ = reinterpret_cast<nv::CreateProfileFn>(get(0xcc176068));
    findProfile_ = reinterpret_cast<nv::FindProfileFn>(get(0x7e4a9a0b));
    createApp_ = reinterpret_cast<nv::CreateAppFn>(get(0x4347a9de));
    getSetting_ = reinterpret_cast<nv::GetSettingFn>(get(0x73bf8338));
    setSetting_ = reinterpret_cast<nv::SetSettingFn>(get(0x577dd202));
    deleteSetting_ = reinterpret_cast<nv::DeleteSettingFn>(get(0xe4a26362));
    deleteProfile_ = reinterpret_cast<nv::DeleteProfileFn>(get(0x17093206));
    getDisplayId_ = reinterpret_cast<nv::GetDisplayIdFn>(get(0xae457190));  // opcionales
    getVrrInfo_ = reinterpret_cast<nv::GetVrrInfoFn>(get(0xdf8fda57));
    if (!init_ || !createSession_ || !destroySession_ || !load_ || !save_ || !findApp_ ||
        !createProfile_ || !findProfile_ || !createApp_ || !getSetting_ || !setSetting_ ||
        !deleteSetting_ || !deleteProfile_) {
      error = "faltan funciones de NVAPI en este driver";
      return false;
    }
    nv::Status st = init_();
    if (st != nv::OK) { error = Describe(st); return false; }
    ok_ = true;
    return true;
  }

  bool Available() const { return ok_; }

  // Deja en el perfil del juego exactamente los ajustes "desired". Guarda el valor
  // original de cada ajuste la primera vez que se toca, y devuelve a su estado
  // original los que la app toco antes pero ya no hacen falta.
  bool ApplySettings(const std::string& exe, const std::vector<std::pair<uint32_t, uint32_t>>& desired,
                     AppliedChange& change) {
    if (!ok_) return false;
    nv::Session s = nullptr;
    if (!Open(s)) return false;
    nv::Profile p = nullptr;
    bool created = false;
    if (!FindOrCreateProfile(s, exe, p, created)) { Close(s); return false; }
    if (change.exe.empty()) {
      change = AppliedChange{};
      change.exe = exe;
      change.createdProfile = created;
    }
    nv::Status st = nv::OK;
    for (const auto& [id, value] : desired) {
      bool known = false;
      for (const auto& sv : change.saved) known = known || sv.id == id;
      if (!known) {
        SavedSetting sv;
        sv.id = id;
        nv::Setting cur{};
        cur.version = nv::SETTING_VER;
        if (getSetting_(s, p, id, &cur) == nv::OK &&
            cur.settingLocation == nv::CURRENT_PROFILE_LOCATION && !cur.isCurrentPredefined) {
          sv.had = true;
          sv.old = cur.u32CurrentValue;
        }
        change.saved.push_back(sv);
      }
    }
    for (const auto& sv : change.saved) {
      bool wanted = false;
      for (const auto& d : desired) wanted = wanted || d.first == sv.id;
      if (!wanted && st == nv::OK) st = Restore(s, p, sv);
    }
    for (const auto& [id, value] : desired)
      if (st == nv::OK) st = SetDword(s, p, id, value);
    if (st == nv::OK) st = save_(s);
    Close(s);
    if (st != nv::OK) { error = Describe(st); return false; }
    return true;
  }

  // Anade un ajuste a un cambio ya aplicado (guarda su valor original para deshacerlo).
  bool AddSetting(AppliedChange& change, uint32_t id, uint32_t value) {
    if (!ok_) return false;
    for (const auto& sv : change.saved)
      if (sv.id == id) return true;  // ya estaba
    nv::Session s = nullptr;
    if (!Open(s)) return false;
    nv::Profile p = nullptr;
    bool created = false;
    if (!FindOrCreateProfile(s, change.exe, p, created)) { Close(s); return false; }
    SavedSetting sv;
    sv.id = id;
    nv::Setting cur{};
    cur.version = nv::SETTING_VER;
    if (getSetting_(s, p, id, &cur) == nv::OK && cur.settingLocation == nv::CURRENT_PROFILE_LOCATION &&
        !cur.isCurrentPredefined) {
      sv.had = true;
      sv.old = cur.u32CurrentValue;
    }
    nv::Status st = SetDword(s, p, id, value);
    if (st == nv::OK) st = save_(s);
    Close(s);
    if (st != nv::OK) { error = Describe(st); return false; }
    change.saved.push_back(sv);
    return true;
  }

  // G-SYNC en una pantalla: posible (pantalla compatible y activado) y actuando ahora.
  bool Vrr(const std::string& gdiName, bool& possible, bool& active) {
    possible = active = false;
    if (!ok_ || !getDisplayId_ || !getVrrInfo_) return false;
    nv::NvU32 id = 0;
    if (getDisplayId_(gdiName.c_str(), &id) != nv::OK) return false;
    nv::VrrInfo info{};
    info.version = static_cast<nv::NvU32>(sizeof(nv::VrrInfo)) | (1u << 16);
    if (getVrrInfo_(id, &info) != nv::OK) return false;
    possible = (info.flags & 0x2) != 0;
    active = (info.flags & 0x1) != 0 || (info.flags & 0x10) != 0;
    return true;
  }

  // Busca un perfil creado por la app ("FlatPace - juego.exe") y lo que tiene puesto.
  bool FindOwnProfile(const std::string& exe, AppliedChange& out) {
    if (!ok_) return false;
    nv::Session s = nullptr;
    if (!Open(s)) return false;
    nv::UnicodeString pname;
    nv::Copy(pname, L"FlatPace - " + Widen(exe));
    nv::Profile p = nullptr;
    bool found = false;
    if (findProfile_(s, pname, &p) == nv::OK) {
      out = AppliedChange{};
      out.exe = exe;
      out.createdProfile = true;
      nv::Setting cur{};
      cur.version = nv::SETTING_VER;
      if (getSetting_(s, p, nv::VSYNCMODE_ID, &cur) == nv::OK &&
          cur.settingLocation == nv::CURRENT_PROFILE_LOCATION &&
          nv::DivisorForVsync(cur.u32CurrentValue) > 0) {
        out.method = Method::DriverVsync;
        out.divisor = nv::DivisorForVsync(cur.u32CurrentValue);
        found = true;
      }
      nv::Setting frl{};
      frl.version = nv::SETTING_VER;
      if (!found && getSetting_(s, p, nv::FRL_FPS_ID, &frl) == nv::OK &&
          frl.settingLocation == nv::CURRENT_PROFILE_LOCATION && frl.u32CurrentValue > 0) {
        out.method = Method::FpsLimit;
        out.newValue = frl.u32CurrentValue;
        found = true;
      }
    }
    Close(s);
    return found;
  }

  // Devuelve el perfil del juego a como estaba antes de la app.
  bool Revert(const AppliedChange& change) {
    if (!ok_) return false;
    nv::Session s = nullptr;
    if (!Open(s)) return false;
    nv::Application app{};
    app.version = nv::APPLICATION_VER;
    nv::UnicodeString name;
    nv::Copy(name, Widen(change.exe));
    nv::Profile p = nullptr;
    nv::Status st = findApp_(s, name, &p, &app);
    if (st == nv::EXECUTABLE_NOT_FOUND) { Close(s); return true; }  // ya no existe: nada que deshacer
    if (st == nv::OK) {
      if (change.createdProfile) {
        st = deleteProfile_(s, p);
      } else {
        for (const auto& sv : change.saved)
          if (st == nv::OK) st = Restore(s, p, sv);
      }
    }
    if (st == nv::OK) st = save_(s);
    Close(s);
    if (st != nv::OK) { error = Describe(st); return false; }
    return true;
  }

  std::string error;

 private:
  static std::string Describe(nv::Status st) {
    switch (st) {
      case nv::INVALID_USER_PRIVILEGE: return "faltan permisos de administrador";
      case nv::NVIDIA_DEVICE_NOT_FOUND: return "no se encontro una GPU NVIDIA";
      case nv::PROFILE_NOT_FOUND: return "perfil no encontrado";
      case nv::EXECUTABLE_NOT_FOUND: return "juego no encontrado en los perfiles";
      default: return "error de NVAPI " + std::to_string(st);
    }
  }

  bool Open(nv::Session& s) {
    nv::Status st = createSession_(&s);
    if (st == nv::OK) st = load_(s);
    if (st != nv::OK) {
      error = Describe(st);
      if (s) destroySession_(s);
      s = nullptr;
      return false;
    }
    return true;
  }

  void Close(nv::Session s) { if (s) destroySession_(s); }

  nv::Status SetDword(nv::Session s, nv::Profile p, uint32_t id, uint32_t value) {
    nv::Setting set{};
    set.version = nv::SETTING_VER;
    set.settingId = id;
    set.settingType = nv::DWORD_TYPE;
    set.u32CurrentValue = value;
    return setSetting_(s, p, &set);
  }

  nv::Status Restore(nv::Session s, nv::Profile p, const SavedSetting& sv) {
    if (sv.had) return SetDword(s, p, sv.id, sv.old);
    nv::Status st = deleteSetting_(s, p, sv.id);
    return st == nv::SETTING_NOT_FOUND ? nv::OK : st;
  }

  bool FindOrCreateProfile(nv::Session s, const std::string& exe, nv::Profile& p, bool& created) {
    created = false;
    nv::Application app{};
    app.version = nv::APPLICATION_VER;
    nv::UnicodeString name;
    nv::Copy(name, Widen(exe));
    nv::Status st = findApp_(s, name, &p, &app);
    if (st == nv::OK) return true;
    if (st != nv::EXECUTABLE_NOT_FOUND) { error = Describe(st); return false; }

    // El juego no tiene perfil: se crea uno propio y reconocible.
    std::wstring profileName = L"FlatPace - " + Widen(exe);
    nv::UnicodeString pname;
    nv::Copy(pname, profileName);
    st = findProfile_(s, pname, &p);
    if (st != nv::OK) {
      nv::ProfileInfo info{};
      info.version = nv::PROFILE_VER;
      nv::Copy(info.profileName, profileName);
      st = createProfile_(s, &info, &p);
      if (st != nv::OK) { error = Describe(st); return false; }
    }
    nv::Application newApp{};
    newApp.version = nv::APPLICATION_VER;
    nv::Copy(newApp.appName, Widen(exe));
    st = createApp_(s, p, &newApp);
    if (st != nv::OK) { error = Describe(st); return false; }
    created = true;
    return true;
  }

  HMODULE dll_ = nullptr;
  bool ok_ = false;
  nv::InitializeFn init_ = nullptr;
  nv::CreateSessionFn createSession_ = nullptr;
  nv::SessionFn destroySession_ = nullptr, load_ = nullptr, save_ = nullptr;
  nv::FindAppFn findApp_ = nullptr;
  nv::CreateProfileFn createProfile_ = nullptr;
  nv::FindProfileFn findProfile_ = nullptr;
  nv::CreateAppFn createApp_ = nullptr;
  nv::GetSettingFn getSetting_ = nullptr;
  nv::GetDisplayIdFn getDisplayId_ = nullptr;
  nv::GetVrrInfoFn getVrrInfo_ = nullptr;
  nv::SetSettingFn setSetting_ = nullptr;
  nv::DeleteSettingFn deleteSetting_ = nullptr;
  nv::DeleteProfileFn deleteProfile_ = nullptr;
};

// Registro persistente de cambios, para poder deshacerlos aunque la app se cierre.
class ChangeStore {
 public:
  explicit ChangeStore(std::wstring path) : path_(std::move(path)) {}

  void Load() {
    items.clear();
    LoadFrom(path_);
  }

  // Une los cambios apuntados en otra ubicacion (versiones antiguas) y la retira.
  int Migrate(const std::wstring& oldPath) {
    if (oldPath == path_ || GetFileAttributesW(oldPath.c_str()) == INVALID_FILE_ATTRIBUTES) return 0;
    size_t before = items.size();
    LoadFrom(oldPath);
    Save();
    MoveFileExW(oldPath.c_str(), (oldPath + L".migrado").c_str(), MOVEFILE_REPLACE_EXISTING);
    return static_cast<int>(items.size() - before);
  }

  void LoadFrom(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"r");
    if (!f) return;
    char line[2048];
    while (std::fgets(line, sizeof(line), f)) {
      std::vector<std::string> t;
      {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        size_t a = 0;
        while (true) {
          size_t b = l.find('\t', a);
          t.push_back(l.substr(a, b == std::string::npos ? std::string::npos : b - a));
          if (b == std::string::npos) break;
          a = b + 1;
        }
      }
      AppliedChange c;
      if (t.size() >= 7 && t[0] == "v2") {
        // v2  exe  creado  metodo  divisor  fps  n  (id  tenia  original)*n
        c.exe = t[1];
        c.createdProfile = t[2] == "1";
        c.method = t[3] == "1" ? Method::DriverVsync : Method::FpsLimit;
        c.divisor = std::atoi(t[4].c_str());
        c.newValue = static_cast<uint32_t>(std::strtoul(t[5].c_str(), nullptr, 10));
        size_t n = std::strtoul(t[6].c_str(), nullptr, 10);
        for (size_t k = 0; k < n && 7 + 3 * k + 2 < t.size(); ++k) {
          SavedSetting sv;
          sv.id = static_cast<uint32_t>(std::strtoul(t[7 + 3 * k].c_str(), nullptr, 16));
          sv.had = t[8 + 3 * k] == "1";
          sv.old = static_cast<uint32_t>(std::strtoul(t[9 + 3 * k].c_str(), nullptr, 16));
          c.saved.push_back(sv);
        }
        for (size_t e = 7 + 3 * n; e + 1 < t.size(); ++e) {
          if (t[e] == "fg" && e + 2 < t.size()) {
            c.fgFactor = std::atoi(t[e + 1].c_str());
            c.frlValue = static_cast<uint32_t>(std::strtoul(t[e + 2].c_str(), nullptr, 10));
          } else if (t[e] == "hz") {
            c.displayHz = std::atoi(t[e + 1].c_str());
          } else if (t[e] == "ok") {
            c.verified = t[e + 1] == "1";
          } else if (t[e] == "vrr") {
            c.vrr = t[e + 1] == "1";
          } else if (t[e] == "manual") {
            c.manual = t[e + 1] == "1";
          } else if (t[e] == "base") {
            c.basisReal = static_cast<uint32_t>(std::strtoul(t[e + 1].c_str(), nullptr, 10));
          }
        }
      } else if (t.size() >= 5) {
        // formato antiguo: exe  creado  tenia  original  fps  (solo limite de FPS)
        c.exe = t[0];
        c.createdProfile = t[1] == "1";
        SavedSetting sv;
        sv.id = nv::FRL_FPS_ID;
        sv.had = t[2] == "1";
        sv.old = static_cast<uint32_t>(std::strtoul(t[3].c_str(), nullptr, 10));
        c.saved.push_back(sv);
        c.newValue = static_cast<uint32_t>(std::strtoul(t[4].c_str(), nullptr, 10));
      } else {
        continue;
      }
      if (!c.exe.empty() && !items.count(c.exe)) items[c.exe] = c;
    }
    std::fclose(f);
  }

  void Save() const {
    if (items.empty()) { _wremove(path_.c_str()); return; }
    FILE* f = _wfopen(path_.c_str(), L"w");
    if (!f) return;
    for (const auto& [exe, c] : items) {
      std::fprintf(f, "v2\t%s\t%d\t%d\t%d\t%u\t%zu", c.exe.c_str(), c.createdProfile ? 1 : 0,
                   c.method == Method::DriverVsync ? 1 : 0, c.divisor, c.newValue, c.saved.size());
      for (const auto& sv : c.saved) std::fprintf(f, "\t%08X\t%d\t%08X", sv.id, sv.had ? 1 : 0, sv.old);
      if (c.fgFactor > 1) std::fprintf(f, "\tfg\t%d\t%u", c.fgFactor, c.frlValue);
      if (c.displayHz > 0) std::fprintf(f, "\thz\t%d", c.displayHz);
      if (c.verified) std::fprintf(f, "\tok\t1");
      if (c.basisReal > 0) std::fprintf(f, "\tbase\t%u", c.basisReal);
      if (c.vrr) std::fprintf(f, "\tvrr\t1");
      if (c.manual) std::fprintf(f, "\tmanual\t1");
      std::fprintf(f, "\n");
    }
    std::fclose(f);
  }

  std::map<std::string, AppliedChange> items;

 private:
  std::wstring path_;
};

// Limite entero que encaja con los Hz reales (el driver solo admite enteros).
// Debe quedar justo por debajo de Hz/n, a menos de un 0.1 %.
static uint32_t DriverFps(const Recommendation& r, double hz, int* divisorOut) {
  if (!r.valid) return 0;
  for (int n = r.divisor; n <= cfg::kMaxDivisor; ++n) {
    double f = hz / n;
    if (f < cfg::kMinCapFps) break;
    double whole = std::floor(f + 1e-9);
    if ((f - whole) / f < 0.001) {
      if (divisorOut) *divisorOut = n;
      return static_cast<uint32_t>(whole);
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Sesion: la medicion de un juego concreto
// ---------------------------------------------------------------------------
static std::string Fmt(const char* fmt, ...) {
  char buf[512];
  va_list a;
  va_start(a, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, a);
  va_end(a);
  return buf;
}

// Ritmo real que el juego esta sosteniendo bajo un limite activo (pocos fotogramas tardios), o 0.
template <typename Stats>
static double HeldRate(const Stats& st) {
  if (!st.capped || st.capFps <= 0) return 0.0;
  double period = 1000.0 / st.capFps;
  return st.FractionRealOver(period * 1.25) <= 0.02 ? st.capFps : 0.0;
}

struct Session;
static Recommendation SessionRec(const Session& ss, double hz);

struct Session {
  DWORD pid = 0;
  std::string exe, label;
  std::deque<FrameSample> samples;
  double start = -1.0, phaseStart = 0.0, lastStatus = 0.0, lastFrameArrival = 0.0;
  bool haveRec = false, fgActive = false, fgEverSeen = false, paused = false;
  bool background = false, seenForeground = false, rateChangeNoted = false;
  bool fgAdviceDue = false;
  int fgOnCount = 0, fgOffCount = 0;  // segundos seguidos con/sin generacion
  double fgSince = 0.0;               // desde cuando la generacion esta activa
  bool lowerPending = false;  // ya se propuso bajar: esperar a que se aplique antes de otra
  bool keepNoted = false;
  bool accepted = false;        // el limite de este juego ya se comprobo: no se recomienda nada mas
  bool acceptedNoted = false;
  bool appliedThisSession = false;
  int fgMismatchSecs = 0;
  int betterSecs = 0;
  int lastWarnKind = 0;
  // Aplicacion del limite
  double lastApply = -1e9;
  double expectedFps = 0;       // ritmo que deberia verse si el driver lo aplico
  Method expectMethod = Method::FpsLimit;
  bool appliedAtStart = false;  // el cambio ya estaba puesto al abrir el juego
  double baseSwing = -1.0;      // variacion del ritmo de este juego sin limite (guardada)
  int expectFg = 1;             // factor de generacion con el que se aplico
  std::map<std::string, uint64_t> presentModes;  // como muestra Windows las imagenes del juego
  std::string announcedMode;

  std::string MainPresentMode() const {
    std::string best;
    uint64_t n = 0;
    for (const auto& [m, c] : presentModes)
      if (c > n) { n = c; best = m; }
    return best;
  }
  // "Composed: ..." = pasa por el compositor del escritorio: sin tearing posible.
  bool IsComposed() const { return MainPresentMode().rfind("Composed", 0) == 0; }
  double verifyDeadline = 0.0;
  bool verified = false, restartNoted = false, noTargetNoted = false, ownCapNoted = false;
  Recommendation current;
  WindowStats lastStats;
  int downCount = 0, upCount = 0, regimeCount = 0, capChangeCount = 0;
  int hitchTotal = 0, pauseTotal = 0;
  double worstHitch = 0.0;
  bool announcedCapped = false;
  double announcedCapFps = 0.0;
  // Recomendacion anterior, por si el ultimo cambio ocurrio al cerrar el juego.
  bool hasPrevRec = false;
  Recommendation prevRec;
  WindowStats prevStats, recStats;  // estadisticas con las que se decidio cada una
  double recChangedAt = 0.0;

  void ChangeRec(const Recommendation& r, const WindowStats& st, double now) {
    hasPrevRec = true;
    prevRec = current;
    prevStats = recStats;
    recChangedAt = now;
    current = r;
    recStats = st;
  }

  double T(double t) const { return start > 0 ? t - start : 0.0; }

  // Empieza una medicion limpia tras esperar a que el ritmo se estabilice.
  std::deque<std::pair<double, double>> longReal;  // (momento, ms) de los ultimos minutos
  std::deque<float> longWork;                       // trabajo de CPU/GPU de esos fotogramas (ms)
  // Fases de juego (BOCPD sobre el log de la mediana de cada segundo). Una fase nueva solo cuenta si,
  // tras 10 s, su ritmo difiere al menos un 8 % del de la fase anterior.
  PhaseTracker phases;
  // Variacion entre fotogramas consecutivos en toda la partida (para el resumen).
  double jPrev = -1.0, sPrev = -1.0, jSum = 0.0, sSum = 0.0;
  size_t jN = 0, j2 = 0, sN = 0, s2 = 0;
  double aeSum = 0.0, aeMax = 0.0;
  size_t aeN = 0, aeOver1 = 0;  // fotogramas con error de animacion > 1 ms
  bool vrrActive = false;   // VRR actuando con este juego (y permitido por el usuario)
  bool vrrNv = false, vrrSeen = false;  // segun el driver de NVIDIA / segun los tiempos de pantalla
  int seenOn = 0, seenOff = 0;
  size_t presN = 0, presOff = 0, dispN = 0, dispOff = 0;
  // Toda la partida (para decidir al cerrar el juego), separada por factor de generacion.
  std::map<int, std::vector<float>> sessionReal;
  double gameSecs = 0.0, vrrSecs = 0.0, lastTick = 0.0;
  double ownCapSecs = 0.0, ownCapRateSum = 0.0;  // juego limitado por si mismo (sin limite nuestro)
  // Regla de fallos de un limite aceptado: mas de un 13 % por debajo, en tramos de al
  // menos 5 s, durante al menos el 10 % de la partida y 1 minuto en total.
  double limPlaySecs = 0.0, failSecs = 0.0;
  int belowRun = 0;
  std::vector<float> secRates;
  int vrrOnCount = 0, vrrOffCount = 0;
  bool expectVrr = false;

  std::vector<double> LongSorted() const {
    std::vector<double> v;
    v.reserve(longReal.size());
    for (const auto& r : longReal) v.push_back(r.second);
    std::sort(v.begin(), v.end());
    return v;
  }

  void ResetPhase(double now) {
    samples.clear();
    longReal.clear();
    phaseStart = now + cfg::kSettleSec;
    haveRec = false;
    current = Recommendation{};
    lastStats = WindowStats{};
    downCount = upCount = regimeCount = capChangeCount = 0;
    rateChangeNoted = false;
  }
};

static void WriteKv(const std::wstring& file, const std::string& key, const std::string& value);
static std::string ReadKv(const std::wstring& file, const std::string& key);

// Periodo (ms de fotograma real) del limite que esta actuando, para el modelo de datos censurados:
// el de FlatPace si esta comprobado o se aplico al abrir el juego, o el que se detecta (del juego).
static double ActiveCapMs(const Session& ss) {
  if (ss.expectedFps > 0 && (ss.verified || ss.appliedAtStart)) return 1000.0 / ss.expectedFps;
  if (ss.lastStats.capped && ss.lastStats.capFps > 0) return 1000.0 / ss.lastStats.capFps;
  return 0.0;
}

static double MedianOf(std::vector<double> v) {
  if (v.empty()) return 0.0;
  std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
  return v[v.size() / 2];
}

// ---------------------------------------------------------------------------
// Grabacion de fotogramas: un CSV por partida en la carpeta "grabaciones" junto al programa,
// con una cabecera de contexto (lineas que empiezan por #) y una fila por fotograma del juego.
// Sirve para analizar y validar modelos con datos reales, sin tocar la app.
// ---------------------------------------------------------------------------
static std::wstring RecordingsDirectory() {
  std::wstring dir = ExeDirectory() + L"\\grabaciones";
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

struct FrameRecorder {
  FILE* f = nullptr;
  std::wstring path;
  double t0 = 0.0, tSum = 0.0;
  size_t rows = 0;

  bool Open(const std::string& exe, const std::vector<std::string>& header) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::string base = exe;
    for (char& c : base)
      if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    path = RecordingsDirectory() + L"\\grabacion-" + Widen(base) + L"-" + std::to_wstring(t.wYear) + Pad2(t.wMonth) +
           Pad2(t.wDay) + L"-" + Pad2(t.wHour) + Pad2(t.wMinute) + Pad2(t.wSecond) + L".csv";
    f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    setvbuf(f, nullptr, _IOFBF, 1 << 16);
    for (const auto& h : header) std::fprintf(f, "# %s\n", h.c_str());
    std::fprintf(f, "t_llegada_s,t_acumulado_s,presentacion_ms,real_ms,real_presentacion_ms,generado,"
                    "cpu_ms,gpu_ms,pantalla_ms,error_animacion_ms,vrr,limite_confirmado\n");
    rows = 0;
    tSum = 0.0;
    return true;
  }

  void Write(const FrameSample& s, double sessionStart, bool vrr, bool verified) {
    if (!f) return;
    if (s.presentMs > 0) tSum += s.presentMs / 1000.0;
    auto num = [](double v, const char* fmt) { return std::isnan(v) || v < -1e8 ? std::string() : Fmt(fmt, v); };
    std::fprintf(f, "%.4f,%.4f,%.3f,%s,%s,%d,%s,%s,%s,%s,%d,%d\n", s.t - sessionStart, tSum, s.presentMs,
                 num(s.realMs > 0 ? s.realMs : NAN, "%.3f").c_str(),
                 num(s.presentRealMs > 0 ? s.presentRealMs : NAN, "%.3f").c_str(), s.generated ? 1 : 0,
                 num(s.cpuBusyMs >= 0 ? s.cpuBusyMs : NAN, "%.3f").c_str(),
                 num(s.gpuBusyMs >= 0 ? s.gpuBusyMs : NAN, "%.3f").c_str(),
                 num(s.displayMs > 0 ? s.displayMs : NAN, "%.3f").c_str(), num(s.animErrMs, "%.3f").c_str(),
                 vrr ? 1 : 0, verified ? 1 : 0);
    ++rows;
  }

  void Flush() {
    if (f) std::fflush(f);
  }

  // Devuelve el numero de filas escritas.
  size_t Close() {
    size_t n = rows;
    if (f) std::fclose(f);
    f = nullptr;
    rows = 0;
    return n;
  }
};

static void PrintSessionSummary(const Session& s, double hz) {
  if (s.start <= 0.0 || !s.seenForeground) return;
  double minutes = std::max(1.0 / 60.0, (NowSec() - s.start) / 60.0);
  Log("\n=== Resumen: %s ===\n", s.label.c_str());
  Log("  Duracion: %.1f min | tirones: %d (%.1f por minuto)", minutes, s.hitchTotal,
      s.hitchTotal / minutes);
  if (s.hitchTotal > 0) Log(", el peor de %.0f ms", s.worstHitch);
  if (s.jN > 0)
    Log(" | consistencia: %.1f %% de cambios < 2 ms entre fotogramas reales consecutivos (media %.2f ms)",
        100.0 * s.j2 / s.jN, s.jSum / s.jN);
  if (s.sN > 0 && s.sN > s.jN * 1.3)
    Log(" | en pantalla: %.1f %% < 2 ms (media %.2f ms)", 100.0 * s.s2 / s.sN, s.sSum / s.sN);
  {
    std::vector<double> msv;
    msv.reserve(s.longReal.size());
    for (const auto& r : s.longReal) msv.push_back(r.second);
    double swing = PaceSwingMs(msv);
    if (swing >= 0) {
      Log("\n  Variacion del ritmo percibido (ultimos minutos): +-%.2f ms", swing);
      if (!s.appliedAtStart && !s.exe.empty() && s.gameSecs >= 60.0) {
        // Partida sin limite de la app: es la referencia "antes" de este juego.
        WriteKv(L"equipo.txt", "ritmo-antes|" + s.exe, Fmt("%.2f", swing));
        Log(" (guardada como referencia sin limite)");
      } else if (s.baseSwing > 0) {
        Log(" | antes, sin limite: +-%.2f ms", s.baseSwing);
      }
    }
  }
  if (s.aeN > 100)
    Log("\n  Error de animacion: media %.2f ms | %.1f %% de fotogramas con mas de 1 ms | maximo %.1f ms",
        s.aeSum / s.aeN, 100.0 * s.aeOver1 / s.aeN, s.aeMax);
  else
    Log("\n  Error de animacion: no disponible para este juego");
  Log("\n  Pausas largas ignoradas: %d\n", s.pauseTotal);
  Log("  Generacion de fotogramas: %s\n", s.fgEverSeen ? "detectada" : "no detectada");
  if (!s.MainPresentMode().empty())
    Log("  Presentacion: %s\n", s.MainPresentMode().c_str());

  // Modelos estadisticos: capacidad con el limite puesto (datos censurados), memoria del juego
  // entre partidas y fases de juego. Por ahora informan y sugieren; no cambian las decisiones.
  {
    std::vector<double> msv;
    msv.reserve(s.longReal.size());
    for (const auto& r : s.longReal) msv.push_back(r.second);
    const double capMs = ActiveCapMs(s);
    double stableFps = 0.0;  // ritmo que el juego sostiene (criterio de Estabilidad: p99 x 1,05)
    bool plenty = false;     // casi todo en el limite: va sobrado
    CapacityEstimate ce;
    if (capMs > 0) ce = EstimateCapacity(msv, capMs);
    // Con un limite activo, el 1 % mas lento causado por el juego (sin los retrasos del limitador).
    double gameP99 = 0.0;
    if (capMs > 0 && msv.size() >= 200) {
      std::vector<double> wv(s.longWork.begin(), s.longWork.end());
      gameP99 = GameCausedPercentileMs(msv, wv, capMs, 99);
      if (gameP99 > 0) {
        std::vector<double> raw = msv;
        std::sort(raw.begin(), raw.end());
        Log("  1 %% bajo con el limite: %.0f fps medidos | %.0f fps causados por el juego (sin los retrasos del limitador)\n",
            1000.0 / PercentileSorted(raw, 99), 1000.0 / gameP99);
      }
    }
    if (ce.valid) {
      if (ce.medianReliable)
        Log("  Capacidad estimada sin limite (%.0f %% de fotogramas en el limite, modelo de datos censurados):\n"
            "    mediana ~%.0f fps | 1 %% mas lento ~%.0f fps\n",
            100.0 * ce.censoredFraction, 1000.0 / ce.medianMs, 1000.0 / ce.p99Ms);
      else
        Log("  Capacidad estimada (%.0f %% de fotogramas en el limite, modelo de datos censurados):\n"
            "    1 %% mas lento ~%.0f fps | la mediana queda por encima del limite (no se puede precisar)\n",
            100.0 * ce.censoredFraction, 1000.0 / ce.p99Ms);
    } else if (ce.censoredFraction > 0.98) {
      plenty = true;
      Log("  Capacidad: el juego va sobrado con su limite (%.0f %% de fotogramas en el limite).\n",
          100.0 * ce.censoredFraction);
    } else if (msv.size() >= 200 && ce.censoredFraction < 0.05) {
      std::vector<double> sorted = msv;
      std::sort(sorted.begin(), sorted.end());
      const double p99 = PercentileSorted(sorted, 99);
      if (p99 > 0) stableFps = 1000.0 / (p99 * 1.05);
    }
    // Proteccion contra la espiral: con un limite activo solo se aprende la capacidad si el juego NO
    // lo sostuvo (su 1 % mas lento, causado por el juego, supera el limite). Si lo sostuvo, solo se
    // sabe que da "al menos eso", y anotar el limite como capacidad haria bajarlo partida a partida.
    if (capMs > 0) {
      stableFps = 0.0;
      if (!plenty && gameP99 > capMs * 1.02) stableFps = 1000.0 / (gameP99 * 1.05);
    }
    GameMemory mem = GameMemory::Parse(ReadKv(L"equipo.txt", "memoria2|" + s.exe));
    if (stableFps > 0 && s.gameSecs >= 60.0 && !s.exe.empty()) {
      mem.Add(stableFps, s.gameSecs / 60.0);
      WriteKv(L"equipo.txt", "memoria2|" + s.exe, mem.Serialize());
    }
    if (mem.sessions > 0)
      Log("  Memoria del juego: %d partida%s; sostiene ~%.0f fps reales (con un 80 %% de probabilidad, al menos %.0f)\n",
          mem.sessions, mem.sessions == 1 ? "" : "s", mem.MeanFps(), mem.LowerFps());
    // Sugerencia: el limite confirmado podria subir al siguiente escalon.
    if (s.verified && s.expectedFps > 0 && !s.exe.empty()) {
      if (SuggestRaise(stableFps, plenty, mem, s.expectedFps, s.expectFg, s.expectVrr, hz) > 0) {
        std::string tip = plenty ? Fmt(T("Va sobrado con su límite: podría sostener bastante más de %.0f FPS reales. "
                                         "Pulsa «Deshacer» para recalcularlo.",
                                         "It has plenty of headroom: it could hold well over %.0f real FPS. "
                                         "Click “Undo” to recalculate it."),
                                       s.expectedFps)
                                 : Fmt(T("Podría sostener ~%.0f FPS reales (su límite: %.0f). Pulsa «Deshacer» para "
                                         "recalcularlo.",
                                         "It could hold ~%.0f real FPS (its limit: %.0f). Click “Undo” to recalculate it."),
                                       stableFps, s.expectedFps);
        Log("  Sugerencia: %s\n", tip.c_str());
        std::lock_guard<std::mutex> lock(g_ui.m);
        g_ui.warnings[s.exe] = tip;
      }
    }
    // Fases de juego
    std::vector<PhaseTracker::Phase> ph = s.phases.All(NowSec());
    if (ph.size() >= 2) {
      Log("  Fases de juego: %zu\n", ph.size());
      for (const auto& f : ph)
        Log("    %4.0f-%4.0f s: ~%.0f fps\n", f.start - s.start, f.end - s.start, 1000.0 / f.medianMs);
    }
  }

  if (s.haveRec) {
    bool lastChangeAtEnd = s.hasPrevRec && NowSec() - s.recChangedAt < 6.0;
    Log("\n  Recomendacion final:\n");
    if (lastChangeAtEnd) {
      Log("  (descarto el ultimo cambio: ocurrio justo antes de terminar)\n");
      PrintRecommendation(s.prevRec, hz, s.prevStats);
    } else {
      PrintRecommendation(s.current, hz, s.lastStats);
    }
  }
}

// Un segundo de analisis para el juego de la sesion.
// 1 % bajo (fps) causado por el juego en los ultimos 20 s, con un limite activo (0 si no hay limite).
static double GameLowFps(const Session& ss) {
  const double capMs = ActiveCapMs(ss);
  if (capMs <= 0 || ss.samples.empty()) return 0.0;
  const double from = ss.samples.back().t - cfg::kWindowSec;
  std::vector<double> r, w;
  for (const auto& f : ss.samples)
    if (!f.generated && f.realMs > 0 && f.t >= from) {
      r.push_back(f.realMs);
      w.push_back(std::max(f.cpuBusyMs, f.gpuBusyMs));
    }
  if (r.size() < 100) return 0.0;
  const double p99 = GameCausedPercentileMs(r, w, capMs, 99);
  return p99 > 0 ? 1000.0 / p99 : 0.0;
}

// Fases: un dato por segundo (mediana de los fotogramas reales de ese segundo).
static void TrackPhases(Session& ss, const std::vector<FrameSample>& fresh, double now) {
  std::vector<double> sec;
  for (const auto& f : fresh)
    if (!f.generated && f.realMs > 0 && f.realMs <= cfg::kSpikeMs) sec.push_back(f.realMs);
  if (sec.size() < 5 || ss.paused) return;
  PhaseTracker::Event ev = ss.phases.Push(now, MedianOf(sec));
  if (ev.confirmed)
    Log("[%5.0f s] Fase nueva desde los %.0f s: ~%.0f fps (antes ~%.0f fps durante %.0f s)\n", ss.T(now), ss.T(ev.at),
        1000.0 / ev.afterMs, 1000.0 / ev.beforeMs, ev.beforeSec);
}

static void ProcessSession(Session& ss, std::vector<FrameSample>& fresh, double now, double hz) {
  std::vector<double> longCache;
  bool longReady = false;
  auto Long = [&]() -> const std::vector<double>& {
    if (!longReady) {
      longCache = ss.LongSorted();
      longReady = true;
    }
    return longCache;
  };
  if (!fresh.empty()) {
    ss.lastFrameArrival = now;
    if (ss.paused) {
      ss.paused = false;
      Log("[%5.0f s] Fotogramas de nuevo. Espero %.0f s a que el ritmo se estabilice\n"
          "          y vuelvo a medir desde cero.\n", ss.T(now), cfg::kSettleSec);
      ss.ResetPhase(now);
    }
  } else if (!ss.paused && now - ss.lastFrameArrival > cfg::kGapSec) {
    ss.paused = true;
    Log("[%5.0f s] Sin fotogramas: juego minimizado o cargando. Medicion en pausa.\n", ss.T(now));
  }

  // Tirones: solo con una referencia valida y fuera de transiciones.
  if (ss.lastStats.medianMs > 0 && now >= ss.phaseStart) {
    int realN = 0, slowN = 0, events = 0, pauses = 0;
    double worst = 0.0;
    bool inSlow = false;
    for (const auto& s : fresh) {
      if (s.swapchain != ss.lastStats.dominant || s.realMs <= 0) continue;
      realN++;
      if (s.realMs > cfg::kSpikeMs) { pauses++; inSlow = false; continue; }
      bool slow = s.realMs > ss.lastStats.medianMs * cfg::kHitchFactor &&
                  s.realMs > ss.lastStats.medianMs + cfg::kHitchMinExtraMs;
      if (slow) {
        slowN++;
        worst = std::max(worst, s.realMs);
        if (!inSlow) events++;
      }
      inSlow = slow;
    }
    if (realN > 0 && slowN > cfg::kRateChangeFraction * realN) {
      if (!ss.rateChangeNoted) {
        Log("[%5.0f s] Muchos fotogramas lentos seguidos: parece un cambio de ritmo\n"
            "          (ajustes, limite o escena), no tirones. No los cuento.\n", ss.T(now));
        ss.rateChangeNoted = true;
      }
    } else if (events > 0) {
      ss.hitchTotal += events;
      ss.worstHitch = std::max(ss.worstHitch, worst);
      Log("[%5.0f s] TIRON%s: %d, el peor de %.0f ms (normal ~%.0f ms)\n", ss.T(now),
          events > 1 ? "ES" : "", events, worst, ss.lastStats.medianMs);
    }
    if (pauses > 0) {
      ss.pauseTotal += pauses;
      Log("[%5.0f s] Pausa larga ignorada (carga o menu)\n", ss.T(now));
    }
  }

  TrackPhases(ss, fresh, now);

  for (const auto& s : fresh) {
    if (!s.presentMode.empty()) ss.presentModes[s.presentMode]++;
    if (now >= ss.phaseStart && !std::isnan(s.animErrMs) && s.presentMs <= cfg::kSpikeMs &&
        std::fabs(s.animErrMs) <= cfg::kSpikeMs) {
      double a = std::fabs(s.animErrMs);
      ss.aeSum += a;
      ss.aeN++;
      if (a > 1.0) ss.aeOver1++;
      ss.aeMax = std::max(ss.aeMax, a);
    }
    if (now >= ss.phaseStart && s.presentMs > 0 && s.presentMs <= cfg::kSpikeMs) {
      if (ss.sPrev > 0) {
        double d = std::fabs(s.presentMs - ss.sPrev);
        ss.sSum += d;
        ss.sN++;
        if (d < 2.0) ss.s2++;
      }
      ss.sPrev = s.presentMs;
    }
    if (!s.generated && s.realMs > 0 && s.realMs <= cfg::kSpikeMs && now >= ss.phaseStart) {
      if (ss.jPrev > 0) {
        double d = std::fabs(s.realMs - ss.jPrev);
        ss.jSum += d;
        ss.jN++;
        if (d < 2.0) ss.j2++;
      }
      ss.jPrev = s.realMs;
      ss.longReal.push_back({s.t, s.realMs});
      ss.longWork.push_back(static_cast<float>(std::max(s.cpuBusyMs, s.gpuBusyMs)));
      int kNow = ss.fgActive ? std::max(2, ss.lastStats.FgFactor()) : 1;
      ss.sessionReal[kNow].push_back(static_cast<float>(s.realMs));
    }
  }
  while (!ss.longReal.empty() && ss.longReal.front().first < now - 180.0) {
    ss.longReal.pop_front();
    if (!ss.longWork.empty()) ss.longWork.pop_front();
  }
  {
    std::string mode = ss.MainPresentMode();
    if (!mode.empty() && mode != ss.announcedMode && ss.presentModes[mode] > 200) {
      ss.announcedMode = mode;
      Log("[%5.0f s] Presentacion del juego: %s (%s)\n", ss.T(now), mode.c_str(),
          ss.IsComposed() ? "pasa por el compositor: sin tearing posible"
                          : "directa: puede haber tearing si el VSync esta apagado");
    }
  }
  for (auto& s : fresh) ss.samples.push_back(std::move(s));
  while (!ss.samples.empty() && ss.samples.front().t < now - cfg::kWindowSec)
    ss.samples.pop_front();
  if (ss.paused || ss.samples.empty() || now < ss.phaseStart) return;

  double windowStart = std::max(ss.phaseStart, now - cfg::kWindowSec);
  WindowStats st = ComputeStats(ss.samples, windowStart, now);
  if (st.frames == 0) return;
  DetectCap(st, hz);
  // Con G-SYNC/FreeSync actuando, cualquier limite dentro del rango encaja (no hace falta un divisor).
  if (ss.vrrActive && st.capped && !st.capFits && st.capFps <= VrrCeiling(hz) + 3.0) {
    st.capFits = true;
    st.capDivisor = 0;
  }
  ss.lastStats = st;

  // Generacion de fotogramas: aviso al activarse o desactivarse.
  double shortFrom = std::max(ss.phaseStart, now - cfg::kRegimeShortSec);
  double fgFraction = ComputeStats(ss.samples, shortFrom, now).GeneratedFraction();
  // Solo cuenta un cambio que dure 3 s seguidos (algunos juegos la alternan en menus).
  ss.fgOnCount = fgFraction > cfg::kFgOnFraction ? ss.fgOnCount + 1 : 0;
  ss.fgOffCount = fgFraction < 0.01 ? ss.fgOffCount + 1 : 0;
  if (!ss.fgActive && ss.fgOnCount >= 3) {
    ss.fgSince = now;
    ss.fgActive = ss.fgEverSeen = true;
    Log("[%5.0f s] Generacion de fotogramas detectada. Mido los fotogramas reales y elijo\n"
        "          un ritmo en pantalla que encaje con tus Hz. Vuelvo a medir.\n", ss.T(now));
    ss.fgAdviceDue = true;
    if (ss.haveRec) { ss.ResetPhase(now); return; }
  } else if (ss.fgActive && ss.fgOffCount >= 3) {
    ss.fgActive = false;
    Log("[%5.0f s] Generacion de fotogramas desactivada. Vuelvo a medir.\n", ss.T(now));
    if (ss.haveRec) { ss.ResetPhase(now); return; }
  }

  // Cambio brusco de rendimiento: esperar a que se estabilice y medir de nuevo.
  WindowStats shortSt = ComputeStats(ss.samples, shortFrom, now);
  WindowStats longSt = ComputeStats(ss.samples, windowStart, now - cfg::kRegimeShortSec);
  if (shortSt.frames >= 20 && longSt.frames >= 100 && longSt.medianMs > 0) {
    double ratio = shortSt.medianMs / longSt.medianMs;
    bool changed = ratio > cfg::kRegimeRatio || ratio < 1.0 / cfg::kRegimeRatio;
    ss.regimeCount = changed ? ss.regimeCount + 1 : 0;
    if (ss.regimeCount >= cfg::kRegimeChecks) {
      Log("\n[%5.0f s] Cambio brusco de rendimiento: ~%.1f ms -> ~%.1f ms por fotograma real.\n"
          "          Posibles motivos: cambiaste ajustes o el limite, generacion de\n"
          "          fotogramas sin identificar o una zona muy distinta del juego.\n"
          "          Espero %.0f s a que se estabilice y vuelvo a medir.\n",
          ss.T(now), longSt.medianMs, shortSt.medianMs, cfg::kSettleSec);
      ss.ResetPhase(now);
      return;
    }
  }

  // Unos segundos despues de activar la generacion: aviso si la base es baja.
  if (ss.fgAdviceDue && st.GeneratedFraction() > cfg::kFgOnFraction && st.frames >= 100) {
    ss.fgAdviceDue = false;
    if (1000.0 / st.medianMs < cfg::kFgMinBaseFps)
      Log("[%5.0f s] Con generacion: ~%.0f fps reales y ~%.0f en pantalla. La base es baja:\n"
          "          el juego responde a ~%.0f fps y con mas latencia. Suele ser mejor subir\n"
          "          la base bajando ajustes graficos.\n",
          ss.T(now), 1000.0 / st.medianMs, st.presentedFps, 1000.0 / st.medianMs);
  }

  double elapsedPhase = now - ss.phaseStart;

  // Con el limite de la app funcionando, los datos no sirven para recalcular: la GPU
  // baja frecuencias y aparenta un margen que no existe. Solo se vigila que se mantenga.
  const bool holding = ss.expectedFps > 0 && (ss.verified || ss.accepted);

  // Fase rapida: primera recomendacion.
  if (!ss.haveRec) {
    bool ready = (elapsedPhase >= cfg::kQuickMinSec && st.frames >= cfg::kQuickMinFrames) ||
                 elapsedPhase >= cfg::kQuickMaxSec;
    if (!ready) return;
    if (holding) {
      int k = std::max(1, ss.expectFg);
      int n = std::max(1, static_cast<int>(std::lround(hz / (ss.expectedFps * k))));
      ss.current = DivisorRec(hz, n, k);
      if (ss.expectVrr) {
        ss.current.vrr = true;
        ss.current.capFps = ss.expectedFps * k;
        ss.current.realFps = ss.expectedFps;
      }
      ss.recStats = st;
      ss.hasPrevRec = false;
      ss.haveRec = true;
      ss.lastStatus = now;
      if (!ss.acceptedNoted) {
        ss.acceptedNoted = true;
        Log("\n=== %s con su limite (%.3f fps reales) ===\n", ss.label.c_str(), ss.expectedFps);
        PrintStats(st, hz);
        Log("  Limite aceptado: no recomiendo ni cambio nada para este juego mientras no lo\n"
            "  deshagas. Solo muestro las mediciones y aviso si conviene revisarlo.\n");
      }
      return;
    }
    ss.current = RecommendFor(st, hz, cfg::kMarginInitial);
    if (ss.vrrActive) {
      Recommendation vr = RecommendVrr(Long(), st.WorkP99(), hz, st.FgFactor(), g_priority == 1,
                                       HeldRate(st));
      if (vr.valid) ss.current = vr;
    } else if (g_priority == 1) {
      Recommendation fr = RecommendFluid(Long(), hz, st.FgFactor());
      if (fr.valid) ss.current = fr;
    }
    ss.recStats = st;
    ss.hasPrevRec = false;
    ss.haveRec = true;
    ss.lastStatus = now;
    ss.announcedCapped = st.capped;
    ss.announcedCapFps = st.capFps;
    Log("\n=== Recomendacion para %s (%.0f s, %zu fotogramas reales) ===\n", ss.label.c_str(),
        elapsedPhase, st.frames);
    PrintStats(st, hz);
    PrintRecommendation(ss.current, hz, st);
    Log("\n  Monitoreo continuo activo: baja rapido si hace falta, sube despacio.\n");
    return;
  }

  // Cambios en el limite activo (confirmados durante varios segundos).
  bool capDiffers = st.capped != ss.announcedCapped ||
                    (st.capped && std::fabs(st.capFps - ss.announcedCapFps) > 0.5);
  ss.capChangeCount = capDiffers ? ss.capChangeCount + 1 : 0;
  if (ss.capChangeCount >= cfg::kCapChangeChecks) {
    Log("\n[%5.0f s] %s\n", ss.T(now),
        st.capped ? "Nuevo limite activo:" : "Ya no hay un limite activo.");
    PrintCap(st, hz);
    ss.announcedCapped = st.capped;
    ss.announcedCapFps = st.capFps;
    ss.capChangeCount = 0;
  }

  if (holding) {
    // Solo bajar si una parte importante de los fotogramas llega tarde de forma continuada
    // (unos pocos tirones no se arreglan bajando el limite). Y nunca en cascada: tras
    // proponer un limite mas bajo, se espera a que se aplique y se compruebe.
    double period = 1000.0 / ss.current.realFps;
    bool failing = g_priority == 1 ? !FluidHolds(Long(), period * 1.02)
                                   : st.FractionRealOver(period * kHoldLateFactor) > kHoldFailLate;
    // Con el limite aceptado no se cambia nada: el contador solo alimenta el aviso
    // "el juego no esta sosteniendo este limite" (la decision es del usuario).
    ss.downCount = failing ? std::min(ss.downCount + 1, 1000) : 0;
  } else if (g_priority == 1 || ss.vrrActive) {
    // Fluidez (o G-SYNC): decide con los ultimos minutos de juego; sube tras 30 s y baja
    // tras 10 s. Con G-SYNC solo cuenta un cambio de al menos un 5 %.
    Recommendation fr = ss.vrrActive
                            ? RecommendVrr(Long(), st.WorkP99(), hz, st.FgFactor(), g_priority == 1,
                                           HeldRate(st))
                            : RecommendFluid(Long(), hz, st.FgFactor());
    if (!fr.valid) fr = RecommendFor(st, hz, cfg::kMarginInitial);
    double cur = ss.current.valid ? ss.current.capFps : 0.0;
    double nxt = fr.valid ? fr.capFps : 0.0;
    double minStep = ss.vrrActive && fr.vrr && ss.current.vrr ? cur * 0.05 : 0.01;
    ss.upCount = nxt > cur + minStep ? ss.upCount + 1 : 0;
    ss.downCount = nxt < cur - minStep ? ss.downCount + 1 : 0;
    if (ss.upCount >= cfg::kUpChecks || ss.downCount >= 10) {
      if (g_priority == 1)
        Log("\n[%5.0f s] %s (prioridad: fluidez).\n", ss.T(now),
            nxt > cur ? "SUBIR limite: el juego alcanza un ritmo mas alto la mayor parte del tiempo"
                      : "BAJAR limite: el juego ya no alcanza este ritmo la mayor parte del tiempo");
      else
        Log("\n[%5.0f s] %s (prioridad: estabilidad).\n", ss.T(now),
            nxt > cur ? "SUBIR limite: el juego sostiene un ritmo mas alto incluso en sus peores momentos"
                      : "BAJAR limite: en sus peores momentos el juego ya no sostiene este ritmo");
      ss.ChangeRec(fr, st, now);
      PrintRecommendation(ss.current, hz, st);
      ss.upCount = ss.downCount = 0;
    }
  } else {
  // Monitoreo continuo: bajar rapido, subir despacio.
  double curCap = ss.current.valid ? ss.current.capFps : 0.0;
  double curReal = ss.current.valid ? ss.current.realFps : 0.0;
  bool failing = st.capped
                     ? st.FractionRealOver((1000.0 / curReal) * kHoldLateFactor) > kHoldFailLate
                 : ss.current.tight
                     ? st.FractionOver(1000.0 / curReal) > kTightMaxLate
                     : st.WorkP99() > (1000.0 / curReal) * cfg::kDownTolerance;
  ss.downCount = (ss.current.valid && failing) ? ss.downCount + 1 : 0;
  Recommendation higher = Recommend(hz, st.WorkP99(), cfg::kMarginRaise, st.FgFactor());
  ss.upCount = (higher.valid && higher.capFps > curCap + 0.01) ? ss.upCount + 1 : 0;

  if (ss.downCount >= cfg::kDownChecks) {
    Recommendation lower = RecommendFor(st, hz, cfg::kMarginInitial);
    if (!lower.valid || lower.capFps < curCap) {
      Log("\n[%5.0f s] BAJAR limite: el actual ya no es sostenible.\n", ss.T(now));
      ss.ChangeRec(lower, st, now);
      PrintRecommendation(ss.current, hz, st);
    }
    ss.downCount = ss.upCount = 0;
  } else if (ss.upCount >= cfg::kUpChecks) {
    Log("\n[%5.0f s] %s: hay margen estable desde hace %d s.\n", ss.T(now),
        ss.current.valid ? "SUBIR limite" : "Ahora si hay un limite sostenible", cfg::kUpChecks);
    ss.ChangeRec(higher, st, now);
    PrintRecommendation(ss.current, hz, st);
    ss.downCount = ss.upCount = 0;
  }
  }  // fin del modo normal (sin limite propio funcionando)

  if (now - ss.lastStatus >= 5.0) {
    std::string extra;
    char buf[96];
    if (st.GeneratedFraction() > cfg::kFgOnFraction) {
      std::snprintf(buf, sizeof(buf), " | en pantalla ~%.0f fps", st.presentedFps);
      extra += buf;
    }
    if (st.capped) {
      std::snprintf(buf, sizeof(buf), " | limite activo ~%.0f%s", st.capFps,
                    st.capFits ? "" : " (no encaja)");
      extra += buf;
    }
    if (const double capMs = ActiveCapMs(ss); capMs > 0) {
      std::vector<double> win;
      for (const auto& f : ss.samples)
        if (!f.generated && f.realMs > 0 && f.t >= now - cfg::kWindowSec) win.push_back(f.realMs);
      CapacityEstimate ce = EstimateCapacity(win, capMs);
      if (ce.valid && ce.medianReliable)
        extra += Fmt(" | sin limite ~%.0f fps (p99 ~%.0f)", 1000.0 / ce.medianMs, 1000.0 / ce.p99Ms);
      else if (ce.valid)
        extra += Fmt(" | capacidad: p99 ~%.0f fps, mediana por encima del limite", 1000.0 / ce.p99Ms);
      else if (ce.censoredFraction > 0.98)
        extra += " | sobrado (casi todo en el limite)";
      if (const double gl = GameLowFps(ss); gl > 0) extra += Fmt(" | 1%% bajo del juego ~%.0f fps", gl);
    }
    std::string rec = ss.current.valid
                          ? std::to_string(static_cast<int>(ss.current.capFps + 0.5)) + " fps"
                          : std::string("ninguno");
    Log("[%5.0f s] reales: mediana %.2f ms (~%.0f fps)%s | p99 %.2f ms | trabajo p99 %.2f ms"
        " | consistencia %.0f%% (var %.2f ms)%s | recomendado %s | tirones %d\n",
        ss.T(now), st.medianMs, 1000.0 / st.medianMs, extra.c_str(), st.p99Ms, st.WorkP99(),
        100.0 * st.steady2, st.jitterMs,
        st.animErrMs >= 0 ? Fmt(" | animacion %.2f ms", st.animErrMs).c_str() : "", rec.c_str(),
        ss.hitchTotal);
    ss.lastStatus = now;
  }
}

// ---------------------------------------------------------------------------
// Frecuencia real fiable: mide, valida y, si sale ruidosa, repite o usa respaldo
// ---------------------------------------------------------------------------
static std::wstring RefreshCachePath() { return DataDirectory() + L"\\pantalla.txt"; }

static double ReadCachedHz(const std::string& key) {
  FILE* f = _wfopen(RefreshCachePath().c_str(), L"r");
  if (!f) return 0.0;
  char line[256];
  double hz = 0.0;
  while (std::fgets(line, sizeof(line), f)) {
    char k[200] = {};
    double v = 0.0;
    if (std::sscanf(line, "%199[^\t]\t%lf", k, &v) == 2 && key == k) hz = v;
  }
  std::fclose(f);
  return hz;
}

static void WriteCachedHz(const std::string& key, double hz) {
  std::map<std::string, double> all;
  if (FILE* f = _wfopen(RefreshCachePath().c_str(), L"r")) {
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
      char k[200] = {};
      double v = 0.0;
      if (std::sscanf(line, "%199[^\t]\t%lf", k, &v) == 2) all[k] = v;
    }
    std::fclose(f);
  }
  all[key] = hz;
  if (FILE* f = _wfopen(RefreshCachePath().c_str(), L"w")) {
    for (const auto& [k, v] : all) std::fprintf(f, "%s\t%.6f\n", k.c_str(), v);
    std::fclose(f);
  }
}

// Datos simples clave=valor en la carpeta de datos (por ejemplo, lo aprendido del equipo).
static std::string ReadKv(const std::wstring& file, const std::string& key) {
  FILE* f = _wfopen((DataDirectory() + L"\\" + file).c_str(), L"r");
  if (!f) return {};
  char line[512];
  std::string value;
  while (std::fgets(line, sizeof(line), f)) {
    std::string l = line;
    while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
    size_t tab = l.find('\t');
    if (tab != std::string::npos && l.substr(0, tab) == key) value = l.substr(tab + 1);
  }
  std::fclose(f);
  return value;
}

static void WriteKv(const std::wstring& file, const std::string& key, const std::string& value) {
  std::wstring path = DataDirectory() + L"\\" + file;
  std::map<std::string, std::string> all;
  if (FILE* f = _wfopen(path.c_str(), L"r")) {
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
      std::string l = line;
      while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
      size_t tab = l.find('\t');
      if (tab != std::string::npos) all[l.substr(0, tab)] = l.substr(tab + 1);
    }
    std::fclose(f);
  }
  all[key] = value;
  if (FILE* f = _wfopen(path.c_str(), L"w")) {
    for (const auto& [k, v] : all) std::fprintf(f, "%s\t%s\n", k.c_str(), v.c_str());
    std::fclose(f);
  }
}

static double ReadCachedHzForKey(const std::string& key) { return ReadCachedHz(key); }

static double ReliableRefresh(IDXGIOutput* out, const std::string& key, double nominal,
                              double signal, bool* cleanOut, int attempts = 3) {
  double ref = signal > 0 ? signal : nominal;
  if (cleanOut) *cleanOut = false;
  for (int attempt = 1; attempt <= attempts; ++attempt) {
    RefreshResult rr = MeasureRefresh(out, cfg::kRefreshMeasureSec);
    bool clean = rr.ok && rr.missed <= 2 && rr.jitterUs < 500.0;
    bool coherent = ref <= 0 || (rr.ok && std::fabs(rr.hz - ref) / ref < 0.002);
    Log("  Intento %d: %.3f Hz | perdidos: %d | dispersion: %.0f us%s\n", attempt, rr.hz, rr.missed,
        rr.jitterUs, clean && coherent ? "  -> valida" : "  -> ruidosa, repito");
    if (clean && coherent) {
      WriteCachedHz(key, rr.hz);
      if (cleanOut) *cleanOut = true;
      return rr.hz;
    }
  }
  if (attempts < 3) return 0.0;  // reintento silencioso: el llamador decide
  double cached = ReadCachedHz(key);
  if (cached > 0 && (ref <= 0 || std::fabs(cached - ref) / ref < 0.002)) {
    Log("  Mediciones ruidosas (el juego u otro programa cargaba el sistema).\n"
        "  Uso la ultima medicion buena guardada: %.3f Hz\n", cached);
    return cached;
  }
  Log("  Mediciones ruidosas. Uso el valor de Windows: %.3f Hz\n", ref);
  return ref;
}


// ---------------------------------------------------------------------------
// Overlay: ventana propia, transparente y siempre encima, que el raton atraviesa.
// No toca el juego: dibuja con GDI los datos de PresentMon que ya lee la app.
// ---------------------------------------------------------------------------
namespace overlay {
static std::atomic<bool> enabled{true};
// Estado del overlay, para el registro: 0 visible, 1 desactivado, 2 sin juego, 3 el juego no
// esta en primer plano, 4 pocos datos, 5 Windows no pudo dibujarlo.
static std::atomic<int> state{-1};
static std::atomic<unsigned long> lastError{0};

// Estado del limite para el overlay: 0 sin limite, 1 aplicado pero sin confirmar, 2 confirmado.
enum class LimitState { None = 0, Pending = 1, Locked = 2 };

struct Feed {
  std::mutex m;
  DWORD pid = 0;                      // juego que se muestra (0 = ninguno)
  // (momento, ms) de fotogramas reales. El momento es una linea de tiempo continua: la suma de
  // las duraciones de los fotogramas, anclada al reloj real. Los datos de PresentMon llegan por
  // tandas; con esta linea de tiempo la grafica puede desplazarse de forma continua.
  std::deque<std::pair<double, double>> real;
  std::deque<double> presents;        // momentos de llegada de todo lo presentado
  double lastVt = 0.0;                // ultimo momento de la linea de tiempo continua
  double targetMs = 0.0;              // periodo objetivo de los fotogramas reales
  std::string limitLabel;             // "225 fps", "122 en pantalla"...
  LimitState limitState = LimitState::None;
  double baseSwingMs = -1.0;          // variacion del ritmo de este juego sin limite ("antes")
  double gameLowFps = 0.0;            // 1 % bajo causado por el juego con un limite activo (0 = no)
  // Bufer de reproduccion: sigue al mayor intervalo reciente entre tandas de datos de PresentMon,
  // para que el borde derecho de la grafica nunca quede vacio.
  double lagSec = 1.1;
  double lagUpdated = 0.0;
  double lastArrival = 0.0;
  // Reloj de reproduccion de la grafica (como un reproductor de video con bufer): avanza a
  // velocidad real y se corrige suavemente hacia "dato mas reciente - bufer".
  double playT = 0.0, playNow = 0.0;
  bool playInit = false;
  // Referencia sin limite: mediana de los ultimos 10 s, suavizada (no salta de un dibujo a otro).
  double refAuto = 0.0, refNow = 0.0;
  unsigned generation = 0;  // cambia con cada juego (para olvidar clasificaciones de tramos)
};
static Feed feed;

// Llamado por el lector de PresentMon con cada fotograma.
static void Push(const FrameSample& s) {
  std::lock_guard<std::mutex> lock(feed.m);
  if (s.pid == 0 || s.pid != feed.pid) return;
  feed.presents.push_back(s.t);
  if (!s.generated && s.realMs > 0) {
    // Una tanda nueva de datos: su intervalo con la anterior marca el retraso de dibujo.
    if (s.t - feed.lastArrival > 0.01) {
      double gap = feed.lastArrival > 0 ? s.t - feed.lastArrival : 1.0;
      double decayed = feed.lagSec - 0.05 * std::max(0.0, s.t - feed.lagUpdated);  // baja despacio
      feed.lagSec = std::clamp(std::max(gap + 0.35, decayed), 0.4, 4.0);
      feed.lagUpdated = s.t;
    }
    feed.lastArrival = s.t;
    // La grafica muestra el intervalo entre presentaciones de los fotogramas reales: el ritmo que
    // regula el limitador (comparable a lo que muestran los limitadores que se inyectan en el juego).
    // Instante del fotograma: el anterior + su duracion. Se fija una vez, al llegar, y no cambia:
    // la linea de tiempo es continua y estable (una tanda nunca se amontona ni mueve lo anterior).
    const double ms = s.presentRealMs > 0 ? s.presentRealMs : s.realMs;
    double vt = feed.lastVt > 0 ? feed.lastVt + ms / 1000.0 : s.t;
    feed.lastVt = vt;
    feed.real.push_back({vt, ms});
  }
  while (feed.presents.size() > 8000) feed.presents.pop_front();
  while (feed.real.size() > 4000) feed.real.pop_front();
}

static DWORD lastGamePid = 0;
#include "src/core/pace_basics.h"

// Reloj de reproduccion (con feed.m tomado): avanza a velocidad real hacia "dato mas reciente,
// extrapolado al reloj actual, menos el bufer", con una correccion suave (entre 0,8x y 1,25x).
// Asi la grafica se desplaza a velocidad constante aunque los datos lleguen por tandas.
static double GraphPlaybackLocked(double now) {
  if (feed.real.empty()) return now;
  const double head = feed.real.back().first;
  const double target = head + std::max(0.0, now - feed.lastArrival) - feed.lagSec;
  if (!feed.playInit || std::fabs(target - feed.playT) > 3.0) {
    feed.playT = target;
    feed.playInit = true;
  } else if (now > feed.playNow) {
    const double dt = std::min(now - feed.playNow, 0.25);
    double rate = std::clamp(1.0 + 0.5 * (target - feed.playT), 0.8, 1.25);
    // Si una tanda se retrasa, frenar suavemente al acercarse al ultimo dato (en vez de pararse
    // en seco al alcanzarlo).
    const double ahead = head - feed.playT;
    if (ahead < 0.4) rate *= std::max(0.0, ahead / 0.4);
    feed.playT += dt * rate;
  }
  feed.playNow = std::max(feed.playNow, now);
  feed.playT = std::min(feed.playT, head);  // nunca por delante de los datos (pausa: se detiene)
  return feed.playT;
}

// Referencia sin limite (con feed.m tomado): mediana de los ultimos 10 s, suavizada en ~1,5 s.
static double GraphAutoRefLocked(double now, double display) {
  std::vector<double> v;
  for (auto it = feed.real.rbegin(); it != feed.real.rend() && it->first >= display - 10.0; ++it)
    if (it->first <= display) v.push_back(it->second);
  if (v.empty()) return feed.refAuto > 0 ? feed.refAuto : 16.7;
  std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
  const double m = v[v.size() / 2];
  if (feed.refAuto <= 0) feed.refAuto = m;
  else if (now > feed.refNow) feed.refAuto += (m - feed.refAuto) * std::min(1.0, (now - feed.refNow) / 1.5);
  feed.refNow = std::max(feed.refNow, now);
  return feed.refAuto;
}

#include "src/core/pace_series.h"

static void SetGameLow(double fps) {
  std::lock_guard<std::mutex> lock(feed.m);
  feed.gameLowFps = fps;
}

static void SetGame(DWORD pid, double targetMs, const std::string& limitLabel, LimitState state,
                    double baseSwingMs = -1.0) {
  std::lock_guard<std::mutex> lock(feed.m);
  // Solo se borran los datos al cambiar de juego: asi la ventana sigue mostrando la
  // ultima grafica mientras miras la app.
  if (pid != 0 && pid != lastGamePid) {
    feed.real.clear();
    feed.presents.clear();
    feed.lastVt = 0.0;
    feed.playInit = false;
    feed.refAuto = 0.0;
    feed.generation++;
    lastGamePid = pid;
  }
  feed.pid = pid;
  if (pid != 0 || targetMs > 0) feed.targetMs = targetMs;
  feed.limitLabel = limitLabel;
  feed.limitState = state;
  feed.baseSwingMs = baseSwingMs;
}

class Window {
 public:
  void Create(float scale) {
    scale_ = scale;
    W_ = static_cast<int>(420 * scale);
    H_ = static_cast<int>(112 * scale);
    WNDCLASSEXW wc{sizeof(wc), 0, DefWindowProcW, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr,
                   nullptr, nullptr, L"FlatPaceOverlay", nullptr};
    RegisterClassExW(&wc);
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
                                WS_EX_NOACTIVATE,
                            wc.lpszClassName, L"FlatPace overlay", WS_POPUP, 0, 0, W_, H_, nullptr,
                            nullptr, wc.hInstance, nullptr);
    HDC screen = GetDC(nullptr);
    dc_ = CreateCompatibleDC(screen);
    maskDc_ = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = W_;
    bi.bmiHeader.biHeight = -H_;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bmp_ = CreateDIBSection(dc_, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&px_), nullptr, 0);
    maskBmp_ = CreateDIBSection(maskDc_, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&mask_), nullptr, 0);
    SelectObject(dc_, bmp_);
    SelectObject(maskDc_, maskBmp_);
    big_ = CreateFontW(static_cast<int>(-22 * scale), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                       ANTIALIASED_QUALITY, 0, L"Segoe UI");
    small_ = CreateFontW(static_cast<int>(-12 * scale), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                         ANTIALIASED_QUALITY, 0, L"Segoe UI");
  }

  void Destroy() {
    if (hwnd_) DestroyWindow(hwnd_);
    if (big_) DeleteObject(big_);
    if (small_) DeleteObject(small_);
    if (bmp_) DeleteObject(bmp_);
    if (maskBmp_) DeleteObject(maskBmp_);
    if (dc_) DeleteDC(dc_);
    if (maskDc_) DeleteDC(maskDc_);
    hwnd_ = nullptr;
  }

  bool Visible() const { return visible_; }

  // Se llama desde el bucle de la ventana principal; dibuja hasta 60 veces por segundo.
  void Update() {
    if (!hwnd_) return;
    double now = NowSec();
    if (now - lastDraw_ < 0.0155) return;
    lastDraw_ = now;

    DWORD pid;
    double targetMs;
    std::string limitLabel;
    LimitState limitState;
    double baseSwing = -1.0, gameLow = 0.0;
    double display = now, refAuto = 0.0;
    std::vector<std::pair<double, double>> real;
    size_t presents5 = 0;
    {
      std::lock_guard<std::mutex> lock(feed.m);
      pid = feed.pid;
      targetMs = feed.targetMs;
      limitLabel = feed.limitLabel;
      limitState = feed.limitState;
      baseSwing = feed.baseSwingMs;
      gameLow = feed.gameLowFps;
      display = GraphPlaybackLocked(now);
      generation_ = feed.generation;
      refAuto = GraphAutoRefLocked(now, display);
      for (const auto& r : feed.real)
        if (r.first >= display - 11.0) real.push_back(r);
      for (double t : feed.presents)
        if (t >= now - 5.0) presents5++;
    }
    HWND fg = GetForegroundWindow();
    DWORD fgPid = 0;
    if (fg) GetWindowThreadProcessId(fg, &fgPid);
    bool show = enabled && pid != 0 && fgPid == pid && real.size() >= 10;
    state = !enabled ? 1 : pid == 0 ? 2 : fgPid != pid ? 3 : real.size() < 10 ? 4 : 0;
    if (!show) {
      if (visible_) { ShowWindow(hwnd_, SW_HIDE); visible_ = false; }
      return;
    }

    // Estadisticas de los ultimos 5 s
    std::vector<double> ms;
    double sum = 0;
    for (size_t i = real.size(); i-- > 0 && sum < 5000.0;) {
      ms.push_back(real[i].second);
      sum += real[i].second;
    }
    if (ms.empty()) return;
    double fps = 1000.0 * ms.size() / sum;
    std::vector<double> sorted = ms;
    std::sort(sorted.begin(), sorted.end());
    double low = 1000.0 / PercentileSorted(sorted, 99);
    if (gameLow > 0) low = gameLow;  // con un limite activo, sin los retrasos del limitador
    double screenFps = presents5 / 5.0;
    bool fg2 = screenFps > fps * 1.4;

    double swing = RecentSwing(real);
    Draw(display, fps, low, fg2 ? screenFps : 0.0, limitLabel, limitState, targetMs > 0 ? targetMs : refAuto, targetMs > 0,
         real, swing, baseSwing);

    // Esquina superior izquierda de la pantalla del juego
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTOPRIMARY), &mi);
    POINT pos{mi.rcMonitor.left + static_cast<LONG>(16 * scale_), mi.rcMonitor.top + static_cast<LONG>(16 * scale_)};
    SIZE size{W_, H_};
    POINT src{0, 0};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    if (!UpdateLayeredWindow(hwnd_, nullptr, &pos, &size, dc_, &src, 0, &bf, ULW_ALPHA)) {
      state = 5;
      lastError = GetLastError();
    }
    if (!visible_) {
      ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
      visible_ = true;
    }
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  }

 private:
  // Pixel con alfa premultiplicado (formato BGRA)
  void Blend(int x, int y, uint8_t r, uint8_t g, uint8_t b, float a) {
    if (x < 0 || y < 0 || x >= W_ || y >= H_ || a <= 0) return;
    if (a > 1.0f) a = 1.0f;
    uint32_t& p = px_[y * W_ + x];
    float ia = 1.0f - a;
    uint8_t pb = p & 0xFF, pg = (p >> 8) & 0xFF, pr = (p >> 16) & 0xFF, pa = (p >> 24) & 0xFF;
    uint8_t nb = static_cast<uint8_t>(b * a + pb * ia);
    uint8_t ng = static_cast<uint8_t>(g * a + pg * ia);
    uint8_t nr = static_cast<uint8_t>(r * a + pr * ia);
    uint8_t na = static_cast<uint8_t>(255 * a + pa * ia);
    p = nb | (ng << 8) | (nr << 16) | (static_cast<uint32_t>(na) << 24);
  }

  void Rect(int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b, float a) {
    for (int y = y0; y < y1; ++y)
      for (int x = x0; x < x1; ++x) Blend(x, y, r, g, b, a);
  }

  // Rectangulo con esquinas redondeadas y bordes suavizados.
  void RoundRect(float x0, float y0, float x1, float y1, float rad, uint8_t r, uint8_t g, uint8_t b, float a) {
    for (int y = static_cast<int>(y0); y < static_cast<int>(std::ceil(y1)); ++y)
      for (int x = static_cast<int>(x0); x < static_cast<int>(std::ceil(x1)); ++x) {
        float px = x + 0.5f, py = y + 0.5f;
        float cx = std::clamp(px, x0 + rad, x1 - rad), cy = std::clamp(py, y0 + rad, y1 - rad);
        float d = std::hypot(px - cx, py - cy);
        float cov = std::clamp(rad - d + 0.5f, 0.0f, 1.0f);
        if (cov > 0) Blend(x, y, r, g, b, a * cov);
      }
  }

  // Linea con bordes suavizados (cobertura por distancia al segmento), recortada al area clip_.
  void LineAA(float x0, float y0, float x1, float y1, uint8_t r, uint8_t g, uint8_t b, float w, float a = 1.0f) {
    float hw = w * 0.5f;
    int minX = std::max(static_cast<int>(std::floor(std::min(x0, x1) - hw - 1)), clipX0_);
    int maxX = std::min(static_cast<int>(std::ceil(std::max(x0, x1) + hw + 1)), clipX1_ - 1);
    int minY = std::max(static_cast<int>(std::floor(std::min(y0, y1) - hw - 1)), clipY0_);
    int maxY = std::min(static_cast<int>(std::ceil(std::max(y0, y1) + hw + 1)), clipY1_ - 1);
    float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
    for (int y = minY; y <= maxY; ++y)
      for (int x = minX; x <= maxX; ++x) {
        float px = x + 0.5f - x0, py = y + 0.5f - y0;
        float t = len2 > 0 ? std::clamp((px * dx + py * dy) / len2, 0.0f, 1.0f) : 0.0f;
        float d = std::hypot(px - t * dx, py - t * dy);
        float cov = std::clamp(hw + 0.5f - d, 0.0f, 1.0f);
        if (cov > 0) Blend(x, y, r, g, b, a * cov);
      }
  }

  // Relleno degradado bajo la curva, columna a columna, hasta yBottom. El degradado es fijo
  // respecto a la grafica (yTop..yBottom), asi bajo un pico no aparece una franja distinta.
  void FillUnder(float xa, float ya, float xb, float yb, float yTop, float yBottom, uint8_t r, uint8_t g,
                 uint8_t b) {
    int from = std::max(static_cast<int>(std::ceil(xa)), clipX0_);
    int to = std::min(static_cast<int>(std::floor(xb)), clipX1_ - 1);
    for (int x = from; x <= to; ++x) {
      float t = xb > xa ? (x - xa) / (xb - xa) : 0.0f;
      float yl = ya + (yb - ya) * t;
      if (yBottom - yl <= 0) continue;
      const float full = std::max(1.0f, yBottom - yTop);
      for (int y = std::max(static_cast<int>(yl) + 1, clipY0_); y < std::min(static_cast<int>(yBottom), clipY1_); ++y)
        Blend(x, y, r, g, b, 0.04f + 0.36f * std::clamp((yBottom - y) / full, 0.0f, 1.0f));
    }
  }

  int TextWidth(const std::string& utf8, HFONT font) {
    SelectObject(maskDc_, font);
    std::wstring w = Widen(utf8);
    SIZE sz{};
    GetTextExtentPoint32W(maskDc_, w.c_str(), static_cast<int>(w.size()), &sz);
    return sz.cx;
  }

  // Texto: se dibuja en blanco sobre negro en la mascara (solo en su rectangulo) y se mezcla
  // con su luminancia.
  void Text(const std::string& utf8, int x, int y, HFONT font, uint8_t r, uint8_t g, uint8_t b) {
    SelectObject(maskDc_, font);
    std::wstring w = Widen(utf8);
    SIZE sz{};
    GetTextExtentPoint32W(maskDc_, w.c_str(), static_cast<int>(w.size()), &sz);
    int rx0 = std::max(0, x - 2), ry0 = std::max(0, y - 2);
    int rx1 = std::min(W_, x + static_cast<int>(sz.cx) + 4), ry1 = std::min(H_, y + static_cast<int>(sz.cy) + 4);
    if (rx1 <= rx0 || ry1 <= ry0) return;
    for (int yy = ry0; yy < ry1; ++yy) std::memset(mask_ + yy * W_ + rx0, 0, static_cast<size_t>(rx1 - rx0) * 4);
    SetBkMode(maskDc_, TRANSPARENT);
    SetTextColor(maskDc_, RGB(255, 255, 255));
    TextOutW(maskDc_, x, y, w.c_str(), static_cast<int>(w.size()));
    GdiFlush();
    for (int yy = ry0; yy < ry1; ++yy)
      for (int xx = rx0; xx < rx1; ++xx) {
        uint32_t m = mask_[yy * W_ + xx];
        uint8_t lum = std::max({static_cast<uint8_t>(m & 0xFF), static_cast<uint8_t>((m >> 8) & 0xFF),
                                static_cast<uint8_t>((m >> 16) & 0xFF)});
        if (lum) Blend(xx, yy, r, g, b, lum / 255.0f);
      }
  }

  void Draw(double now, double fps, double low, double screenFps, const std::string& limitLabel,
            LimitState limitState, double refMs, bool isTarget, const std::vector<std::pair<double, double>>& real,
            double swing, double baseSwing) {
    const unsigned generation = generation_;
    std::memset(px_, 0, static_cast<size_t>(W_) * H_ * 4);
    clipX0_ = 0; clipY0_ = 0; clipX1_ = W_; clipY1_ = H_;
    const float s = scale_;
    const double span = kPaceSpanSec;
    // Color de estado: azul con el limite confirmado; rojo sin limite o sin confirmar.
    const bool locked = limitState == LimitState::Locked;
    const uint8_t sr = locked ? 47 : 255, sg = locked ? 168 : 107, sb = locked ? 255 : 107;

    RoundRect(0, 0, static_cast<float>(W_), static_cast<float>(H_), 10 * s, 14, 16, 21, 0.86f);

    // Fila superior: punto de estado, FPS, 1 % bajo y etiqueta del limite
    RoundRect(12 * s, 13 * s, 20 * s, 21 * s, 4 * s, sr, sg, sb, 1.0f);
    std::string fpsText = Fmt("%.0f", fps);
    Text(fpsText, static_cast<int>(26 * s), static_cast<int>(3 * s), big_, 240, 242, 245);
    int fw = TextWidth(fpsText, big_);
    Text("FPS", static_cast<int>(26 * s) + fw + static_cast<int>(4 * s), static_cast<int>(10 * s), small_, 150, 158, 170);
    std::string sub = Fmt(T("1%% bajo %.0f", "1%% low %.0f"), low);
    if (screenFps > 0) sub += Fmt(T("  ·  pantalla %.0f", "  ·  screen %.0f"), screenFps);
    Text(sub, static_cast<int>(26 * s) + fw + static_cast<int>(34 * s), static_cast<int>(10 * s), small_, 150, 158, 170);

    std::string pill = locked ? std::string(T("FIJADO  ", "LOCKED  ")) + limitLabel
                       : limitState == LimitState::Pending ? std::string(T("SIN CONFIRMAR  ", "UNCONFIRMED  ")) + limitLabel
                                                           : std::string(T("SIN LÍMITE", "NO LIMIT"));
    int pw = TextWidth(pill, small_);
    float px1 = W_ - 10 * s, px0 = px1 - pw - 16 * s;
    RoundRect(px0, 7 * s, px1, 27 * s, 10 * s, sr, sg, sb, 0.18f);
    Text(pill, static_cast<int>(px0 + 8 * s), static_cast<int>(9 * s), small_, sr, sg, sb);

    // Grafica
    const float gx0 = 10 * s, gx1 = W_ - 10 * s, gy0 = 34 * s, gy1 = H_ - 10 * s;
    RoundRect(gx0, gy0, gx1, gy1, 6 * s, 26, 30, 38, 0.92f);
    clipX0_ = static_cast<int>(gx0); clipX1_ = static_cast<int>(gx1);
    clipY0_ = static_cast<int>(gy0); clipY1_ = static_cast<int>(gy1);
    const float iy0 = gy0 + 6 * s, iy1 = gy1 - 4 * s;  // margen interior

    const double ref = refMs;  // objetivo, o el ritmo actual suavizado si no hay limite
    const PaceRange pr = PaceScale(ref);
    auto Y = [&](double ms) {
      double t = std::clamp((ms - pr.lo) / (pr.hi - pr.lo), 0.0, 1.0);
      return static_cast<float>(iy1 - t * (iy1 - iy0));
    };
    const double display = now;  // Update ya resta el retraso de dibujo
    auto X = [&](float xf) { return gx0 + xf * (gx1 - gx0); };

    // Rejilla vertical: una linea por segundo, que se desplaza con la grafica
    for (double k = std::ceil(display - span); k <= display; k += 1.0) {
      float x = X(static_cast<float>(1.0 - (display - k) / span));
      LineAA(x, gy0, x, gy1, 255, 255, 255, 1.0f, 0.06f);
    }
    // Referencia: linea discontinua
    const float ry = Y(ref);
    for (float x = gx0; x < gx1; x += 8 * s) LineAA(x, ry, std::min(x + 4 * s, gx1), ry, 255, 255, 255, 1.0f, 0.28f);

    // Curva: mediana de cada tramo (los tramos con tiron, en ambar), relleno degradado bajo ella
    // y banda de tolerancia si el limite esta confirmado.
    const int cols = std::max(2, static_cast<int>((gx1 - gx0) / (3.0f * s)));
    std::vector<PacePoint> pts = BuildTimeSeries(real, display, span, cols, ref, classCache_, generation);
    for (size_t k = 1; k < pts.size(); ++k)
      FillUnder(X(pts[k - 1].x), Y(pts[k - 1].ms), X(pts[k].x), Y(pts[k].ms), gy0, gy1, sr, sg, sb);
    if (locked) {
      const float bTop = Y(ref + PaceTolerance(ref)), bBot = Y(std::max(0.0, ref - PaceTolerance(ref)));
      for (int y = std::max(static_cast<int>(bTop), clipY0_); y < std::min(static_cast<int>(bBot), clipY1_); ++y)
        for (int x = clipX0_; x < clipX1_; ++x) Blend(x, y, sr, sg, sb, 0.12f);
      LineAA(gx0, bTop, gx1, bTop, sr, sg, sb, 1.0f, 0.38f);
      LineAA(gx0, bBot, gx1, bBot, sr, sg, sb, 1.0f, 0.38f);
    }
    for (size_t k = 1; k < pts.size(); ++k) {
      const bool spike = pts[k].spike || pts[k - 1].spike;
      LineAA(X(pts[k - 1].x), Y(pts[k - 1].ms), X(pts[k].x), Y(pts[k].ms), spike ? 245 : sr, spike ? 185 : sg,
             spike ? 66 : sb, 1.8f * s);
    }

    // Etiquetas abajo, sobre el relleno (donde no llega la curva): variacion del ritmo a la
    // izquierda (con la de antes del limite, si se conoce) y la referencia a la derecha.
    const int ly = static_cast<int>(gy1 - 19 * s);
    if (swing >= 0) {
      std::string sw = Fmt(T("variación ±%.1f ms", "swing ±%.1f ms"), swing);
      if (locked && baseSwing > 0) sw += Fmt(T("  ·  antes ±%.1f", "  ·  before ±%.1f"), baseSwing);
      int sw_w = TextWidth(sw, small_);
      RoundRect(gx0 + 4 * s, ly - 1 * s, gx0 + 14 * s + sw_w, ly + 16 * s, 5 * s, 20, 24, 31, 0.85f);
      Text(sw, static_cast<int>(gx0 + 9 * s), ly, small_, 200, 206, 214);
    }
    std::string refText = Fmt(isTarget ? T("objetivo %.1f ms ±%.1f", "target %.1f ms ±%.1f") : T("actual %.1f ms ±%.1f", "current %.1f ms ±%.1f"),
                              ref, PaceTolerance(ref));
    int tw = TextWidth(refText, small_);
    float lx = gx1 - 9 * s - tw;
    RoundRect(lx - 5 * s, ly - 1 * s, gx1 - 4 * s, ly + 16 * s, 5 * s, 20, 24, 31, 0.85f);
    Text(refText, static_cast<int>(lx), ly, small_, 150, 158, 170);
    clipX0_ = 0; clipY0_ = 0; clipX1_ = W_; clipY1_ = H_;
  }

  HWND hwnd_ = nullptr;
  HDC dc_ = nullptr, maskDc_ = nullptr;
  HBITMAP bmp_ = nullptr, maskBmp_ = nullptr;
  uint32_t* px_ = nullptr;
  uint32_t* mask_ = nullptr;
  HFONT big_ = nullptr, small_ = nullptr;
  int W_ = 0, H_ = 0;
  int clipX0_ = 0, clipY0_ = 0, clipX1_ = 0, clipY1_ = 0;
  float scale_ = 1.0f;
  PaceClassCache classCache_;
  unsigned generation_ = 0;
  bool visible_ = false;
  double lastDraw_ = 0.0;
};
}  // namespace overlay

// ---------------------------------------------------------------------------
// Frecuencia de la pantalla: como la Steam Deck, ajustar la pantalla al juego.
// Cambio temporal (no se guarda en la configuracion de Windows) y siempre reversible.
// ---------------------------------------------------------------------------
namespace display {
static std::vector<int> AvailableRates(const std::wstring& device) {
  std::vector<int> rates;
  DEVMODEW cur{};
  cur.dmSize = sizeof(cur);
  if (!EnumDisplaySettingsW(device.c_str(), ENUM_CURRENT_SETTINGS, &cur)) return rates;
  DEVMODEW dm{};
  dm.dmSize = sizeof(dm);
  for (DWORD i = 0; EnumDisplaySettingsExW(device.c_str(), i, &dm, 0); ++i) {
    if (dm.dmPelsWidth == cur.dmPelsWidth && dm.dmPelsHeight == cur.dmPelsHeight &&
        dm.dmBitsPerPel == cur.dmBitsPerPel && dm.dmDisplayFrequency >= 48)
      rates.push_back(static_cast<int>(dm.dmDisplayFrequency));
  }
  std::sort(rates.begin(), rates.end());
  rates.erase(std::unique(rates.begin(), rates.end()), rates.end());
  return rates;
}

static int CurrentRate(const std::wstring& device) {
  DEVMODEW cur{};
  cur.dmSize = sizeof(cur);
  return EnumDisplaySettingsW(device.c_str(), ENUM_CURRENT_SETTINGS, &cur)
             ? static_cast<int>(cur.dmDisplayFrequency) : 0;
}

// Cambio temporal: CDS_FULLSCREEN no lo guarda en el registro de Windows.
static bool SetRate(const std::wstring& device, int hz) {
  DEVMODEW dm{};
  dm.dmSize = sizeof(dm);
  if (!EnumDisplaySettingsW(device.c_str(), ENUM_CURRENT_SETTINGS, &dm)) return false;
  dm.dmDisplayFrequency = static_cast<DWORD>(hz);
  dm.dmFields = DM_DISPLAYFREQUENCY | DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
  bool ok = ChangeDisplaySettingsExW(device.c_str(), &dm, nullptr, CDS_FULLSCREEN, nullptr) ==
            DISP_CHANGE_SUCCESSFUL;
  if (ok) WriteKv(L"equipo.txt", "pantalla-por-restaurar", Narrow(device));
  return ok;
}

// Vuelve a la configuracion guardada de Windows.
static void Restore(const std::wstring& device) {
  ChangeDisplaySettingsExW(device.c_str(), nullptr, nullptr, 0, nullptr);
  WriteKv(L"equipo.txt", "pantalla-por-restaurar", "");
}

// Si la app se cerro con la pantalla cambiada, la devuelve a su estado normal.
static void RestorePending() {
  std::string dev = ReadKv(L"equipo.txt", "pantalla-por-restaurar");
  if (!dev.empty()) Restore(Widen(dev));
}
}  // namespace display

// Elige la mejor frecuencia para un juego: la que permite mas FPS estables. Solo propone
// cambiar si la mejora es clara (al menos un 10 %).
struct RatePlan {
  int rate = 0;      // frecuencia a usar (0 = la actual)
  double hz = 0.0;   // frecuencia efectiva para los calculos
  Recommendation rec;
};

template <typename Stats>
static RatePlan BestRate(const Stats& st, double currentHz, int currentRate,
                         const std::vector<int>& rates, const std::string& monitorKeyBase) {
  RatePlan best;
  best.hz = currentHz;
  best.rec = RecommendFor(st, currentHz, cfg::kMarginInitial);
  double bestFps = best.rec.valid ? best.rec.capFps : 0.0;
  for (int r : rates) {
    if (r == currentRate) continue;
    double cached = ReadCachedHzForKey(monitorKeyBase + Fmt("@%d", r));
    double h = cached > 0 ? cached : static_cast<double>(r);
    Recommendation rec = RecommendFor(st, h, cfg::kMarginInitial);
    if (rec.valid && !rec.tight && rec.capFps > bestFps * 1.10) {
      best.rate = r;
      best.hz = h;
      best.rec = rec;
      bestFps = rec.capFps;
    }
  }
  return best;
}

// Recomendacion con TODA la partida: el estado de generacion de fotogramas dominante, la
// prioridad elegida y G-SYNC si actuo la mayor parte del tiempo.
static Recommendation SessionRec(const Session& ss, double hz) {
  Recommendation none;
  int k = 1;
  size_t best = 0;
  for (const auto& [kk, v] : ss.sessionReal)
    if (v.size() > best) { best = v.size(); k = kk; }
  if (best < 600) return none;
  const auto& vf = ss.sessionReal.at(k);
  std::vector<double> d(vf.begin(), vf.end());
  std::sort(d.begin(), d.end());
  bool vrr = ss.gameSecs > 0 && ss.vrrSecs > ss.gameSecs * 0.5;
  bool fluid = g_priority == 1;
  double p99 = PercentileSorted(d, 99), p10 = PercentileSorted(d, 10), p90 = PercentileSorted(d, 90);
  double med = PercentileSorted(d, 50);
  // Si casi todos los fotogramas duran lo mismo, el juego estaba limitado (su VSync o su
  // propio limite): ese ritmo esta demostrado y no se puede recomendar menos.
  double held = (p10 > 0 && p90 / p10 < 1.06) ? 1000.0 / med : 0.0;
  // Si el detector vio al juego limitado (su VSync o su limite) la mayor parte de la
  // partida, ese ritmo esta demostrado.
  if (ss.gameSecs > 0 && ss.ownCapSecs > ss.gameSecs * 0.5)
    held = std::max(held, ss.ownCapRateSum / ss.ownCapSecs);
  if (vrr) {
    Recommendation vr = RecommendVrr(d, p99, hz, k, fluid, held);
    if (vr.valid) return vr;  // si no, (por debajo del rango VRR sin LFC) un divisor
  }
  Recommendation r = fluid ? RecommendFluid(d, hz, k) : Recommendation{};
  if (!r.valid) r = Recommend(hz, p99, cfg::kMarginInitial, k);
  return r;
}

// ---------------------------------------------------------------------------
// Motor: medicion y aplicacion de limites en un hilo aparte; publica su estado para la ventana
// ---------------------------------------------------------------------------
static void SetStatus(UiStatus s, const std::string& text, const std::string& detail) {
  std::lock_guard<std::mutex> lock(g_ui.m);
  g_ui.status = s;
  g_ui.statusText = text;
  g_ui.statusDetail = detail;
}

static std::map<std::string, std::string> g_gameLabels;  // .exe -> nombre visible (solo hilo del motor)

static void PublishChanges(const ChangeStore& store) {
  std::lock_guard<std::mutex> lock(g_ui.m);
  g_ui.changes.clear();
  for (const auto& [exe, c] : store.items) {
    UiChange u;
    u.exe = exe;
    auto lb = g_gameLabels.find(exe);
    if (lb == g_gameLabels.end()) {
      std::string saved = ReadKv(L"equipo.txt", "nombre|" + exe);
      lb = g_gameLabels.emplace(exe, saved.empty() ? exe : saved).first;
    }
    u.label = lb->second;
    u.desc = DescribeChange(c);
    u.value = static_cast<int>(c.newValue);
    u.fg = std::max(1, c.fgFactor);
    u.manual = c.manual;
    auto w = g_ui.warnings.find(exe);
    if (w != g_ui.warnings.end()) u.warning = w->second;
    g_ui.changes.push_back(u);
  }
}

static void SetWarning(const ChangeStore& store, const std::string& exe, const std::string& warning) {
  {
    std::lock_guard<std::mutex> lock(g_ui.m);
    auto it = g_ui.warnings.find(exe);
    if ((it == g_ui.warnings.end() ? std::string() : it->second) == warning) return;
    if (warning.empty()) g_ui.warnings.erase(exe);
    else g_ui.warnings[exe] = warning;
  }
  PublishChanges(store);
}


// Deshace todos los cambios. Devuelve un resumen legible.
static std::string UndoAll(NvDriver& nvd, ChangeStore& store, bool nvOk) {
  if (store.items.empty()) return T("No hay cambios de FlatPace que deshacer.", "There are no FlatPace changes to undo.");
  if (!nvOk) return T("No puedo acceder al driver de NVIDIA (", "I can't access the NVIDIA driver (") + nvd.error + ").";
  int done = 0, fails = 0;
  for (auto it = store.items.begin(); it != store.items.end();) {
    if (nvd.Revert(it->second)) {
      Log("  Deshecho: %s (vuelve a como estaba antes)\n", it->first.c_str());
      it = store.items.erase(it);
      ++done;
    } else {
      Log("  No pude deshacer %s: %s\n", it->first.c_str(), nvd.error.c_str());
      ++fails;
      ++it;
    }
  }
  store.Save();
  if (fails) return Fmt(T("Deshechos %d cambios; %d no se pudieron deshacer (ver registro).", "Undid %d changes; %d could not be undone (see log)."), done, fails);
  return Fmt(T("Listo: %d cambio%s deshecho%s. Si un juego estaba abierto, reinícialo.", "Done: %d change%s undone%s. If a game was open, restart it."), done,
             done == 1 ? "" : "s", done == 1 ? "" : "s");
}

static void PublishSession(const Session& ss, const ChangeStore& store, bool apply,
                           const std::string& manualExe) {
  std::lock_guard<std::mutex> lock(g_ui.m);
  g_ui.haveGame = ss.pid != 0;
  if (!g_ui.haveGame) {
    g_ui.status = UiStatus::Searching;
    g_ui.statusText = manualExe.empty() ? T("Buscando juego", "Looking for a game") : T("Esperando a ", "Waiting for ") + manualExe;
    g_ui.statusDetail = manualExe.empty()
                            ? T("Abre un juego y ponlo en primer plano. Lo detecto solo.", "Open a game and bring it to the front. I detect it automatically.")
                            : T("Abre ese juego. Para volver a la detección automática, usa «Automático».", "Open that game. To go back to automatic detection, use “Automatic”.");
    return;
  }
  const WindowStats& st = ss.lastStats;
  g_ui.game = ss.label;
  g_ui.exe = ss.exe;
  {
    std::string m = ss.MainPresentMode();
    g_ui.presentText = m.empty() ? std::string("—")
                       : ss.IsComposed() ? std::string(T("por el compositor (sin tearing)", "through the compositor (no tearing)"))
                                         : std::string(T("directa (tearing posible sin VSync)", "direct (tearing possible without VSync)"));
  }
  bool fg = st.GeneratedFraction() > cfg::kFgOnFraction;
  g_ui.realFps = st.medianMs > 0 ? 1000.0 / st.medianMs : 0.0;
  g_ui.steadyPct = 100.0 * st.steady2;
  g_ui.jitterMs = st.jitterMs;
  g_ui.lowFps = st.p99Ms > 0 ? 1000.0 / st.p99Ms : 0.0;
  if (const double gl = GameLowFps(ss); gl > 0) g_ui.lowFps = gl;
  overlay::SetGameLow(GameLowFps(ss));
  g_ui.screenFps = fg ? st.presentedFps : 0.0;
  g_ui.hitches = ss.hitchTotal;
  g_ui.capText = st.capped ? Fmt(T("~%.0f fps (%s)", "~%.0f fps (%s)"), st.capFps, st.capFits ? T("encaja", "fits") : T("no encaja", "does not fit"))
                           : std::string(T("ninguno", "none"));
  g_ui.bottleneck = st.frames ? st.Bottleneck() : std::string("—");
  if (ss.accepted || (ss.expectedFps > 0 && ss.verified))
    g_ui.recText = T("— (límite aceptado; se recalcula si lo deshaces)", "— (limit accepted; recalculated if you undo it)");
  else if (ss.haveRec)
    g_ui.recText = !ss.current.valid ? std::string(T("ninguno (no sostiene 30 fps reales)", "none (can't hold 30 real fps)"))
                   : ss.current.fgFactor > 1
                       ? Fmt(T("%.0f fps en pantalla (%.0f reales)%s", "%.0f fps on screen (%.0f real)%s"), ss.current.capFps, ss.current.realFps,
                             ss.current.tight ? T(" · ajustado", " · tight") : "")
                       : Fmt("%.0f fps%s%s", ss.current.capFps, ss.current.tight ? T(" · ajustado", " · tight") : "",
                             ss.current.vrr ? " · G-SYNC" : "");
  else
    g_ui.recText = T("midiendo…", "measuring…");
  auto known = store.items.find(ss.exe);
  g_ui.appliedText = known != store.items.end() ? DescribeChange(known->second)
                                                : std::string(T("ninguno", "none"));

  double exp = ss.expectedFps * std::max(1, ss.expectFg);  // ritmo en pantalla
  if (!ss.seenForeground) {
    g_ui.status = UiStatus::Paused;
    g_ui.statusText = T("Juego detectado: ", "Game detected: ") + ss.label;
    g_ui.statusDetail = T("Cambia al juego para empezar a medir. Mientras está detrás no mido, "
                        "porque muchos juegos bajan el ritmo sin el foco.", "Switch to the game to start measuring. While it's in the background I don't measure, "
                        "because many games slow down without focus.");
  } else if (ss.background) {
    g_ui.status = UiStatus::Paused;
    g_ui.statusText = T("En pausa", "Paused");
    g_ui.statusDetail = T("El juego está en segundo plano. Vuelve a él para seguir midiendo.", "The game is in the background. Go back to it to keep measuring.");
  } else if (ss.paused) {
    g_ui.status = UiStatus::Paused;
    g_ui.statusText = T("En pausa", "Paused");
    g_ui.statusDetail = T("El juego no muestra imágenes (minimizado o cargando).", "The game isn't showing frames (minimized or loading).");
  } else if (exp && ss.verified) {
    g_ui.status = UiStatus::Applied;
    g_ui.statusText = Fmt(T("Límite funcionando: %.0f fps", "Limit working: %.0f fps"), exp);
    g_ui.statusDetail = T("Límite aceptado: no lo cambio mientras no lo deshagas en Juegos.",
                          "Limit accepted: I won't change it unless you undo it in Games.");
  } else if (exp && ss.restartNoted) {
    g_ui.status = UiStatus::Restart;
    g_ui.statusText = T("Reinicia el juego", "Restart the game");
    g_ui.statusDetail = Fmt(T("El driver aplica el cambio (%.0f fps) al abrir el juego. Ciérralo y "
                            "ábrelo de nuevo; yo lo compruebo solo.", "The driver applies the change (%.0f fps) when the game starts. Close it and "
                            "open it again; I'll check it automatically."), exp);
  } else if (exp) {
    g_ui.status = UiStatus::Measuring;
    g_ui.statusText = Fmt(T("Comprobando el límite de %.0f fps…", "Checking the %.0f fps limit…"), exp);
    g_ui.statusDetail = T("Unos segundos para confirmar que el driver lo está aplicando.", "A few seconds to confirm the driver is applying it.");
  } else if (!ss.haveRec) {
    g_ui.status = UiStatus::Measuring;
    g_ui.statusText = T("Midiendo…", "Measuring…");
    g_ui.statusDetail = T("Unos segundos para conocer cómo se comporta el juego.", "A few seconds to learn how the game behaves.");
  } else if (ss.ownCapNoted) {
    g_ui.status = UiStatus::Applied;
    g_ui.statusText = T("Todo en orden", "All good");
    g_ui.statusDetail = Fmt(T("El juego ya tiene un límite que encaja con tu pantalla (~%.0f fps).", "The game already has a limit that fits your display (~%.0f fps)."),
                            st.capFps);
  } else if (!ss.current.valid) {
    g_ui.status = UiStatus::Restart;
    g_ui.statusText = T("El juego va muy justo", "The game is running too close to the edge");
    g_ui.statusDetail = T("No sostiene ni 30 fps estables. Baja ajustes gráficos o usa DLSS/FSR.", "It can't hold even 30 steady fps. Lower graphics settings or use DLSS/FSR.");
  } else if (ss.noTargetNoted) {
    g_ui.status = UiStatus::Measuring;
    g_ui.statusText = Fmt(T("Recomendado: %.0f fps", "Recommended: %.0f fps"), ss.current.capFps);
    g_ui.statusDetail = T("El driver de NVIDIA solo admite números enteros y no puede fijarlo exacto.", "The NVIDIA driver only accepts whole numbers and can't set it exactly.");
  } else {
    g_ui.status = UiStatus::Measuring;
    g_ui.statusText = Fmt(T("Recomendado: %.0f fps", "Recommended: %.0f fps"), ss.current.capFps);
    g_ui.statusDetail = apply ? T("Lo decido con toda la partida al cerrar el juego, para la próxima vez. "
                                  "Si lo quieres ya, pulsa «Aplicar ya» y reinicia el juego.",
                                  "I'll decide it with the whole session when you close the game, for next time. "
                                  "If you want it now, press “Apply now” and restart the game.")
                              : T("Modo solo medir: no cambio nada.", "Measure-only mode: I don't change anything.");
  }
}

static void RunEngine(std::wstring presentMonPath) {
  std::wstring logPath = OpenLog();
  {
    std::lock_guard<std::mutex> lock(g_ui.m);
    g_ui.logPath = logPath;
  }
  if (g_log) std::fprintf(g_log, "FlatPace %s\n\n", kVersion);
  SetStatus(UiStatus::Starting, T("Iniciando…", "Starting…"), T("Revisando tu equipo y midiendo la pantalla.", "Checking your system and measuring the display."));

  NvDriver nvd;
  bool nvOk = nvd.Init();
  ChangeStore store(DataDirectory() + L"\\cambios-aplicados.txt");
  store.Load();
  if (int moved = store.Migrate(ExeDirectory() + L"\\cambios-aplicados.txt"))
    Log("Cambios de una version anterior movidos a la carpeta de datos: %d\n", moved);
  if (ReadKv(L"equipo.txt", "rendimiento-maximo") == "no") g_maxPerf = false;
  if (nvOk && g_maxPerf) {
    int added = 0;
    for (auto& [exe, c] : store.items) {
      bool has = false;
      for (const auto& sv : c.saved) has = has || sv.id == nv::PREFERRED_PSTATE_ID;
      if (!has && nvd.AddSetting(c, nv::PREFERRED_PSTATE_ID, nv::PSTATE_PREFER_MAX)) ++added;
    }
    if (added > 0) {
      store.Save();
      Log("Rendimiento maximo de la GPU anadido a %d juego%s con limite (actua al abrir el juego).\n", added,
          added == 1 ? "" : "s");
    }
  }
  PublishChanges(store);

  // 1. Hardware ------------------------------------------------------------
  std::vector<DisplayPathInfo> paths = QueryDisplayPaths();
  std::vector<AdapterInfo> adapters;
  std::vector<OutputInfo> outputs;
  EnumerateDxgi(adapters, outputs);
  bool laptop = HasSystemBattery();

  Log("=== Equipo ===\n  Tipo: %s\n", laptop ? "portatil (bateria detectada)" : "sobremesa");
  bool anyIntegrated = false, dedicatedWithoutOutputs = false;
  std::string gpuText;
  for (const auto& a : adapters) {
    anyIntegrated |= a.integrated;
    if (!a.integrated && a.outputs == 0) dedicatedWithoutOutputs = true;
    Log("  GPU: %s | %zu MB | %s | monitores conectados: %d\n", Narrow(a.name).c_str(), a.vramMB,
        a.integrated ? "integrada (estimado)" : "dedicada", a.outputs);
    if (!a.integrated) gpuText += (gpuText.empty() ? "" : " + ") + Narrow(a.name);
  }
  if (gpuText.empty() && !adapters.empty()) gpuText = Narrow(adapters[0].name);

  Log("\n=== Monitores ===\n");
  const OutputInfo* target = nullptr;
  double nominalHz = 0.0, signalHz = 0.0;
  for (const auto& o : outputs) {
    std::wstring friendly = L"(sin nombre)";
    double hzNom = 0.0, hzSig = 0.0;
    for (const auto& p : paths)
      if (p.gdiName == o.gdiName) {
        if (!p.friendlyName.empty()) friendly = p.friendlyName;
        else if (!p.edidName.empty()) friendly = p.edidName;
        hzNom = p.nominalHz;
        hzSig = p.signalHz;
      }
    Log("  %s \"%s\" %ldx%ld | %.3f Hz (Windows) | senal %.4f Hz | HDR: %s | en: %s%s\n",
        Narrow(o.gdiName).c_str(), Narrow(friendly).c_str(), o.width, o.height, hzNom, hzSig,
        o.hdrOn ? "activado" : "no", Narrow(adapters[o.adapterIndex].name).c_str(),
        o.primary ? " [principal]" : "");
    if (!target || o.primary) { target = &o; nominalHz = hzNom; signalHz = hzSig; }
  }
  if (!target) {
    SetStatus(UiStatus::Error, T("No encontré ninguna pantalla", "No display found"), "");
    return;
  }

  std::string tip;
  const AdapterInfo& shownOn = adapters[target->adapterIndex];
  if (laptop && shownOn.integrated && dedicatedWithoutOutputs) {
    tip = T("Portátil en modo híbrido: la gráfica integrada muestra la imagen. Si tu portátil "
          "tiene un modo «solo GPU dedicada» (MUX, dGPU o Ultimate), actívalo para mejor pacing.", "Hybrid-mode laptop: the integrated GPU drives the display. If your laptop "
          "has a “dedicated GPU only” mode (MUX, dGPU or Ultimate), enable it for better pacing.");
    Log("\n  Portatil en modo hibrido (la integrada muestra la imagen).\n");
  }

  // 2. Hz reales -----------------------------------------------------------
  Log("\n=== Midiendo la frecuencia real (%.0f s) ===\n", cfg::kRefreshMeasureSec);
  std::string monitorKey = Narrow(target->gdiName) + Fmt("@%.0f", nominalHz);
  // Lo aprendido sobre este equipo: si el driver impone su VSync (depende de que grafica
  // muestra la imagen, por ejemplo en portatiles en modo hibrido).
  const std::string machineKey = "vsync-driver|" + Narrow(shownOn.name) + "|" +
                                 (laptop && shownOn.integrated && dedicatedWithoutOutputs ? "hibrido" : "directo");
  std::string vsyncCap = ReadKv(L"equipo.txt", machineKey);  // "", "si" o "no"
  // Con generacion de fotogramas: el limitador del driver actua sobre lo que se ve en
  // pantalla ("salida") o sobre los fotogramas reales ("reales"). Se aprende una vez.
  const std::string frlFgKey = "limite-con-generacion|" + gpuText;
  std::string frlFgMode = ReadKv(L"equipo.txt", frlFgKey);
  bool hzClean = false;
  display::RestorePending();
  double hz = ReliableRefresh(target->output.Get(), monitorKey, nominalHz, signalHz, &hzClean);
  // Frecuencias que ofrece la pantalla, para ajustarla al juego (como la Steam Deck).
  const std::wstring displayDev = target->gdiName;
  const int baseRate = display::CurrentRate(displayDev);
  const double baseHz = hz;
  const std::vector<int> rates = display::AvailableRates(displayDev);
  const std::string monitorKeyBase = Narrow(target->gdiName);
  int switchedRate = 0;  // frecuencia puesta por la app (0 = la normal)
  bool vrrPossible = false, nvVrrKnown = false;
  {
    bool vrrNow = false;
    nvVrrKnown = nvOk && nvd.Vrr(Narrow(target->gdiName), vrrPossible, vrrNow);
    if (nvVrrKnown)
      Log("  G-SYNC (driver de NVIDIA): %s\n", vrrPossible ? "disponible en esta pantalla" : "no disponible en esta pantalla");
    else
      Log("  VRR: lo detecto por los tiempos de pantalla (sirve para G-SYNC, FreeSync y Adaptive-Sync)\n");
    for (const auto& pth : paths)
      if (pth.gdiName == target->gdiName) {
        if (pth.freeSyncMin > 0) Log("  Rango FreeSync declarado por el monitor: %d-%d Hz\n", pth.freeSyncMin, pth.freeSyncMax);
        if (pth.rangeMin > 0) Log("  Rango de frecuencias declarado por el monitor: %d-%d Hz\n", pth.rangeMin, pth.rangeMax);
        const int vmin = pth.freeSyncMin > 0 ? pth.freeSyncMin : pth.rangeMin;
        const int vmax = pth.freeSyncMin > 0 ? pth.freeSyncMax : pth.rangeMax;
        if (vmin > 0 && vmax > vmin) {
          g_vrrMinHz = vmin;
          g_vrrMaxHz = vmax;
          const bool lfc = vmax >= 2 * vmin;
          Log("  Rango VRR usado: %d-%d Hz%s\n", vmin, vmax,
              lfc ? " (con LFC: por debajo del minimo, el monitor duplica fotogramas)"
                  : " (sin LFC: por debajo del minimo, mejor un divisor)");
        }
        std::lock_guard<std::mutex> lock(g_ui.m);
        g_ui.vrrRange = vmin > 0 && vmax > vmin
                            ? Fmt("%d-%d Hz%s%s", vmin, vmax, pth.freeSyncMin > 0 ? " (FreeSync)" : "",
                                  vmax >= 2 * vmin ? " · LFC" : "")
                            : std::string();
        g_ui.vrrPossible = vrrPossible;
        g_ui.vrrNvKnown = nvVrrKnown;
      }
  }
  double lastVrrPoll = 0.0;
  {
    std::string list;
    for (int r : rates) list += (list.empty() ? "" : ", ") + std::to_string(r);
    Log("  Frecuencias disponibles en esta resolucion: %s Hz\n", list.c_str());
  }
  auto RestoreDisplay = [&]() {
    if (switchedRate == 0) return;
    display::Restore(displayDev);
    Log("  Pantalla devuelta a su frecuencia normal (%d Hz).\n", baseRate);
    switchedRate = 0;
    hz = baseHz;
    std::lock_guard<std::mutex> lock(g_ui.m);
    g_ui.monitor = Fmt(T("%ldx%ld · %.3f Hz reales", "%ldx%ld · %.3f Hz measured"), target->width, target->height, hz);
    g_ui.hz = hz;
  };
  bool hzFromCache = !hzClean && ReadCachedHz(monitorKey) > 0;
  double lastHzRetry = NowSec();
  Log("  Frecuencia real: %.3f Hz (Windows dice %.3f)\n", hz, nominalHz);

  {
    std::lock_guard<std::mutex> lock(g_ui.m);
    g_ui.gpu = gpuText;
    g_ui.monitor = Fmt(T("%ldx%ld · %.3f Hz reales", "%ldx%ld · %.3f Hz measured"), target->width, target->height, hz);
    g_ui.hz = hz;
    g_ui.tip = tip;
    g_ui.driver = nvOk ? std::string(T("NVIDIA: disponible", "NVIDIA: available")) : std::string(T("NVIDIA: no disponible (", "NVIDIA: not available (")) + nvd.error + ")";
    g_ui.driverOk = nvOk;
  }
  Log("\n=== Driver ===\n  %s\n", nvOk ? "NVIDIA: disponible" : ("NVIDIA: no disponible (" + nvd.error + ")").c_str());
  if (!nvOk) g_applyMode = false;
  if (!IsElevated()) Log("  Aviso: sin permisos de administrador; PresentMon y el driver pueden fallar.\n");

  if (presentMonPath.empty()) {
    SetStatus(UiStatus::Error, T("Falta PresentMon", "PresentMon is missing"),
              T("Debe estar en la misma carpeta que FlatPace.exe (viene en el paquete).", "It must be in the same folder as FlatPace.exe (it comes in the package)."));
    Log("\nNo encontre PresentMon.\n");
    return;
  }

  // 3. Juego ---------------------------------------------------------------
  PresentMonReader pm;
  StopEtwSession(kEtwSession);  // por si quedo abierta de una ejecucion anterior
  const double pmStartedAt = NowSec();
  bool pmRestarted = false;
  if (!pm.Start(presentMonPath, std::wstring())) {
    SetStatus(UiStatus::Error, T("No pude iniciar PresentMon", "Couldn't start PresentMon"), Narrow(presentMonPath));
    return;
  }
  Log("\n=== Deteccion automatica del juego ===\n");

  Session session;
  FrameRecorder recorder;
  DWORD candidatePid = 0;
  // Al cerrarse un juego, su proceso puede seguir presentando un instante (o relanzarse para
  // cerrarse): durante unos segundos no se vuelve a detectar el mismo .exe.
  std::string lastClosedExe;
  double lastClosedAt = -1e9;
  int candidateStreak = 0;
  int gamesSeen = 0;
  bool lastApplyMode = g_applyMode;

  // Aplica la recomendacion actual de la sesion (decidir al cerrar el juego o "Aplicar ya").
  auto TryApply = [&](double now, const WindowStats& st, bool decideNow, const char* reason) -> bool {
    bool appliedOk = false;
      // Frecuencia de pantalla: si otra frecuencia del monitor permite claramente mas FPS
      // estables, se planea usarla la proxima vez que se abra el juego.
      Recommendation plan = session.current;
      double planHz = hz;
      int planRate = 0;
      if (!session.verified && switchedRate == 0 && rates.size() > 1 && !st.capped && !session.vrrActive) {
        RatePlan rp = BestRate(st, hz, display::CurrentRate(displayDev), rates, monitorKeyBase);
        if (rp.rate > 0) {
          plan = rp.rec;
          planHz = rp.hz;
          planRate = rp.rate;
        }
      } else if (switchedRate > 0) {
        planRate = switchedRate;
      }
      Recommendation savedCurrent = session.current;
      session.current = plan;
      double savedHz = hz;
      hz = planHz;
      // Decision: VSync del driver (exacto, sin tearing, sirve con cualquier frecuencia)
      // si el equipo lo admite o aun no se sabe; si no, limite de FPS entero.
      int div = 0;
      uint32_t intFps = DriverFps(session.current, hz, &div);
      int n = session.current.valid ? session.current.divisor : 0;
      // El VSync del driver solo aporta si no hay un limite entero posible (pantallas con
      // frecuencias no redondas) o si el juego se muestra de forma directa (puede haber
      // tearing). Si pasa por el compositor, el limite de FPS basta y tiene menos latencia.
      bool modeKnown = !session.MainPresentMode().empty();
      bool vsyncAdds = (intFps == 0 || (modeKnown && !session.IsComposed())) && planRate == 0;
      const int fgK = session.current.valid ? session.current.fgFactor : 1;
      const bool vrrPlan = session.current.valid && session.current.vrr;
      if (vrrPlan) planRate = 0;
      bool useVsync = n >= 1 && n <= 4 && vsyncCap != "no" && vsyncAdds && fgK == 1 && !vrrPlan;
      std::vector<std::pair<uint32_t, uint32_t>> desired;
      Method method = Method::FpsLimit;
      double targetFps = 0.0;
      uint32_t shownFps = 0;
      if (vrrPlan) {
        // G-SYNC: limite entero al ritmo sostenible + VSync del driver activado, la
        // combinacion recomendada para que G-SYNC nunca se salga de su rango.
        uint32_t screen = static_cast<uint32_t>(session.current.capFps);
        uint32_t frl = fgK > 1 && frlFgMode == "reales" ? screen / static_cast<uint32_t>(fgK) : screen;
        targetFps = session.current.realFps;
        shownFps = screen;
        n = 0;
        desired = {{nv::FRL_FPS_ID, frl}, {nv::VSYNCMODE_ID, nv::VSYNC_FORCEON}};
      } else if (useVsync) {
        method = Method::DriverVsync;
        targetFps = hz / n;
        shownFps = static_cast<uint32_t>(std::lround(targetFps));
        desired = {{nv::VSYNCMODE_ID, nv::VsyncForDivisor(n)}, {nv::PRERENDERLIMIT_ID, 1}};
      } else if (fgK > 1) {
        // Con generacion: el ritmo en pantalla debe encajar con los Hz. El valor del
        // limitador depende de sobre que actua el driver (aprendido en este equipo).
        auto whole = [](double f) -> uint32_t {
          double w = std::floor(f + 1e-9);
          return (f > 0 && (f - w) / f < 0.001) ? static_cast<uint32_t>(w) : 0;
        };
        uint32_t frl = whole(frlFgMode == "reales" ? session.current.realFps : session.current.capFps);
        if (frl > 0) {
          targetFps = session.current.realFps;
          shownFps = static_cast<uint32_t>(std::lround(session.current.capFps));
          n = session.current.divisor;
          desired = {{nv::FRL_FPS_ID, frl}};
        }
      } else if (intFps > 0) {
        targetFps = intFps;
        shownFps = intFps;
        n = div;
        desired = {{nv::FRL_FPS_ID, intFps}};
      }
      if (g_maxPerf && !desired.empty()) desired.push_back({nv::PREFERRED_PSTATE_ID, nv::PSTATE_PREFER_MAX});
      uint32_t frlWanted = 0;
      for (const auto& d : desired)
        if (d.first == nv::FRL_FPS_ID) frlWanted = d.second;
      auto known = store.items.find(session.exe);
      // Un limite ya aplicado no se redecide de cero en cada partida:
      // - si se comprobo funcionando, solo cambia cuando la vigilancia decide bajarlo
      //   porque el juego deja de sostenerlo de forma continuada;
      // - si aun no se comprobo, se espera 90 s de partida para comprobarlo;
      // - si la generacion de fotogramas no esta como cuando se aplico, no se toca.
      // El limite aplicado en esta partida aun no actua (hace falta reiniciar el juego):
      // afinarlo no cuesta nada. Se ajusta en silencio si la diferencia es clara.
      const bool refine = session.appliedThisSession;
      bool keepKnown = false;
      // Un limite manual solo cambia si el usuario pide "Aplicar ya" o lo deshace.
      if (known != store.items.end() && known->second.manual && std::strstr(reason, "Aplicar ya") == nullptr) {
        keepKnown = true;
        session.keepNoted = true;
      }
      if (!keepKnown && known != store.items.end()) {
        const AppliedChange& kc = known->second;
        bool fgDiffers = (kc.fgFactor > 1) != (fgK > 1);
        if (kc.verified && !session.lowerPending) keepKnown = true;
        else if (!kc.verified && session.appliedAtStart && now - session.start < 90.0) keepKnown = true;
        else if (fgDiffers) keepKnown = true;
        if (keepKnown && !session.keepNoted && !desired.empty() && !session.appliedThisSession) {
          session.keepNoted = true;
          Log("[%5.0f s] Mantengo el limite ya aplicado (%s); no lo cambio por una medicion puntual.\n",
              session.T(now), DescribeChange(kc).c_str());
        }
      }
      if (keepKnown) desired.clear();
      bool same = known != store.items.end() && known->second.method == method &&
                  known->second.fgFactor == fgK && known->second.displayHz == planRate &&
                  known->second.vrr == vrrPlan &&
                  (method == Method::DriverVsync ? known->second.divisor == n
                                                 : known->second.newValue == shownFps &&
                                                       (known->second.frlValue == 0 ||
                                                        known->second.frlValue == frlWanted));
      if (refine && !same && known != store.items.end() && known->second.newValue > 0) {
        double diff = std::fabs(static_cast<double>(shownFps) - known->second.newValue) / known->second.newValue;
        if (diff < cfg::kJnd && known->second.method == method && known->second.vrr == vrrPlan)
          same = true;  // diferencia pequena: no vale la pena tocarlo
      }
      // Se decide al cerrar el juego (con toda la partida) o cuando el usuario pulsa "Aplicar ya".
      std::string readyWhy = reason;
      bool stable = decideNow;
      bool spaced = decideNow;
      if (desired.empty() && !keepKnown) {
        if (!session.noTargetNoted && stable) {
          session.noTargetNoted = true;
          Log("[%5.0f s] No aplico limite: %s\n", session.T(now),
              session.current.valid ? "tus Hz no tienen un divisor entero y el VSync del driver no funciona aqui."
                                    : "el juego no sostiene ni el limite mas bajo.");
        }
      } else if (!desired.empty() && !same && stable && spaced) {
        if (known == store.items.end() && st.capped && st.capFits && st.capFps <= targetFps + 0.5 &&
            method == Method::FpsLimit) {
          if (!session.ownCapNoted) {
            session.ownCapNoted = true;
            Log("[%5.0f s] El juego ya tiene un limite adecuado (~%.0f fps).\n", session.T(now), st.capFps);
          }
        } else {
          AppliedChange change = known != store.items.end() ? known->second : AppliedChange{};
          if (nvd.ApplySettings(session.exe, desired, change)) {
            change.method = method;
            change.divisor = n;
            change.newValue = shownFps;
            change.fgFactor = fgK;
            change.frlValue = frlWanted;
            change.displayHz = planRate;
            change.verified = false;
            change.vrr = vrrPlan;
            change.manual = false;
            change.basisReal = st.medianMs > 0 ? static_cast<uint32_t>(std::lround(1000.0 / st.medianMs)) : 0;
            session.appliedThisSession = true;
            store.items[session.exe] = change;
            store.Save();
            PublishChanges(store);
            session.lastApply = now;
            appliedOk = true;
            session.expectMethod = method;
            session.expectedFps = targetFps;
            session.expectFg = fgK;
            session.expectVrr = vrrPlan;
            session.appliedAtStart = false;
            session.verified = false;
            if (refine) {
              // Ajuste silencioso del limite pendiente: sin nuevos avisos de reinicio.
              Log("[%5.0f s] Ajusto el limite pendiente de %s: %s (se aplicara al reiniciar el juego).\n",
                  session.T(now), session.label.c_str(), DescribeChange(change).c_str());
            } else {
              session.restartNoted = false;
              session.verifyDeadline = now + cfg::kVerifySec;
              Log("\n[%5.0f s] APLICADO a %s: %s.\n", session.T(now), session.label.c_str(),
                  DescribeChange(change).c_str());
              Log("          Decidido con %s.\n", readyWhy.c_str());
            }
            if (planRate > 0 && planRate != switchedRate)
              Log("          La proxima vez que abras el juego pondre la pantalla a %d Hz\n"
                  "          (%.0f fps en vez de %.0f). Al cerrarlo volvera a %d Hz.\n",
                  planRate, plan.capFps, savedCurrent.valid ? savedCurrent.capFps : 0.0, baseRate);
            if (fgK > 1 && frlFgMode.empty())
              Log("          Primera vez con generacion de fotogramas: al reabrir el juego\n"
                  "          comprobare si el driver limita lo de pantalla o los fotogramas reales.\n");
            if (method == Method::DriverVsync && vsyncCap.empty())
              Log("          Primera vez en este equipo: al reabrir el juego comprobare si el\n"
                  "          driver impone su VSync; si no, pasare solo al limite de FPS.\n");
          } else {
            Log("[%5.0f s] No pude aplicar: %s\n", session.T(now), nvd.error.c_str());
            session.lastApply = now;
          }
        }
      }
      if (planRate > 0 && planRate != switchedRate) {
        // El plan es para la proxima partida: la sesion actual sigue con la frecuencia actual.
        session.current = savedCurrent;
        hz = savedHz;
      }
        return appliedOk;
  };

  auto CloseDecision = [&](double now) {
    if (!g_applyMode || session.exe.empty() || !session.seenForeground) return;
    auto known = store.items.find(session.exe);
    const WindowStats st = session.lastStats;
    std::string result;
    if (known == store.items.end() ||
        (!known->second.manual && !known->second.verified && session.appliedThisSession)) {
      Recommendation rec = SessionRec(session, hz);
      if (!rec.valid) {
        Log("\n  Al cerrar: con esta partida no hay un limite sostenible (o hubo pocos datos).\n");
        return;
      }
      session.current = rec;
      session.haveRec = true;
      std::string why = Fmt("toda la partida (%.1f min de juego)", session.gameSecs / 60.0);
      if (TryApply(now, st, true, why.c_str())) {
        auto c = store.items.find(session.exe);
        result = Fmt(T("Límite para la próxima partida de %s: %s.", "Limit for the next session of %s: %s."),
                     session.label.c_str(), DescribeChange(c->second).c_str());
      }
    } else if (known->second.manual) {
      // Limite manual: no se cambia nunca; solo se informa si no se sostuvo.
      if (session.failSecs >= 60.0 && session.failSecs >= 0.10 * session.limPlaySecs)
        Log("\n  Al cerrar: el juego no sostuvo su limite manual durante %.0f s (%.0f%% de la partida).\n"
            "  No lo cambio porque es manual; si quieres, ajustalo o deshazlo.\n",
            session.failSecs, 100.0 * session.failSecs / std::max(1.0, session.limPlaySecs));
      return;
    } else if (known->second.verified || session.accepted) {
      const AppliedChange& c = known->second;
      bool failed = session.failSecs >= 60.0 && session.failSecs >= 0.10 * session.limPlaySecs;
      if (failed) {
        std::vector<float> r = session.secRates;
        std::sort(r.begin(), r.end());
        int k = std::max(1, c.fgFactor);
        Recommendation lower;
        if (c.vrr) {
          double held = r.empty() ? 0.0 : r[r.size() / 10];  // ritmo sostenido el 90 % del tiempo
          double real = std::floor(held);
          if (real >= cfg::kMinCapFps) {
            lower.valid = lower.vrr = lower.vrrValid = true;
            lower.fgFactor = k;
            lower.realFps = real;
            lower.capFps = real * k;
            lower.vrrCapFps = lower.capFps;
          }
        } else {
          int n = std::max(1, static_cast<int>(std::lround(hz / std::max(1.0, session.expectedFps * k))));
          lower = DivisorRec(hz, n + 1, k);
        }
        Log("\n  Al cerrar: el limite no se sostuvo durante %.0f s (%.0f %% de la partida).\n",
            session.failSecs, 100.0 * session.failSecs / std::max(1.0, session.limPlaySecs));
        if (lower.valid) {
          session.current = lower;
          session.lowerPending = true;
          session.accepted = false;
          if (TryApply(now, st, true, "la regla de fallos al cerrar el juego")) {
            auto nc = store.items.find(session.exe);
            result = Fmt(T("%s no sostuvo su límite durante el %.0f %% de la partida. En la próxima usaré: %s.",
                           "%s didn't hold its limit for %.0f %% of the session. Next time I'll use: %s."),
                         session.label.c_str(), 100.0 * session.failSecs / std::max(1.0, session.limPlaySecs),
                         DescribeChange(nc->second).c_str());
          }
        }
      } else if (session.failSecs > 0) {
        Log("\n  Al cerrar: bajadas aisladas del limite (%.0f s, %.0f %% de la partida): no cambio nada.\n",
            session.failSecs, 100.0 * session.failSecs / std::max(1.0, session.limPlaySecs));
      }
    }
    if (!result.empty()) {
      Log("  %s\n", result.c_str());
      std::lock_guard<std::mutex> lock(g_ui.m);
      g_ui.notice = result;
    }
  };

  while (!g_quit) {
    for (int i = 0; i < 10 && !g_quit; ++i) Sleep(100);
    if (g_quit) break;
    for (const auto& m : pm.DrainMessages()) Log("  %s\n", m.c_str());
    if (!pm.HeaderSeen() && !pmRestarted && NowSec() - pmStartedAt > 8.0) {
      pmRestarted = true;
      Log("  PresentMon no entrega datos: cierro su sesion de captura y lo reinicio.\n");
      pm.Stop();
      if (!pm.Start(presentMonPath, std::wstring()))
        Log("  No pude reiniciar PresentMon.\n");
    }
    std::vector<FrameSample> fresh = pm.DrainSamples();
    double now = NowSec();
    bool applyMode = g_applyMode;
    const bool displayRestoreNeeded = switchedRate != 0;
    std::string manualExe;
    {
      std::lock_guard<std::mutex> lock(g_manualMutex);
      manualExe = g_manualExe;
    }
    const bool manual = !manualExe.empty();
    if (applyMode != lastApplyMode) {
      Log("[%5.0f s] Modo: %s\n", session.T(now), applyMode ? "aplicar limites" : "solo medir");
      lastApplyMode = applyMode;
    }

    // Limites manuales pedidos desde la ventana.
    {
      std::vector<std::pair<std::string, int>> reqs;
      {
        std::lock_guard<std::mutex> lock(g_manualLimitMutex);
        reqs.swap(g_manualLimitReq);
      }
      for (const auto& [exe, fps] : reqs) {
        std::string name = g_gameLabels.count(exe) ? g_gameLabels[exe] : exe;
        std::string result;
        auto known = store.items.find(exe);
        int k = known != store.items.end() ? std::max(1, known->second.fgFactor) : 1;
        if (session.exe == exe && session.lastStats.GeneratedFraction() > cfg::kFgOnFraction)
          k = std::max(1, session.lastStats.FgFactor());
        // G-SYNC: limite + VSync del driver (la combinacion recomendada); sin VRR, solo el limite.
        const bool vrrNow = g_useVrr && ((nvVrrKnown && vrrPossible) || (session.exe == exe && session.vrrActive));
        const uint32_t screen = static_cast<uint32_t>(fps);
        const uint32_t frl = k > 1 && frlFgMode == "reales" ? screen / static_cast<uint32_t>(k) : screen;
        std::vector<std::pair<uint32_t, uint32_t>> desired = {{nv::FRL_FPS_ID, frl}};
        if (vrrNow) desired.push_back({nv::VSYNCMODE_ID, nv::VSYNC_FORCEON});
        if (g_maxPerf) desired.push_back({nv::PREFERRED_PSTATE_ID, nv::PSTATE_PREFER_MAX});
        bool ok = nvOk;
        // Se parte de cero: se deshace lo anterior (puede incluir otros ajustes, como el VSync por intervalos).
        if (ok && known != store.items.end()) {
          ok = nvd.Revert(known->second);
          if (ok) store.items.erase(known);
        }
        AppliedChange change;
        if (ok) ok = nvd.ApplySettings(exe, desired, change);
        if (ok) {
          change.exe = exe;
          change.method = Method::FpsLimit;
          change.divisor = 0;
          change.newValue = screen;
          change.fgFactor = k;
          change.frlValue = frl;
          change.displayHz = 0;
          change.verified = false;
          change.vrr = vrrNow;
          change.manual = true;
          store.items[exe] = change;
          store.Save();
          const bool running = session.exe == exe && session.pid != 0;
          Log("\n[%5.0f s] LIMITE MANUAL para %s: %s.\n", session.T(now), name.c_str(), DescribeChange(change).c_str());
          Log("          La app no lo cambiara; se comprueba al abrir el juego%s.\n", running ? " (reinicialo)" : "");
          result = Fmt(running ? T("Límite manual para %s: %s. Reinicia el juego para aplicarlo.",
                                   "Manual limit for %s: %s. Restart the game to apply it.")
                               : T("Límite manual para %s: %s. Se aplicará al abrir el juego.",
                                   "Manual limit for %s: %s. It will apply when you open the game."),
                       name.c_str(), DescribeChange(change).c_str());
          if (session.exe == exe) {
            session.expectMethod = Method::FpsLimit;
            session.expectedFps = static_cast<double>(screen) / k;
            session.expectFg = k;
            session.expectVrr = vrrNow;
            session.appliedAtStart = false;
            session.appliedThisSession = true;
            session.verified = session.accepted = false;
            session.lowerPending = false;
            session.restartNoted = false;
            session.keepNoted = true;
            session.lastApply = now;
            session.verifyDeadline = now + cfg::kVerifySec;
          }
        } else {
          result = Fmt(T("No pude poner el límite manual a %s: %s", "Couldn't set the manual limit for %s: %s"),
                       name.c_str(), nvOk ? nvd.error.c_str() : T("driver de NVIDIA no disponible", "NVIDIA driver not available"));
          Log("\n[%5.0f s] %s\n", session.T(now), result.c_str());
        }
        {
          std::lock_guard<std::mutex> lock(g_ui.m);
          g_ui.warnings.erase(exe);
          g_ui.notice = result;
        }
        PublishChanges(store);
      }
    }

    // Deshacer pedido desde la ventana.
    {
      std::vector<std::string> one;
      {
        std::lock_guard<std::mutex> lock(g_undoOneMutex);
        one.swap(g_undoOne);
      }
      for (const auto& exe : one) {
        auto it = store.items.find(exe);
        if (it == store.items.end()) continue;
        std::string name = g_gameLabels.count(exe) ? g_gameLabels[exe] : exe;
        std::string result;
        if (nvOk && nvd.Revert(it->second)) {
          store.items.erase(it);
          store.Save();
          result = Fmt(T("Deshecho: %s vuelve a como estaba. Si estaba abierto, reinícialo.",
                         "Undone: %s is back to how it was. If it was open, restart it."), name.c_str());
          Log("\n[%5.0f s] Deshecho %s (pedido desde la ventana).\n", session.T(now), exe.c_str());
          if (session.exe == exe) {
            session.expectedFps = 0;
            session.verified = session.restartNoted = session.keepNoted = false;
            session.lowerPending = false;
            session.lastApply = now;
          }
          if (exe == session.exe && displayRestoreNeeded) RestoreDisplay();
        } else {
          result = Fmt(T("No pude deshacer %s: %s", "Couldn't undo %s: %s"), name.c_str(), nvd.error.c_str());
        }
        {
          std::lock_guard<std::mutex> lock(g_ui.m);
          g_ui.warnings.erase(exe);
          g_ui.notice = result;
        }
        PublishChanges(store);
      }
    }
    if (g_undoRequested.exchange(false)) {
      g_applyMode = false;
      applyMode = lastApplyMode = false;
      Log("\n[%5.0f s] Deshacer todos los cambios (pedido desde la ventana):\n", session.T(now));
      std::string result = UndoAll(nvd, store, nvOk);
      RestoreDisplay();
      Log("  %s\n  Modo cambiado a solo medir.\n", result.c_str());
      PublishChanges(store);
      {
        std::lock_guard<std::mutex> lock(g_ui.m);
        g_ui.notice = result;
      }
      session.expectedFps = 0;
      session.verified = session.restartNoted = false;
    }

    std::map<DWORD, int> perPid;
    std::map<DWORD, std::string> pidApp;
    for (const auto& f : fresh) {
      perPid[f.pid]++;
      pidApp[f.pid] = f.app;
    }
    HWND fgWnd = nullptr;
    DWORD fgPid = ForegroundPid(&fgWnd);

    // Si los Hz no se pudieron medir bien, repetir cuando no haya juego abierto.
    if (!hzClean && !hzFromCache && session.pid == 0 && fresh.size() < 30 &&
        now - lastHzRetry > 30.0) {
      lastHzRetry = now;
      Log("\nSin juego abierto: repito la medicion de Hz.\n");
      bool ok = false;
      double m = ReliableRefresh(target->output.Get(), monitorKey, nominalHz, signalHz, &ok, 1);
      if (ok) {
        hz = m;
        hzClean = true;
        Log("  Frecuencia real: %.3f Hz (guardada para las proximas veces)\n", hz);
        std::lock_guard<std::mutex> lock(g_ui.m);
        g_ui.monitor = Fmt(T("%ldx%ld · %.3f Hz reales", "%ldx%ld · %.3f Hz measured"), target->width, target->height, hz);
        g_ui.hz = hz;
      }
      continue;
    }

    if (session.pid != 0 && !ProcessAlive(session.pid)) {
      Log("\n[%5.0f s] %s se ha cerrado.\n", session.T(now), session.label.c_str());
      if (recorder.f) {
        std::wstring recPath = recorder.path;
        size_t n = recorder.Close();
        Log("  Grabacion guardada: %s (%zu fotogramas)\n", Narrow(recPath).c_str(), n);
      }
      lastClosedExe = session.exe;
      lastClosedAt = now;
      CloseDecision(now);
      PrintSessionSummary(session, hz);
      RestoreDisplay();
      session = Session{};
      candidatePid = 0;
      candidateStreak = 0;
    }

    bool searching = session.pid == 0 || session.background || !session.seenForeground;
    if (searching) {
      DWORD found = 0;
      HWND foundWnd = nullptr;
      std::string foundExe;
      bool foundInFront = false;
      if (manual) {
        for (const auto& [pid, n] : perPid)
          if (pidApp[pid] == manualExe && n > 0 && pid != session.pid) { found = pid; break; }
        foundExe = manualExe;
        candidateStreak = found ? cfg::kDetectConfirmSec : 0;
        candidatePid = found;
      } else {
        if (fgPid != 0 && fgPid != session.pid && perPid[fgPid] >= cfg::kDetectMinPresents &&
            !IsExcludedApp(pidApp[fgPid])) {
          std::wstring path = ProcessImagePath(fgPid);
          if (WindowCoversMonitor(fgWnd) || IsLibraryPath(path)) {
            found = fgPid;
            foundWnd = fgWnd;
            foundExe = pidApp[fgPid];
            foundInFront = true;
          }
        }
        if (!found && session.pid == 0)
          found = FindBackgroundGame(perPid, pidApp, &foundWnd, &foundExe);
        if (found && foundExe == lastClosedExe && now - lastClosedAt < 8.0) {
          found = 0;
          foundExe.clear();
        }
        if (found) {
          candidateStreak = (candidatePid == found) ? candidateStreak + 1 : 1;
          candidatePid = found;
        }
      }
      if (!found) candidateStreak = 0;

      if (found && candidateStreak >= cfg::kDetectConfirmSec) {
        if (session.pid != 0) {
          Log("\n[%5.0f s] Cambiaste de juego.\n", session.T(now));
          CloseDecision(now);
          PrintSessionSummary(session, hz);
          RestoreDisplay();
        }
        std::wstring path = ProcessImagePath(found);
        if (foundExe.empty()) foundExe = ExeNameFromPath(path);
        std::string label = GameLabel(manual ? nullptr : foundWnd, path, foundExe);
        session = Session{};
        session.pid = found;
        session.exe = foundExe;
        {
          std::string b = ReadKv(L"equipo.txt", "ritmo-antes|" + foundExe);
          if (!b.empty()) session.baseSwing = std::atof(b.c_str());
        }
        session.label = label;
        g_gameLabels[foundExe] = label;
        WriteKv(L"equipo.txt", "nombre|" + foundExe, label);
        PublishChanges(store);
        session.start = now;
        session.phaseStart = now;
        session.lastFrameArrival = now;
        gamesSeen++;
        Log("\n[Juego detectado] %s  (%s)%s\n", label.c_str(), session.exe.c_str(),
            IsLibraryPath(path) ? "  [biblioteca de juegos]" : "");
        if (!foundInFront && !manual)
          Log("  Esta en segundo plano: empiezo a medir cuando vuelvas al juego.\n");
        auto prev = store.items.find(session.exe);
        AppliedChange own;
        if (prev == store.items.end() && nvOk && nvd.FindOwnProfile(session.exe, own)) {
          if (own.method == Method::DriverVsync) own.newValue = static_cast<uint32_t>(std::lround(hz / own.divisor));
          store.items[own.exe] = own;
          store.Save();
          PublishChanges(store);
          prev = store.items.find(session.exe);
          Log("  Recuperado: %s, aplicado antes por FlatPace.\n", DescribeChange(own).c_str());
        }
        if (prev != store.items.end() && prev->second.displayHz > 0 &&
            prev->second.displayHz != display::CurrentRate(displayDev)) {
          int want = prev->second.displayHz;
          if (display::SetRate(displayDev, want)) {
            switchedRate = want;
            double cached = ReadCachedHzForKey(monitorKeyBase + Fmt("@%d", want));
            hz = cached > 0 ? cached : static_cast<double>(want);
            Log("  Pantalla puesta a %d Hz para este juego (vuelve a %d Hz al cerrarlo).\n", want, baseRate);
            std::lock_guard<std::mutex> lock(g_ui.m);
            g_ui.monitor = Fmt(T("%ldx%ld · %d Hz para este juego", "%ldx%ld · %d Hz for this game"), target->width, target->height, want);
            g_ui.hz = hz;
          } else {
            Log("  No pude poner la pantalla a %d Hz.\n", want);
          }
        }
        if (prev != store.items.end()) {
          const AppliedChange& c = prev->second;
          session.expectMethod = c.method;
          session.expectedFps = c.method == Method::DriverVsync ? hz / std::max(1, c.divisor)
                                                                 : static_cast<double>(c.newValue);
          session.appliedAtStart = true;
          session.accepted = c.verified;
          session.expectVrr = c.vrr;
          session.expectFg = std::max(1, c.fgFactor);
          if (c.fgFactor > 1) session.expectedFps = static_cast<double>(c.newValue) / c.fgFactor;
          session.verifyDeadline = now + cfg::kVerifySec;
          Log("  Tiene aplicado: %s. Compruebo que funcione.\n", DescribeChange(c).c_str());
        }
        candidatePid = 0;
        candidateStreak = 0;
        PublishSession(session, store, applyMode, manualExe);
        continue;
      }
      if (session.pid == 0) {
        overlay::SetGame(0, 0.0, "", overlay::LimitState::None);
        PublishSession(session, store, applyMode, manualExe);
        continue;
      }
    }

    std::vector<FrameSample> mine;
    for (auto& f : fresh)
      if (f.pid == session.pid) mine.push_back(std::move(f));

    bool isForeground = fgPid == session.pid;
    if (!session.seenForeground) {
      if (!isForeground) {
        PublishSession(session, store, applyMode, manualExe);
        continue;
      }
      session.seenForeground = true;
      Log("[%5.0f s] El juego esta en primer plano: empiezo a medir.\n", session.T(now));
      session.start = now;
      session.ResetPhase(now);
    }
    // G-SYNC actuando con el juego (se consulta cada 2 s; cambia tras 3 lecturas iguales).
    // VRR: con NVIDIA, la consulta al driver (exacta); con cualquier marca, los tiempos de
    // pantalla. Si el juego entrega imagenes a ritmos que no son multiplos del refresco y
    // la pantalla las muestra a esos mismos ritmos, hay VRR; si las "redondea" a multiplos
    // del refresco, no. Si el juego ya va a un multiplo exacto, no se puede saber.
    if (isForeground) {
      if (nvVrrKnown && vrrPossible && now - lastVrrPoll >= 2.0) {
        lastVrrPoll = now;
        bool possible = false, active = false;
        nvd.Vrr(Narrow(target->gdiName), possible, active);
        session.vrrOnCount = active ? session.vrrOnCount + 1 : 0;
        session.vrrOffCount = active ? 0 : session.vrrOffCount + 1;
        if (session.vrrOnCount >= 3) session.vrrNv = true;
        if (session.vrrOffCount >= 3) session.vrrNv = false;
      }
      const double P = 1000.0 / hz;
      auto off = [&](double x) {
        double q = x / P;
        return std::fabs(q - std::round(q)) > 0.15;
      };
      for (const auto& f : mine) {
        if (f.presentMs > P * 0.9 && f.presentMs < P * 12) {
          session.presN++;
          if (off(f.presentMs)) session.presOff++;
        }
        if (f.displayMs > P * 0.9 && f.displayMs < P * 12) {
          session.dispN++;
          if (off(f.displayMs)) session.dispOff++;
        }
      }
      if (session.presN >= 120 && session.dispN >= 120) {
        double ps = static_cast<double>(session.presOff) / session.presN;
        double ds = static_cast<double>(session.dispOff) / session.dispN;
        int vote = ps > 0.3 ? (ds > 0.3 ? 1 : ds < 0.1 ? -1 : 0) : 0;
        session.seenOn = vote > 0 ? session.seenOn + 1 : 0;
        session.seenOff = vote < 0 ? session.seenOff + 1 : 0;
        if (session.seenOn >= 3) session.vrrSeen = true;
        if (session.seenOff >= 3) session.vrrSeen = false;
        session.presN = session.presOff = session.dispN = session.dispOff = 0;
      }
      bool detected = (nvVrrKnown && vrrPossible) ? session.vrrNv : session.vrrSeen;
      bool effective = detected && g_useVrr;
      if (effective != session.vrrActive) {
        session.vrrActive = effective;
        if (effective)
          Log("[%5.0f s] VRR (G-SYNC/FreeSync) actua con este juego (%s): el limite no necesita ser\n"
              "          un divisor de los Hz.\n",
              session.T(now), (nvVrrKnown && vrrPossible) ? "segun el driver de NVIDIA" : "segun los tiempos de pantalla");
        else
          Log("[%5.0f s] %s: uso los divisores de los Hz.\n", session.T(now),
              detected ? "Divisores forzados (ajuste avanzado)" : "El VRR ya no actua con este juego");
        if (session.haveRec && !session.accepted) session.ResetPhase(now);
      }
      std::lock_guard<std::mutex> lock(g_ui.m);
      g_ui.vrrState = detected ? (g_useVrr ? 2 : 3) : 1;
    }
    bool shouldBackground = !isForeground;
    if (shouldBackground && !session.background) {
      session.background = true;
      Log("[%5.0f s] El juego paso a segundo plano. Medicion en pausa.\n", session.T(now));
    } else if (!shouldBackground && session.background) {
      session.background = false;
      Log("[%5.0f s] El juego vuelve al primer plano. Vuelvo a medir.\n", session.T(now));
      session.ResetPhase(now);
    }
    if (session.background) {
      PublishSession(session, store, applyMode, manualExe);
      continue;
    }

    ProcessSession(session, mine, now, hz);

    // Grabacion de fotogramas (si esta activada)
    if (g_recordFrames && !recorder.f && !mine.empty()) {
      std::vector<std::string> h = {
          Fmt("FlatPace %s - grabacion de fotogramas", kVersion),
          "juego: " + session.label + " (" + session.exe + ")",
          Fmt("pantalla: %.3f Hz reales; rango VRR del monitor: %d-%d Hz (0 = desconocido)", hz, g_vrrMinHz.load(),
              g_vrrMaxHz.load()),
          std::string("prioridad: ") + (g_priority == 0 ? "estabilidad" : "fluidez"),
      };
      auto known = store.items.find(session.exe);
      h.push_back("limite al empezar: " + (known != store.items.end() && session.appliedAtStart
                                               ? DescribeChange(known->second)
                                               : std::string("ninguno de FlatPace")));
      h.push_back("columnas: tiempos en ms; t_llegada = cuando llego el dato (por tandas); t_acumulado = suma de "
                  "presentaciones; vrr y limite_confirmado = estado en ese momento");
      if (recorder.Open(session.exe, h))
        Log("[%5.0f s] Grabando los fotogramas en %s\n", session.T(now), Narrow(recorder.path).c_str());
      else
        Log("[%5.0f s] No pude crear el archivo de grabacion.\n", session.T(now));
    }
    if (recorder.f) {
      if (!g_recordFrames) {
        size_t n = recorder.Close();
        Log("[%5.0f s] Grabacion detenida (%zu fotogramas).\n", session.T(now), n);
      } else {
        for (const auto& fr : mine) recorder.Write(fr, session.start, session.vrrActive, session.verified);
        recorder.Flush();
      }
    }

    // ---- Verificar y aplicar el limite ----
    const WindowStats& st = session.lastStats;
    // Con generacion de fotogramas: averiguar sobre que actua el limitador del driver,
    // solo con la generacion activa y estable al menos 8 s, comparando el valor puesto con
    // lo que se ve en pantalla y con los fotogramas reales.
    if (session.expectFg > 1 && session.appliedAtStart && frlFgMode.empty() && session.fgActive &&
        now - session.fgSince > 8.0 && st.FgFactor() > 1 && st.frames >= 100) {
      auto kc = store.items.find(session.exe);
      double frl = kc != store.items.end() ? static_cast<double>(kc->second.frlValue) : 0.0;
      double realNow = st.medianMs > 0 ? 1000.0 / st.medianMs : 0.0;
      if (frl > 0 && std::fabs(st.presentedFps - frl) / frl < 0.04) {
        frlFgMode = "salida";
        WriteKv(L"equipo.txt", frlFgKey, frlFgMode);
        Log("[%5.0f s] Con generacion, el limitador del driver actua sobre lo que se ve en pantalla\n"
            "          (~%.0f en pantalla con el limite en %.0f). Lo recordare.\n",
            session.T(now), st.presentedFps, frl);
      } else if (frl > 0 && std::fabs(realNow - frl) / frl < 0.04) {
        frlFgMode = "reales";
        WriteKv(L"equipo.txt", frlFgKey, frlFgMode);
        Log("[%5.0f s] Con generacion, el limitador del driver actua sobre los fotogramas reales\n"
            "          (~%.0f reales con el limite en %.0f). Corrijo el valor; habra que reabrir el juego.\n",
            session.T(now), realNow, frl);
        session.expectedFps = 0;
        session.appliedAtStart = false;
        session.lastApply = -1e9;
      }
    }
    if (session.expectedFps > 0 && !session.verified && st.frames > 0) {
      double exp = session.expectedFps;
      double fpsNow = st.medianMs > 0 ? 1000.0 / st.medianMs : 0.0;
      // Con generacion la GPU parece siempre ocupada y el detector de limites no sirve:
      // se comparan directamente los ritmos real y en pantalla con los esperados.
      bool fgMatch = session.expectFg > 1 && st.FgFactor() > 1 && fpsNow > 0 &&
                     std::fabs(fpsNow - exp) / exp < 0.02 &&
                     std::fabs(st.presentedFps - exp * session.expectFg) / (exp * session.expectFg) < 0.04;
      // Verificacion directa: el juego va al ritmo exacto esperado (+-1,5 %) y casi ningun
      // fotograma va mas rapido. Reconoce limites que no son divisores (G-SYNC).
      // El limitador del driver deja pasar algun fotograma algo mas rapido: se acepta si el
      // ritmo coincide (+-1,5 %) y menos del 10 % va mas de un 7 % por debajo del periodo.
      bool directMatch = session.expectFg <= 1 && fpsNow > 0 && std::fabs(fpsNow - exp) / exp < 0.015 &&
                         st.FractionRealUnder((1000.0 / exp) * 0.93) < 0.10;
      if (fgMatch || directMatch || (st.capped && std::fabs(st.capFps - exp) / exp < 0.02)) {
        session.verified = true;
        Log("[%5.0f s] LIMITE FUNCIONANDO: el juego va a ~%.3f fps estables.\n", session.T(now), exp);
        {
          auto kc = store.items.find(session.exe);
          if (kc != store.items.end() && !kc->second.verified) {
            kc->second.verified = true;
            store.Save();
          }
          session.accepted = true;
        }

        if (session.expectMethod == Method::DriverVsync && vsyncCap != "si") {
          vsyncCap = "si";
          WriteKv(L"equipo.txt", machineKey, vsyncCap);
          Log("          El driver impone su VSync en este equipo: lo usare en todos los juegos.\n");
        }
        if (session.haveRec) {
          int k = std::max(1, session.expectFg);
          int n = std::max(1, static_cast<int>(std::lround(hz / (exp * k))));
          session.current = DivisorRec(hz, n, k);
          session.downCount = session.upCount = 0;
        }
      } else if (session.expectMethod == Method::DriverVsync && session.appliedAtStart &&
                 vsyncCap != "si" && now > session.start + cfg::kVerifySec && fpsNow > exp * 1.10) {
        // El juego se abrio con el VSync del driver puesto y aun asi va por encima:
        // en este equipo el driver no puede imponerlo. Se pasa al limite de FPS.
        vsyncCap = "no";
        WriteKv(L"equipo.txt", machineKey, vsyncCap);
        Log("[%5.0f s] El driver no logra imponer su VSync en este equipo (~%.0f fps en vez de %.0f).\n"
            "          Paso al limite de FPS del driver, que si funciona aqui.\n",
            session.T(now), fpsNow, exp);
        session.expectedFps = 0;
        session.appliedAtStart = false;
        session.lastApply = -1e9;
      } else if (now > session.verifyDeadline && !session.restartNoted && !session.appliedAtStart) {
        session.restartNoted = true;
        Log("[%5.0f s] Aun no veo el cambio (%.0f fps): hay que reiniciar el juego.\n",
            session.T(now), exp);
      }
    }
    // Tiempo de juego, tiempo con G-SYNC y regla de fallos del limite aceptado.
    {
      double dt = session.lastTick > 0 ? std::min(2.0, now - session.lastTick) : 0.0;
      session.lastTick = now;
      if (st.frames > 0 && now >= session.phaseStart) {
        session.gameSecs += dt;
        if (session.vrrActive) session.vrrSecs += dt;
        if (st.capped && st.capFps > 0 && session.expectedFps <= 0 && HeldRate(st) > 0) {
          session.ownCapSecs += dt;
          session.ownCapRateSum += st.capFps * dt;
        }
        auto kc = store.items.find(session.exe);
        bool fgSame = kc != store.items.end() && ((kc->second.fgFactor > 1) == (st.FgFactor() > 1));
        if (session.expectedFps > 0 && (session.verified || session.accepted) && fgSame && st.medianMs > 0) {
          double rate = 1000.0 / st.medianMs;
          session.limPlaySecs += dt;
          session.secRates.push_back(static_cast<float>(rate));
          if (rate < session.expectedFps * (1.0 - cfg::kJnd)) {
            session.belowRun++;
            if (session.belowRun == 5) session.failSecs += 5.0;
            else if (session.belowRun > 5) session.failSecs += dt;
          } else {
            session.belowRun = 0;
          }
        }
      }
    }
    // Advertencias: cuando conviene deshacer el limite de este juego.
    {
      auto kc = store.items.find(session.exe);
      std::string warn;
      if (kc != store.items.end() && st.frames >= 100 && now - session.start > 30.0) {
        const AppliedChange& c = kc->second;
        double realNow = st.medianMs > 0 ? 1000.0 / st.medianMs : 0.0;
        bool fgNow = st.FgFactor() > 1;
        session.fgMismatchSecs = ((c.fgFactor > 1) != fgNow) ? session.fgMismatchSecs + 1 : 0;
        // "Va mucho mejor que cuando se decidio": solo para limites de partidas anteriores
        // que aun no actuan, con un 25 % mas de forma continuada (por ejemplo, tras bajar ajustes).
        bool better = session.appliedAtStart && !session.verified && c.basisReal > 0 &&
                      realNow > c.basisReal * 1.25 && !st.capped;
        session.betterSecs = better ? session.betterSecs + 1 : 0;
        int kind = 0;
        if (session.betterSecs >= 60) {
          kind = 1;
          warn = Fmt(T("El juego va claramente mejor que cuando se decidió este límite (~%.0f reales frente a %u). "
                       "Si cambiaste ajustes, deshazlo para que se recalcule.",
                       "The game runs clearly better than when this limit was chosen (~%.0f real vs %u). "
                       "If you changed settings, undo it so it's recalculated."), realNow, c.basisReal);
        } else if ((session.verified || session.accepted) && session.downCount >= 60) {
          kind = 2;
          warn = T("El juego no está sosteniendo este límite. Si sigue así, deshazlo para que se recalcule.",
                   "The game isn't holding this limit. If it keeps happening, undo it so it's recalculated.");
        } else if (session.fgMismatchSecs >= 60) {
          kind = 3;
          warn = c.fgFactor > 1
                     ? T("Este límite se aplicó con generación de fotogramas y ahora está desactivada. "
                         "Si juegas así, deshazlo para que se recalcule.",
                         "This limit was applied with frame generation, which is now off. "
                         "If you play like this, undo it so it's recalculated.")
                     : T("Este límite se aplicó sin generación de fotogramas y ahora está activada. "
                         "Si juegas así, deshazlo para que se recalcule.",
                         "This limit was applied without frame generation, which is now on. "
                         "If you play like this, undo it so it's recalculated.");
        }
        if (kind != 0 && kind != session.lastWarnKind)
          Log("[%5.0f s] Revisar el limite: %s\n", session.T(now), warn.c_str());
        session.lastWarnKind = kind;
      }
      if (!session.exe.empty()) SetWarning(store, session.exe, warn);
    }
    if (applyMode && session.haveRec && st.frames > 0 && g_applyNowRequest.exchange(false))
      TryApply(now, st, true, "tu peticion (Aplicar ya)");


    {
      double target = 0.0;
      std::string limitLabel;
      overlay::LimitState state = overlay::LimitState::None;
      auto known = store.items.find(session.exe);
      if (session.expectedFps > 0) target = 1000.0 / session.expectedFps;
      if (known != store.items.end()) {
        const AppliedChange& c = known->second;
        limitLabel = std::to_string(c.newValue) + (c.fgFactor > 1 ? T(" en pantalla", " on screen") : " fps");
        state = session.verified ? overlay::LimitState::Locked : overlay::LimitState::Pending;
      }
      overlay::SetGame(session.background || !session.seenForeground ? 0 : session.pid, target, limitLabel, state,
                       session.baseSwing);
    }
    {
      static int lastOv = -2;
      int ov = overlay::state;
      if (ov != lastOv && session.pid != 0) {
        static const char* why[] = {"visible", "desactivado en la app", "sin juego detectado",
                                    "el juego no esta en primer plano", "aun no hay datos suficientes",
                                    "Windows no pudo dibujarlo"};
        // "Sin juego detectado" es el instante normal antes de que el juego se conecte: no se anota.
        if (ov >= 0 && ov <= 5 && ov != 2)
          Log("[%5.0f s] Overlay: %s%s\n", session.T(now), why[ov],
              ov == 5 ? Fmt(" (error %lu)", overlay::lastError.load()).c_str() : "");
        lastOv = ov;
      }
    }
    PublishSession(session, store, applyMode, manualExe);
    if (!pm.Running()) {
      SetStatus(UiStatus::Error, T("PresentMon se detuvo", "PresentMon stopped"), T("Cierra y vuelve a abrir FlatPace.", "Close and reopen FlatPace."));
      Log("PresentMon se detuvo.\n");
      break;
    }
  }

  pm.Stop();
  if (session.pid != 0) CloseDecision(NowSec());
  if (session.pid != 0) PrintSessionSummary(session, hz);
  else if (gamesSeen == 0) Log("\nNo se detecto ningun juego.\n");
  RestoreDisplay();
  if (!logPath.empty()) Log("\nRegistro guardado en: %s\n", Narrow(logPath).c_str());
}



// ---------------------------------------------------------------------------
// Ventana (Dear ImGui + DirectX 11), modo oscuro
// ---------------------------------------------------------------------------
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace gui {
static ID3D11Device* dev = nullptr;
static ID3D11DeviceContext* ctx = nullptr;
static IDXGISwapChain* swap = nullptr;
static ID3D11RenderTargetView* rtv = nullptr;
static UINT resizeW = 0, resizeH = 0;
static float scale = 1.0f;
static ImFont* fontBody = nullptr;
static ImFont* fontStrong = nullptr;
static ImFont* fontBig = nullptr;
static ImFont* fontHead = nullptr;  // titulos (Bahnschrift)
static ImFont* fontNum = nullptr;   // cifra grande de FPS (Bahnschrift)

// Colores del tema
static ImVec4 Hex(unsigned v) {
  return ImVec4(((v >> 16) & 0xFF) / 255.0f, ((v >> 8) & 0xFF) / 255.0f, (v & 0xFF) / 255.0f, 1.0f);
}
// Identidad de FlatPace: fondo casi negro, tarjetas oscuras, azul de marca (la linea plana del
// logo y el estado confirmado), rojo para lo que aun no esta bajo control y ambar para avisos.
static const ImVec4 kBg = Hex(0x0E1015);      // fondo
static const ImVec4 kSide = Hex(0x12151B);    // barra lateral
static const ImVec4 kCard = Hex(0x171B22);    // tarjetas
static const ImVec4 kCardHi = Hex(0x1F2430);  // tarjetas realzadas, seleccion y grafica
static const ImVec4 kLine = Hex(0x2A303B);    // bordes
static const ImVec4 kText = Hex(0xECEEF1);
static const ImVec4 kDim = Hex(0x8B95A5);
static const ImVec4 kBlue = Hex(0x2FA8FF);    // azul de marca
static const ImVec4 kGreen = kBlue;           // estado confirmado (la identidad usa el azul)
static const ImVec4 kAmber = Hex(0xF5B942);   // atencion
static const ImVec4 kRed = Hex(0xFF6B6B);     // sin confirmar, tirones
static const ImVec4 kGray = Hex(0x6B7684);

static void CreateRenderTarget() {
  ID3D11Texture2D* back = nullptr;
  swap->GetBuffer(0, IID_PPV_ARGS(&back));
  if (back) {
    dev->CreateRenderTargetView(back, nullptr, &rtv);
    back->Release();
  }
}
static void CleanupRenderTarget() {
  if (rtv) { rtv->Release(); rtv = nullptr; }
}

static bool CreateDevice(HWND hwnd) {
  DXGI_SWAP_CHAIN_DESC sd{};
  sd.BufferCount = 2;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = hwnd;
  sd.SampleDesc.Count = 1;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL got;
  HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                             D3D11_SDK_VERSION, &sd, &swap, &dev, &got, &ctx);
  if (FAILED(hr))
    hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                       D3D11_SDK_VERSION, &sd, &swap, &dev, &got, &ctx);
  if (FAILED(hr)) return false;
  CreateRenderTarget();
  return true;
}

static void CleanupDevice() {
  CleanupRenderTarget();
  if (swap) { swap->Release(); swap = nullptr; }
  if (ctx) { ctx->Release(); ctx = nullptr; }
  if (dev) { dev->Release(); dev = nullptr; }
}

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
  switch (msg) {
    case WM_SIZE:
      if (wParam == SIZE_MINIMIZED) return 0;
      resizeW = LOWORD(lParam);
      resizeH = HIWORD(lParam);
      return 0;
    case WM_GETMINMAXINFO: {
      auto* mm = reinterpret_cast<MINMAXINFO*>(lParam);
      mm->ptMinTrackSize.x = static_cast<LONG>(860 * scale);
      mm->ptMinTrackSize.y = static_cast<LONG>(560 * scale);
      return 0;
    }
    case WM_SYSCOMMAND:
      if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
      break;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void ApplyTheme() {
  ImGuiStyle& s = ImGui::GetStyle();
  s.WindowRounding = 0.0f;
  s.ChildRounding = 14.0f;
  s.FrameRounding = 10.0f;
  s.PopupRounding = 8.0f;
  s.GrabRounding = 7.0f;
  s.ScrollbarRounding = 7.0f;
  s.WindowPadding = ImVec2(20, 18);
  s.FramePadding = ImVec2(14, 8);
  s.ItemSpacing = ImVec2(10, 10);
  s.WindowBorderSize = 0.0f;
  s.ChildBorderSize = 0.0f;
  s.PopupBorderSize = 1.0f;
  ImVec4* c = s.Colors;
  c[ImGuiCol_WindowBg] = kBg;
  c[ImGuiCol_ChildBg] = kCard;
  c[ImGuiCol_PopupBg] = kCardHi;
  c[ImGuiCol_Border] = kLine;
  c[ImGuiCol_Text] = kText;
  c[ImGuiCol_TextDisabled] = kDim;
  c[ImGuiCol_FrameBg] = kCardHi;
  c[ImGuiCol_FrameBgHovered] = Hex(0x262C38);
  c[ImGuiCol_FrameBgActive] = kLine;
  c[ImGuiCol_Button] = kLine;
  c[ImGuiCol_ButtonHovered] = Hex(0x343B48);
  c[ImGuiCol_ButtonActive] = Hex(0x3D4554);
  c[ImGuiCol_CheckMark] = kBlue;
  c[ImGuiCol_Header] = kCard;
  c[ImGuiCol_HeaderHovered] = kCardHi;
  c[ImGuiCol_HeaderActive] = kCardHi;
  c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ScrollbarGrab] = kLine;
  c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);
  c[ImGuiCol_Separator] = kLine;
  s.ScaleAllSizes(scale);
}

static void LoadFontsInto(ImFont*& body, ImFont*& strong, ImFont*& big) {
  ImGuiIO& io = ImGui::GetIO();
  static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2014, 0x2014, 0x2026, 0x2026,
                                   0x2190, 0x2193, 0x2022, 0x2022, 0};
  wchar_t winDir[MAX_PATH];
  GetWindowsDirectoryW(winDir, MAX_PATH);
  std::string fonts = Narrow(std::wstring(winDir)) + "\\Fonts\\";
  auto load = [&](const char* file, float px) -> ImFont* {
    std::string path = fonts + file;
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) return nullptr;
    return io.Fonts->AddFontFromFileTTF(path.c_str(), px * scale, nullptr, ranges);
  };
  body = load("segoeui.ttf", 17.0f);
  if (!body) body = io.Fonts->AddFontDefault();
  strong = load("seguisb.ttf", 18.0f);
  if (!strong) strong = body;
  big = load("seguisb.ttf", 27.0f);
  if (!big) big = strong;
}

static void LoadFonts() {
  LoadFontsInto(fontBody, fontStrong, fontBig);
  ImGuiIO& io = ImGui::GetIO();
  wchar_t winDir[MAX_PATH];
  GetWindowsDirectoryW(winDir, MAX_PATH);
  std::string path = Narrow(std::wstring(winDir)) + "\\Fonts\\bahnschrift.ttf";
  static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2014, 0x2014, 0x2026, 0x2026, 0x2190, 0x2193, 0x2713, 0x2713, 0};
  if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
    fontHead = io.Fonts->AddFontFromFileTTF(path.c_str(), 24.0f * scale, nullptr, ranges);
    fontNum = io.Fonts->AddFontFromFileTTF(path.c_str(), 60.0f * scale, nullptr, ranges);
  }
  if (!fontHead) fontHead = fontBig;
  if (!fontNum) fontNum = fontBig;
}

// Copia del estado sin el candado, para dibujar sin bloquear al motor.
struct View {
  UiStatus status;
  std::string statusText, statusDetail, gpu, monitor, tip, driver, game, exe;
  std::string capText, bottleneck, recText, appliedText, notice, presentText;
  bool driverOk, haveGame;
  double realFps, lowFps, screenFps, steadyPct, jitterMs;
  int hitches;
  std::vector<UiChange> changes;
  std::string currentWarning;
  std::string vrrRange;
  bool vrrPossible, vrrNvKnown;
  int vrrState;
  std::vector<std::string> log;
  std::wstring logPath;
  double hz;
};

static View Snapshot() {
  std::lock_guard<std::mutex> lock(g_ui.m);
  View v;
  v.status = g_ui.status;
  v.statusText = g_ui.statusText;
  v.statusDetail = g_ui.statusDetail;
  v.gpu = g_ui.gpu;
  v.monitor = g_ui.monitor;
  v.tip = g_ui.tip;
  v.driver = g_ui.driver;
  v.driverOk = g_ui.driverOk;
  v.haveGame = g_ui.haveGame;
  v.game = g_ui.game;
  v.exe = g_ui.exe;
  v.realFps = g_ui.realFps;
  v.steadyPct = g_ui.steadyPct;
  v.jitterMs = g_ui.jitterMs;
  v.lowFps = g_ui.lowFps;
  v.screenFps = g_ui.screenFps;
  v.hitches = g_ui.hitches;
  v.capText = g_ui.capText;
  v.bottleneck = g_ui.bottleneck;
  v.recText = g_ui.recText;
  v.appliedText = g_ui.appliedText;
  v.presentText = g_ui.presentText;
  v.notice = g_ui.notice;
  v.changes = g_ui.changes;
  v.vrrRange = g_ui.vrrRange;
  v.vrrPossible = g_ui.vrrPossible;
  v.vrrNvKnown = g_ui.vrrNvKnown;
  v.vrrState = g_ui.vrrState;
  {
    auto w = g_ui.warnings.find(g_ui.exe);
    if (g_ui.haveGame && w != g_ui.warnings.end()) v.currentWarning = w->second;
  }
  v.log.assign(g_ui.log.begin(), g_ui.log.end());
  v.logPath = g_ui.logPath;
  v.hz = g_ui.hz;
  return v;
}

// ---- Diseno: estilo SteamOS. El protagonista es la grafica de ritmo ("la linea"). ----
static int page = 0;  // 0 Ahora, 1 Juegos, 2 Equipo, 3 Registro

static ImU32 Col(const ImVec4& c, float a = 1.0f) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * a)); }

static ImVec4 StatusColor(UiStatus s) {
  switch (s) {
    case UiStatus::Applied: return kGreen;
    case UiStatus::Measuring: return kBlue;
    case UiStatus::Restart: return kAmber;
    case UiStatus::Paused: return kAmber;
    case UiStatus::Error: return kRed;
    default: return kGray;
  }
}

static const char* StatusWord(UiStatus s) {
  switch (s) {
    case UiStatus::Applied: return T("Estable", "Steady");
    case UiStatus::Measuring: return T("Midiendo", "Measuring");
    case UiStatus::Restart: return T("Atención", "Attention");
    case UiStatus::Paused: return T("En pausa", "Paused");
    case UiStatus::Error: return T("Error", "Error");
    default: return T("En espera", "Waiting");
  }
}

// Pastilla de estado: punto de color y una palabra.
static void StatusPill(UiStatus st) {
  ImVec4 c = StatusColor(st);
  const char* w = StatusWord(st);
  ImVec2 ts = ImGui::CalcTextSize(w);
  float h = ts.y + 10 * scale, pad = 12 * scale, dot = 7 * scale;
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImVec2 size(pad * 2 + dot + 8 * scale + ts.x, h);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), Col(c, 0.16f), h * 0.5f);
  dl->AddCircleFilled(ImVec2(p.x + pad + dot * 0.5f, p.y + h * 0.5f), dot * 0.5f, Col(c));
  dl->AddText(ImVec2(p.x + pad + dot + 8 * scale, p.y + 5 * scale), Col(c), w);
  ImGui::Dummy(size);
}

// Fila etiqueta / valor sin tabla, alineada a una columna.
static void KV(const char* label, const std::string& value, float col, const ImVec4* color = nullptr) {
  ImGui::TextColored(kDim, "%s", label);
  ImGui::SameLine(col);
  if (color) ImGui::TextColored(*color, "%s", value.c_str());
  else ImGui::TextUnformatted(value.c_str());
}

// La grafica de ritmo: linea objetivo y tiempos reales de los ultimos 10 s.
static void PacingGraph(ImVec2 size) {
  std::vector<std::pair<double, double>> real;
  double target = 0.0;
  overlay::LimitState state;
  double baseSwing = -1.0;
  double display = 0.0, refAuto = 0.0;
  unsigned generation = 0;
  {
    std::lock_guard<std::mutex> lock(overlay::feed.m);
    real.assign(overlay::feed.real.begin(), overlay::feed.real.end());
    display = overlay::GraphPlaybackLocked(NowSec());
    generation = overlay::feed.generation;
    refAuto = overlay::GraphAutoRefLocked(NowSec(), display);
    target = overlay::feed.targetMs;
    state = overlay::feed.limitState;
    baseSwing = overlay::feed.baseSwingMs;
  }
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), Col(kCardHi), 10 * scale);
  ImGui::Dummy(size);
  if (real.size() < 5) {
    const char* msg = T("La gráfica aparece al jugar.", "The graph appears while you play.");
    ImVec2 ts = ImGui::CalcTextSize(msg);
    dl->AddText(ImVec2(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f), Col(kDim), msg);
    return;
  }
  const double span = overlay::kPaceSpanSec;
  // Reloj de reproduccion compartido con el overlay: desplazamiento a velocidad constante.
  const bool isTarget = target > 0;
  const double ref = isTarget ? target : refAuto;
  const bool locked = state == overlay::LimitState::Locked;
  const ImVec4 stateCol = locked ? kGreen : kRed;
  float padX = 14 * scale, padY = 16 * scale;
  float x0 = p.x + padX, x1 = p.x + size.x - padX, y0 = p.y + padY, y1 = p.y + size.y - padY;
  const overlay::PaceRange pr = overlay::PaceScale(ref);
  auto Y = [&](double ms) {
    double t = std::clamp((ms - pr.lo) / (pr.hi - pr.lo), 0.0, 1.0);
    return static_cast<float>(y1 - t * (y1 - y0));
  };
  auto X = [&](float xf) { return x0 + xf * (x1 - x0); };
  dl->PushClipRect(ImVec2(x0, p.y), ImVec2(x1, p.y + size.y), true);
  // Rejilla de un segundo que se desplaza con la grafica
  for (double k = std::ceil(display - span); k <= display; k += 1.0) {
    float x = X(static_cast<float>(1.0 - (display - k) / span));
    dl->AddLine(ImVec2(x, p.y + 6 * scale), ImVec2(x, p.y + size.y - 6 * scale), Col(kText, 0.05f), 1.0f);
  }
  float ry = Y(ref);
  for (float x = x0; x < x1; x += 10 * scale)
    dl->AddLine(ImVec2(x, ry), ImVec2(std::min(x + 5 * scale, x1), ry), Col(kText, 0.25f), 1.0f * scale);
  const int cols = std::max(2, static_cast<int>((x1 - x0) / (3.0f * scale)));
  static overlay::PaceClassCache classCache;
  std::vector<overlay::PacePoint> pts = overlay::BuildTimeSeries(real, display, span, cols, ref, classCache, generation);
  const float fillBottom = y1 + padY * 0.6f;
  auto FillAlpha = [&](float y) { return 0.04f + 0.30f * std::clamp((fillBottom - y) / (fillBottom - y0), 0.0f, 1.0f); };
  for (size_t k = 1; k < pts.size(); ++k) {
    ImVec2 a(X(pts[k - 1].x), Y(pts[k - 1].ms)), b(X(pts[k].x), Y(pts[k].ms));
    float top = std::max(a.y, b.y);  // relleno hasta la parte baja del tramo; la linea cubre el resto
    dl->AddRectFilledMultiColor(ImVec2(a.x, top), ImVec2(b.x, fillBottom), Col(stateCol, FillAlpha(top)),
                                Col(stateCol, FillAlpha(top)), Col(stateCol, 0.04f), Col(stateCol, 0.04f));
  }
  if (locked) {  // banda de tolerancia (+-2 ms), solo con el limite confirmado: sobre el relleno
    float bTop = Y(ref + overlay::PaceTolerance(ref)), bBot = Y(std::max(0.0, ref - overlay::PaceTolerance(ref)));
    dl->AddRectFilled(ImVec2(x0, bTop), ImVec2(x1, bBot), Col(stateCol, 0.12f));
    dl->AddLine(ImVec2(x0, bTop), ImVec2(x1, bTop), Col(stateCol, 0.38f), 1.0f);
    dl->AddLine(ImVec2(x0, bBot), ImVec2(x1, bBot), Col(stateCol, 0.38f), 1.0f);
  }
  for (size_t k = 1; k < pts.size(); ++k) {
    const bool spike = pts[k].spike || pts[k - 1].spike;
    dl->AddLine(ImVec2(X(pts[k - 1].x), Y(pts[k - 1].ms)), ImVec2(X(pts[k].x), Y(pts[k].ms)),
                spike ? Col(kAmber) : Col(stateCol), 2.0f * scale);
  }
  dl->PopClipRect();
  std::string refText = isTarget ? Fmt(T("objetivo %.1f ms ±%.1f", "target %.1f ms ±%.1f"), ref, overlay::PaceTolerance(ref))
                                 : Fmt(T("actual %.1f ms ±%.1f", "current %.1f ms ±%.1f"), ref, overlay::PaceTolerance(ref));
  ImVec2 rts = ImGui::CalcTextSize(refText.c_str());
  const float ly = p.y + size.y - rts.y - 8 * scale;
  ImVec2 tp(x1 - rts.x, ly);
  dl->AddRectFilled(ImVec2(tp.x - 6 * scale, tp.y - 2 * scale), ImVec2(tp.x + rts.x + 4 * scale, tp.y + rts.y + 2 * scale),
                    Col(kCard, 0.85f), 5 * scale);
  dl->AddText(tp, Col(kDim), refText.c_str());
  {
    double swing = overlay::RecentSwing(real);
    if (swing >= 0) {
      std::string sw = Fmt(T("variación ±%.1f ms", "swing ±%.1f ms"), swing);
      if (locked && baseSwing > 0) sw += Fmt(T("  ·  antes ±%.1f", "  ·  before ±%.1f"), baseSwing);
      ImVec2 ss = ImGui::CalcTextSize(sw.c_str());
      ImVec2 sp(x0 + 2 * scale, ly);
      dl->AddRectFilled(ImVec2(sp.x - 6 * scale, sp.y - 2 * scale), ImVec2(sp.x + ss.x + 6 * scale, sp.y + ss.y + 2 * scale),
                        Col(kCard, 0.85f), 5 * scale);
      dl->AddText(sp, Col(kText, 0.85f), sw.c_str());
    }
  }
}

// Logo: una f trazada como una grafica, cruzada por la linea plana (variante compacta).
// Diseno en una rejilla de 64; se dibuja en una baldosa redondeada de lado `size`.
static void DrawLogo(ImDrawList* dl, ImVec2 p, float size) {
  dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), Col(Hex(0x12151B)), size * 0.22f);
  dl->AddRect(p, ImVec2(p.x + size, p.y + size), Col(kLine), size * 0.22f, 0, std::max(1.0f, size / 64.0f));
  const float k = size / 64.0f * 0.92f, off = (size - 64.0f * k) * 0.5f;
  auto P = [&](float x, float y) { return ImVec2(p.x + off + x * k, p.y + off + y * k); };
  const float w = 7.0f * k * (size < 40 ? 1.12f : 1.0f);
  const ImU32 stem = Col(Hex(0xE8EBEF)), bar = Col(kBlue);
  dl->PathLineTo(P(12, 52));
  dl->PathLineTo(P(19, 52));
  dl->PathBezierCubicCurveTo(P(24, 52), P(26, 48), P(27, 42));
  dl->PathLineTo(P(31, 20));
  dl->PathBezierCubicCurveTo(P(32, 14), P(35, 12), P(40, 12));
  dl->PathLineTo(P(52, 12));
  dl->PathStroke(stem, 0, w);
  dl->AddCircleFilled(P(12, 52), w * 0.5f, stem);  // extremos redondeados
  dl->AddCircleFilled(P(52, 12), w * 0.5f, stem);
  dl->AddLine(P(12, 32), P(52, 32), bar, w);
  dl->AddCircleFilled(P(12, 32), w * 0.5f, bar);
  dl->AddCircleFilled(P(52, 32), w * 0.5f, bar);
}

// Iconos de navegacion: trazo redondeado sobre una rejilla de 24 (como en la guia de iconos).
static void DrawNavIcon(ImDrawList* dl, int idx, ImVec2 p, float size, ImU32 col) {
  const float k = size / 24.0f, t = 1.8f * k;
  auto P = [&](float x, float y) { return ImVec2(p.x + x * k, p.y + y * k); };
  auto Dot = [&](float x, float y) { dl->AddCircleFilled(P(x, y), t * 0.6f, col); };
  switch (idx) {
    case 0: {  // Ahora: pulso
      ImVec2 pts[] = {P(2, 13), P(6, 13), P(8, 8), P(11, 17), P(13.5f, 11), P(15, 13), P(22, 13)};
      dl->AddPolyline(pts, 7, col, 0, t);
      break;
    }
    case 1: {  // Juegos: mando
      dl->AddRect(P(2.5f, 8), P(21.5f, 18), col, 4.5f * k, 0, t);
      dl->AddLine(P(8, 11), P(8, 15), col, t);
      dl->AddLine(P(6, 13), P(10, 13), col, t);
      Dot(15.5f, 12);
      Dot(17.5f, 14);
      break;
    }
    case 2: {  // Equipo: chip
      dl->AddRect(P(6, 6), P(18, 18), col, 2 * k, 0, t);
      dl->AddRect(P(9.5f, 9.5f), P(14.5f, 14.5f), col, 1 * k, 0, t);
      for (float c : {9.0f, 15.0f}) {
        dl->AddLine(P(c, 2), P(c, 6), col, t);
        dl->AddLine(P(c, 18), P(c, 22), col, t);
        dl->AddLine(P(2, c), P(6, c), col, t);
        dl->AddLine(P(18, c), P(22, c), col, t);
      }
      break;
    }
    default: {  // Registro: lista
      for (float y : {6.0f, 12.0f, 18.0f}) {
        dl->AddLine(P(8, y), P(y == 18.0f ? 17 : 21, y), col, t);
        Dot(3.5f, y);
      }
      break;
    }
  }
}

static bool NavItem(const char* label, int idx) {
  bool sel = page == idx;
  ImVec2 p = ImGui::GetCursorScreenPos();
  float w = ImGui::GetContentRegionAvail().x, h = std::max(40 * scale, fontStrong->FontSize + 18 * scale);
  ImGui::InvisibleButton(label, ImVec2(w, h));
  bool hov = ImGui::IsItemHovered();
  if (ImGui::IsItemClicked()) page = idx;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (sel || hov)
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), Col(sel ? kCardHi : kCard), 10 * scale);
  const float icon = 20 * scale;
  DrawNavIcon(dl, idx, ImVec2(p.x + 14 * scale, p.y + (h - icon) * 0.5f), icon, Col(sel ? kBlue : kDim));
  ImGui::PushFont(fontStrong);
  ImVec2 ts = ImGui::CalcTextSize(label);
  dl->AddText(ImVec2(p.x + 46 * scale, p.y + (h - ts.y) * 0.5f), Col(sel ? kText : kDim), label);
  ImGui::PopFont();
  return sel;
}

static void Section(const char* t) {
  ImGui::PushFont(fontHead);
  ImGui::TextUnformatted(t);
  ImGui::PopFont();
  ImGui::Dummy(ImVec2(0, 4 * scale));
}

static void PrimaryButtonStyle(bool on) {
  if (on) {
    ImGui::PushStyleColor(ImGuiCol_Button, kBlue);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.68f, 1.0f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.08f, 0.52f, 0.9f, 1));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
  }
}
static void PrimaryButtonEnd(bool on) { if (on) ImGui::PopStyleColor(4); }

// Ventana del limite manual: se pide desde cualquier pagina y se abre en la raiz.
struct ManualUi {
  bool request = false;
  std::string exe, label;
  int value = 60;
  int fg = 1;
};
static ManualUi g_manualUi;

static void RequestManual(const std::string& exe, const std::string& label, int value, int fg) {
  g_manualUi.request = true;
  g_manualUi.exe = exe;
  g_manualUi.label = label;
  g_manualUi.value = value > 0 ? value : 60;
  g_manualUi.fg = std::max(1, fg);
}

static void ManualPopup(const View& v) {
  if (g_manualUi.request) {
    ImGui::OpenPopup("manual");
    g_manualUi.request = false;
  }
  if (!ImGui::BeginPopupModal("manual", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
  ManualUi& m = g_manualUi;
  ImGui::PushFont(fontStrong);
  ImGui::Text(T("Límite manual · %s", "Manual limit · %s"), m.label.c_str());
  ImGui::PopFont();
  ImGui::Dummy(ImVec2(0, 4 * scale));
  ImGui::SetNextItemWidth(160 * scale);
  ImGui::InputInt(m.fg > 1 ? T("fps en pantalla", "fps on screen") : "fps", &m.value, 1, 10);
  m.value = std::clamp(m.value, 20, 1000);
  const int k = std::max(1, m.fg);
  const int value = (m.value / k) * k;  // con generacion, multiplo del factor

  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 420 * scale);
  if (k > 1)
    ImGui::TextColored(kDim, T("Con generación de fotogramas ×%d: %d en pantalla = %d reales.",
                               "With frame generation ×%d: %d on screen = %d real."), k, value, value / k);
  const bool vrrOn = v.vrrState == 2 || (v.vrrState == 0 && v.vrrNvKnown && v.vrrPossible);
  if (v.hz > 0) {
    if (vrrOn) {
      double ceilFps = VrrCeiling(v.hz);
      if (value > ceilFps)
        ImGui::TextColored(kAmber, T("Por encima de %.0f, el máximo recomendado con G-SYNC en tu pantalla: en los "
                                     "momentos más rápidos el juego podría salir del rango de G-SYNC.",
                                     "Above %.0f, the recommended maximum with G-SYNC on your display: in the "
                                     "fastest moments the game could leave the G-SYNC range."), ceilFps);
      else
        ImGui::TextColored(kDim, "%s", T("Con G-SYNC, cualquier valor hasta el máximo recomendado da un ritmo regular.",
                                         "With G-SYNC, any value up to the recommended maximum gives a steady pace."));
    } else {
      double q = v.hz / value;
      if (std::fabs(q - std::round(q)) > 0.01) {
        std::string divs;
        for (int n = 1; n <= 8; ++n) {
          double f = v.hz / n;
          if (f < 30) break;
          divs += (divs.empty() ? "" : ", ") + Fmt("%.0f", f);
        }
        ImGui::TextColored(kAmber, T("Sin G-SYNC, %d no divide los %.0f Hz de tu pantalla: el ritmo en pantalla será "
                                     "irregular. Valores que encajan: %s.",
                                     "Without G-SYNC, %d doesn't divide your display's %.0f Hz: the on-screen pace "
                                     "will be uneven. Values that fit: %s."), value, v.hz, divs.c_str());
      } else {
        ImGui::TextColored(kDim, "%s", T("Encaja con la frecuencia de tu pantalla.", "It fits your display's refresh rate."));
      }
    }
  }
  ImGui::TextColored(kDim, "%s", T("La app no cambiará este límite: solo comprobará que funciona. Para volver al modo "
                                   "automático, usa «Deshacer».",
                                   "The app won't change this limit: it only checks that it works. To go back to "
                                   "automatic, use “Undo”."));
  ImGui::PopTextWrapPos();
  ImGui::Dummy(ImVec2(0, 6 * scale));
  PrimaryButtonStyle(true);
  if (ImGui::Button(T("Aplicar", "Apply"))) {
    std::lock_guard<std::mutex> lock(g_manualLimitMutex);
    g_manualLimitReq.push_back({m.exe, value});
    ImGui::CloseCurrentPopup();
  }
  PrimaryButtonEnd(true);
  ImGui::SameLine();
  if (ImGui::Button(T("Cancelar", "Cancel"))) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

static void Popups() {
  if (ImGui::BeginPopupModal("confirmar", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted(T("¿Quitar todos los límites que aplicó FlatPace?", "Remove all the limits FlatPace applied?"));
    ImGui::TextColored(kDim, "%s", T("Cada juego vuelve exactamente a como estaba. La app pasa a solo medir.",
                                     "Each game goes back exactly to how it was. The app switches to measure-only."));
    ImGui::Dummy(ImVec2(0, 4 * scale));
    PrimaryButtonStyle(true);
    if (ImGui::Button(T("Sí, deshacer", "Yes, undo"))) {
      g_undoRequested = true;
      ImGui::CloseCurrentPopup();
    }
    PrimaryButtonEnd(true);
    ImGui::SameLine();
    if (ImGui::Button(T("Cancelar", "Cancel"))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("elegir")) {
    static char buf[128] = "";
    ImGui::TextUnformatted(T("Para juegos en ventana que no se detectan solos:", "For windowed games that aren't detected automatically:"));
    ImGui::SetNextItemWidth(260 * scale);
    ImGui::InputTextWithHint("##exe", "nombre.exe", buf, sizeof(buf));
    if (ImGui::Button(T("Usar este juego", "Use this game")) && buf[0]) {
      std::string e = Lower(Trim(buf));
      if (e.find(".exe") == std::string::npos) e += ".exe";
      std::lock_guard<std::mutex> lock(g_manualMutex);
      g_manualExe = e;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Automático", "Automatic"))) {
      std::lock_guard<std::mutex> lock(g_manualMutex);
      g_manualExe.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

static std::string VrrDescription(const View& v) {
  std::string vrr = v.vrrState == 2   ? T("actúa con el juego", "active with the game")
                    : v.vrrState == 3 ? T("actúa, pero se usan divisores (ajuste avanzado)", "active, but divisors are forced (advanced setting)")
                    : v.vrrState == 1 ? T("no actúa con el juego", "not active with the game")
                    : v.vrrNvKnown    ? (v.vrrPossible ? T("disponible (G-SYNC)", "available (G-SYNC)")
                                                       : T("no disponible", "not available"))
                                      : T("se comprobará al jugar", "checked while playing");
  if (!v.vrrRange.empty()) vrr += "  ·  " + v.vrrRange;
  return vrr;
}

// Etiqueta redondeada con punto y texto, del color indicado (la del estado del limite).
static float PillWidth(const char* text) { return ImGui::CalcTextSize(text).x + 47 * scale; }
static void Pill(const char* text, const ImVec4& c) {
  ImVec2 ts = ImGui::CalcTextSize(text);
  float h = ts.y + 12 * scale, pad = 14 * scale, dot = 8 * scale;
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImVec2 size(pad * 2 + dot + 9 * scale + ts.x, h);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), Col(c, 0.15f), h * 0.5f);
  dl->AddCircleFilled(ImVec2(p.x + pad + dot * 0.5f, p.y + h * 0.5f), dot * 0.5f, Col(c));
  dl->AddText(ImVec2(p.x + pad + dot + 9 * scale, p.y + 6 * scale), Col(c), text);
  ImGui::Dummy(size);
}

// Metrica de la tarjeta principal: etiqueta pequena y valor destacado.
static void Metric(const char* label, const std::string& value) {
  ImGui::BeginGroup();
  ImGui::TextColored(kDim, "%s", label);
  ImGui::PushFont(fontStrong);
  ImGui::TextUnformatted(value.c_str());
  ImGui::PopFont();
  ImGui::EndGroup();
}

static void PageNow(const View& v) {
  const float full = ImGui::GetContentRegionAvail().x;
  const float pad = 22 * scale, gap = 16 * scale;
  // Estado del limite (el mismo que muestra el overlay) y datos para la variacion del ritmo
  overlay::LimitState ls = overlay::LimitState::None;
  std::string limLabel;
  double baseSwing = -1.0;
  std::vector<std::pair<double, double>> frames;
  {
    std::lock_guard<std::mutex> lock(overlay::feed.m);
    ls = overlay::feed.limitState;
    limLabel = overlay::feed.limitLabel;
    baseSwing = overlay::feed.baseSwingMs;
    frames.assign(overlay::feed.real.begin(), overlay::feed.real.end());
  }
  const bool locked = ls == overlay::LimitState::Locked;
  const double swing = overlay::RecentSwing(frames);

  // Cabecera: juego, estado y etiqueta del limite a la derecha
  const ImVec2 h0 = ImGui::GetCursorPos();
  std::string pill;
  ImVec4 pillCol = kRed;
  if (v.haveGame) {
    pill = locked ? std::string(T("FIJADO · ", "LOCKED · ")) + limLabel
           : ls == overlay::LimitState::Pending ? std::string(T("SIN CONFIRMAR · ", "UNCONFIRMED · ")) + limLabel
                                                : std::string(T("SIN LÍMITE", "NO LIMIT"));
    pillCol = locked ? kBlue : kRed;
  }
  const float pillW = v.haveGame ? PillWidth(pill.c_str()) : ImGui::CalcTextSize(StatusWord(v.status)).x + 47 * scale;
  ImGui::PushTextWrapPos(h0.x + full - pillW - gap);
  ImGui::PushFont(fontHead);
  ImGui::TextUnformatted(v.haveGame ? v.game.c_str() : v.statusText.c_str());
  ImGui::PopFont();
  if (v.haveGame) ImGui::TextColored(kDim, "%s", v.statusText.c_str());
  if (!v.statusDetail.empty()) ImGui::TextColored(kDim, "%s", v.statusDetail.c_str());
  ImGui::PopTextWrapPos();
  const float afterHeader = ImGui::GetCursorPosY();
  ImGui::SetCursorPos(ImVec2(h0.x + full - pillW, h0.y + 4 * scale));
  if (v.haveGame) Pill(pill.c_str(), pillCol);
  else StatusPill(v.status);
  ImGui::SetCursorPos(ImVec2(h0.x, std::max(afterHeader, ImGui::GetCursorPosY())));
  if (!v.currentWarning.empty()) {
    ImGui::PushTextWrapPos(h0.x + full);
    ImGui::TextColored(kAmber, "%s", v.currentWarning.c_str());
    ImGui::PopTextWrapPos();
    if (ImGui::SmallButton(T("Ir a Juegos", "Go to Games"))) page = 1;
  }
  ImGui::Dummy(ImVec2(0, 8 * scale));

  // Tarjeta principal: cifra, metricas y grafica
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float lineH = ImGui::GetTextLineHeightWithSpacing();
  const float heroH = std::max(240 * scale, fontNum->FontSize + lineH * 6 + 70 * scale);
  ImVec2 p = ImGui::GetCursorScreenPos();
  dl->AddRectFilled(p, ImVec2(p.x + full, p.y + heroH), Col(kCard), 18 * scale);
  const float leftW = 230 * scale;
  ImGui::SetCursorScreenPos(ImVec2(p.x + pad, p.y + 18 * scale));
  ImGui::BeginGroup();
  ImGui::PushFont(fontNum);
  ImGui::TextUnformatted(v.realFps > 0 ? Fmt("%.0f", v.realFps).c_str() : "—");
  ImGui::PopFont();
  if (v.screenFps > 0)
    ImGui::TextColored(kDim, T("FPS reales · %.0f en pantalla", "real FPS · %.0f on screen"), v.screenFps);
  else
    ImGui::TextColored(kDim, "%s", T("FPS reales", "real FPS"));
  ImGui::Dummy(ImVec2(0, 10 * scale));
  const float mcol = 100 * scale;
  const float mx = ImGui::GetCursorPosX();
  Metric(T("1 % bajo", "1 % low"), v.lowFps > 0 ? Fmt("%.0f", v.lowFps) : "—");
  ImGui::SameLine(mx + mcol);
  Metric(T("Consistencia", "Consistency"), v.realFps > 0 ? Fmt("%.0f %%", v.steadyPct) : "—");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(T("Fotogramas reales cuya duración cambia menos de 2 ms respecto al anterior.\n"
                        "Variación media: %.2f ms. Cuanto más alto el porcentaje, más regular el ritmo.",
                        "Real frames whose duration changes by less than 2 ms from the previous one.\n"
                        "Average change: %.2f ms. The higher the percentage, the steadier the pace."),
                      v.jitterMs);
  ImGui::Dummy(ImVec2(0, 6 * scale));
  Metric(T("Tirones", "Stutters"), Fmt("%d", v.hitches));
  ImGui::SameLine(mx + mcol);
  Metric(T("Variación", "Swing"), swing >= 0 ? Fmt("±%.1f ms", swing) : "—");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", baseSwing > 0 ? Fmt(T("Variación del ritmo en los últimos 10 s. Sin límite: ±%.1f ms.",
                                                  "Pace swing over the last 10 s. Without a limit: ±%.1f ms."), baseSwing).c_str()
                                          : T("Variación del ritmo en los últimos 10 s.", "Pace swing over the last 10 s."));
  ImGui::EndGroup();
  ImGui::SetCursorScreenPos(ImVec2(p.x + leftW, p.y + 14 * scale));
  PacingGraph(ImVec2(full - leftW - 14 * scale, heroH - 28 * scale));
  ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + heroH + gap));

  // Tarjetas inferiores: Limite y Detalles, de la misma altura
  const float cardW = (full - gap) * 0.5f;
  const ImVec2 top = ImGui::GetCursorScreenPos();
  dl->ChannelsSplit(2);
  dl->ChannelsSetCurrent(1);
  auto CardTitle = [&](const char* t) {
    ImGui::PushFont(fontHead);
    ImGui::TextUnformatted(t);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 2 * scale));
  };
  float bottomA, bottomB;
  {  // Limite
    ImGui::SetCursorScreenPos(ImVec2(top.x + pad, top.y + 18 * scale));
    ImGui::BeginGroup();
    const float lx = ImGui::GetCursorPosX();
    ImGui::PushTextWrapPos(lx + cardW - pad * 2);
    CardTitle(T("Límite", "Limit"));
    const float k1 = lx + 120 * scale;
    const bool applied = v.appliedText != T("ninguno", "none");
    KV(T("Aplicado", "Applied"), v.appliedText, k1, applied ? &kBlue : nullptr);
    KV(T("Recomendado", "Recommended"), v.recText.empty() ? "—" : v.recText, k1);
    if (v.haveGame) {
      const char* st = locked ? T("comprobado y aceptado", "checked and accepted")
                       : ls == overlay::LimitState::Pending ? T("pendiente de comprobar", "waiting to be checked")
                                                            : T("sin límite todavía", "no limit yet");
      KV(T("Estado", "Status"), st, k1, locked ? &kBlue : &kRed);
    }
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, 6 * scale));
    bool can = v.haveGame && g_applyMode && !applied && !v.recText.empty() &&
               v.recText.find("—") == std::string::npos && v.recText != T("midiendo…", "measuring…");
    ImGui::BeginDisabled(!can);
    PrimaryButtonStyle(can);
    if (ImGui::Button(T("Aplicar ya", "Apply now"))) g_applyNowRequest = true;
    PrimaryButtonEnd(can);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("%s", T("Guarda ya la recomendación actual (luego reinicia el juego). Si no, la app\n"
                                "decide con toda la partida al cerrar el juego.",
                                "Saves the current recommendation now (then restart the game). Otherwise the\n"
                                "app decides with the whole session when you close the game."));
    ImGui::SameLine();
    ImGui::BeginDisabled(!v.haveGame || v.exe.empty());
    if (ImGui::Button(T("Límite manual…", "Manual limit…"))) {
      int k = v.screenFps > v.realFps * 1.4 && v.realFps > 0 ? static_cast<int>(std::lround(v.screenFps / v.realFps)) : 1;
      int def = 0;
      for (const auto& c : v.changes)
        if (c.exe == v.exe) { def = c.value; k = c.fg; }
      if (def == 0) def = static_cast<int>(std::lround(v.realFps > 0 ? v.realFps * k : 60));
      RequestManual(v.exe, v.game, def, k);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("%s", T("Pon tú el límite de este juego. La app no lo cambiará.",
                                "Set this game's limit yourself. The app won't change it."));
    bool hasChange = false;
    for (const auto& c : v.changes) hasChange = hasChange || c.exe == v.exe;
    if (v.haveGame && hasChange) {
      ImGui::SameLine();
      if (ImGui::Button(T("Deshacer", "Undo"))) {
        std::lock_guard<std::mutex> lock(g_undoOneMutex);
        g_undoOne.push_back(v.exe);
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Devuelve este juego a como estaba; se recalculará en la próxima partida.",
                                  "Returns this game to how it was; it will be recalculated next session."));
    }
    ImGui::EndGroup();
    bottomA = ImGui::GetItemRectMax().y;
  }
  {  // Detalles
    ImGui::SetCursorScreenPos(ImVec2(top.x + cardW + gap + pad, top.y + 18 * scale));
    ImGui::BeginGroup();
    const float lx = ImGui::GetCursorPosX();
    ImGui::PushTextWrapPos(lx + cardW - pad * 2);
    CardTitle(T("Detalles", "Details"));
    const float k2 = lx + 140 * scale;
    KV("VRR", VrrDescription(v), k2, v.vrrState == 2 ? &kBlue : nullptr);
    KV(T("Presentación", "Presentation"), v.presentText.empty() ? "—" : v.presentText, k2);
    KV(T("Límite del juego", "Game's own limit"), v.capText.empty() ? "—" : v.capText, k2);
    if (!v.bottleneck.empty()) KV(T("Cuello de botella", "Bottleneck"), v.bottleneck, k2);
    KV(T("Pantalla", "Display"), v.monitor.empty() ? "—" : v.monitor, k2);
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    bottomB = ImGui::GetItemRectMax().y;
  }
  const float cardH = std::max(bottomA, bottomB) + 18 * scale - top.y;
  dl->ChannelsSetCurrent(0);
  dl->AddRectFilled(top, ImVec2(top.x + cardW, top.y + cardH), Col(kCard), 18 * scale);
  dl->AddRectFilled(ImVec2(top.x + cardW + gap, top.y), ImVec2(top.x + full, top.y + cardH), Col(kCard), 18 * scale);
  dl->ChannelsMerge();
  ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + cardH + 10 * scale));
  ImGui::Dummy(ImVec2(full, 0));
  if (!v.notice.empty()) {
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + full);
    ImGui::TextColored(kBlue, "%s", v.notice.c_str());
    ImGui::PopTextWrapPos();
  }
}

static void PageGames(const View& v) {
  Section(T("Prioridad", "Priority"));
  {
    int pr = g_priority;
    const char* names[2] = {T("Estabilidad", "Stability"), T("Fluidez", "Smoothness")};
    for (int i = 0; i <= 1; ++i) {  // Estabilidad primero (predeterminada)
      if (i == 1) ImGui::SameLine(0, 4 * scale);
      bool sel = pr == i;
      PrimaryButtonStyle(sel);
      if (ImGui::Button(names[i], ImVec2(140 * scale, 0)) && !sel) {
        g_priority = i;
        WriteKv(L"equipo.txt", "prioridad", i == 1 ? "fluidez" : "estabilidad");
      }
      PrimaryButtonEnd(sel);
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x * 0.85f);
    ImGui::TextColored(kDim, "%s", pr == 1
        ? T("Fluidez: el límite más alto que el juego alcanza la mayor parte del tiempo, aceptando "
            "pequeñas irregularidades en los momentos más exigentes.",
            "Smoothness: the highest limit the game reaches most of the time, accepting small "
            "irregularities in the most demanding moments.")
        : T("Estabilidad (recomendada): un límite que el juego sostiene siempre, incluso en sus peores "
            "momentos. El ritmo más regular, aunque a veces algo más bajo.",
            "Stability (recommended): a limit the game always holds, even in its worst moments. The "
            "steadiest pace, though sometimes a bit lower."));
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, 4 * scale));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x * 0.85f);
    ImGui::TextColored(kDim, "%s", T("Los límites ya aplicados no cambian solos: para recalcular un juego con el "
                                     "nuevo criterio, usa su botón «Deshacer».",
                                     "Limits already applied don't change on their own: to recalculate a game "
                                     "with the new criterion, use its “Undo” button."));
    ImGui::PopTextWrapPos();
  }
  ImGui::Dummy(ImVec2(0, 14 * scale));
  Section(T("Juegos con límite", "Games with a limit"));
  if (v.changes.empty()) {
    ImGui::TextColored(kDim, "%s", T("Todavía no hay límites aplicados. Abre un juego con «Aplicar límites» "
                                     "activado.",
                                     "No limits applied yet. Open a game with “Apply limits” on."));
  } else {
    for (const auto& c : v.changes) {
      ImGui::PushID(c.exe.c_str());
      ImVec2 p = ImGui::GetCursorScreenPos();
      float w = ImGui::GetContentRegionAvail().x;
      float line = ImGui::GetTextLineHeightWithSpacing();
      float wrapW = w - 160 * scale;
      float warnH = c.warning.empty() ? 0.0f
                    : ImGui::CalcTextSize(c.warning.c_str(), nullptr, false, wrapW).y + 6 * scale;
      float h = line * 2 + warnH + 22 * scale;
      ImDrawList* dl = ImGui::GetWindowDrawList();
      dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), Col(kCard), 10 * scale);
      if (!c.warning.empty())
        dl->AddRectFilled(ImVec2(p.x, p.y + 10 * scale), ImVec2(p.x + 3 * scale, p.y + h - 10 * scale), Col(kAmber), 2 * scale);
      ImGui::SetCursorScreenPos(ImVec2(p.x + 16 * scale, p.y + 11 * scale));
      ImGui::BeginGroup();
      ImGui::PushFont(fontStrong);
      ImGui::TextUnformatted(c.label.c_str());
      ImGui::PopFont();
      ImGui::TextColored(kDim, "%s", c.desc.c_str());
      if (!c.warning.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapW);
        ImGui::TextColored(kAmber, "%s", c.warning.c_str());
        ImGui::PopTextWrapPos();
      }
      ImGui::EndGroup();
      float bw = ImGui::CalcTextSize(T("Deshacer", "Undo")).x + ImGui::GetStyle().FramePadding.x * 2;
      float mw = ImGui::CalcTextSize(T("Fijar…", "Set…")).x + ImGui::GetStyle().FramePadding.x * 2;
      ImGui::SetCursorScreenPos(ImVec2(p.x + w - bw - mw - 22 * scale, p.y + (h - ImGui::GetFrameHeight()) * 0.5f));
      if (ImGui::Button(T("Fijar…", "Set…"))) RequestManual(c.exe, c.label, c.value, c.fg);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Pon tú el límite de este juego. La app no lo cambiará.",
                                  "Set this game's limit yourself. The app won't change it."));
      ImGui::SetCursorScreenPos(ImVec2(p.x + w - bw - 14 * scale, p.y + (h - ImGui::GetFrameHeight()) * 0.5f));
      PrimaryButtonStyle(!c.warning.empty());
      if (ImGui::Button(T("Deshacer", "Undo"))) {
        std::lock_guard<std::mutex> lock(g_undoOneMutex);
        g_undoOne.push_back(c.exe);
      }
      PrimaryButtonEnd(!c.warning.empty());
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Devuelve este juego a como estaba. Si «Aplicar límites» está activado,\n"
                                  "la app lo recalculará la próxima vez que juegues.",
                                  "Returns this game to how it was. If “Apply limits” is on,\n"
                                  "the app will recalculate it the next time you play."));
      ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + 8 * scale));
      ImGui::PopID();
    }
  }
  ImGui::Dummy(ImVec2(0, 10 * scale));
  ImGui::BeginDisabled(v.changes.empty());
  if (ImGui::Button(T("Deshacer todos los cambios", "Undo all changes"))) ImGui::OpenPopup("confirmar");
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button(T("Elegir juego…", "Choose game…"))) ImGui::OpenPopup("elegir");
  if (!v.notice.empty()) {
    ImGui::Dummy(ImVec2(0, 6 * scale));
    ImGui::TextColored(kGreen, "%s", v.notice.c_str());
  }
  Popups();
  ManualPopup(v);
}

static void PageSystem(const View& v) {
  Section(T("Equipo", "System"));
  float k = 150 * scale;
  KV(T("Gráfica", "GPU"), v.gpu.empty() ? "…" : v.gpu, k);
  KV(T("Pantalla", "Display"), v.monitor.empty() ? T("midiendo…", "measuring…") : v.monitor, k);
  KV("Driver", v.driver.empty() ? "…" : v.driver, k, v.driverOk ? &kGreen : nullptr);
  KV("VRR", VrrDescription(v), k, v.vrrState == 2 ? &kGreen : nullptr);
  if (!v.tip.empty()) {
    ImGui::Dummy(ImVec2(0, 10 * scale));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x * 0.8f);
    ImGui::TextColored(kAmber, "%s", v.tip.c_str());
    ImGui::PopTextWrapPos();
  }
}

static void PageLog(const View& v) {
  Section(T("Registro", "Log"));
  if (ImGui::Button(T("Abrir el archivo", "Open the file")) && !v.logPath.empty())
    ShellExecuteW(nullptr, L"open", v.logPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  ImGui::SameLine();
  bool rec = g_recordFrames;
  if (ImGui::Checkbox(T("Grabar los datos de cada fotograma", "Record per-frame data"), &rec)) {
    g_recordFrames = rec;
    WriteKv(L"equipo.txt", "grabar-fotogramas", rec ? "si" : "no");
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T("Guarda un archivo CSV por partida con cada fotograma medido, para analizarlo.\n"
                              "Unos 10 MB por cada 10 minutos a 240 FPS. Empieza con la próxima partida.",
                              "Saves a CSV file per session with every measured frame, for analysis.\n"
                              "About 10 MB per 10 minutes at 240 FPS. Starts with the next session."));
  ImGui::SameLine();
  if (ImGui::Button(T("Abrir grabaciones", "Open recordings")))
    ShellExecuteW(nullptr, L"explore", RecordingsDirectory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  ImGui::Dummy(ImVec2(0, 6 * scale));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
  ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_HorizontalScrollbar);
  for (const auto& line : v.log) ImGui::TextColored(kDim, "%s", line.c_str());
  if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
  ImGui::EndChild();
  ImGui::PopStyleColor();
}

static void Draw() {
  View v = Snapshot();
  ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::Begin("FlatPace", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImGui::PopStyleVar();

  // Barra lateral
  float sideW = 220 * scale;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kSide);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * scale, 18 * scale));
  ImGui::BeginChild("side", ImVec2(sideW, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
  {
    // Cabecera: logo y nombre ("Flat" en claro, "Pace" en azul)
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float logo = 40 * scale;
    DrawLogo(dl, ImVec2(p.x + 4 * scale, p.y), logo);
    ImGui::PushFont(fontHead);
    float tx = p.x + 4 * scale + logo + 12 * scale, ty = p.y + 1 * scale;
    dl->AddText(ImVec2(tx, ty), Col(kText), "Flat");
    dl->AddText(ImVec2(tx + ImGui::CalcTextSize("Flat").x, ty), Col(kBlue), "Pace");
    float th = ImGui::GetFontSize();
    ImGui::PopFont();
    dl->AddText(ImVec2(tx, ty + th + 1 * scale), Col(kDim), ("v" + std::string(kVersion)).c_str());
    ImGui::Dummy(ImVec2(0, std::max(logo, th + ImGui::GetFontSize() + 2 * scale)));
  }
  ImGui::Dummy(ImVec2(0, 18 * scale));
  NavItem(T("Ahora", "Now"), 0);
  NavItem(T("Juegos", "Games"), 1);
  NavItem(T("Equipo", "System"), 2);
  NavItem(T("Registro", "Log"), 3);

  // Ajustes al pie de la barra
  // Altura real de los tres controles del pie (dependen de la escala de pantalla).
  const ImGuiStyle& st = ImGui::GetStyle();
  float footH = ImGui::GetFrameHeightWithSpacing() * 3 + 4 * scale + st.WindowPadding.y;
  ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY() + 8 * scale, ImGui::GetWindowHeight() - footH));
  bool apply = g_applyMode;
  ImGui::BeginDisabled(!v.driverOk);
  if (ImGui::Checkbox(T("Aplicar límites", "Apply limits"), &apply)) g_applyMode = apply;
  ImGui::EndDisabled();
  if (!v.driverOk && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("%s", T("Hace falta una tarjeta NVIDIA. Por ahora solo se puede medir.",
                              "An NVIDIA GPU is required. For now it can only measure."));
  bool ovOn = overlay::enabled;
  if (ImGui::Checkbox(T("Overlay en el juego", "In-game overlay"), &ovOn)) {
    overlay::enabled = ovOn;
    WriteKv(L"equipo.txt", "overlay", ovOn ? "si" : "no");
  }
  ImGui::Dummy(ImVec2(0, 4 * scale));
  for (int li = 0; li < 2; ++li) {
    if (li) ImGui::SameLine(0, 4 * scale);
    bool sel = g_lang == li;
    PrimaryButtonStyle(sel);
    if (ImGui::Button(li == 0 ? "ES" : "EN", ImVec2(44 * scale, 0)) && !sel) {
      g_lang = li;
      WriteKv(L"equipo.txt", "idioma", li == 0 ? "es" : "en");
    }
    PrimaryButtonEnd(sel);
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();

  // Contenido
  ImGui::SameLine(0, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(30 * scale, 26 * scale));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kBg);
  ImGui::BeginChild("main", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
  switch (page) {
    case 0: PageNow(v); break;
    case 1: PageGames(v); break;
    case 2: PageSystem(v); break;
    default: PageLog(v); break;
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
  ImGui::End();
}
}  // namespace gui


int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
  {
    if (ReadKv(L"equipo.txt", "prioridad-v2") != "1") {
      WriteKv(L"equipo.txt", "prioridad", "estabilidad");
      WriteKv(L"equipo.txt", "prioridad-v2", "1");
    }
    g_priority = ReadKv(L"equipo.txt", "prioridad") == "fluidez" ? 1 : 0;
    if (ReadKv(L"equipo.txt", "divisores-siempre") == "si") g_useVrr = false;
    if (ReadKv(L"equipo.txt", "overlay") == "no") overlay::enabled = false;
    if (ReadKv(L"equipo.txt", "grabar-fotogramas") == "si") g_recordFrames = true;
    std::string lang = ReadKv(L"equipo.txt", "idioma");
    if (lang == "en") g_lang = 1;
    else if (lang.empty()) g_lang = (PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_SPANISH) ? 0 : 1;
  }
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  bool undo = false;
  std::wstring presentMonPath;
  for (int i = 1; i < argc; ++i) {
    std::wstring a = argv[i];
    if (a == L"--deshacer") undo = true;
    else if (a == L"--presentmon" && i + 1 < argc) presentMonPath = argv[++i];
  }
  if (argv) LocalFree(argv);

  // "Deshacer cambios.bat": sin ventana, solo el resultado.
  if (undo) {
    OpenLog();
    NvDriver nvd;
    bool ok = nvd.Init();
    ChangeStore store(DataDirectory() + L"\\cambios-aplicados.txt");
    store.Load();
    store.Migrate(ExeDirectory() + L"\\cambios-aplicados.txt");
    std::string r = UndoAll(nvd, store, ok);
    MessageBoxW(nullptr, Widen(r).c_str(), L"FlatPace", MB_OK | MB_ICONINFORMATION);
    if (g_log) std::fclose(g_log);
    return 0;
  }
  if (presentMonPath.empty()) presentMonPath = FindPresentMon();

  ImGui_ImplWin32_EnableDpiAwareness();
  WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, gui::WndProc, 0, 0, hInst, LoadIconW(hInst, MAKEINTRESOURCEW(1)),
                 LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, L"FlatPaceWnd",
                 static_cast<HICON>(LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), 0))};
  RegisterClassExW(&wc);
  POINT origin{0, 0};
  gui::scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY));
  if (gui::scale <= 0) gui::scale = 1.0f;
  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"FlatPace", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, static_cast<int>(1040 * gui::scale),
                              static_cast<int>(680 * gui::scale), nullptr, nullptr, hInst, nullptr);
  BOOL dark = TRUE;
  DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
  // Windows 11: barra de titulo del color de la ventana (en Windows 10 se ignora).
  COLORREF caption = RGB(0x0E, 0x10, 0x15), captionText = RGB(0xEC, 0xEE, 0xF1);
  DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
  DwmSetWindowAttribute(hwnd, 36 /* DWMWA_TEXT_COLOR */, &captionText, sizeof(captionText));
  if (!gui::CreateDevice(hwnd)) {
    MessageBoxW(nullptr, g_lang == 1 ? L"Couldn't start DirectX 11." : L"No pude iniciar DirectX 11.",
                L"FlatPace", MB_OK | MB_ICONERROR);
    return 1;
  }
  ShowWindow(hwnd, SW_SHOWDEFAULT);
  UpdateWindow(hwnd);

  IMGUI_CHECKVERSION();
  ImGuiContext* mainCtx = ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  gui::ApplyTheme();
  gui::LoadFonts();
  ImGui_ImplWin32_Init(hwnd);
  ImGui_ImplDX11_Init(gui::dev, gui::ctx);

  overlay::Window ov;
  ov.Create(gui::scale);
  std::thread engine(RunEngine, presentMonPath);

  bool running = true;
  double lastUiDraw = 0.0;
  while (running) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
      if (msg.message == WM_QUIT) running = false;
    }
    if (!running) break;
    ov.Update();
    ImGui::SetCurrentContext(mainCtx);
    // La ventana se dibuja a ~60 fps en primer plano y a ~10 fps detras; el overlay, siempre a
    // 60 fps aunque la ventana este minimizada o detras del juego.
    const bool mineFront = GetForegroundWindow() == hwnd;
    const double tNow = NowSec();
    if (IsIconic(hwnd) || (!mineFront && tNow - lastUiDraw < 0.1)) {
      Sleep(ov.Visible() ? 4 : 30);
      continue;
    }
    lastUiDraw = tNow;
    if (gui::resizeW != 0 && gui::resizeH != 0) {
      gui::CleanupRenderTarget();
      gui::swap->ResizeBuffers(0, gui::resizeW, gui::resizeH, DXGI_FORMAT_UNKNOWN, 0);
      gui::resizeW = gui::resizeH = 0;
      gui::CreateRenderTarget();
    }
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    gui::Draw();
    ImGui::Render();
    const float clear[4] = {gui::kBg.x, gui::kBg.y, gui::kBg.z, 1.0f};
    gui::ctx->OMSetRenderTargets(1, &gui::rtv, nullptr);
    gui::ctx->ClearRenderTargetView(gui::rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    gui::swap->Present(mineFront ? 1 : 0, 0);
    Sleep(ov.Visible() ? 4 : (mineFront ? 12 : 30));
  }

  g_quit = true;
  ov.Destroy();
  engine.join();
  ImGui::SetCurrentContext(mainCtx);
  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  gui::CleanupDevice();
  DestroyWindow(hwnd);
  UnregisterClassW(wc.lpszClassName, wc.hInstance);
  if (g_log) std::fclose(g_log);
  return 0;
}
