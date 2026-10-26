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
#include <dcomp.h>
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
#include <ctime>
#include <deque>
#include <limits>
#include <map>
#include <string_view>
#include <unordered_map>
#include <mutex>
#include <pdh.h>
#include <tlhelp32.h>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "version.lib")
#pragma comment(lib, "pdh.lib")
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

static constexpr const char* kVersion = "0.67.3";

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

// ---------------------------------------------------------------------------
// Que FlatPace no compita con el juego. Con las grabaciones se vio que los fotogramas que el juego
// calcula justo despues de cada tanda de datos (cada ~1 s) salen mas lentos (Greylock: 1,6-2,1 veces
// mas fotogramas lentos; en Space Marine 2, limitado por la CPU, la generacion de fotogramas se
// saltaba imagenes: 44 % de generadas frente a 48 %). FlatPace baja su prioridad y pide a Windows 11
// el modo de eficiencia (EcoQoS: nucleos de eficiencia y frecuencias bajas). Quedan sin modo de
// eficiencia, pero con prioridad baja, el hilo que lee PresentMon y el que dibuja la ventana y el
// overlay. PresentMon tambien funciona con prioridad baja y modo de eficiencia (desde la 0.42.1).
// ---------------------------------------------------------------------------
namespace eco {
struct PowerThrottlingState {
  ULONG Version, ControlMask, StateMask;
};
constexpr ULONG kExecutionSpeed = 0x1;
using SetProcInfoFn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
using SetThreadInfoFn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);

static void LowerProcess() {
  SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
  auto fn = reinterpret_cast<SetProcInfoFn>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation")));
  PowerThrottlingState st{1, kExecutionSpeed, kExecutionSpeed};
  if (fn) fn(GetCurrentProcess(), 4 /*ProcessPowerThrottling*/, &st, sizeof(st));
}

// Otro proceso (PresentMon): modo de eficiencia (la prioridad baja se da al crearlo).
static void LowerOtherProcess(HANDLE proc) {
  auto fn = reinterpret_cast<SetProcInfoFn>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation")));
  PowerThrottlingState st{1, kExecutionSpeed, kExecutionSpeed};
  if (fn) fn(proc, 4 /*ProcessPowerThrottling*/, &st, sizeof(st));
}

// El hilo actual queda sin modo de eficiencia, con la prioridad baja del proceso: el que lee
// PresentMon (para no retrasarse) y el que dibuja la ventana y el overlay (para que no vaya a tirones).
// El juego sigue teniendo preferencia sobre ambos.
static void ExemptCurrentThread() {
  auto fn = reinterpret_cast<SetThreadInfoFn>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadInformation")));
  PowerThrottlingState st{1, kExecutionSpeed, 0};
  if (fn) fn(GetCurrentThread(), 3 /*ThreadPowerThrottling*/, &st, sizeof(st));
}

// Proceso de PresentMon (para medir su coste) y tiempo de dibujo del overlay (suma y fotogramas).
static std::atomic<HANDLE> presentMonProcess{nullptr};
static std::atomic<double> overlayDrawMsSum{0.0};
static std::atomic<long long> overlayDraws{0};
// Desde la 0.65.1: el trabajo completo de cada dibujo del overlay (con la preparacion de los datos, que
// overlayDrawMsSum no incluye) y los dibujos de la ventana principal.
static std::atomic<double> overlayFullMsSum{0.0};
static std::atomic<double> windowDrawMsSum{0.0};
static std::atomic<long long> windowDraws{0};

// Tiempo de CPU (s) consumido por un proceso (por defecto, todo FlatPace).
// Tiempo de procesador de cada hilo (0.65.2): el de todo el proceso no decia donde se iba. Cada hilo
// guarda aqui su identificador al arrancar.
static std::atomic<HANDLE> mainThread{nullptr}, engineThread{nullptr}, readerThread{nullptr};
static std::atomic<long long> pmLines{0}, pmOwnLines{0};  // lineas de PresentMon; las de FlatPace se descartan
// Si PresentMon se reinicia, su proceso y el hilo lector son otros: se guarda lo que consumieron los
// anteriores (0.66.1; antes la cuenta restaba el antiguo al nuevo y podia salir negativa).
static std::atomic<double> pmCpuDone{0.0}, readerCpuDone{0.0};
// Momento de la ultima linea de PresentMon, de cualquier programa (tambien las del propio FlatPace que se
// descartan): la vigilancia la usa para saber si PresentMon sigue enviando.
static std::atomic<double> pmLastLineAt{0.0};
static HANDLE OpenCurrentThread() {
  return OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentThreadId());
}
static double ThreadCpuSec(HANDLE h) {
  FILETIME c, e, k, u;
  if (!h || !GetThreadTimes(h, &c, &e, &k, &u)) return 0.0;
  auto toSec = [](const FILETIME& f) {
    return (static_cast<double>(f.dwHighDateTime) * 4294967296.0 + f.dwLowDateTime) * 1e-7;
  };
  return toSec(k) + toSec(u);
}

static double ProcessCpuSec(HANDLE proc = GetCurrentProcess()) {
  FILETIME c, e, k, u;
  if (!proc || !GetProcessTimes(proc, &c, &e, &k, &u)) return 0.0;
  auto toSec = [](const FILETIME& f) {
    return (static_cast<double>(f.dwHighDateTime) * 4294967296.0 + f.dwLowDateTime) * 1e-7;
  };
  return toSec(k) + toSec(u);
}
static double PresentMonCpuTotal() { return pmCpuDone.load() + ProcessCpuSec(presentMonProcess.load()); }
static double ReaderCpuTotal() { return readerCpuDone.load() + ThreadCpuSec(readerThread.load()); }
}  // namespace eco


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
    // 1 MB de bufer: PresentMon no se bloquea aunque el lector (con prioridad baja) tarde un poco.
    if (!CreatePipe(&read_, &writeEnd, &sa, 1 << 20)) return false;
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
           L" --session_name FlatPace --track_frame_type --track_pc_latency"
           // Sin seguimiento de entrada (raton y teclado): FlatPace no usa esos datos, y un raton de
           // juego genera cientos o miles de eventos por segundo que PresentMon tendria que procesar.
           L" --no_track_input";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si, &pi);
    CloseHandle(writeEnd);
    if (!ok) {
      CloseHandle(read_);
      read_ = nullptr;
      return false;
    }
    AssignProcessToJobObject(job_, pi.hProcess);
    // PresentMon tambien con prioridad baja y modo de eficiencia: Windows le entrega los eventos en
    // rafagas cada ~1 s, y procesarlas con prioridad normal competia con el juego justo en ese
    // instante. Mientras espera, los eventos quedan en los bufer de Windows (si se perdieran,
    // PresentMon lo avisaria y el aviso aparece en el registro).
    eco::LowerOtherProcess(pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    eco::presentMonProcess = pi.hProcess;
    running_ = true;
    thread_ = std::thread([this] {
      eco::ExemptCurrentThread();
      if (HANDLE old = eco::readerThread.exchange(eco::OpenCurrentThread())) CloseHandle(old);
      ReadLoop();
    });
    return true;
  }

  void Stop() {
    if (job_) TerminateJobObject(job_, 0);
    StopEtwSession(kEtwSession);  // no dejar la sesion de captura abierta
    if (thread_.joinable()) thread_.join();
    // Lo que consumieron este PresentMon y su lector se guarda antes de soltarlos (si se reinicia).
    if (HANDLE h = eco::readerThread.exchange(nullptr)) {
      eco::readerCpuDone = eco::readerCpuDone.load() + eco::ThreadCpuSec(h);
      CloseHandle(h);
    }
    if (process_ && eco::presentMonProcess.load() == process_) {
      eco::pmCpuDone = eco::pmCpuDone.load() + eco::ProcessCpuSec(process_);
      eco::presentMonProcess = nullptr;
    }
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
  std::vector<std::string> cols_;  // columnas de la linea en curso (se reutiliza)
  void ReadLoop() {
    std::string buffer;
    char chunk[4096];
    DWORD n = 0;
    std::string line;
    while (ReadFile(read_, chunk, sizeof(chunk), &n, nullptr) && n > 0) {
      buffer.append(chunk, n);
      // Las lineas completas del bloque, y lo leido se quita una sola vez (antes, en cada linea, lo
      // que movia todo lo que quedaba detras).
      size_t start = 0, pos;
      while ((pos = buffer.find('\n', start)) != std::string::npos) {
        line.assign(buffer, start, pos - start);
        start = pos + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        HandleLine(line);
      }
      buffer.erase(0, start);
    }
    running_ = false;
  }

  static std::vector<std::string> SplitCsv(const std::string& line) {
    std::vector<std::string> cols;
    SplitCsvInto(line, cols);
    return cols;
  }
  // Igual, reutilizando la memoria de la linea anterior (0.65.2).
  static void SplitCsvInto(const std::string& line, std::vector<std::string>& cols) {
    size_t i = 0, start = 0;
    while (true) {
      const size_t comma = line.find(',', start);
      const size_t len = comma == std::string::npos ? std::string::npos : comma - start;
      if (i == cols.size()) cols.emplace_back();
      cols[i++].assign(line, start, len);
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
    cols.resize(i);
  }
  // Proceso de una linea sin dividirla entera (para descartar pronto las del propio FlatPace).
  static long long FieldNumber(const std::string& line, int col) {
    size_t start = 0;
    for (int i = 0; i < col; ++i) {
      start = line.find(',', start);
      if (start == std::string::npos) return -1;
      ++start;
    }
    return std::strtoll(line.c_str() + start, nullptr, 10);
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
    eco::pmLastLineAt = NowSec();
    if (headerParsed_) {
      ++eco::pmLines;
      // Las imagenes del propio FlatPace (el overlay presenta 60 por segundo, y la ventana): nada las
      // usa, y analizarlas costaba tanto como las del juego.
      if (colPid_ >= 0 && FieldNumber(line, colPid_) == static_cast<long long>(GetCurrentProcessId())) {
        ++eco::pmOwnLines;
        return;
      }
    }
    SplitCsvInto(line, cols_);
    const std::vector<std::string>& cols = cols_;

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
        colPcLat_ = FindColumn(cols, {"mspclatency", "pclatency"});
        colGpuLat_ = FindColumn(cols, {"msgpulatency", "gpulatency"});
        headerParsed_ = true;
        auto name = [&](int c) { return c >= 0 ? Trim(cols[c]) : std::string(T("no disponible", "not available")); };
        Message(T("Columnas de PresentMon: fotograma=", "PresentMon columns: frame=") +
                (colFrame_ >= 0 ? Trim(cols[colFrame_]) : std::string(T("NO ENCONTRADA", "NOT FOUND"))) +
                ", CPU=" + name(colCpu_) + ", GPU=" + name(colGpu_) +
                T(", tipo=", ", type=") + name(colType_) + T(", simulacion=", ", simulation=") + name(colSim_) +
                T(", animacion=", ", animation=") + name(colAnim_) + T(", latencia=", ", latency=") + name(colPcLat_));
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
    if (HasCol(colPcLat_, cols) && ParseNumber(cols[colPcLat_], v) && v > 0) s.pcLatencyMs = v;
    if (HasCol(colGpuLat_, cols) && ParseNumber(cols[colGpuLat_], v) && v >= 0) s.gpuQueueMs = v;
    if (HasCol(colPid_, cols) && ParseNumber(cols[colPid_], v)) s.pid = static_cast<DWORD>(v);
    s.swapchain = std::to_string(s.pid) + ":" + (HasCol(colSwap_, cols) ? Trim(cols[colSwap_]) : "");
    if (HasCol(colType_, cols)) {
      std::string type = Lower(Trim(cols[colType_]));
      s.generated = !IsRealFrameType(type);
      if (seenTypes_.insert(type).second && !type.empty())
        Message(T("Tipo de fotograma visto: ", "Frame type seen: ") + Trim(cols[colType_]) +
                (s.generated ? T(" (generado)", " (generated)") : T(" (real)", " (real)")));
    }

    // Marcadores de simulacion (NVIDIA Reflex / PC Latency): cada fotograma real
    // tiene uno; los generados no. Si el juego los emite, son la mejor referencia.
    double sim = 0.0;
    bool simValid = HasCol(colSim_, cols) && ParseNumber(cols[colSim_], sim) && sim > 0.0;
    int& simCount = simSeen_[s.pid];
    if (simValid) {
      if (++simCount == kSimTrustFrames)
        Message(T("Marcadores de simulacion (Reflex) disponibles en este juego: los uso para\n"
                  "  separar fotogramas reales y generados.",
                  "Simulation markers (Reflex) available in this game: I use them to\n"
                  "  separate real and generated frames."));
    }
    bool simTrusted = simCount >= kSimTrustFrames;
    // Si los marcadores desaparecen, dejar de confiar en ellos: con generacion de fotogramas nunca hay
    // mas de 3 generados seguidos (modo x4), asi que una racha mas larga significa que el juego dejo de
    // emitirlos (Space Marine 2 lo hace al desactivar la generacion). Hasta la 0.51 la confianza era
    // permanente y, a partir de ahi, todos los fotogramas se contaban como generados.
    int& simMiss = simMiss_[s.pid];
    if (simValid) {
      simMiss = 0;
    } else if (simTrusted && ++simMiss > kMaxGeneratedRun) {
      simCount = 0;
      simTrusted = false;
      accum_[s.swapchain] = 0.0;  // lo acumulado de la racha era de fotogramas reales mal contados
      accumGpu_[s.swapchain] = 0.0;
      Message(T("Marcadores de simulacion (Reflex) desaparecidos (el juego dejo de emitirlos, por ejemplo al\n"
                "  desactivar la generacion de fotogramas): cuento los fotogramas como reales hasta que vuelvan.",
                "Simulation markers (Reflex) gone (the game stopped sending them, for example when\n"
                "  frame generation was turned off): I count frames as real until they come back."));
    }
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
  int colPcLat_ = -1, colGpuLat_ = -1;
  int colApp_ = -1, colPid_ = -1, colSim_ = -1, colMode_ = -1;
  static constexpr int kSimTrustFrames = 60;
  static constexpr int kMaxGeneratedRun = 4;  // mas generados seguidos que esto: los marcadores se fueron
  std::map<DWORD, int> simSeen_, simMiss_;
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
  double suggestFps = 0;  // limite recomendado (fps), 0 si no hay
  int suggestKind = 0;     // 0 subir, 1 bajar, 2 bajar (urgente)
  bool prioSuggest = false;  // se recomienda pasar el juego a "Ritmo constante"
  bool guidedPending = false;  // modo guiado: limite aun sin comprobar (la fila muestra la instruccion)
  int prioMode = 0;        // 0 = automatica, 1 = siempre Fluidez, 2 = siempre Estabilidad
  bool usesFluid = true;   // prioridad que se aplica a este juego
  bool autoStability = false;  // la app paso este juego a Estabilidad
  // Desplegable (0.63): como se decidio el limite, la ultima partida y el estado de las subidas.
  std::string explain, lastSession, climb;
  bool vrr = true;
  int divisor = 0;
};

struct UiState {
  std::mutex m;
  UiStatus status = UiStatus::Starting;
  std::string statusText = "Iniciando…", statusDetail;
  std::string gpu, monitor, tip, driver;
  // G-SYNC no se esta usando: 0 = sin aviso, 1 = portatil en modo hibrido, 2 = desactivado en el panel
  // (la pantalla lo tenia antes). Se muestra en Ahora y en Juegos, con los pasos para activarlo.
  int vrrAlert = 0;
  bool driverOk = false;
  bool haveGame = false;
  // Ultima partida terminada (para "Ahora" sin juego abierto).
  std::string lastGame;
  double lastEnd = 0, lastMinutes = 0, lastFps = 0;
  int lastHitches = 0;
  bool lastLocked = false;
  std::string game, exe;
  double realFps = 0, lowFps = 0, screenFps = 0, steadyPct = 0, jitterMs = 0;
  int hitches = 0;
  std::string capText, bottleneck, recText, appliedText, notice, presentText;
  std::vector<UiChange> changes;
  std::map<std::string, std::string> warnings;  // .exe -> motivo para revisar su limite
  std::map<std::string, double> suggestions;    // .exe -> limite recomendado (fps), con prioridad elegida a mano
  std::map<std::string, int> suggestKind;       // .exe -> 0 subir, 1 bajar, 2 bajar (urgente: empeora el juego)
  std::set<std::string> prioSuggest;            // .exe -> se recomienda pasarlo a "Ritmo constante"
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
// Prioridad global (desde la 0.54): 0 = automatica (empieza con "Mas FPS" y pasa a "Ritmo constante" los
// juegos inestables), 1 = siempre "Ritmo constante", 2 = siempre "Mas FPS". Se guarda como "prioridad":
// auto, estabilidad o fluidez (los nombres internos de siempre). De el se derivan los dos de abajo.
static std::atomic<int> g_prioMode{0};
static std::atomic<int> g_priority{1};  // prioridad de partida: 0 = ritmo constante, 1 = mas FPS
// Prioridad por juego: con Fluidez, un juego inestable con su limite pasa a Estabilidad
// (prioridad-auto=no en equipo.txt lo desactiva). La prioridad global es el punto de partida.
static std::atomic<bool> g_autoPriority{true};  // solo en modo automatico
static void SetPrioMode(int mode) {
  g_prioMode = mode;
  g_priority = mode == 1 ? 0 : 1;
  g_autoPriority = mode == 0;
}
// Usar G-SYNC / FreeSync cuando actuen (automatico). Ajuste avanzado, sin interfaz:
// "divisores-siempre=si" en equipo.txt fuerza los divisores de los Hz aunque haya VRR, para
// pantallas que parpadean con VRR (algunas OLED y VA).
static std::atomic<bool> g_useVrr{true};
// Con cada limite, pedir al driver de NVIDIA que no baje las frecuencias de la GPU (avanzado:
// rendimiento-maximo=no en equipo.txt lo desactiva).
static std::atomic<bool> g_maxPerf{true};
// Con cada limite, cola de render de un solo fotograma ("Modo de baja latencia: Activado" del panel de
// NVIDIA): cuando el juego no llega al limite y la GPU se satura, los fotogramas ya no se acumulan en
// la cola (en simulacion, la latencia del PC en esas zonas bajaba de 40 a 20 ms, sin coste en FPS; con
// el limite sostenido no cambia nada). En los juegos con Reflex, Reflex tiene prioridad.
// Avanzado: baja-latencia=no en equipo.txt lo desactiva.
static std::atomic<bool> g_lowLatency{true};
// Modo guiado (AMD e Intel, desde la 0.56): la app mide, decide y comprueba el limite, pero lo aplica el
// usuario en el software de su grafica. 0 = no (NVIDIA o sin limitador), 1 = AMD, 2 = Intel.
// El modo automatico solo existe con NVIDIA: los kits de AMD (ADLX) e Intel (IGCL) tienen licencias que
// no encajan con un proyecto MIT de codigo abierto (ver README).
static std::atomic<int> g_guidedVendor{0};
// Limitador en uso: 0 = driver de NVIDIA, 1 = guiado, 2 = RivaTuner (RTSS). Solo el de NVIDIA lleva
// "GPU al maximo"; sin el, la regla del 1 % bajo no baja limites (ver tailRuleOk).
static std::atomic<int> g_limiterKind{0};
// Desde la 0.58, el limitador es una eleccion del usuario en Juegos ("limitador" en equipo.txt: driver,
// recomendar o rtss). Con AMD o Intel se empieza en "Solo recomendar": RTSS nunca se usa sin elegirlo.
static std::atomic<bool> g_nvidiaAvailable{false}, g_rtssAvailable{false};
static std::atomic<int> g_limiterReq{-1};  // cambio de limitador pedido desde la ventana (lo hace el motor)
static const char* GuidedSoftware() {
  return g_guidedVendor == 1 ? "AMD Software" : g_guidedVendor == 2 ? "Intel Graphics Software" : "";
}
// G-SYNC no disponible ahora (modo hibrido o desactivado): los limites decididos con el pueden empeorar
// el movimiento, y cada juego afectado lo indica en su fila.
static std::atomic<bool> g_vrrMissing{false};
// Ultimo rendimiento de la CPU medido (% de su frecuencia nominal; -1 si no hay), para la grabacion.
static std::atomic<double> g_cpuPerfPct{-1.0};
// Overlay clasico (dibujado con el procesador y UpdateLayeredWindow) en lugar del de la GPU
// (DirectComposition): overlay=clasico en equipo.txt. La app tambien vuelve a el sola si el de la
// GPU no se puede iniciar o falla durante la partida.
static std::atomic<bool> g_overlayClassic{false};
// Ajuste de limites: automatico (sube solo cuando el juego va sobrado) o "Solo sugerir" (muestra la
// sugerencia con un boton para aplicarla). Se elige en Juegos (subir-solo en equipo.txt). Las bajadas
// por la regla de fallos son automaticas en ambos modos.
// Desde la 0.55, la app solo cambia los limites por su cuenta con la prioridad automatica y en los juegos
// sin prioridad propia; si no, solo los recomienda (ver AppActsAlone).
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
static std::vector<std::string> g_applySuggestionReq;  // "Aplicar sugerencia" (con g_manualLimitMutex)
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

#include "src/core/log_text.h"

static void Log(const char* fmt, ...) {
  char buf[2048];
  va_list a;
  va_start(a, fmt);
  if (g_lang == 1) fmt = LogEnglish(fmt);
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
#include "src/core/decision.h"
#include "src/core/explain.h"

// Cuello de botella en el idioma de la app (el modulo compartido con las pruebas lo describe en espanol).
static std::string BottleneckText(const WindowStats& st) {
  const std::string b = st.Bottleneck();
  if (b == "ninguno: hay un limite activo (el margen de CPU no se puede medir)")
    return T("ninguno: hay un limite activo (el margen de CPU no se puede medir)",
             "none: there is an active limit (the CPU headroom can't be measured)");
  if (b == "desconocido") return T("desconocido", "unknown");
  if (b == "mixto") return T("mixto", "mixed");
  return b;
}

static void PrintAdvice(const WindowStats& st) {
  std::string b = BottleneckText(st);
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
        st.capFps, hz, st.capDivisor, st.capDivisor == 1 ? T(", probablemente VSync", ", probably VSync") : "");
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
        st.UseGpuOnly() ? T(" (no fiable con limite activo)", " (not reliable with an active limit)") : "", std::max(0.0, st.gpuBusyP99));
  PrintCap(st, hz);
  Log("  Cuello de botella: %s\n", BottleneckText(st).c_str());
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

// Herramientas de captura de pantalla: al abrirlas pasan a primer plano, pero el usuario no ha salido del
// juego (quiere capturarlo, overlay incluido). Se reconocen por el ejecutable; se recuerda por proceso.
static bool IsCaptureTool(DWORD pid) {
  if (pid == 0) return false;
  static std::mutex m;
  static std::map<DWORD, bool> cache;
  std::lock_guard<std::mutex> lock(m);
  auto it = cache.find(pid);
  if (it != cache.end()) return it->second;
  bool tool = false;
  if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
    wchar_t path[MAX_PATH];
    DWORD n = MAX_PATH;
    if (QueryFullProcessImageNameW(h, 0, path, &n)) {
      std::wstring name(path, n);
      name = name.substr(name.find_last_of(L"\\/") + 1);
      std::transform(name.begin(), name.end(), name.begin(), ::towlower);
      for (const wchar_t* t : {L"screenclippinghost.exe", L"snippingtool.exe", L"screensketch.exe", L"sharex.exe",
                               L"greenshot.exe", L"lightshot.exe", L"gamebar.exe"})
        tool = tool || name == t;
    }
    CloseHandle(h);
  }
  if (cache.size() > 256) cache.clear();
  cache[pid] = tool;
  return tool;
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
  return name.empty() ? std::string(T("(juego sin nombre)", "(unnamed game)")) : name;
}

// ---------------------------------------------------------------------------
// Driver de NVIDIA (NVAPI): limite de FPS en el perfil de cada juego
// Las declaraciones de NVAPI (de NVIDIA, licencia MIT) estan en third_party/nvapi/nvapi_min.h.
// Las funciones se obtienen de nvapi64.dll, que instala el propio driver.
// ---------------------------------------------------------------------------
#include "third_party/nvapi/nvapi_min.h"
#include "src/core/limit_request.h"

namespace nv {
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
static std::string GuidedInstruction(const std::string& label, uint32_t fps, bool vrr);  // modo guiado (AMD e Intel)
static bool HasMaxPerf(const AppliedChange& c) {
  for (const auto& sv : c.saved)
    if (sv.id == nv::PREFERRED_PSTATE_ID) return true;
  return false;
}

static std::string DescribeChange(const AppliedChange& c) {
  std::vector<std::string> extras;
  if (c.vrr) extras.push_back("G-SYNC");
  if (HasMaxPerf(c)) extras.push_back(T("GPU al máximo", "GPU at max"));
  if (c.method != Method::DriverVsync)  // con el VSync por intervalos la cola de 1 ya iba incluida
    for (const auto& sv : c.saved)
      if (sv.id == nv::PRERENDERLIMIT_ID) {
        extras.push_back(T("baja latencia", "low latency"));
        break;
      }
  if (c.displayHz > 0) extras.push_back(T("la pantalla a ", "the display at ") + std::to_string(c.displayHz) + " Hz");
  std::string d = DescribeChangeBase(c);
  for (size_t k = 0; k < extras.size(); ++k)
    d += (k == 0 ? T(", con ", ", with ") : k + 1 == extras.size() ? T(" y ", " and ") : ", ") + extras[k];
  if (c.manual) d += T(" (puesto a mano)", " (set by hand)");
  return d;
}
// Descripcion del limite con palabras (desde la 0.61; antes, una cadena con puntos medios):
// "114 FPS en el driver de NVIDIA", y los extras en DescribeChange.
static std::string DescribeChangeBase(const AppliedChange& c) {
  const std::string fps = std::to_string(c.newValue) + " FPS";
  if (c.fgFactor > 1)
    return fps + T(" en pantalla (", " on screen (") + std::to_string(c.newValue / static_cast<uint32_t>(c.fgFactor)) +
           T(" reales)", " real)") +
           (g_limiterKind == 2   ? T(" en RTSS", " in RTSS")
            : g_limiterKind == 1 ? std::string(T(" en ", " in ")) + GuidedSoftware()
                                 : std::string(T(" en el driver de NVIDIA", " in the NVIDIA driver")));
  if (c.method == Method::DriverVsync)
    return fps + T(" con el VSync del driver (1/", " with the driver's VSync (1/") + std::to_string(c.divisor) +
           T(" del refresco)", " of refresh)");
  if (g_limiterKind == 2) return fps + T(" en RTSS", " in RTSS");
  if (g_limiterKind == 1) return fps + T(" en ", " in ") + GuidedSoftware();
  return fps + T(" en el driver de NVIDIA", " in the NVIDIA driver");
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
    init_ = reinterpret_cast<nv::InitializeFn>(get(nv::fn::Initialize));
    createSession_ = reinterpret_cast<nv::CreateSessionFn>(get(nv::fn::DRS_CreateSession));
    destroySession_ = reinterpret_cast<nv::SessionFn>(get(nv::fn::DRS_DestroySession));
    load_ = reinterpret_cast<nv::SessionFn>(get(nv::fn::DRS_LoadSettings));
    save_ = reinterpret_cast<nv::SessionFn>(get(nv::fn::DRS_SaveSettings));
    findApp_ = reinterpret_cast<nv::FindAppFn>(get(nv::fn::DRS_FindApplicationByName));
    createProfile_ = reinterpret_cast<nv::CreateProfileFn>(get(nv::fn::DRS_CreateProfile));
    findProfile_ = reinterpret_cast<nv::FindProfileFn>(get(nv::fn::DRS_FindProfileByName));
    createApp_ = reinterpret_cast<nv::CreateAppFn>(get(nv::fn::DRS_CreateApplication));
    getSetting_ = reinterpret_cast<nv::GetSettingFn>(get(nv::fn::DRS_GetSetting));
    setSetting_ = reinterpret_cast<nv::SetSettingFn>(get(nv::fn::DRS_SetSetting));
    deleteSetting_ = reinterpret_cast<nv::DeleteSettingFn>(get(nv::fn::DRS_DeleteProfileSetting));
    deleteProfile_ = reinterpret_cast<nv::DeleteProfileFn>(get(nv::fn::DRS_DeleteProfile));
    getDisplayId_ = reinterpret_cast<nv::GetDisplayIdFn>(get(nv::fn::DISP_GetDisplayIdByDisplayName));  // opcionales
    getVrrInfo_ = reinterpret_cast<nv::GetVrrInfoFn>(get(nv::fn::Disp_GetVRRInfo));
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

  // Lo contrario de AddSetting: devuelve un ajuste anadido a un limite a su valor original (como
  // Deshacer) y lo quita de la lista. Para desactivar "GPU al maximo" o el modo de baja latencia en
  // limites que ya los tenian.
  bool RemoveSetting(AppliedChange& change, uint32_t id) {
    if (!ok_) return false;
    auto it = std::find_if(change.saved.begin(), change.saved.end(), [&](const SavedSetting& sv) { return sv.id == id; });
    if (it == change.saved.end()) return true;  // no lo tenia
    nv::Session s = nullptr;
    if (!Open(s)) return false;
    nv::Profile p = nullptr;
    bool created = false;
    if (!FindOrCreateProfile(s, change.exe, p, created)) { Close(s); return false; }
    nv::Status st = Restore(s, p, *it);
    if (st == nv::OK) st = save_(s);
    Close(s);
    if (st != nv::OK) { error = Describe(st); return false; }
    change.saved.erase(it);
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
      case nv::INVALID_USER_PRIVILEGE: return T("faltan permisos de administrador", "administrator rights are missing");
      case nv::NVIDIA_DEVICE_NOT_FOUND: return T("no se encontro una GPU NVIDIA", "no NVIDIA GPU found");
      case nv::PROFILE_NOT_FOUND: return T("perfil no encontrado", "profile not found");
      case nv::EXECUTABLE_NOT_FOUND: return T("juego no encontrado en los perfiles", "game not found in the profiles");
      default: return T("error de NVAPI ", "NVAPI error ") + std::to_string(st);
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

// ---------------------------------------------------------------------------
// Limitador del driver, comun a los fabricantes. El motor pide lo que quiere (LimitRequest) y cada
// fabricante lo traduce a su driver. Hoy solo NVIDIA; Intel (IGCL) y AMD (ADLX) en preparacion.
// ---------------------------------------------------------------------------
enum class LimitExtra { MaxPerf, LowLatency };

class LimitBackend {
 public:
  virtual ~LimitBackend() = default;
  virtual const char* Vendor() const = 0;
  // Deja en el driver exactamente lo pedido para el juego (guardando lo que hubiera para deshacerlo).
  virtual bool Apply(const std::string& exe, const LimitRequest& req, AppliedChange& change) = 0;
  virtual bool Revert(const AppliedChange& change) = 0;
  // Un ajuste propio que quedo en el driver sin registro (por ejemplo, tras borrar los datos de la app).
  virtual bool FindOwn(const std::string& exe, AppliedChange& out) = 0;
  virtual bool HasExtra(const AppliedChange& c, LimitExtra e) const = 0;
  virtual bool AddExtra(AppliedChange& c, LimitExtra e) = 0;
  virtual bool RemoveExtra(AppliedChange& c, LimitExtra e) = 0;
  virtual bool Vrr(const std::string& gdiName, bool& possible, bool& active) = 0;
  virtual std::string Error() const = 0;
};

class NvidiaBackend final : public LimitBackend {
 public:
  explicit NvidiaBackend(NvDriver& d) : d_(d) {}
  const char* Vendor() const override { return "NVIDIA"; }
  bool Apply(const std::string& exe, const LimitRequest& req, AppliedChange& change) override {
    return d_.ApplySettings(exe, NvDesired(req), change);
  }
  bool Revert(const AppliedChange& change) override { return d_.Revert(change); }
  bool FindOwn(const std::string& exe, AppliedChange& out) override { return d_.FindOwnProfile(exe, out); }
  bool HasExtra(const AppliedChange& c, LimitExtra e) const override {
    const uint32_t id = Id(e);
    return std::any_of(c.saved.begin(), c.saved.end(), [&](const SavedSetting& sv) { return sv.id == id; });
  }
  bool AddExtra(AppliedChange& c, LimitExtra e) override {
    return d_.AddSetting(c, Id(e), e == LimitExtra::MaxPerf ? nv::PSTATE_PREFER_MAX : 1u);
  }
  bool RemoveExtra(AppliedChange& c, LimitExtra e) override { return d_.RemoveSetting(c, Id(e)); }
  bool Vrr(const std::string& gdiName, bool& possible, bool& active) override {
    return d_.Vrr(gdiName, possible, active);
  }
  std::string Error() const override { return d_.error; }

 private:
  static uint32_t Id(LimitExtra e) {
    return e == LimitExtra::MaxPerf ? nv::PREFERRED_PSTATE_ID : nv::PRERENDERLIMIT_ID;
  }
  NvDriver& d_;
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
  int fgToggles = 0;  // cambios de estado de la generacion en esta partida
  // Latencia del PC por fotograma real (ms): con el limite funcionando y sin el (o antes de comprobarlo).
  std::vector<float> pcLatLimited, pcLatOther;
  std::vector<float> gpuQueueLimited;  // espera en la cola de la GPU con el limite funcionando (ms)
  double baseLat = -1.0;  // latencia del PC de este juego sin limite (guardada)
  // Error de animacion en fotogramas de ritmo plano (el fotograma dura lo mismo que el anterior,
  // +-0,3 ms): movimiento descompasado que la grafica no muestra. Para investigarlo por juego.
  double aePrevReal = -1.0;
  long flatN = 0, flatOverRel = 0;
  // Trabajo de CPU y GPU de cada fotograma real (para saber que limita los momentos lentos) y
  // rendimiento de la CPU por segundo (para ver si baja de frecuencia en ellos).
  std::vector<float> wkT, wkReal, wkCpu, wkGpu;
  std::vector<std::pair<float, float>> cpuPerf;  // (segundo de la partida, % de la frecuencia nominal)
  double baseMotion = -1.0;  // % de fotogramas con mas de 1 ms de error, sin limite (guardado)
  bool climbHandled = false;  // la subida automatica se ocupo de este limite al cerrar (sin sugerencia)
  bool fluid = true;          // prioridad de este juego: la global, o Estabilidad si la app la eligio
  bool prioFixed = false;     // prioridad elegida por el usuario para este juego (la app no la cambia)
  // Coste de FlatPace durante la partida
  double engMsSum = 0.0, engMsMax = 0.0, cpuAtStart = -1.0, pmCpuAtStart = -1.0;
  double ovMsAtStart = 0.0;
  long long ovDrawsAtStart = 0;
  double ovFullAtStart = 0.0, winMsAtStart = 0.0;
  long long winDrawsAtStart = 0;
  double mainCpuAtStart = 0.0, engCpuAtStart = 0.0, rdCpuAtStart = 0.0;
  long long pmLinesAtStart = 0, pmOwnAtStart = 0;
  int engTicks = 0;
  // Ultimos minutos ordenados, recalculados como mucho cada 5 s (era cada segundo)
  std::vector<double> longSortedCache;
  double longSortedAt = -1e9;
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
  std::vector<float> sessionWork;  // trabajo (CPU o GPU, el mayor) de cada fotograma de sessionReal[1]
  double closeSessionLow = 0.0;    // 1 % mas lento de toda la partida al cerrar (para el resumen)
  // Segundo a segundo con el limite funcionando (para la grafica de la comprobacion en Juegos): ritmo
  // real y 1 % mas lento causado por el juego en los ultimos 20 s (0 si no hay dato).
  std::vector<float> tlFps, tlLow;
  double tlSecs = 0.0;  // segundos que cubren (cada vuelta del motor dura algo mas de 1 s)
  // Ultimo calculo del 1 % mas lento causado por el juego (GameLowFps), para no repetirlo con el mismo lote.
  mutable double glAt = -1.0, glCapMs = 0.0, glValue = 0.0;
  mutable size_t glCount = 0;
  double gameSecs = 0.0, vrrSecs = 0.0, lastTick = 0.0;
  double ownCapSecs = 0.0, ownCapRateSum = 0.0;  // juego limitado por si mismo (sin limite nuestro)
  // Regla de fallos de un limite aceptado: mas de un 13 % por debajo, en tramos de al
  // menos 5 s, durante al menos el 10 % de la partida y 1 minuto en total.
  FailureTracker fail;  // regla de fallos (tramos claramente por debajo del limite)
  // El mismo criterio con el 1 % bajo causado por el juego (ventana de 20 s): en Greylock, a 125 fps
  // la mediana nunca cayo pero el 1 % bajo se hundio un 34-48 % del tiempo en las zonas pesadas
  // (a 111, un 2,5 %). Decide si una subida automatica se sostiene y si el juego va sobrado.
  FailureTracker tailFail;
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
// Con generacion de fotogramas (vista en la partida o con la que se aplico el limite), los modelos de
// capacidad no se aplican: suponen que el limite rige siempre sobre los fotogramas reales, y con
// generacion (sobre todo intermitente) no es asi. En Control Resonant, con la generacion apagada los
// fotogramas reales iban a 60-72 fps con un limite de 36 reales, y el modelo los tomaba por fotogramas
// esperando en el limite ("va sobrado").
static bool FrameGenInvolved(const Session& ss) { return ss.fgEverSeen || ss.expectFg > 1; }

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
                    "cpu_ms,gpu_ms,pantalla_ms,error_animacion_ms,vrr,limite_confirmado,latencia_pc_ms,"
                    "espera_gpu_ms,cpu_rendimiento_pct\n");
    rows = 0;
    tSum = 0.0;
    return true;
  }

  void Write(const FrameSample& s, double sessionStart, bool vrr, bool verified) {
    if (!f) return;
    if (s.presentMs > 0) tSum += s.presentMs / 1000.0;
    auto num = [](double v, const char* fmt) { return std::isnan(v) || v < -1e8 ? std::string() : Fmt(fmt, v); };
    std::fprintf(f, "%.4f,%.4f,%.3f,%s,%s,%d,%s,%s,%s,%s,%d,%d,%s,%s,%s\n", s.t - sessionStart, tSum, s.presentMs,
                 num(s.realMs > 0 ? s.realMs : NAN, "%.3f").c_str(),
                 num(s.presentRealMs > 0 ? s.presentRealMs : NAN, "%.3f").c_str(), s.generated ? 1 : 0,
                 num(s.cpuBusyMs >= 0 ? s.cpuBusyMs : NAN, "%.3f").c_str(),
                 num(s.gpuBusyMs >= 0 ? s.gpuBusyMs : NAN, "%.3f").c_str(),
                 num(s.displayMs > 0 ? s.displayMs : NAN, "%.3f").c_str(), num(s.animErrMs, "%.3f").c_str(),
                 vrr ? 1 : 0, verified ? 1 : 0, num(s.pcLatencyMs, "%.3f").c_str(), num(s.gpuQueueMs, "%.3f").c_str(),
                 num(g_cpuPerfPct.load() > 0 ? g_cpuPerfPct.load() : NAN, "%.0f").c_str());
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
      if (!s.appliedAtStart && s.expectedFps <= 0 && !s.exe.empty() && s.gameSecs >= 60.0) {
        // Partida sin ningun limite de la app: es la referencia "antes" de este juego. (Hasta la 0.48
        // tambien se guardaba con un limite ya aceptado de una sesion anterior; por eso la clave es nueva.)
        WriteKv(L"equipo.txt", "ritmo-antes2|" + s.exe, Fmt("%.2f", swing));
        Log(" (guardada como referencia sin limite)");
      } else if (s.baseSwing > 0) {
        Log(" | antes, sin limite: +-%.2f ms", s.baseSwing);
      }
    }
  }
  // Latencia del PC (solo juegos con marcadores de Reflex/PC Latency)
  auto latLine = [](const std::vector<float>& v, const char* what) {
    if (v.size() < 300) return;
    std::vector<double> d(v.begin(), v.end());
    std::sort(d.begin(), d.end());
    Log("\n  Latencia del PC%s%s: mediana %.1f ms | 1 %% peor %.1f ms (%zu fotogramas)", *what ? " " : "", what,
        PercentileSorted(d, 50),
        PercentileSorted(d, 99), d.size());
  };
  if (s.gpuQueueLimited.size() >= 300) {
    // Espera de cada fotograma entre que el juego lo entrega y la GPU lo empieza: crece si la cola de
    // render se llena (la GPU no da abasto). Mide el efecto de la cola de un fotograma en juegos sin Reflex.
    std::vector<double> d(s.gpuQueueLimited.begin(), s.gpuQueueLimited.end());
    std::sort(d.begin(), d.end());
    Log("\n  Espera en la cola de la GPU con el limite: mediana %.1f ms | 1 %% peor %.1f ms", PercentileSorted(d, 50),
        PercentileSorted(d, 99));
  }
  if (s.pcLatLimited.size() >= 300 || s.pcLatOther.size() >= 300) {
    latLine(s.pcLatLimited, T("con el limite", "with the limit"));
    latLine(s.pcLatOther, s.pcLatLimited.size() >= 300 ? T("sin limite o antes de comprobarlo", "without a limit or before checking it") : "");
  }
  // Partida sin limite de la app y sin generacion de fotogramas: su latencia es la referencia "antes".
  if (!s.appliedAtStart && s.expectedFps <= 0 && !FrameGenInvolved(s) && s.pcLatOther.size() >= 1000 &&
      s.gameSecs >= 60.0 && !s.exe.empty()) {
    std::vector<double> d(s.pcLatOther.begin(), s.pcLatOther.end());
    std::sort(d.begin(), d.end());
    WriteKv(L"equipo.txt", "latencia-antes|" + s.exe, Fmt("%.1f", PercentileSorted(d, 50)));
  }
  // Que limita los momentos lentos, CPU o GPU, y si la CPU bajo de frecuencia en ellos. Momentos lentos:
  // con el limite, los fotogramas mas de un 13 % por encima de su tiempo; sin el, el 10 % mas lento.
  if (s.wkReal.size() >= 3000) {
    double slowMs = 0.0;
    if (s.expectedFps > 0 && (s.verified || s.accepted)) {
      slowMs = 1000.0 / s.expectedFps * (1.0 + cfg::kJnd);
    } else {
      std::vector<float> r = s.wkReal;
      std::nth_element(r.begin(), r.begin() + r.size() * 9 / 10, r.end());
      slowMs = r[r.size() * 9 / 10];
    }
    std::vector<double> sc, sg;
    size_t cpuLim = 0, gpuLim = 0;
    for (size_t i = 0; i < s.wkReal.size(); ++i) {
      if (s.wkReal[i] < slowMs) continue;
      sc.push_back(s.wkCpu[i]);
      sg.push_back(s.wkGpu[i]);
      if (s.wkCpu[i] > s.wkGpu[i] * 1.1f) ++cpuLim;
      else if (s.wkGpu[i] > s.wkCpu[i] * 1.1f) ++gpuLim;
    }
    if (sc.size() >= 300) {
      std::sort(sc.begin(), sc.end());
      std::sort(sg.begin(), sg.end());
      const double pc = 100.0 * cpuLim / sc.size(), pg = 100.0 * gpuLim / sc.size();
      Log("\n  Momentos lentos (%zu fotogramas de mas de %.1f ms): los limita la CPU en el %.0f %% y la GPU en el %.0f %%"
          " (trabajo mediano: CPU %.1f ms, GPU %.1f ms)",
          sc.size(), slowMs, pc, pg, PercentileSorted(sc, 50), PercentileSorted(sg, 50));
      if (pc >= 70.0)
        Log("\n    -> Bajar los ajustes graficos no los acelerara; los que cargan la CPU si (poblacion, fisica,\n"
            "       distancia de vision, cantidad de enemigos o efectos).");
      else if (pg >= 70.0)
        Log("\n    -> Bajar la resolucion o los ajustes graficos los acelerara.");
      // Frecuencia de la CPU en los segundos lentos frente al resto.
      if (s.cpuPerf.size() >= 60) {
        std::vector<double> slowP, restP, allP;
        size_t j = 0;
        for (const auto& [t, perf] : s.cpuPerf) {
          std::vector<float> sec;
          while (j < s.wkT.size() && s.wkT[j] <= t) {
            if (s.wkT[j] > t - 1.0f) sec.push_back(s.wkReal[j]);
            ++j;
          }
          allP.push_back(perf);
          if (sec.size() < 10) continue;
          std::nth_element(sec.begin(), sec.begin() + sec.size() / 2, sec.end());
          (sec[sec.size() / 2] >= slowMs ? slowP : restP).push_back(perf);
        }
        std::sort(allP.begin(), allP.end());
        Log("\n  Rendimiento de la CPU: mediana %.0f %% de su frecuencia nominal (minimo %.0f %%)", PercentileSorted(allP, 50),
            allP.front());
        if (slowP.size() >= 10 && restP.size() >= 10) {
          std::sort(slowP.begin(), slowP.end());
          std::sort(restP.begin(), restP.end());
          const double ps = PercentileSorted(slowP, 50), pr = PercentileSorted(restP, 50);
          Log(" | en los segundos lentos %.0f %%, en el resto %.0f %%", ps, pr);
          if (pc >= 50.0 && ps < pr * 0.90)
            Log("\n    -> La CPU baja de frecuencia justo en los momentos lentos: posible calor o limite de energia\n"
                "       (comprueba la ventilacion, que el portatil este enchufado y el modo de energia de Windows).");
        }
      }
    }
  }
  if (s.aeN > 100) {
    const double over = 100.0 * s.aeOver1 / s.aeN;
    Log("\n  Error de animacion: media %.2f ms | %.1f %% de fotogramas con mas de 1 ms", s.aeSum / s.aeN, over);
    // Comparacion con la partida sin limite (como la variacion): el limite mejora el movimiento?
    const bool noLimit = !s.appliedAtStart && s.expectedFps <= 0;
    if (noLimit && !FrameGenInvolved(s) && !s.exe.empty() && s.gameSecs >= 60.0) {
      WriteKv(L"equipo.txt", "movimiento-antes|" + s.exe, Fmt("%.1f", over));
      Log(" (guardado como referencia sin limite)");
    } else if (!noLimit && s.baseMotion >= 0) {
      Log(" (sin limite: %.1f %%)", s.baseMotion);
    }
    Log(" | maximo %.1f ms", s.aeMax);
    // Movimiento descompasado aunque el ritmo sea plano (la grafica no lo muestra): error de animacion
    // mayor que el 13 % del fotograma. En las grabaciones: Greylock a 130 y Megabonk a 225, ~2 %;
    // Greylock sin limite, 16 %; Space Marine 2 y Control Resonant con generacion de fotogramas, 17-18 %.
    if (s.flatN > 1000) {
      const double flatPct = 100.0 * s.flatOverRel / s.flatN;
      Log("\n  Movimiento con ritmo plano: %.1f %% de fotogramas descompasados de forma perceptible"
          " (error de animacion mayor que el 13 %% del fotograma)",
          flatPct);
      // Con generacion de fotogramas, el descompas lo causa la propia generacion (Space Marine 2, en la
      // misma partida: 37 % con ella, 0,5 % sin ella); el limite no lo arregla ni lo empeora.
      if (FrameGenInvolved(s) && flatPct >= 10.0)
        Log("\n    -> Lo causa la generacion de fotogramas, no el limite: la grafica se ve plana, pero el\n"
            "       movimiento no. Si lo notas irregular, prueba el juego sin generacion de fotogramas.");
    }
  } else {
    Log("\n  Error de animacion: no disponible para este juego");
  }
  Log("\n  Pausas largas ignoradas: %d\n", s.pauseTotal);
  if (s.engTicks > 0) {
    const double wall = std::max(1.0, NowSec() - s.start);
    const double cpu = s.cpuAtStart >= 0 ? eco::ProcessCpuSec() - s.cpuAtStart : -1.0;
    Log("  Coste de FlatPace: motor %.1f ms por segundo de media (maximo %.0f ms)", s.engMsSum / s.engTicks, s.engMsMax);
    if (cpu >= 0) Log(" | proceso completo (con la ventana y el overlay): %.1f %% de un nucleo", 100.0 * cpu / wall);
    const double pmCpu = s.pmCpuAtStart >= 0 ? eco::PresentMonCpuTotal() - s.pmCpuAtStart : -1.0;
    if (pmCpu >= 0) Log("\n                     PresentMon: %.1f %% de un nucleo", 100.0 * pmCpu / wall);
    const long long draws = eco::overlayDraws.load() - s.ovDrawsAtStart;
    if (draws > 0)
      Log(" | overlay: %.2f ms por dibujo, %.2f ms con la preparacion (%.0f dibujos por segundo)",
          (eco::overlayDrawMsSum.load() - s.ovMsAtStart) / draws, (eco::overlayFullMsSum.load() - s.ovFullAtStart) / draws,
          draws / wall);
    const long long wdraws = eco::windowDraws.load() - s.winDrawsAtStart;
    if (wdraws > 0)
      Log("\n                     ventana: %.2f ms por dibujo (%.1f dibujos por segundo)",
          (eco::windowDrawMsSum.load() - s.winMsAtStart) / wdraws, wdraws / wall);
    Log("\n                     por hilo: ventana y overlay %.1f %%, motor %.1f %%, lector de PresentMon %.1f %% de un nucleo",
        100.0 * (eco::ThreadCpuSec(eco::mainThread.load()) - s.mainCpuAtStart) / wall,
        100.0 * (eco::ThreadCpuSec(eco::engineThread.load()) - s.engCpuAtStart) / wall,
        100.0 * (eco::ReaderCpuTotal() - s.rdCpuAtStart) / wall);
    Log("\n                     PresentMon envia %.0f lineas por segundo (%.0f del propio FlatPace, descartadas)",
        (eco::pmLines.load() - s.pmLinesAtStart) / wall, (eco::pmOwnLines.load() - s.pmOwnAtStart) / wall);
    Log("\n");
  }
  Log("  Generacion de fotogramas: %s\n", s.fgEverSeen ? T("detectada", "detected") : T("no detectada", "not detected"));
  if (!s.MainPresentMode().empty())
    Log("  Presentacion: %s\n", s.MainPresentMode().c_str());

  // Modelos estadisticos: capacidad con el limite puesto (datos censurados), memoria del juego
  // entre partidas y fases de juego. Por ahora informan y sugieren; no cambian las decisiones.
  if (FrameGenInvolved(s)) {
    Log("  Capacidad, memoria y sugerencias: no se calculan con generacion de fotogramas (los\n"
        "  fotogramas reales cambian de ritmo con ella y el limite no rige siempre sobre ellos).\n");
    // Quitar una sugerencia de subir que se hubiera calculado antes con datos mezclados.
    std::lock_guard<std::mutex> lock(g_ui.m);
    auto w = g_ui.warnings.find(s.exe);
    if (w != g_ui.warnings.end() && (w->second.rfind(T("Va sobrado", "It has plenty"), 0) == 0 ||
                                     w->second.rfind(T("Podría sostener", "It could hold"), 0) == 0))
      g_ui.warnings.erase(w);
  }
  if (!FrameGenInvolved(s)) {
    std::vector<double> msv;
    msv.reserve(s.longReal.size());
    for (const auto& r : s.longReal) msv.push_back(r.second);
    const double capMs = ActiveCapMs(s);
    double stableFps = 0.0;  // ritmo que el juego sostiene (criterio de Estabilidad: 1,13 x 1000 / p99)
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
        Log("  1 %% bajo con el limite (ultimos 3 min): %.0f fps medidos | %.0f fps causados por el juego (sin los retrasos del limitador)\n",
            1000.0 / PercentileSorted(raw, 99), 1000.0 / gameP99);
        if (s.closeSessionLow > 0)
          Log("  1 %% bajo de toda la partida (el que usan las reglas): %.0f fps causados por el juego\n", s.closeSessionLow);
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
      // Sin limite: el criterio de Estabilidad (1 % mas lento sin tirones, hasta un 13 % tarde).
      std::vector<double> sorted = msv;
      std::sort(sorted.begin(), sorted.end());
      const double med = PercentileSorted(sorted, 50);
      std::vector<double> body;
      for (double x : sorted)
        if (x <= med * 2.5) body.push_back(x);
      const double p99 = body.empty() ? 0.0 : PercentileSorted(body, 99);
      if (p99 > 0) stableFps = (1.0 + cfg::kJnd) * 1000.0 / p99;
    }
    // Proteccion contra la espiral: con un limite activo solo se aprende la capacidad si el juego NO
    // lo sostuvo (su 1 % mas lento, causado por el juego, supera el limite de forma perceptible). Si lo sostuvo, solo se
    // sabe que da "al menos eso", y anotar el limite como capacidad haria bajarlo partida a partida.
    if (capMs > 0) {
      stableFps = 0.0;
      if (!plenty && gameP99 > capMs * (1.0 + cfg::kJnd)) stableFps = (1.0 + cfg::kJnd) * 1000.0 / gameP99;
    }
    GameMemory mem = GameMemory::Parse(ReadKv(L"equipo.txt", "memoria3|" + s.exe));
    if (stableFps > 0 && s.gameSecs >= 60.0 && !s.exe.empty()) {
      mem.Add(stableFps, s.gameSecs / 60.0);
      WriteKv(L"equipo.txt", "memoria3|" + s.exe, mem.Serialize());
    }
    if (mem.sessions > 0)
      Log("  Memoria del juego: %d partida%s; sostiene ~%.0f fps reales (con un 80 %% de probabilidad, al menos %.0f)\n",
          mem.sessions, mem.sessions == 1 ? "" : "s", mem.MeanFps(), mem.LowerFps());
    // Sugerencia: el limite confirmado podria subir al siguiente escalon.
    if (s.verified && s.expectedFps > 0 && !s.exe.empty() && !s.climbHandled) {
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

  if (s.haveRec && s.accepted) {
    Log("\n  Limite aceptado: no se recalcula (usa Deshacer para recalcularlo).\n");
  } else if (s.haveRec) {
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
// Prioridad de un juego: elegida por el usuario en su fila ("prioridad-fija|exe": fluidez o
// estabilidad), o automatica: la global, salvo que la app haya pasado el juego a Estabilidad.
// mode: 0 = automatica, 1 = siempre Fluidez, 2 = siempre Estabilidad. Devuelve si usa Fluidez.
// El limite que detecta la medicion es el que aplico FlatPace (con un margen del 5 %: el detector lo ve
// redondeado, y en las zonas pesadas aparece y desaparece). Ese no es un limite ajeno: no se anuncia.
static bool IsOwnCap(double capFps, double expectedFps) {
  return expectedFps > 0 && capFps > 0 && std::fabs(capFps - expectedFps) <= 0.05 * expectedFps;
}

// La app solo cambia limites y prioridades por su cuenta con la prioridad automatica y en los juegos sin
// prioridad propia. Con una prioridad elegida a mano, el usuario decide: la app solo recomienda.
static bool AppActsAlone(bool gamePrioFixed) { return g_prioMode == 0 && !gamePrioFixed; }

// Recomendacion de limite guardada en equipo.txt ("sugerencia|exe" = limite;tipo;limite actual) y su texto.
static std::string RecommendationText(int kind, double from, double to) {
  if (kind == 0)
    return Fmt(T("Va sobrado: podría subir de %.0f a %.0f FPS.", "It has headroom: it could go from %.0f to %.0f FPS."), from, to);
  if (kind == 2)
    return Fmt(T("No sostiene %.0f FPS y, sin G-SYNC, eso empeora el movimiento: te recomiendo bajarlo a %.0f.",
                 "It can't hold %.0f FPS and, without G-SYNC, that makes motion worse: I recommend lowering it to %.0f."),
               from, to);
  return Fmt(T("No sostiene %.0f FPS en buena parte de la partida: te recomiendo bajarlo a %.0f.",
               "It can't hold %.0f FPS for a good part of the session: I recommend lowering it to %.0f."),
             from, to);
}
static void SetRecommendation(const std::string& exe, int kind, double from, double to) {
  WriteKv(L"equipo.txt", "sugerencia|" + exe, Fmt("%.0f;%d;%.0f", to, kind, from));
  std::lock_guard<std::mutex> lock(g_ui.m);
  g_ui.suggestions[exe] = to;
  g_ui.suggestKind[exe] = kind;
  g_ui.warnings[exe] = RecommendationText(kind, from, to);
}
static void ClearRecommendation(const std::string& exe) {
  WriteKv(L"equipo.txt", "sugerencia|" + exe, "");
  std::lock_guard<std::mutex> lock(g_ui.m);
  g_ui.suggestions.erase(exe);
  g_ui.suggestKind.erase(exe);
  g_ui.warnings.erase(exe);
}
static const char* kPrioSuggestText_es = "Es inestable con «Más FPS» (sus fotogramas más lentos se hunden): te recomiendo «Ritmo constante».";
static const char* kPrioSuggestText_en = "It's unstable with “More FPS” (its slowest frames sink): I recommend “Steady pace”.";

static bool GameUsesFluid(const std::string& exe, int* mode = nullptr, bool* autoStability = nullptr) {
  const std::string fixed = ReadKv(L"equipo.txt", "prioridad-fija|" + exe);
  const bool autoSt = g_autoPriority && AutoPriority::Parse(ReadKv(L"equipo.txt", "prioridad|" + exe)).stability;
  if (mode) *mode = fixed == "fluidez" ? 1 : fixed == "estabilidad" ? 2 : 0;
  if (autoStability) *autoStability = autoSt;
  if (fixed == "fluidez") return true;
  if (fixed == "estabilidad") return false;
  return g_priority == 1 && !autoSt;
}

// Va sobrado con su limite en TODA la partida (para subirlo solo): al menos 3 min con el limite
// funcionando, ningun tramo por debajo (regla de fallos), como mucho un 1 % de segundos sueltos
// claramente por debajo, y al menos el 98 % de los fotogramas de la partida esperando al limitador
// (modelo de datos censurados). Solo sin generacion de fotogramas.
static bool PlentyWholeSession(const Session& s) {
  if (FrameGenInvolved(s) || !s.verified || s.expectedFps <= 0) return false;
  if (s.fail.limPlaySecs < 180.0 || s.fail.failSecs > 0 || s.tailFail.failSecs > 0) return false;
  size_t below = 0;
  for (float r : s.fail.secRates)
    if (r < s.expectedFps * (1.0 - cfg::kJnd)) ++below;
  if (below > 0.01 * s.fail.secRates.size()) return false;
  auto it = s.sessionReal.find(1);
  if (it == s.sessionReal.end() || it->second.size() < 1000) return false;
  std::vector<double> ms(it->second.begin(), it->second.end());
  const CapacityEstimate ce = EstimateCapacity(ms, 1000.0 / s.expectedFps);
  return !ce.valid && ce.censoredFraction > 0.98;
}

// ---- Por que cada juego tiene su limite (desplegable de Juegos, 0.63) ----
// Como se decidio el limite: la regla, sus numeros y la curva de la partida que lo decidio. Solo explica:
// ninguna decision lo lee.
static void SaveLimitExplain(const Session& s, int kind, double from, double to, double pct,
                             const std::vector<float>* secRates = nullptr, double sessionLow = 0.0) {
  if (s.exe.empty()) return;
  LimitExplain e;
  e.kind = kind;
  e.when = static_cast<long long>(std::time(nullptr));
  e.from = static_cast<float>(from);
  e.to = static_cast<float>(to);
  e.pct = static_cast<float>(pct);
  e.fluid = s.fluid;
  e.minutes = static_cast<float>(s.gameSecs / 60.0);
  // Los fotogramas reales del tipo de generacion mas habitual en la partida (sin generacion, x2, x3...),
  // como DecideFromSession: con generacion (0.67), la curva tambien es la de la decision, y los menus,
  // que muchos juegos muestran sin generacion, quedan fuera.
  auto it = s.sessionReal.end();
  for (auto g = s.sessionReal.begin(); g != s.sessionReal.end(); ++g)
    if (it == s.sessionReal.end() || g->second.size() > it->second.size()) it = g;
  if (it != s.sessionReal.end() && it->second.size() >= 1000) {
    // Los mismos datos que la decision con la partida completa (DecideFromSession): los fotogramas
    // reales de la partida sin los tirones (mas de 2,5 veces la mediana). Mas FPS toma el ritmo que
    // alcanza el 90 % de ellos; Ritmo constante, 1,13 x el de su 1 % mas lento. Hasta la 0.63 la
    // curva contaba segundos y quitaba los que quedan por debajo del rango del VRR: no era el calculo
    // de la decision, y el limite no caia justo donde la curva cruza el 90 %.
    std::vector<double> ms(it->second.begin(), it->second.end());
    std::sort(ms.begin(), ms.end());
    const double med = PercentileSorted(ms, 50);
    std::vector<double> body;
    for (double x : ms)
      if (x <= med * 2.5) body.push_back(x);  // sin tirones
    if (body.size() >= 600) {
      e.low1 = static_cast<float>(1000.0 / PercentileSorted(body, 99));
      e.p90 = static_cast<float>(1000.0 / PercentileSorted(body, 90));
      std::vector<float> fps;
      fps.reserve(body.size());
      for (double x : body) fps.push_back(static_cast<float>(1000.0 / x));
      e.SetCurve(std::move(fps));
    }
  }
  // La regla de fallos baja al ritmo superado el 90 % de los segundos: su curva es la de esos segundos.
  if (kind == LimitExplain::FailMedian && secRates && secRates->size() >= 30) e.SetCurve(*secRates);
  // Regla del 1 % mas lento (0.66): el 1 % mas lento causado por el juego en la partida, el de la regla.
  if (sessionLow > 0) e.low1 = static_cast<float>(sessionLow);
  WriteKv(L"equipo.txt", "explica|" + s.exe, e.Serialize());
}

// La ultima partida con el limite: segundo a segundo y el resultado de cada regla.
static void SaveSessionExplain(const Session& s, const FailureTracker& failT, const FailureTracker& tailT, bool fg,
                               double sessionLow) {
  if (s.exe.empty() || s.expectedFps <= 0) return;
  SessionExplain x;
  x.when = static_cast<long long>(std::time(nullptr));
  x.minutes = static_cast<float>(s.gameSecs / 60.0);
  x.limit = static_cast<float>(s.expectedFps);
  x.verified = s.verified;
  x.fg = fg;
  x.failSecs = static_cast<float>(failT.failSecs);
  x.failPct = static_cast<float>(failT.Percent());
  x.tailSecs = static_cast<float>(tailT.failSecs);
  x.tailPct = static_cast<float>(tailT.Percent());
  x.trimSecs = static_cast<float>(std::max(failT.trimmed, tailT.trimmed));
  x.sessionLow = static_cast<float>(sessionLow);
  auto it = s.sessionReal.find(1);
  if (!fg && it != s.sessionReal.end() && it->second.size() >= 1000) {
    std::vector<double> ms(it->second.begin(), it->second.end());
    x.waitPct = static_cast<float>(100.0 * EstimateCapacity(ms, 1000.0 / s.expectedFps).censoredFraction);
  }
  x.SetSeries(s.tlFps, s.tlLow);
  if (!s.tlFps.empty() && s.tlSecs > 0) x.secsPerPoint *= static_cast<float>(s.tlSecs / s.tlFps.size());
  WriteKv(L"equipo.txt", "partida|" + s.exe, x.Serialize());
}

// 1 % bajo (fps) causado por el juego en los ultimos 20 s, con un limite activo (0 si no hay limite).
static double GameLowFps(const Session& ss) {
  if (FrameGenInvolved(ss)) return 0.0;
  const double capMs = ActiveCapMs(ss);
  if (capMs <= 0 || ss.samples.empty()) return 0.0;
  // Cada segundo se pedia tres veces con los mismos fotogramas (registro, ventana y overlay): se guarda
  // el resultado para el mismo lote (mismo ultimo fotograma, mismo numero de fotogramas y mismo limite).
  if (ss.glAt == ss.samples.back().t && ss.glCount == ss.samples.size() && ss.glCapMs == capMs) return ss.glValue;
  ss.glAt = ss.samples.back().t;
  ss.glCount = ss.samples.size();
  ss.glCapMs = capMs;
  ss.glValue = 0.0;
  const double from = ss.samples.back().t - cfg::kWindowSec;
  std::vector<double> r, w;
  for (const auto& f : ss.samples)
    if (!f.generated && f.realMs > 0 && f.t >= from) {
      r.push_back(f.realMs);
      w.push_back(std::max(f.cpuBusyMs, f.gpuBusyMs));
    }
  if (r.size() < 100) return 0.0;
  const double p99 = GameCausedPercentileMs(r, w, capMs, 99);
  ss.glValue = p99 > 0 ? 1000.0 / p99 : 0.0;
  return ss.glValue;
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
  // Ultimos minutos de fotogramas reales, ordenados. Con un limite activo, los retrasos del
  // limitador se cuentan como el limite (tiempos causados por el juego), para que no empujen la
  // recomendacion hacia abajo sin motivo.
  auto Long = [&]() -> const std::vector<double>& {
    if (!longReady && now - ss.longSortedAt < 5.0 && !ss.longSortedCache.empty()) {
      longCache = ss.longSortedCache;
      longReady = true;
    }
    if (!longReady) {
      const double capMs = ActiveCapMs(ss);
      if (capMs > 0 && ss.longWork.size() == ss.longReal.size()) {
        longCache.clear();
        longCache.reserve(ss.longReal.size());
        for (size_t i = 0; i < ss.longReal.size(); ++i) {
          double x = ss.longReal[i].second;
          if (x > capMs && ss.longWork[i] >= 0 && ss.longWork[i] < capMs * 0.95) x = capMs;
          longCache.push_back(x);
        }
        std::sort(longCache.begin(), longCache.end());
      } else {
        longCache = ss.LongSorted();
      }
      ss.longSortedCache = longCache;
      ss.longSortedAt = now;
      longReady = true;
    }
    return longCache;
  };
  const double capReal = ActiveCapMs(ss) > 0 ? 1000.0 / ActiveCapMs(ss) : 0.0;
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
          events > 1 ? T("ES", "S") : "", events, worst, ss.lastStats.medianMs);
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
    if (now >= ss.phaseStart && !s.generated && !std::isnan(s.gpuQueueMs) && s.gpuQueueMs < 1000.0 && ss.verified &&
        ss.expectedFps > 0 && ss.gpuQueueLimited.size() < 400000)
      ss.gpuQueueLimited.push_back(static_cast<float>(s.gpuQueueMs));
    if (now >= ss.phaseStart && !s.generated && !std::isnan(s.pcLatencyMs) && s.pcLatencyMs < 1000.0) {
      auto& v = ss.verified && ss.expectedFps > 0 ? ss.pcLatLimited : ss.pcLatOther;
      if (v.size() < 400000) v.push_back(static_cast<float>(s.pcLatencyMs));
    }
    if (now >= ss.phaseStart && !std::isnan(s.animErrMs) && !s.generated && s.realMs > 0 &&
        s.realMs <= cfg::kSpikeMs && ss.aePrevReal > 0 && std::fabs(s.realMs - ss.aePrevReal) < 0.3) {
      const double a = std::fabs(s.animErrMs);
      ++ss.flatN;
      if (a > cfg::kJnd * s.realMs) ++ss.flatOverRel;  // descompasado de forma perceptible
    }
    if (!s.generated && s.realMs > 0) ss.aePrevReal = s.realMs;
    if (now >= ss.phaseStart && !s.generated && s.realMs > 0 && s.realMs <= cfg::kSpikeMs && s.cpuBusyMs >= 0 &&
        s.gpuBusyMs >= 0 && ss.wkT.size() < 600000) {
      ss.wkT.push_back(static_cast<float>(now - ss.start));
      ss.wkReal.push_back(static_cast<float>(s.realMs));
      ss.wkCpu.push_back(static_cast<float>(s.cpuBusyMs));
      ss.wkGpu.push_back(static_cast<float>(s.gpuBusyMs));
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
      if (kNow == 1) ss.sessionWork.push_back(static_cast<float>(std::max(s.cpuBusyMs, s.gpuBusyMs)));
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
          ss.IsComposed() ? T("pasa por el compositor: sin tearing posible", "goes through the compositor: no tearing possible")
                          : T("directa: puede haber tearing si el VSync esta apagado", "direct: tearing possible if VSync is off"));
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
  // Generacion intermitente: algunos juegos (o el driver) la activan y desactivan solos, a menudo
  // con un modo dinamico. Tras 4 cambios en la partida se avisa una vez y ya no se anota cada cambio.
  auto NoteFgToggle = [&]() {
    ++ss.fgToggles;
    if (ss.fgToggles == 4) {
      Log("[%5.0f s] La generacion de fotogramas se activa y desactiva sola (4 cambios): lo mas probable es que\n"
          "          el juego la quite en los menus, o un modo dinamico del juego o del driver. Las decisiones\n"
          "          usan solo la generacion mas habitual de la partida. Ya no anoto cada cambio.\n",
          ss.T(now));
      std::lock_guard<std::mutex> lock(g_ui.m);
      g_ui.warnings[ss.exe] =
          T("La generación de fotogramas se activa y desactiva sola. Lo más probable es que el juego la quite en los menús, "
            "y entonces no hay que hacer nada: las decisiones usan solo la generación más habitual de la partida. Si ocurre "
            "jugando y el juego tiene un modo de generación automático o dinámico, fijarlo (activado o desactivado) da un ritmo más constante.",
            "Frame generation turns itself on and off. Most likely the game turns it off in its menus, and then there's "
            "nothing to do: decisions only use the session's most common frame generation. If it happens while "
            "playing and the game has an automatic or dynamic frame generation mode, setting it (on or off) gives "
            "a steadier pace.");
    }
    return ss.fgToggles < 4;  // anotar en el registro solo los primeros cambios
  };
  if (!ss.fgActive && ss.fgOnCount >= 3) {
    ss.fgSince = now;
    ss.fgActive = ss.fgEverSeen = true;
    if (NoteFgToggle())
      Log("[%5.0f s] Generacion de fotogramas detectada. Mido los fotogramas reales y elijo\n"
          "          un ritmo en pantalla que encaje con tus Hz. Vuelvo a medir.\n", ss.T(now));
    ss.fgAdviceDue = ss.fgToggles <= 1;
    if (ss.haveRec) { ss.ResetPhase(now); return; }
  } else if (ss.fgActive && ss.fgOffCount >= 3) {
    ss.fgActive = false;
    if (NoteFgToggle()) Log("[%5.0f s] Generacion de fotogramas desactivada. Vuelvo a medir.\n", ss.T(now));
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
      Recommendation vr = RecommendVrr(Long(), st.WorkP99(), hz, st.FgFactor(), ss.fluid,
                                       HeldRate(st), capReal);
      if (vr.valid) ss.current = vr;
    } else if (ss.fluid) {
      Recommendation fr = RecommendFluid(Long(), hz, st.FgFactor());
      if (fr.valid) ss.current = fr;
    }
    ss.recStats = st;
    ss.hasPrevRec = false;
    ss.haveRec = true;
    ss.lastStatus = now;
    ss.announcedCapped = st.capped && !IsOwnCap(st.capFps, ss.expectedFps);
    ss.announcedCapFps = st.capFps;
    Log("\n=== Recomendacion para %s (%.0f s, %zu fotogramas reales) ===\n", ss.label.c_str(),
        elapsedPhase, st.frames);
    PrintStats(st, hz);
    PrintRecommendation(ss.current, hz, st);
    Log("\n  Monitoreo continuo activo: baja rapido si hace falta, sube despacio.\n");
    return;
  }

  // Cambios en el limite activo (confirmados durante varios segundos). El de la propia app no cuenta como
  // limite ajeno: hasta la 0.55 se anunciaba una y otra vez, segun la zona del juego.
  const bool foreignCap = st.capped && !IsOwnCap(st.capFps, ss.expectedFps);
  bool capDiffers = foreignCap != ss.announcedCapped ||
                    (foreignCap && std::fabs(st.capFps - ss.announcedCapFps) > 0.5);
  ss.capChangeCount = capDiffers ? ss.capChangeCount + 1 : 0;
  if (ss.capChangeCount >= cfg::kCapChangeChecks) {
    Log("\n[%5.0f s] %s\n", ss.T(now),
        foreignCap ? T("Nuevo limite activo:", "New active limit:") : T("Ya no hay un limite activo.", "There is no active limit anymore."));
    if (foreignCap) PrintCap(st, hz);
    ss.announcedCapped = foreignCap;
    ss.announcedCapFps = st.capFps;
    ss.capChangeCount = 0;
  }

  if (holding) {
    // Solo bajar si una parte importante de los fotogramas llega tarde de forma continuada
    // (unos pocos tirones no se arreglan bajando el limite). Y nunca en cascada: tras
    // proponer un limite mas bajo, se espera a que se aplique y se compruebe.
    double period = 1000.0 / ss.current.realFps;
    bool failing = ss.fluid ? !FluidHolds(Long(), period * 1.02)
                                   : st.FractionRealOver(period * kHoldLateFactor) > kHoldFailLate;
    // Con el limite aceptado no se cambia nada: el contador solo alimenta el aviso
    // "el juego no esta sosteniendo este limite" (la decision es del usuario).
    ss.downCount = failing ? std::min(ss.downCount + 1, 1000) : 0;
  } else if (ss.fluid || ss.vrrActive) {
    // Fluidez (o G-SYNC): decide con los ultimos minutos de juego; sube tras 30 s y baja
    // tras 10 s. Con G-SYNC solo cuenta un cambio de al menos un 5 %.
    Recommendation fr = ss.vrrActive
                            ? RecommendVrr(Long(), st.WorkP99(), hz, st.FgFactor(), ss.fluid,
                                           HeldRate(st), capReal)
                            : RecommendFluid(Long(), hz, st.FgFactor());
    if (!fr.valid) fr = RecommendFor(st, hz, cfg::kMarginInitial);
    double cur = ss.current.valid ? ss.current.capFps : 0.0;
    double nxt = fr.valid ? fr.capFps : 0.0;
    double minStep = ss.vrrActive && fr.vrr && ss.current.vrr ? cur * 0.05 : 0.01;
    ss.upCount = nxt > cur + minStep ? ss.upCount + 1 : 0;
    ss.downCount = nxt < cur - minStep ? ss.downCount + 1 : 0;
    if (ss.upCount >= cfg::kUpChecks || ss.downCount >= 10) {
      if (ss.fluid)
        Log("\n[%5.0f s] %s (prioridad: mas FPS).\n", ss.T(now),
            nxt > cur ? T("SUBIR limite: el juego alcanza un ritmo mas alto la mayor parte del tiempo", "RAISE limit: the game reaches a higher pace most of the time")
                      : T("BAJAR limite: el juego ya no alcanza este ritmo la mayor parte del tiempo", "LOWER limit: the game no longer reaches this pace most of the time"));
      else
        Log("\n[%5.0f s] %s (prioridad: ritmo constante).\n", ss.T(now),
            nxt > cur ? T("SUBIR limite: el juego sostiene un ritmo mas alto incluso en sus peores momentos", "RAISE limit: the game holds a higher pace even in its worst moments")
                      : T("BAJAR limite: en sus peores momentos el juego ya no sostiene este ritmo", "LOWER limit: in its worst moments the game no longer holds this pace"));
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
        ss.current.valid ? T("SUBIR limite", "RAISE limit") : T("Ahora si hay un limite sostenible", "Now there is a sustainable limit"), cfg::kUpChecks);
    ss.ChangeRec(higher, st, now);
    PrintRecommendation(ss.current, hz, st);
    ss.downCount = ss.upCount = 0;
  }
  }  // fin del modo normal (sin limite propio funcionando)

  if (now - ss.lastStatus >= 5.0) {
    std::string extra;
    char buf[96];
    if (st.GeneratedFraction() > cfg::kFgOnFraction) {
      std::snprintf(buf, sizeof(buf), T(" | en pantalla ~%.0f fps", " | on screen ~%.0f fps"), st.presentedFps);
      extra += buf;
    }
    if (st.capped && !IsOwnCap(st.capFps, ss.expectedFps)) {
      std::snprintf(buf, sizeof(buf), T(" | limite activo ~%.0f%s", " | active limit ~%.0f%s"), st.capFps,
                    st.capFits ? "" : T(" (no encaja)", " (doesn't fit)"));
      extra += buf;
    }
    // El modelo de capacidad (datos censurados) solo se calcula en el resumen de la partida: cada
    // 5 s mientras se juega anadia un pico de trabajo que coincidia con fotogramas lentos.
    if (const double gl = GameLowFps(ss); gl > 0) extra += Fmt(T(" | 1%% bajo del juego ~%.0f fps", " | game's 1%% low ~%.0f fps"), gl);
    std::string rec = ss.current.valid
                          ? std::to_string(static_cast<int>(ss.current.capFps + 0.5)) + " fps"
                          : std::string("ninguno");
    Log("[%5.0f s] reales: mediana %.2f ms (~%.0f fps)%s | p99 %.2f ms | trabajo p99 %.2f ms"
        " | consistencia %.0f%% (var %.2f ms)%s | recomendado %s | tirones %d\n",
        ss.T(now), st.medianMs, 1000.0 / st.medianMs, extra.c_str(), st.p99Ms, st.WorkP99(),
        100.0 * st.steady2, st.jitterMs,
        st.animErrMs >= 0 ? Fmt(T(" | animacion %.2f ms", " | animation %.2f ms"), st.animErrMs).c_str() : "", rec.c_str(),
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
// Rendimiento de la CPU respecto a su frecuencia nominal (contador de Windows "% Processor Performance":
// 100 = frecuencia nominal; por encima, turbo; por debajo, la CPU baja de frecuencia por calor, energia
// o ahorro). En un portatil, CPU y GPU comparten energia y temperatura: si la CPU baja de frecuencia en
// las zonas pesadas, ahi esta la causa de los tirones. Se lee una vez por segundo desde el motor.
struct CpuPerfMeter {
  PDH_HQUERY query = nullptr;
  PDH_HCOUNTER counter = nullptr;
  bool ok = false;
  void Init() {
    if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) return;
    if (PdhAddEnglishCounterW(query, L"\\Processor Information(_Total)\\% Processor Performance", 0, &counter) !=
        ERROR_SUCCESS) {
      PdhCloseQuery(query);
      query = nullptr;
      return;
    }
    PdhCollectQueryData(query);  // el primer valor necesita dos lecturas
    ok = true;
  }
  double Sample() {
    if (!ok || PdhCollectQueryData(query) != ERROR_SUCCESS) return -1.0;
    PDH_FMT_COUNTERVALUE v;
    if (PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &v) != ERROR_SUCCESS ||
        v.CStatus != ERROR_SUCCESS)
      return -1.0;
    return v.doubleValue;
  }
  ~CpuPerfMeter() {
    if (query) PdhCloseQuery(query);
  }
};

// La ventana y el motor leen y escriben los mismos archivos de ajustes: sin este cerrojo, dos escrituras
// a la vez (leer todo, cambiar una clave, reescribir) podian perder la de uno de los dos.
static std::recursive_mutex g_kvMutex;

// Una linea de un archivo de ajustes: "clave<TAB>valor" (como los escribe la app) o "clave=valor" (como
// los escribiria cualquiera a mano; hasta la 0.50 se ignoraban). Devuelve false si no es un ajuste.
static bool ParseKvLine(std::string l, std::string& key, std::string& value) {
  while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
  size_t sep = l.find('\t');
  if (sep == std::string::npos) sep = l.find('=');
  if (sep == std::string::npos || sep == 0) return false;
  auto trim = [](std::string x) {
    const size_t a = x.find_first_not_of(" \t"), b = x.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : x.substr(a, b - a + 1);
  };
  key = trim(l.substr(0, sep));
  value = trim(l.substr(sep + 1));
  return !key.empty();
}

// Una linea completa, sea cual sea su longitud. Hasta la 0.63.1 los ajustes se leian con un espacio fijo
// de 512 caracteres: los registros del desplegable de Juegos (mas largos) se cortaban al leerlos, y
// tambien al guardar cualquier otro ajuste, que reescribe el archivo con lo leido.
static bool ReadFullLine(FILE* f, std::string& out) {
  out.clear();
  char buf[1024];
  while (std::fgets(buf, sizeof(buf), f)) {
    out += buf;
    if (!out.empty() && out.back() == '\n') return true;
  }
  return !out.empty();
}

static std::string ReadKv(const std::wstring& file, const std::string& key) {
  std::lock_guard<std::recursive_mutex> kvLock(g_kvMutex);
  FILE* f = _wfopen((DataDirectory() + L"\\" + file).c_str(), L"r");
  if (!f) return {};
  std::string line;
  std::string value;
  while (ReadFullLine(f, line)) {
    std::string k, v;
    if (ParseKvLine(line, k, v) && k == key) value = v;
  }
  std::fclose(f);
  return value;
}

// Quita de equipo.txt las claves que ya no usa ninguna version (memorias y referencias antiguas,
// marcas de migraciones pasadas). Devuelve cuantas quito.
static int CleanObsoleteKv() {
  std::lock_guard<std::recursive_mutex> kvLock(g_kvMutex);
  const std::wstring path = DataDirectory() + L"\\equipo.txt";
  FILE* f = _wfopen(path.c_str(), L"r");
  if (!f) return 0;
  static const char* kPrefixes[] = {"memoria|", "memoria2|", "ritmo-antes|"};
  static const char* kExact[] = {"prioridad-v2", "aprendizaje-generacion", "vrr"};
  std::map<std::string, std::string> keep;
  int removed = 0;
  std::string line;
  while (ReadFullLine(f, line)) {
    std::string k, v;
    if (!ParseKvLine(line, k, v)) continue;
    bool obsolete = false;
    for (const char* p : kPrefixes) obsolete = obsolete || k.rfind(p, 0) == 0;
    for (const char* e : kExact) obsolete = obsolete || k == e;
    if (obsolete) ++removed;
    else keep[k] = v;
  }
  std::fclose(f);
  if (removed > 0)
    if (FILE* o = _wfopen(path.c_str(), L"w")) {
      for (const auto& [k, v] : keep) std::fprintf(o, "%s\t%s\n", k.c_str(), v.c_str());
      std::fclose(o);
    }
  return removed;
}

static void WriteKv(const std::wstring& file, const std::string& key, const std::string& value) {
  std::lock_guard<std::recursive_mutex> kvLock(g_kvMutex);
  std::wstring path = DataDirectory() + L"\\" + file;
  std::map<std::string, std::string> all;
  if (FILE* f = _wfopen(path.c_str(), L"r")) {
    std::string line;
    while (ReadFullLine(f, line)) {
      std::string k, v;
      if (ParseKvLine(line, k, v)) all[k] = v;  // las lineas "clave=valor" se reescriben con tabulacion
    }
    std::fclose(f);
  }
  if (value.empty()) all.erase(key);  // un valor vacio borra la clave
  else all[key] = value;
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
        rr.jitterUs, clean && coherent ? T("  -> valida", "  -> valid") : T("  -> ruidosa, repito", "  -> noisy, retrying"));
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
  double lastVt = 0.0;                // ultimo momento de la linea de tiempo continua
  double targetMs = 0.0;              // periodo objetivo de los fotogramas reales
  std::string limitLabel;             // "225 fps", "122 en pantalla"...
  LimitState limitState = LimitState::None;
  double baseSwingMs = -1.0;          // variacion del ritmo de este juego sin limite ("antes")
  double gameLowFps = 0.0;            // 1 % bajo causado por el juego con un limite activo (0 = no)
  // Juegos con generacion de fotogramas: la grafica dibuja las imagenes EN PANTALLA (lo que se ve),
  // porque los fotogramas reales cambian de ritmo cuando la generacion se activa o se desactiva (en
  // Control Resonant, 36 reales con ella y 60-68 sin ella, con ~72 en pantalla en ambos casos).
  bool screenMode = false;
  std::deque<double> realOnly;        // fotogramas reales (ms), para las cifras del overlay
  // Intervalo entre imagenes presentadas (ms). La cifra "en pantalla" se calcula con su duracion real:
  // contar las imagenes llegadas en los ultimos 5 s (hasta la 0.43) saltaba con cada tanda de datos.
  std::deque<double> presentMs;
  std::deque<float> latMs;     // latencia del PC de los ultimos fotogramas reales (juegos con Reflex)
  double baseLatMs = -1.0;     // latencia del PC de este juego sin limite (guardada), -1 si no hay
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
  if (s.presentMs > 0 && s.presentMs < 250.0) {
    feed.presentMs.push_back(s.presentMs);  // todas las imagenes presentadas (reales y generadas)
    while (feed.presentMs.size() > 8000) feed.presentMs.pop_front();
  }
  if (!s.generated && !std::isnan(s.pcLatencyMs) && s.pcLatencyMs < 1000.0) {
    feed.latMs.push_back(static_cast<float>(s.pcLatencyMs));
    while (feed.latMs.size() > 240) feed.latMs.pop_front();
  }
  if (!s.generated && s.realMs > 0) {
    feed.realOnly.push_back(s.presentRealMs > 0 ? s.presentRealMs : s.realMs);
    while (feed.realOnly.size() > 4000) feed.realOnly.pop_front();
  }
  const bool forGraph = feed.screenMode ? (s.displayMs > 0 && s.displayMs < 250.0) : (!s.generated && s.realMs > 0);
  if (forGraph) {
    // Una tanda nueva de datos: su intervalo con la anterior marca el retraso de dibujo.
    if (s.t - feed.lastArrival > 0.01) {
      double gap = feed.lastArrival > 0 ? s.t - feed.lastArrival : 1.0;
      double decayed = feed.lagSec - 0.05 * std::max(0.0, s.t - feed.lagUpdated);  // baja despacio
      // Margen de 0,6 s sobre el intervalo entre tandas (antes 0,35): con poco margen, el reloj de
      // reproduccion llegaba casi al ultimo dato antes de cada tanda, frenaba y luego aceleraba: un
      // pequeno tiron del desplazamiento en cada refresco (simulado: avance por dibujo de 12,4 a
      // 20,8 ms en vez de 16,7 constantes; con este margen y la correccion suave, de 15,8 a 17,5).
      feed.lagSec = std::clamp(std::max(gap + 0.6, decayed), 0.4, 4.0);
      feed.lagUpdated = s.t;
    }
    feed.lastArrival = s.t;
    // La grafica muestra el intervalo entre presentaciones de los fotogramas reales: el ritmo que
    // regula el limitador (comparable a lo que muestran los limitadores que se inyectan en el juego).
    // Instante del fotograma: el anterior + su duracion. Se fija una vez, al llegar, y no cambia:
    // la linea de tiempo es continua y estable (una tanda nunca se amontona ni mueve lo anterior).
    // En modo en pantalla, cada imagen mostrada (real o generada) con lo que duro en pantalla.
    const double ms = feed.screenMode ? s.displayMs : (s.presentRealMs > 0 ? s.presentRealMs : s.realMs);
    double vt = feed.lastVt > 0 ? feed.lastVt + ms / 1000.0 : s.t;
    feed.lastVt = vt;
    feed.real.push_back({vt, ms});
  }
  while (feed.real.size() > 4000) feed.real.pop_front();
}

static DWORD lastGamePid = 0;
#include "src/core/pace_basics.h"

// Reloj de reproduccion (con feed.m tomado): avanza a velocidad real hacia "dato mas reciente,
// extrapolado al reloj actual, menos el bufer", con una correccion suave (entre 0,95x y 1,05x).
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
    // Correccion suave (entre 0,95 y 1,05 x): el desplazamiento se ve uniforme.
    double rate = std::clamp(1.0 + 0.2 * (target - feed.playT), 0.95, 1.05);
    // Si una tanda se retrasa, frenar suavemente al acercarse al ultimo dato (en vez de pararse
    // en seco al alcanzarlo).
    const double ahead = head - feed.playT;
    if (ahead < 0.25) rate *= std::max(0.0, ahead / 0.25);
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

// Cambia la grafica a imagenes en pantalla (o la devuelve a fotogramas reales). Al cambiar, se
// empieza la linea de nuevo para no mezclar las dos medidas.
static void SetScreenMode(bool on) {
  std::lock_guard<std::mutex> lock(feed.m);
  if (feed.screenMode == on) return;
  feed.screenMode = on;
  feed.real.clear();
  feed.lastVt = 0.0;
  feed.playInit = false;
  feed.refAuto = 0.0;
  feed.generation++;
}

static void SetGameLow(double fps) {
  std::lock_guard<std::mutex> lock(feed.m);
  feed.gameLowFps = fps;
}

static void SetGame(DWORD pid, double targetMs, const std::string& limitLabel, LimitState state,
                    double baseSwingMs = -1.0, double baseLatMs = -1.0) {
  std::lock_guard<std::mutex> lock(feed.m);
  // Solo se borran los datos al cambiar de juego: asi la ventana sigue mostrando la
  // ultima grafica mientras miras la app.
  if (pid != 0 && pid != lastGamePid) {
    feed.real.clear();
    feed.realOnly.clear();
    feed.presentMs.clear();
    feed.latMs.clear();
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
  feed.baseLatMs = baseLatMs;
}

class Window {
 public:
  // dev y ctx: el dispositivo de Direct3D 11 de la ventana principal (lo comparte el overlay de la GPU).
  void Create(float scale, ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    scale_ = scale;
    dev_ = dev;
    ctx_ = ctx;
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
    textCache_.clear();  // fuentes nuevas: los textos guardados ya no sirven
    base_.clear();
    big_ = CreateFontW(static_cast<int>(-22 * scale), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                       ANTIALIASED_QUALITY, 0, L"Segoe UI");
    small_ = CreateFontW(static_cast<int>(-12 * scale), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                         ANTIALIASED_QUALITY, 0, L"Segoe UI");
    // El registro se abre en el hilo del motor, despues: el aviso se escribe en la primera
    // actualizacion con el registro ya abierto.
    if (!g_overlayClassic) {
      std::string why;
      gpu_ = InitGpu(why);
      pendingNote_ = gpu_ ? std::string(T("Overlay: dibujado con la GPU (DirectComposition).\n",
                                          "Overlay: drawn with the GPU (DirectComposition).\n"))
                          : T("Overlay: no pude usar la GPU (", "Overlay: couldn't use the GPU (") + why +
                                T("); uso el overlay clasico.\n", "); using the classic overlay.\n");
    } else {
      pendingNote_ = T("Overlay: clasico (overlay=clasico en equipo.txt).\n", "Overlay: classic (overlay=clasico in equipo.txt).\n");
    }
  }

  void Destroy() {
    ReleaseGpu();
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
  static constexpr double kDrawInterval = 1.0 / 60.0;
  double statsAt_ = -1.0, sFps_ = 0.0, sLow_ = 0.0, sScreenFps_ = 0.0, sSwing_ = 0.0, topmostAt_ = -1.0;
  // Momento del siguiente dibujo (para que el bucle de la ventana despierte justo a tiempo).
  double NextDrawAt() const { return lastDraw_ + kDrawInterval; }

  void Update() {
    if (!hwnd_) return;
    if (!pendingNote_.empty() && g_log) {
      Log("%s", pendingNote_.c_str());
      pendingNote_.clear();
    }
    double now = NowSec();
    // 60 dibujos por segundo, espaciados por un temporizador de alta precision (0.65). En la 0.64 eran
    // 30 en teoria, pero el temporizador normal de Windows (15,6 ms) los dejaba en ~18, irregulares.
    if (now - lastDraw_ < kDrawInterval - 0.002) return;
    lastDraw_ = now;
    const bool refreshStats = now - statsAt_ >= 0.25;  // las cifras, 4 veces por segundo
    // Trabajo completo de este dibujo (con la preparacion de los datos), si llega a dibujar.
    bool drew = false;
    struct FullTimer {
      double t0;
      const bool& drew;
      ~FullTimer() {
        if (drew) eco::overlayFullMsSum = eco::overlayFullMsSum.load() + (NowSec() - t0) * 1000.0;
      }
    } fullTimer{now, drew};

    DWORD pid;
    double targetMs;
    std::string limitLabel;
    LimitState limitState;
    double baseSwing = -1.0, gameLow = 0.0;
    bool screenMode = false;
    std::vector<double> realMs;
    double display = now, refAuto = 0.0;
    std::vector<std::pair<double, double>> real;
    size_t screenImages = 0;
    double presentsSpanMs = 0.0;
    {
      std::lock_guard<std::mutex> lock(feed.m);
      pid = feed.pid;
      targetMs = feed.targetMs;
      limitLabel = feed.limitLabel;
      limitState = feed.limitState;
      baseSwing = feed.baseSwingMs;
      gameLow = feed.gameLowFps;
      baseLat_ = feed.baseLatMs;
      if (refreshStats) {
        curLat_ = -1.0;
        if (feed.latMs.size() >= 30) {
          std::vector<float> lv(feed.latMs.end() - std::min<size_t>(feed.latMs.size(), 120), feed.latMs.end());
          std::nth_element(lv.begin(), lv.begin() + lv.size() / 2, lv.end());
          curLat_ = lv[lv.size() / 2];
        }
        realMs.assign(feed.realOnly.begin(), feed.realOnly.end());
      }
      screenMode = feed.screenMode;
      screenMode_ = screenMode;
      display = GraphPlaybackLocked(now);
      generation_ = feed.generation;
      refAuto = GraphAutoRefLocked(now, display);
      // La grafica dibuja sus ultimos kPaceSpanSec (3,3 s): basta copiar 4 s; la oscilacion, que mira
      // 10 s, solo al recalcular las cifras.
      const double from = display - (refreshStats ? 11.0 : 4.0);
      for (auto it = feed.real.rbegin(); it != feed.real.rend() && it->first >= from; ++it) real.push_back(*it);
      std::reverse(real.begin(), real.end());
      // Imagenes en pantalla por segundo: las ultimas imagenes que suman 5 s de duracion real.
      if (refreshStats) {
        double sumMs = 0.0;
        for (auto it = feed.presentMs.rbegin(); it != feed.presentMs.rend() && sumMs < 5000.0; ++it) {
          sumMs += *it;
          screenImages++;
        }
        presentsSpanMs = sumMs;
      }
    }
    HWND fg = GetForegroundWindow();
    DWORD fgPid = 0;
    if (fg) GetWindowThreadProcessId(fg, &fgPid);
    // Con una herramienta de captura delante, el overlay sigue visible si lo estaba (para poder capturarlo).
    const bool front = fgPid == pid || (visible_ && IsCaptureTool(fgPid));
    bool show = enabled && pid != 0 && front && real.size() >= 10;
    state = !enabled ? 1 : pid == 0 ? 2 : !front ? 3 : real.size() < 10 ? 4 : 0;
    if (!show) {
      if (visible_) { ShowWindow(ActiveHwnd(), SW_HIDE); visible_ = false; }
      return;
    }

    // Estadisticas de los ultimos 5 s (siempre de fotogramas reales: marcan la respuesta del juego).
    // Se recalculan 4 veces por segundo (antes, en cada dibujo): las cifras no cambian mas deprisa de
    // lo que se pueden leer; la grafica si se dibuja en cada uno.
    if (refreshStats) {
      std::vector<double> ms;
      double sum = 0;
      for (size_t i = realMs.size(); i-- > 0 && sum < 5000.0;) {
        ms.push_back(realMs[i]);
        sum += realMs[i];
      }
      if (ms.empty()) return;
      statsAt_ = now;
      sFps_ = 1000.0 * ms.size() / sum;
      std::sort(ms.begin(), ms.end());
      sLow_ = 1000.0 / PercentileSorted(ms, 99);
      sScreenFps_ = presentsSpanMs > 0 ? screenImages * 1000.0 / presentsSpanMs : 0.0;
      sSwing_ = RecentSwing(real);
    }
    if (sFps_ <= 0) return;
    const double fps = sFps_;
    double low = sLow_;
    if (gameLow > 0) low = gameLow;  // con un limite activo, sin los retrasos del limitador
    const double screenFps = sScreenFps_;
    const bool fg2 = screenFps > fps * 1.4;
    const double swing = sSwing_;
    drew = true;
    const double drawT0 = NowSec();
    struct DrawTimer {
      double t0;
      ~DrawTimer() {
        eco::overlayDrawMsSum = eco::overlayDrawMsSum.load() + (NowSec() - t0) * 1000.0;
        ++eco::overlayDraws;
      }
    } drawTimer{drawT0};
    // Esquina superior izquierda de la pantalla del juego
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTOPRIMARY), &mi);
    POINT pos{mi.rcMonitor.left + static_cast<LONG>(16 * scale_), mi.rcMonitor.top + static_cast<LONG>(16 * scale_)};

    if (gpu_) {
      std::string why;
      if (DrawGpu(fps, low, fg2 ? screenFps : 0.0, limitLabel, limitState, targetMs > 0 ? targetMs : refAuto,
                  targetMs > 0, real, display, swing, baseSwing, why)) {
        if (!visible_ || pos.x != gpuPos_.x || pos.y != gpuPos_.y) {
          SetWindowPos(ghwnd_, HWND_TOPMOST, pos.x, pos.y, W_, H_, SWP_NOACTIVATE);
          gpuPos_ = pos;
        }
        if (!visible_) {
          ShowWindow(ghwnd_, SW_SHOWNOACTIVATE);
          visible_ = true;
        }
        // Reafirmar "siempre encima" una vez por segundo (hasta la 0.65, en cada dibujo): obliga a Windows a
        // revisar el orden de las ventanas. Al aparecer o moverse ya se hace arriba.
        if (now - topmostAt_ >= 1.0) {
          SetWindowPos(ghwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
          topmostAt_ = now;
        }
        return;
      }
      // La GPU fallo (por ejemplo, un reinicio del driver): vuelta al overlay clasico, ya en este dibujo.
      Log("Overlay: el dibujo con la GPU fallo (%s); vuelvo al overlay clasico.\n", why.c_str());
      if (visible_) ShowWindow(ghwnd_, SW_HIDE);
      visible_ = false;
      ReleaseGpu();
      gpu_ = false;
    }

    Draw(display, fps, low, fg2 ? screenFps : 0.0, limitLabel, limitState, targetMs > 0 ? targetMs : refAuto, targetMs > 0,
         real, swing, baseSwing);
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
  // -------------------------------------------------------------------------
  // Overlay con la GPU (DirectComposition). Una ventana sin superficie propia
  // (WS_EX_NOREDIRECTIONBITMAP) muestra una cadena de imagenes de Direct3D 11 con transparencia
  // premultiplicada, compuesta directamente por Windows: sin la copia de todos los pixeles que exige
  // UpdateLayeredWindow en cada dibujo. El contenido lo dibuja Dear ImGui (un contexto propio del
  // overlay, en el mismo dispositivo que la ventana principal) con las mismas formas, colores y
  // posiciones que el overlay clasico. Sigue sin recibir clics (WS_EX_TRANSPARENT).
  // -------------------------------------------------------------------------
  HWND ActiveHwnd() const { return gpu_ ? ghwnd_ : hwnd_; }

  // Cifras de la fila superior: "1 % bajo · pantalla · latencia (antes)". "pantalla" solo si aporta:
  // sin limite fijado, o si el ritmo en pantalla se aleja mas de un 13 % del limite (si no, la
  // etiqueta del limite ya lo dice). Si la linea no cabe en el espacio, se quitan elementos por orden
  // de importancia: el "(antes)", "pantalla" y la latencia; el 1 % bajo nunca.
  template <typename WidthFn>
  std::string TopLine(double low, double screenFps, bool locked, double refMs, float avail, WidthFn width) const {
    std::string lowS = Fmt(T("1%% bajo %.0f", "1%% low %.0f"), low), scr, lat, before;
    const double limitScreen = locked && refMs > 0 ? 1000.0 / refMs : 0.0;
    if (screenFps > 0 && (limitScreen <= 0 || !screenMode_ ||
                          std::fabs(screenFps - limitScreen) > cfg::kJnd * limitScreen))
      scr = Fmt(T("  ·  pantalla %.0f", "  ·  screen %.0f"), screenFps);
    if (curLat_ > 0) {
      lat = Fmt(T("  ·  latencia %.0f ms", "  ·  latency %.0f ms"), curLat_);
      if (locked && baseLat_ > 0 && !screenMode_) before = Fmt(T(" (antes %.0f)", " (before %.0f)"), baseLat_);
    }
    auto line = [&] { return lowS + scr + lat + before; };
    if (width(line()) > avail) before.clear();
    if (width(line()) > avail) scr.clear();
    if (width(line()) > avail) lat.clear();
    return line();
  }

  bool InitGpu(std::string& why) {
    if (!dev_ || !ctx_) {
      why = "sin dispositivo de Direct3D";
      return false;
    }
    WNDCLASSEXW wc{sizeof(wc), 0, DefWindowProcW, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr,
                   nullptr, nullptr, L"FlatPaceOverlayGpu", nullptr};
    RegisterClassExW(&wc);
    ghwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
                                 WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                             wc.lpszClassName, L"FlatPace overlay", WS_POPUP, 0, 0, W_, H_, nullptr, nullptr,
                             wc.hInstance, nullptr);
    if (!ghwnd_) {
      why = "no se pudo crear la ventana";
      return false;
    }
    // Ventana por capas totalmente opaca: la transparencia la da el contenido; asi los clics la
    // atraviesan (WS_EX_TRANSPARENT) y Windows la compone sin copiar pixeles.
    SetLayeredWindowAttributes(ghwnd_, 0, 255, LWA_ALPHA);
    auto fail = [&](const char* what, HRESULT hr) {
      char buf[96];
      snprintf(buf, sizeof(buf), "%s, error 0x%08lX", what, static_cast<unsigned long>(hr));
      why = buf;
      ReleaseGpu();
      return false;
    };
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDev;
    HRESULT hr = dev_->QueryInterface(IID_PPV_ARGS(&dxgiDev));
    if (FAILED(hr)) return fail("sin dispositivo DXGI", hr);
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDev->GetAdapter(&adapter);
    if (FAILED(hr)) return fail("sin adaptador", hr);
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return fail("sin DXGI 1.2", hr);
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = static_cast<UINT>(W_);
    d.Height = static_cast<UINT>(H_);
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.Scaling = DXGI_SCALING_STRETCH;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    hr = factory->CreateSwapChainForComposition(dev_, &d, nullptr, &gsc_);
    if (FAILED(hr)) return fail("CreateSwapChainForComposition", hr);
    hr = DCompositionCreateDevice(dxgiDev.Get(), IID_PPV_ARGS(&gdc_));
    if (FAILED(hr)) return fail("DCompositionCreateDevice", hr);
    hr = gdc_->CreateTargetForHwnd(ghwnd_, TRUE, &gtarget_);
    if (FAILED(hr)) return fail("CreateTargetForHwnd", hr);
    hr = gdc_->CreateVisual(&gvisual_);
    if (FAILED(hr)) return fail("CreateVisual", hr);
    hr = gvisual_->SetContent(gsc_);
    if (SUCCEEDED(hr)) hr = gtarget_->SetRoot(gvisual_);
    if (SUCCEEDED(hr)) hr = gdc_->Commit();
    if (FAILED(hr)) return fail("composicion", hr);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back;
    hr = gsc_->GetBuffer(0, IID_PPV_ARGS(&back));
    if (SUCCEEDED(hr)) hr = dev_->CreateRenderTargetView(back.Get(), nullptr, &grtv_);
    if (FAILED(hr)) return fail("sin destino de dibujo", hr);

    // Contexto de Dear ImGui propio del overlay, con sus fuentes (Segoe UI, como el clasico: el
    // tamano en ImGui es la altura de linea, ~1,33 veces el tamano de letra de Windows).
    ImGuiContext* prev = ImGui::GetCurrentContext();
    gctx_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(gctx_);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2010, 0x2027, 0};
    wchar_t winDir[MAX_PATH];
    GetWindowsDirectoryW(winDir, MAX_PATH);
    const std::string fonts = Narrow(std::wstring(winDir)) + "\\Fonts\\";
    auto load = [&](const char* file, float px) -> ImFont* {
      const std::string path = fonts + file;
      if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) return nullptr;
      return io.Fonts->AddFontFromFileTTF(path.c_str(), px, nullptr, ranges);
    };
    gSmall_ = load("segoeui.ttf", 12.0f * 1.33f * scale_);
    gBig_ = load("seguisb.ttf", 22.0f * 1.33f * scale_);
    if (!gSmall_) gSmall_ = io.Fonts->AddFontDefault();
    if (!gBig_) gBig_ = gSmall_;
    const bool ok = ImGui_ImplDX11_Init(dev_, ctx_);
    ImGui::SetCurrentContext(prev);
    if (!ok) return fail("ImGui_ImplDX11_Init", E_FAIL);
    gImguiReady_ = true;
    return true;
  }

  void ReleaseGpu() {
    if (gctx_) {
      ImGuiContext* prev = ImGui::GetCurrentContext();
      ImGui::SetCurrentContext(gctx_);
      if (gImguiReady_) ImGui_ImplDX11_Shutdown();
      ImGui::DestroyContext(gctx_);
      ImGui::SetCurrentContext(prev == gctx_ ? nullptr : prev);
      gctx_ = nullptr;
      gImguiReady_ = false;
    }
    if (grtv_) { grtv_->Release(); grtv_ = nullptr; }
    if (gvisual_) { gvisual_->Release(); gvisual_ = nullptr; }
    if (gtarget_) { gtarget_->Release(); gtarget_ = nullptr; }
    if (gdc_) { gdc_->Release(); gdc_ = nullptr; }
    if (gsc_) { gsc_->Release(); gsc_ = nullptr; }
    if (ghwnd_) { DestroyWindow(ghwnd_); ghwnd_ = nullptr; }
  }

  // Dibuja el overlay con la GPU: lo mismo que Draw, en el mismo orden (fondos, fila superior,
  // rejilla, referencia, relleno, banda, curva y etiquetas). Devuelve false si la GPU falla.
  bool DrawGpu(double fps, double low, double screenFps, const std::string& limitLabel, LimitState limitState,
               double refMs, bool isTarget, const std::vector<std::pair<double, double>>& real, double display,
               double swing, double baseSwing, std::string& why) {
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(gctx_);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(W_), static_cast<float>(H_));
    io.DeltaTime = 1.0f / 60.0f;
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    const float s = scale_;
    const unsigned generation = generation_;
    const double span = kPaceSpanSec;
    const bool locked = limitState == LimitState::Locked;
    const int sr = locked ? 47 : 255, sg = locked ? 168 : 107, sb = locked ? 255 : 107;
    auto C = [](int r, int g, int b, float a) {
      return IM_COL32(r, g, b, static_cast<int>(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f));
    };
    auto TextW = [&](ImFont* f, const std::string& t) {
      return f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, t.c_str()).x;
    };
    auto Text = [&](ImFont* f, float x, float y, ImU32 col, const std::string& t) {
      dl->AddText(f, f->FontSize, ImVec2(std::round(x), std::round(y)), col, t.c_str());
    };

    // Fondos
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(static_cast<float>(W_), static_cast<float>(H_)), C(14, 16, 21, 0.86f), 10 * s);
    const float gx0 = 10 * s, gx1 = W_ - 10 * s, gy0 = 34 * s, gy1 = H_ - 10 * s;
    dl->AddRectFilled(ImVec2(gx0, gy0), ImVec2(gx1, gy1), C(26, 30, 38, 0.92f), 6 * s);

    // Fila superior: punto de estado, FPS, 1 % bajo y etiqueta del limite
    dl->AddRectFilled(ImVec2(12 * s, 13 * s), ImVec2(20 * s, 21 * s), C(sr, sg, sb, 1.0f), 4 * s);
    const std::string fpsText = Fmt("%.0f", fps);
    Text(gBig_, 26 * s, 3 * s, C(240, 242, 245, 1.0f), fpsText);
    const float fw = TextW(gBig_, fpsText);
    Text(gSmall_, 26 * s + fw + 4 * s, 10 * s, C(150, 158, 170, 1.0f), "FPS");
    const std::string pill = locked ? std::string(T("FIJADO  ", "LOCKED  ")) + limitLabel
                             : limitState == LimitState::Pending ? std::string(T("SIN CONFIRMAR  ", "UNCONFIRMED  ")) + limitLabel
                                                                 : std::string(T("SIN LÍMITE", "NO LIMIT"));
    const float pw = TextW(gSmall_, pill);
    const float px1 = W_ - 10 * s, px0 = px1 - pw - 16 * s;
    const float subX = 26 * s + fw + 34 * s;
    const std::string sub = TopLine(low, screenFps, locked, refMs, px0 - 10 * s - subX,
                                    [&](const std::string& t) { return TextW(gSmall_, t); });
    Text(gSmall_, subX, 10 * s, C(150, 158, 170, 1.0f), sub);
    dl->AddRectFilled(ImVec2(px0, 7 * s), ImVec2(px1, 27 * s), C(sr, sg, sb, 0.18f), 10 * s);
    Text(gSmall_, px0 + 8 * s, 9 * s, C(sr, sg, sb, 1.0f), pill);

    // Grafica
    dl->PushClipRect(ImVec2(gx0, gy0), ImVec2(gx1, gy1), true);
    const float iy0 = gy0 + 6 * s, iy1 = gy1 - 4 * s;
    const double ref = refMs;
    const PaceRange pr = PaceScale(ref);
    auto Y = [&](double ms) {
      const double t = std::clamp((ms - pr.lo) / (pr.hi - pr.lo), 0.0, 1.0);
      return static_cast<float>(iy1 - t * (iy1 - iy0));
    };
    auto X = [&](float xf) { return gx0 + xf * (gx1 - gx0); };
    for (double k = std::ceil(display - span); k <= display; k += 1.0) {
      const float x = X(static_cast<float>(1.0 - (display - k) / span));
      dl->AddLine(ImVec2(x, gy0), ImVec2(x, gy1), C(255, 255, 255, 0.06f), 1.0f);
    }
    const float ry = Y(ref);
    for (float x = gx0; x < gx1; x += 8 * s)
      dl->AddLine(ImVec2(x, ry), ImVec2(std::min(x + 4 * s, gx1), ry), C(255, 255, 255, 0.28f), 1.0f);

    const int cols = std::max(2, static_cast<int>((gx1 - gx0) / (3.0f * s)));
    std::vector<PacePoint> pts = BuildTimeSeries(real, display, span, cols, ref, classCache_, generation, screenMode_);
    // Relleno degradado bajo la curva: el alfa depende solo de la altura (0,04 abajo, 0,40 arriba),
    // como en el clasico; cada tramo es un cuadrilatero con color por vertice.
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    const float full = std::max(1.0f, gy1 - gy0);
    auto FillCol = [&](float y) { return C(sr, sg, sb, 0.04f + 0.36f * std::clamp((gy1 - y) / full, 0.0f, 1.0f)); };
    for (size_t k = 1; k < pts.size(); ++k) {
      const float xa = X(pts[k - 1].x), ya = Y(pts[k - 1].ms), xb = X(pts[k].x), yb = Y(pts[k].ms);
      dl->PrimReserve(6, 4);
      const ImDrawIdx i0 = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
      dl->PrimWriteIdx(i0);
      dl->PrimWriteIdx(static_cast<ImDrawIdx>(i0 + 1));
      dl->PrimWriteIdx(static_cast<ImDrawIdx>(i0 + 2));
      dl->PrimWriteIdx(i0);
      dl->PrimWriteIdx(static_cast<ImDrawIdx>(i0 + 2));
      dl->PrimWriteIdx(static_cast<ImDrawIdx>(i0 + 3));
      dl->PrimWriteVtx(ImVec2(xa, ya), uv, FillCol(ya));
      dl->PrimWriteVtx(ImVec2(xb, yb), uv, FillCol(yb));
      dl->PrimWriteVtx(ImVec2(xb, gy1), uv, FillCol(gy1));
      dl->PrimWriteVtx(ImVec2(xa, gy1), uv, FillCol(gy1));
    }
    if (locked) {
      const float bTop = Y(ref + PaceTolerance(ref)), bBot = Y(std::max(0.0, ref - PaceTolerance(ref)));
      dl->AddRectFilled(ImVec2(gx0, bTop), ImVec2(gx1, bBot), C(sr, sg, sb, 0.12f));
      dl->AddLine(ImVec2(gx0, bTop), ImVec2(gx1, bTop), C(sr, sg, sb, 0.38f), 1.0f);
      dl->AddLine(ImVec2(gx0, bBot), ImVec2(gx1, bBot), C(sr, sg, sb, 0.38f), 1.0f);
    }
    // Curva: tramos seguidos del mismo color en una sola linea (uniones suaves); los tirones en ambar.
    std::vector<ImVec2> run;
    bool runSpike = false;
    auto flush = [&]() {
      if (run.size() >= 2)
        dl->AddPolyline(run.data(), static_cast<int>(run.size()), runSpike ? C(245, 185, 66, 1.0f) : C(sr, sg, sb, 1.0f),
                        ImDrawFlags_None, 1.8f * s);
      run.clear();
    };
    for (size_t k = 1; k < pts.size(); ++k) {
      const bool spike = pts[k].spike || pts[k - 1].spike;
      const ImVec2 a(X(pts[k - 1].x), Y(pts[k - 1].ms)), b(X(pts[k].x), Y(pts[k].ms));
      if (run.empty() || spike != runSpike) {
        flush();
        runSpike = spike;
        run.push_back(a);
      }
      run.push_back(b);
    }
    flush();

    // Etiquetas
    const float ly = std::floor(gy1 - 19 * s);
    if (swing >= 0) {
      std::string sw = Fmt(T("variación ±%.1f ms", "swing ±%.1f ms"), swing);
      if (locked && baseSwing > 0) sw += Fmt(T("  ·  antes ±%.1f", "  ·  before ±%.1f"), baseSwing);
      const float swW = TextW(gSmall_, sw);
      dl->AddRectFilled(ImVec2(gx0 + 4 * s, ly - 1 * s), ImVec2(gx0 + 14 * s + swW, ly + 16 * s), C(20, 24, 31, 0.85f), 5 * s);
      Text(gSmall_, gx0 + 9 * s, ly, C(200, 206, 214, 1.0f), sw);
    }
    const std::string refText =
        Fmt(screenMode_ ? (isTarget ? T("en pantalla %.1f ms ±%.1f", "on screen %.1f ms ±%.1f")
                                    : T("en pantalla ~%.1f ms ±%.1f", "on screen ~%.1f ms ±%.1f"))
                        : isTarget ? T("objetivo %.1f ms ±%.1f", "target %.1f ms ±%.1f") : T("actual %.1f ms ±%.1f", "current %.1f ms ±%.1f"),
            ref, PaceTolerance(ref));
    const float tw = TextW(gSmall_, refText);
    const float lx = gx1 - 9 * s - tw;
    dl->AddRectFilled(ImVec2(lx - 5 * s, ly - 1 * s), ImVec2(gx1 - 4 * s, ly + 16 * s), C(20, 24, 31, 0.85f), 5 * s);
    Text(gSmall_, lx, ly, C(150, 158, 170, 1.0f), refText);
    dl->PopClipRect();

    ImGui::Render();
    const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    ctx_->OMSetRenderTargets(1, &grtv_, nullptr);
    ctx_->ClearRenderTargetView(grtv_, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    // Sin esperar al refresco: el hilo de la ventana no se bloquea; Windows muestra la ultima imagen.
    const HRESULT hr = gsc_->Present(0, 0);
    ImGui::SetCurrentContext(prev);
    if (FAILED(hr)) {
      char buf[64];
      snprintf(buf, sizeof(buf), "Present, error 0x%08lX", static_cast<unsigned long>(hr));
      why = buf;
      return false;
    }
    return true;
  }

  ID3D11Device* dev_ = nullptr;
  ID3D11DeviceContext* ctx_ = nullptr;
  double curLat_ = -1.0, baseLat_ = -1.0;  // latencia del PC actual y sin limite (ms)
  bool gpu_ = false;
  bool gImguiReady_ = false;
  std::string pendingNote_;
  HWND ghwnd_ = nullptr;
  POINT gpuPos_{-100000, -100000};
  IDXGISwapChain1* gsc_ = nullptr;
  IDCompositionDevice* gdc_ = nullptr;
  IDCompositionTarget* gtarget_ = nullptr;
  IDCompositionVisual* gvisual_ = nullptr;
  ID3D11RenderTargetView* grtv_ = nullptr;
  ImGuiContext* gctx_ = nullptr;
  ImFont* gSmall_ = nullptr;
  ImFont* gBig_ = nullptr;

  // Pixel con alfa premultiplicado (formato BGRA)
  void Blend(int x, int y, uint8_t r, uint8_t g, uint8_t b, float a) {
    if (x < 0 || y < 0 || x >= W_ || y >= H_ || a <= 0) return;
    if (a > 1.0f) a = 1.0f;
    // Mezcla con enteros (antes, coma flotante): unas 5 veces mas rapido, con una diferencia de
    // color de como mucho 2 niveles sobre 255.
    uint32_t& p = px_[y * W_ + x];
    const uint32_t a8 = static_cast<uint32_t>(a * 256.0f + 0.5f), ia8 = 256 - a8;
    const uint32_t pb = p & 0xFF, pg = (p >> 8) & 0xFF, pr = (p >> 16) & 0xFF, pa = p >> 24;
    p = ((b * a8 + pb * ia8) >> 8) | (((g * a8 + pg * ia8) >> 8) << 8) | (((r * a8 + pr * ia8) >> 8) << 16) |
        (((255 * a8 + pa * ia8) >> 8) << 24);
  }

  // Rectangulo con esquinas redondeadas y bordes suavizados.
  void RoundRect(float x0, float y0, float x1, float y1, float rad, uint8_t r, uint8_t g, uint8_t b, float a) {
    for (int y = static_cast<int>(y0); y < static_cast<int>(std::ceil(y1)); ++y) {
      const float py = y + 0.5f;
      const bool midRow = py >= y0 + rad && py <= y1 - rad;
      for (int x = static_cast<int>(x0); x < static_cast<int>(std::ceil(x1)); ++x) {
        float px = x + 0.5f;
        if (midRow && px >= x0 + rad && px <= x1 - rad) {  // interior: cobertura completa, sin calculo
          Blend(x, y, r, g, b, a * std::min(rad + 0.5f, 1.0f));
          continue;
        }
        float cx = std::clamp(px, x0 + rad, x1 - rad), cy = std::clamp(py, y0 + rad, y1 - rad);
        float d = std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
        float cov = std::clamp(rad - d + 0.5f, 0.0f, 1.0f);
        if (cov > 0) Blend(x, y, r, g, b, a * cov);
      }
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
        const float ex = px - t * dx, ey = py - t * dy;
        float d = std::sqrt(ex * ex + ey * ey);
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

  // Textos guardados: cada texto se dibuja con Windows (GDI) solo la primera vez; despues se reutiliza
  // su mascara. Antes se median y dibujaban todos los textos en cada fotograma del overlay.
  struct CachedText {
    int w = 0, h = 0, width = 0;
    std::vector<uint8_t> lum;  // cobertura (0-255) de un rectangulo (w x h) con 2 px de margen
  };
  std::map<std::pair<HFONT, std::string>, CachedText> textCache_;

  const CachedText& GetText(const std::string& utf8, HFONT font) {
    auto key = std::make_pair(font, utf8);
    auto it = textCache_.find(key);
    if (it != textCache_.end()) return it->second;
    if (textCache_.size() > 96) textCache_.clear();  // los valores cambian (FPS): se renueva de vez en cuando
    CachedText ct;
    SelectObject(maskDc_, font);
    std::wstring w = Widen(utf8);
    SIZE sz{};
    GetTextExtentPoint32W(maskDc_, w.c_str(), static_cast<int>(w.size()), &sz);
    ct.width = sz.cx;
    ct.w = std::min(W_, static_cast<int>(sz.cx) + 6);
    ct.h = std::min(H_, static_cast<int>(sz.cy) + 6);
    for (int yy = 0; yy < ct.h; ++yy) std::memset(mask_ + yy * W_, 0, static_cast<size_t>(ct.w) * 4);
    SetBkMode(maskDc_, TRANSPARENT);
    SetTextColor(maskDc_, RGB(255, 255, 255));
    TextOutW(maskDc_, 2, 2, w.c_str(), static_cast<int>(w.size()));
    GdiFlush();
    ct.lum.resize(static_cast<size_t>(ct.w) * ct.h);
    for (int yy = 0; yy < ct.h; ++yy)
      for (int xx = 0; xx < ct.w; ++xx) {
        uint32_t m = mask_[yy * W_ + xx];
        ct.lum[static_cast<size_t>(yy) * ct.w + xx] = std::max({static_cast<uint8_t>(m & 0xFF), static_cast<uint8_t>((m >> 8) & 0xFF),
                                                               static_cast<uint8_t>((m >> 16) & 0xFF)});
      }
    return textCache_.emplace(key, std::move(ct)).first->second;
  }

  int TextWidth(const std::string& utf8, HFONT font) { return GetText(utf8, font).width; }



  // Texto: se dibuja en blanco sobre negro en la mascara (solo en su rectangulo) y se mezcla
  // con su luminancia.
  void Text(const std::string& utf8, int x, int y, HFONT font, uint8_t r, uint8_t g, uint8_t b) {
    const CachedText& ct = GetText(utf8, font);
    for (int yy = 0; yy < ct.h; ++yy)
      for (int xx = 0; xx < ct.w; ++xx) {
        const uint8_t lum = ct.lum[static_cast<size_t>(yy) * ct.w + xx];
        if (lum) Blend(x - 2 + xx, y - 2 + yy, r, g, b, lum / 255.0f);
      }
  }

  void Draw(double now, double fps, double low, double screenFps, const std::string& limitLabel,
            LimitState limitState, double refMs, bool isTarget, const std::vector<std::pair<double, double>>& real,
            double swing, double baseSwing) {
    const unsigned generation = generation_;
    clipX0_ = 0; clipY0_ = 0; clipX1_ = W_; clipY1_ = H_;
    const float s = scale_;
    // Fondos fijos (tarjeta y area de la grafica): se dibujan una vez y se copian en cada fotograma.
    if (baseW_ != W_ || baseH_ != H_ || baseScale_ != s || base_.empty()) {
      std::memset(px_, 0, static_cast<size_t>(W_) * H_ * 4);
      RoundRect(0, 0, static_cast<float>(W_), static_cast<float>(H_), 10 * s, 14, 16, 21, 0.86f);
      RoundRect(10 * s, 34 * s, W_ - 10 * s, H_ - 10 * s, 6 * s, 26, 30, 38, 0.92f);
      base_.assign(px_, px_ + static_cast<size_t>(W_) * H_);
      baseW_ = W_;
      baseH_ = H_;
      baseScale_ = s;
    } else {
      std::memcpy(px_, base_.data(), base_.size() * 4);
    }
    const double span = kPaceSpanSec;
    // Color de estado: azul con el limite confirmado; rojo sin limite o sin confirmar.
    const bool locked = limitState == LimitState::Locked;
    const uint8_t sr = locked ? 47 : 255, sg = locked ? 168 : 107, sb = locked ? 255 : 107;

    // Fila superior: punto de estado, FPS, 1 % bajo y etiqueta del limite (el fondo esta en base_)
    RoundRect(12 * s, 13 * s, 20 * s, 21 * s, 4 * s, sr, sg, sb, 1.0f);
    std::string fpsText = Fmt("%.0f", fps);
    Text(fpsText, static_cast<int>(26 * s), static_cast<int>(3 * s), big_, 240, 242, 245);
    int fw = TextWidth(fpsText, big_);
    Text("FPS", static_cast<int>(26 * s) + fw + static_cast<int>(4 * s), static_cast<int>(10 * s), small_, 150, 158, 170);
    std::string pill = locked ? std::string(T("FIJADO  ", "LOCKED  ")) + limitLabel
                       : limitState == LimitState::Pending ? std::string(T("SIN CONFIRMAR  ", "UNCONFIRMED  ")) + limitLabel
                                                           : std::string(T("SIN LÍMITE", "NO LIMIT"));
    int pw = TextWidth(pill, small_);
    float px1 = W_ - 10 * s, px0 = px1 - pw - 16 * s;
    const int subX = static_cast<int>(26 * s) + fw + static_cast<int>(34 * s);
    const std::string sub = TopLine(low, screenFps, locked, refMs, px0 - 10 * s - subX,
                                    [&](const std::string& t) { return static_cast<float>(TextWidth(t, small_)); });
    Text(sub, subX, static_cast<int>(10 * s), small_, 150, 158, 170);
    RoundRect(px0, 7 * s, px1, 27 * s, 10 * s, sr, sg, sb, 0.18f);
    Text(pill, static_cast<int>(px0 + 8 * s), static_cast<int>(9 * s), small_, sr, sg, sb);

    // Grafica
    const float gx0 = 10 * s, gx1 = W_ - 10 * s, gy0 = 34 * s, gy1 = H_ - 10 * s;  // fondo: en base_
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
    std::vector<PacePoint> pts = BuildTimeSeries(real, display, span, cols, ref, classCache_, generation, screenMode_);
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
    std::string refText = Fmt(screenMode_ ? (isTarget ? T("en pantalla %.1f ms ±%.1f", "on screen %.1f ms ±%.1f")
                                                      : T("en pantalla ~%.1f ms ±%.1f", "on screen ~%.1f ms ±%.1f"))
                              : isTarget ? T("objetivo %.1f ms ±%.1f", "target %.1f ms ±%.1f") : T("actual %.1f ms ±%.1f", "current %.1f ms ±%.1f"),
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
  std::vector<uint32_t> base_;  // fondos fijos ya dibujados
  int baseW_ = 0, baseH_ = 0;
  float baseScale_ = 0.0f;
  HFONT big_ = nullptr, small_ = nullptr;
  int W_ = 0, H_ = 0;
  int clipX0_ = 0, clipY0_ = 0, clipX1_ = 0, clipY1_ = 0;
  float scale_ = 1.0f;
  PaceClassCache classCache_;
  unsigned generation_ = 0;
  bool screenMode_ = false;
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
  SessionDecisionInput in;
  in.realByFg = ss.sessionReal;
  in.gameSecs = ss.gameSecs;
  in.vrrSecs = ss.vrrSecs;
  in.ownCapSecs = ss.ownCapSecs;
  in.ownCapRateSum = ss.ownCapRateSum;
  in.fluid = ss.fluid;
  in.hz = hz;
  in.capReal = ss.expectedFps > 0 && (ss.verified || ss.appliedAtStart) ? ss.expectedFps : 0.0;
  return DecideFromSession(in);
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
    u.usesFluid = GameUsesFluid(exe, &u.prioMode, &u.autoStability);
    u.value = static_cast<int>(c.newValue);
    u.fg = std::max(1, c.fgFactor);
    u.manual = c.manual;
    u.vrr = c.vrr;
    u.divisor = c.divisor;
    u.explain = ReadKv(L"equipo.txt", "explica|" + exe);
    u.lastSession = ReadKv(L"equipo.txt", "partida|" + exe);
    u.climb = ReadKv(L"equipo.txt", "escalada|" + exe);
    auto sg = g_ui.suggestions.find(exe);
    if (sg != g_ui.suggestions.end() && !c.manual) {
      u.suggestFps = sg->second;
      auto sk = g_ui.suggestKind.find(exe);
      u.suggestKind = sk != g_ui.suggestKind.end() ? sk->second : 0;
    }
    u.prioSuggest = !c.manual && g_ui.prioSuggest.count(exe) > 0;
    auto w = g_ui.warnings.find(exe);
    if (w != g_ui.warnings.end()) u.warning = w->second;
    if (g_limiterKind == 1 && !c.verified && u.warning.empty()) {
      u.warning = GuidedInstruction(u.label, c.newValue, c.vrr);
      u.guidedPending = true;
    }
    if (g_vrrMissing && c.vrr && u.warning.empty())
      u.warning = T("Decidido con G-SYNC, que ahora no está activo: este límite puede empeorar el movimiento. "
                    "Activa G-SYNC (aviso de arriba) o pulsa «Deshacer» para recalcularlo.",
                    "Decided with G-SYNC, which isn't active now: this limit can make motion worse. Turn "
                    "G-SYNC on (notice above) or press “Undo” to recalculate it.");
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
// Modo guiado: no toca ningun driver. "Aplicar" registra el limite; la instruccion para el usuario y la
// comprobacion de que funciona (midiendo el juego) las hacen la ventana y el motor.
class GuidedBackend final : public LimitBackend {
 public:
  const char* Vendor() const override { return GuidedSoftware(); }
  bool Apply(const std::string& exe, const LimitRequest& req, AppliedChange& change) override {
    if (req.frl == 0) return false;  // el VSync por intervalos es exclusivo del driver de NVIDIA
    if (change.exe.empty()) {
      change = AppliedChange{};
      change.exe = exe;
    }
    return true;
  }
  bool Revert(const AppliedChange&) override { return true; }
  bool FindOwn(const std::string&, AppliedChange&) override { return false; }
  bool HasExtra(const AppliedChange&, LimitExtra) const override { return false; }
  bool AddExtra(AppliedChange&, LimitExtra) override { return false; }
  bool RemoveExtra(AppliedChange&, LimitExtra) override { return false; }
  bool Vrr(const std::string&, bool&, bool&) override { return false; }
  std::string Error() const override { return ""; }
};

// RivaTuner Statistics Server (RTSS): limite automatico con cualquier grafica, si esta instalado. Se usa su
// propia biblioteca (RTSSHooks64.dll, en la carpeta de RTSS), como hacen otras herramientas: se carga el
// perfil del juego ("juego.exe"), se cambia FramerateLimit (fps enteros, hasta 1000), se guarda y se pide a
// RTSS que recargue los perfiles. Solo se declaran sus funciones; no se incluye ningun archivo de RTSS.
// Para deshacer, se guarda el limite que tenia el perfil (si existia) o se borra el perfil creado.
constexpr uint32_t kRtssLimitId = 0x52545353u;  // "RTSS": entrada en AppliedChange::saved
class RtssBackend final : public LimitBackend {
 public:
  ~RtssBackend() override {
    if (dll_) FreeLibrary(dll_);
  }
  bool Init() {
    wchar_t dir[MAX_PATH] = {};
    DWORD size = sizeof(dir), type = 0;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Unwinder\\RTSS", 0, KEY_READ, &key) == ERROR_SUCCESS) {
      if (RegQueryValueExW(key, L"InstallDir", nullptr, &type, reinterpret_cast<LPBYTE>(dir), &size) != ERROR_SUCCESS ||
          type != REG_SZ)
        dir[0] = 0;
      RegCloseKey(key);
    }
    dir_ = dir[0] ? dir : L"C:\\Program Files (x86)\\RivaTuner Statistics Server";
    dll_ = LoadLibraryW((dir_ + L"\\RTSSHooks64.dll").c_str());
    if (!dll_) {
      error_ = "RTSS no instalado";
      return false;
    }
    load_ = Proc<ProfileFn>("LoadProfile");
    save_ = Proc<ProfileFn>("SaveProfile");
    del_ = Proc<ProfileFn>("DeleteProfile");
    get_ = Proc<PropFn>("GetProfileProperty");
    set_ = Proc<PropFn>("SetProfileProperty");
    update_ = Proc<UpdateFn>("UpdateProfiles");
    if (!load_ || !save_ || !del_ || !get_ || !set_ || !update_) {
      error_ = "RTSSHooks64.dll sin las funciones de perfiles";
      FreeLibrary(dll_);
      dll_ = nullptr;
      return false;
    }
    return true;
  }
  const std::wstring& Dir() const { return dir_; }
  const char* Vendor() const override { return "RTSS"; }
  bool Apply(const std::string& exe, const LimitRequest& req, AppliedChange& change) override {
    if (!dll_ || req.frl == 0 || req.frl > 1000) {
      error_ = req.frl == 0 ? "RTSS no hace VSync por intervalos" : "limite fuera del rango de RTSS";
      return false;
    }
    if (change.exe.empty()) {
      change = AppliedChange{};
      change.exe = exe;
    }
    const bool known = std::any_of(change.saved.begin(), change.saved.end(),
                                   [](const SavedSetting& sv) { return sv.id == kRtssLimitId; });
    if (!known) {
      // Lo que habia antes, para deshacerlo: el limite del perfil, si el juego ya tenia uno en RTSS.
      SavedSetting sv;
      sv.id = kRtssLimitId;
      sv.had = GetFileAttributesW((dir_ + L"\\Profiles\\" + Widen(exe) + L".cfg").c_str()) != INVALID_FILE_ATTRIBUTES;
      if (sv.had) {
        DWORD old = 0;
        load_(exe.c_str());
        if (get_("FramerateLimit", reinterpret_cast<BYTE*>(&old), sizeof(old))) sv.old = old;
      }
      change.saved.push_back(sv);
    }
    DWORD den = 1, fps = req.frl;
    load_(exe.c_str());
    set_("FramerateLimitDenominator", reinterpret_cast<BYTE*>(&den), sizeof(den));  // fps enteros
    if (!set_("FramerateLimit", reinterpret_cast<BYTE*>(&fps), sizeof(fps))) {
      error_ = "RTSS no acepto el limite";
      return false;
    }
    save_(exe.c_str());
    update_();
    return true;
  }
  bool Revert(const AppliedChange& change) override {
    if (!dll_) {
      error_ = "RTSS no disponible";
      return false;
    }
    for (const auto& sv : change.saved)
      if (sv.id == kRtssLimitId) {
        if (sv.had) {
          DWORD old = sv.old;
          load_(change.exe.c_str());
          set_("FramerateLimit", reinterpret_cast<BYTE*>(&old), sizeof(old));
          save_(change.exe.c_str());
        } else {
          del_(change.exe.c_str());
        }
        update_();
      }
    return true;
  }
  bool FindOwn(const std::string&, AppliedChange&) override { return false; }
  bool HasExtra(const AppliedChange&, LimitExtra) const override { return false; }
  bool AddExtra(AppliedChange&, LimitExtra) override { return false; }
  bool RemoveExtra(AppliedChange&, LimitExtra) override { return false; }
  bool Vrr(const std::string&, bool&, bool&) override { return false; }
  std::string Error() const override { return error_; }

 private:
  template <typename F>
  F Proc(const char* name) const {
    return reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(dll_, name)));
  }
  using ProfileFn = void (*)(const char*);
  using PropFn = BOOL (*)(const char*, BYTE*, DWORD);
  using UpdateFn = void (*)();
  HMODULE dll_ = nullptr;
  std::wstring dir_;
  std::string error_;
  ProfileFn load_ = nullptr, save_ = nullptr, del_ = nullptr;
  PropFn get_ = nullptr, set_ = nullptr;
  UpdateFn update_ = nullptr;
};

static bool ProcessRunning(const wchar_t* name) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  bool found = false;
  for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
    found = _wcsicmp(pe.szExeFile, name) == 0;
  CloseHandle(snap);
  return found;
}

// El limitador en uso: NVIDIA (automatico), RTSS o el guiado; se elige al detectar las graficas.
class BackendSelector final : public LimitBackend {
 public:
  explicit BackendSelector(LimitBackend* nv) : b_(nv), nv_(nv) {}
  void Use(LimitBackend* b) { b_ = b; }
  void SetRtss(LimitBackend* r) { rtss_ = r; }
  const char* Vendor() const override { return b_->Vendor(); }
  bool Apply(const std::string& e, const LimitRequest& r, AppliedChange& c) override { return b_->Apply(e, r, c); }
  bool Revert(const AppliedChange& c) override {
    // Cada cambio se deshace con la pieza que lo aplico, aunque ahora se use otra: RTSS si guardo su
    // entrada; el driver de NVIDIA si guardo ajustes del driver; si no (modo guiado), la actual.
    const bool viaRtss = std::any_of(c.saved.begin(), c.saved.end(), [](const SavedSetting& sv) { return sv.id == kRtssLimitId; });
    if (viaRtss) return rtss_ && rtss_->Revert(c);
    if (!c.saved.empty() || c.createdProfile) return nv_->Revert(c);
    return b_->Revert(c);
  }
  bool FindOwn(const std::string& e, AppliedChange& o) override { return b_->FindOwn(e, o); }
  bool HasExtra(const AppliedChange& c, LimitExtra x) const override { return b_->HasExtra(c, x); }
  bool AddExtra(AppliedChange& c, LimitExtra x) override { return b_->AddExtra(c, x); }
  bool RemoveExtra(AppliedChange& c, LimitExtra x) override { return b_->RemoveExtra(c, x); }
  bool Vrr(const std::string& g, bool& p, bool& a) override { return b_->Vrr(g, p, a); }
  std::string Error() const override { return b_->Error(); }

 private:
  LimitBackend* b_;
  LimitBackend* nv_;
  LimitBackend* rtss_ = nullptr;
};

// Instruccion para aplicar a mano un limite en el software de la grafica (modo guiado). Con VRR, se
// recuerda tambien el VSync del driver (con NVIDIA lo activa la app: con el limite, evita salirse del
// rango del VRR); y siempre, la opcion de baja latencia del fabricante, en lugar de la de NVIDIA.
static std::string GuidedInstruction(const std::string& label, uint32_t fps, bool vrr) {
  if (g_guidedVendor == 1)
    return Fmt(T("Aplícalo tú: en AMD Software, Juegos → %s → Radeon Chill, con mínimo y máximo a %u FPS (o usa el "
                 "limitador del propio juego).%s Para menos retraso, activa también Radeon Anti-Lag. Al abrir el "
                 "juego, la app comprobará que funciona. Con RivaTuner (RTSS) instalado, lo aplicaría yo.",
                 "Apply it yourself: in AMD Software, Gaming → %s → Radeon Chill, with minimum and maximum at %u FPS "
                 "(or use the game's own limiter).%s For less delay, also turn on Radeon Anti-Lag. When you open the "
                 "game, the app will check that it works. With RivaTuner (RTSS) installed, I'd apply it myself."),
               label.c_str(), fps,
               vrr ? T(" Con FreeSync, pon también «Esperar a la actualización vertical» en «Siempre activado», "
                       "para no salirte de su rango.",
                       " With FreeSync, also set “Wait for Vertical Refresh” to “Always On”, so you stay within "
                       "its range.")
                   : "");
  return Fmt(T("Aplícalo tú: en Intel Graphics Software, en la configuración de %s, pon el limitador de FPS a %u (o usa "
               "el limitador del propio juego).%s Si tu versión tiene una opción de baja latencia, actívala también. "
               "Al abrir el juego, la app comprobará que funciona. Con RivaTuner (RTSS) instalado, lo aplicaría yo.",
               "Apply it yourself: in Intel Graphics Software, in %s's settings, set the FPS limiter to %u (or use the "
               "game's own limiter).%s If your version has a low latency option, turn it on too. When you open the "
               "game, the app will check that it works. With RivaTuner (RTSS) installed, I'd apply it myself."),
             label.c_str(), fps,
             vrr ? T(" Con Adaptive-Sync, activa también la sincronización vertical (VSync), para no salirte de su "
                     "rango.",
                     " With Adaptive-Sync, also turn on vertical sync (VSync), so you stay within its range.")
                 : "");
}

// Texto del limitador en uso para la ventana (seccion Equipo).
static std::string LimiterUiText(const std::string& nvError) {
  if (g_limiterKind == 2) return T("RTSS: límites automáticos con RivaTuner", "RTSS: automatic limits with RivaTuner");
  if (g_limiterKind == 1)
    return std::string(g_guidedVendor == 1 ? "AMD" : "Intel") +
           T(": solo recomendar (tú aplicas los límites; la app los comprueba)",
             ": recommend only (you apply the limits; the app checks them)");
  if (g_nvidiaAvailable) return T("NVIDIA: disponible", "NVIDIA: available");
  return std::string(T("NVIDIA: no disponible (", "NVIDIA: not available (")) + nvError + ")";
}

static void LogLimiter(const std::string& rtssDir) {
  if (g_limiterKind == 2) {
    Log("  Limitador: RivaTuner Statistics Server (RTSS, %s): aplico los limites en su perfil de cada juego.\n",
        rtssDir.c_str());
    if (!ProcessRunning(L"RTSS.exe"))
      Log("  Aviso: RTSS no esta abierto. Su limitador solo actua mientras esta en marcha: abrelo antes de jugar.\n");
    Log("  Con G-SYNC/FreeSync, activa tambien el VSync del driver (no lo hace RTSS): con el limite, evita\n"
        "  salirse del rango del VRR.\n");
    if (g_nvidiaAvailable) Log("  (Con NVIDIA: sin \"GPU al maximo\" ni baja latencia del driver.)\n");
  } else if (g_limiterKind == 1) {
    Log("  Limitador: solo recomendar (%s): mido, decido y compruebo los limites; tu los aplicas en %s.\n"
        "  Con RivaTuner (RTSS) instalado, puedes elegirlo como limitador en Juegos. (El modo automatico con el\n"
        "  driver solo existe con NVIDIA: ver el README.)\n",
        g_guidedVendor == 1 ? "AMD" : "Intel", GuidedSoftware());
  } else if (g_nvidiaAvailable) {
    Log("  Limitador: driver de NVIDIA.\n");
  }
}

static std::string UndoAll(LimitBackend& nvd, ChangeStore& store, bool nvOk) {
  if (store.items.empty()) return T("No hay cambios de FlatPace que deshacer.", "There are no FlatPace changes to undo.");
  if (!nvOk) return T("No puedo acceder al driver de NVIDIA (", "I can't access the NVIDIA driver (") + nvd.Error() + ").";
  int done = 0, fails = 0;
  for (auto it = store.items.begin(); it != store.items.end();) {
    if (nvd.Revert(it->second)) {
      Log("  Deshecho: %s (vuelve a como estaba antes)\n", it->first.c_str());
      it = store.items.erase(it);
      ++done;
    } else {
      Log("  No pude deshacer %s: %s\n", it->first.c_str(), nvd.Error().c_str());
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
  g_ui.bottleneck = st.frames ? BottleneckText(st) : std::string("—");
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
  eco::engineThread = eco::OpenCurrentThread();
  CpuPerfMeter cpuPerf;
  cpuPerf.Init();
  std::wstring logPath = OpenLog();
  {
    std::lock_guard<std::mutex> lock(g_ui.m);
    g_ui.logPath = logPath;
  }
  if (g_log) std::fprintf(g_log, "FlatPace %s\n\n", kVersion);
  SetStatus(UiStatus::Starting, T("Iniciando…", "Starting…"), T("Revisando tu equipo y midiendo la pantalla.", "Checking your system and measuring the display."));

  NvDriver nvDriver;
  bool nvOk = nvDriver.Init();
  NvidiaBackend nvBackend(nvDriver);
  GuidedBackend guidedBackend;
  RtssBackend rtssBackend;
  const bool rtssOk = rtssBackend.Init();
  BackendSelector backend(&nvBackend);
  if (rtssOk) backend.SetRtss(&rtssBackend);
  LimitBackend& nvd = backend;  // el limitador en uso, comun a los fabricantes
  bool limiterOk = nvOk;        // hay forma de aplicar limites (NVIDIA, o el modo guiado)
  ChangeStore store(DataDirectory() + L"\\cambios-aplicados.txt");
  store.Load();
  if (int moved = store.Migrate(ExeDirectory() + L"\\cambios-aplicados.txt"))
    Log("Cambios de una version anterior movidos a la carpeta de datos: %d\n", moved);
  if (const int cleaned = CleanObsoleteKv(); cleaned > 0)
    Log("Ajustes: %d clave%s antigua%s eliminada%s de equipo.txt (de versiones anteriores, ya sin uso).\n", cleaned,
        cleaned == 1 ? "" : "s", cleaned == 1 ? "" : "s", cleaned == 1 ? "" : "s");
  if (ReadKv(L"equipo.txt", "rendimiento-maximo") == "no") g_maxPerf = false;
  if (ReadKv(L"equipo.txt", "baja-latencia") == "no") g_lowLatency = false;
  WriteKv(L"equipo.txt", "subir-solo", "");  // ajuste de la 0.46-0.54, sustituido por la prioridad automatica
  {
    std::lock_guard<std::mutex> lock(g_ui.m);
    for (const auto& [exe, c] : store.items) {
      const std::string sv = ReadKv(L"equipo.txt", "sugerencia|" + exe);
      if (!sv.empty() && !c.manual) {
        double to = 0, from = c.newValue;
        int kind = 0;
        std::sscanf(sv.c_str(), "%lf;%d;%lf", &to, &kind, &from);
        if (to > 0) {
          g_ui.suggestions[exe] = to;
          g_ui.suggestKind[exe] = kind;
          g_ui.warnings[exe] = RecommendationText(kind, from, to);
        }
      }
      if (!c.manual && ReadKv(L"equipo.txt", "prioridad-sugerida|" + exe) == "si") {
        g_ui.prioSuggest.insert(exe);
        if (!g_ui.warnings.count(exe)) g_ui.warnings[exe] = T(kPrioSuggestText_es, kPrioSuggestText_en);
      }
    }
  }
  // Desactivados en equipo.txt: quitarlos tambien de los limites que ya los tenian (hasta la 0.50.1
  // solo se dejaban de anadir a los nuevos). Con el VSync por intervalos, la cola de un fotograma forma
  // parte del metodo y se mantiene.
  if (nvOk && (!g_lowLatency || !g_maxPerf)) {
    int removedLl = 0, removedPerf = 0;
    for (auto& [exe, c] : store.items) {
      auto has = [&](uint32_t id) {
        return std::any_of(c.saved.begin(), c.saved.end(), [&](const SavedSetting& sv) { return sv.id == id; });
      };
      if (!g_lowLatency && c.method != Method::DriverVsync && has(nv::PRERENDERLIMIT_ID) &&
          nvd.RemoveExtra(c, LimitExtra::LowLatency))
        ++removedLl;
      if (!g_maxPerf && has(nv::PREFERRED_PSTATE_ID) && nvd.RemoveExtra(c, LimitExtra::MaxPerf)) ++removedPerf;
    }
    if (removedLl + removedPerf > 0) {
      store.Save();
      if (removedLl > 0)
        Log("Modo de baja latencia quitado de %d juego%s (baja-latencia=no; actua al abrir el juego).\n", removedLl,
            removedLl == 1 ? "" : "s");
      if (removedPerf > 0)
        Log("Rendimiento maximo de la GPU quitado de %d juego%s (rendimiento-maximo=no; actua al abrir el juego).\n",
            removedPerf, removedPerf == 1 ? "" : "s");
    }
  }
  if (nvOk && g_lowLatency) {
    int added = 0;
    for (auto& [exe, c] : store.items) {
      bool has = false;
      for (const auto& sv : c.saved) has = has || sv.id == nv::PRERENDERLIMIT_ID;
      if (!has && nvd.AddExtra(c, LimitExtra::LowLatency)) ++added;
    }
    if (added > 0) {
      store.Save();
      Log("Modo de baja latencia (cola de un fotograma) anadido a %d juego%s con limite (actua al abrir el juego).\n",
          added, added == 1 ? "" : "s");
    }
  }
  if (nvOk && g_maxPerf) {
    int added = 0;
    for (auto& [exe, c] : store.items) {
      bool has = false;
      for (const auto& sv : c.saved) has = has || sv.id == nv::PREFERRED_PSTATE_ID;
      if (!has && nvd.AddExtra(c, LimitExtra::MaxPerf)) ++added;
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

  Log("=== Equipo ===\n  Tipo: %s\n", laptop ? T("portatil (bateria detectada)", "laptop (battery detected)") : T("sobremesa", "desktop"));
  // Sin NVIDIA, con una grafica AMD o Intel: modo guiado (se prefiere la dedicada).
  if (!nvOk) {
    int vendor = 0;
    for (int pass = 0; pass < 2 && vendor == 0; ++pass)
      for (const auto& a : adapters) {
        if (pass == 0 && a.integrated) continue;
        std::wstring n = a.name;
        std::transform(n.begin(), n.end(), n.begin(), ::towlower);
        if (n.find(L"radeon") != std::wstring::npos || n.find(L"amd") != std::wstring::npos) vendor = 1;
        else if (n.find(L"intel") != std::wstring::npos) vendor = 2;
        if (vendor) break;
      }
    g_guidedVendor = vendor;
  }
  // Limitador elegido: RTSS solo si el usuario lo eligio (y esta instalado); si no, el driver de NVIDIA o,
  // con AMD o Intel, "Solo recomendar" (modo guiado).
  g_nvidiaAvailable = nvOk;
  g_rtssAvailable = rtssOk;
  {
    const int byDefault = nvOk ? 0 : g_guidedVendor != 0 ? 1 : -1;
    const std::string chosen = ReadKv(L"equipo.txt", "limitador");
    // Con NVIDIA, siempre el driver (desde la 0.59: RTSS solo se ofrece con AMD o Intel).
    int kind = chosen == "rtss" && rtssOk && !nvOk ? 2 : byDefault;
    if (chosen == "rtss" && !rtssOk) Log("  Elegiste RTSS como limitador, pero no lo encuentro instalado: uso el habitual.\n");
    if (kind < 0 && rtssOk && chosen == "rtss") kind = 2;
    // Limites que quedaron en RTSS con NVIDIA (se podia elegir en la 0.58): se devuelven al driver con el
    // mismo mecanismo que el cambio de limitador desde la ventana.
    if (nvOk && rtssOk) {
      const bool inRtss = std::any_of(store.items.begin(), store.items.end(), [](const auto& kv) {
        return std::any_of(kv.second.saved.begin(), kv.second.saved.end(),
                           [](const SavedSetting& sv) { return sv.id == kRtssLimitId; });
      });
      if (inRtss) {
        kind = 2;          // se empieza como si se usara RTSS...
        g_limiterReq = 0;  // ...y el motor los mueve al driver en cuanto arranca
        Log("  Hay limites en RTSS de una version anterior: los devuelvo al driver de NVIDIA.\n");
      }
    }
    if (nvOk && chosen == "rtss") WriteKv(L"equipo.txt", "limitador", "driver");
    g_limiterKind = std::max(kind, 0);
    backend.Use(kind == 2 ? static_cast<LimitBackend*>(&rtssBackend)
                : kind == 1 ? static_cast<LimitBackend*>(&guidedBackend)
                            : static_cast<LimitBackend*>(&nvBackend));
    limiterOk = kind >= 0;
  }
  bool anyIntegrated = false, dedicatedWithoutOutputs = false;
  std::string gpuText;
  for (const auto& a : adapters) {
    anyIntegrated |= a.integrated;
    if (!a.integrated && a.outputs == 0) dedicatedWithoutOutputs = true;
    Log("  GPU: %s | %zu MB | %s | monitores conectados: %d\n", Narrow(a.name).c_str(), a.vramMB,
        a.integrated ? T("integrada (estimado)", "integrated (estimated)") : T("dedicada", "dedicated"), a.outputs);
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
        o.hdrOn ? T("activado", "on") : "no", Narrow(adapters[o.adapterIndex].name).c_str(),
        o.primary ? T(" [principal]", " [primary]") : "");
    if (!target || o.primary) { target = &o; nominalHz = hzNom; signalHz = hzSig; }
  }
  if (!target) {
    SetStatus(UiStatus::Error, T("No encontré ninguna pantalla", "No display found"), "");
    return;
  }

  std::string tip;
  const AdapterInfo& shownOn = adapters[target->adapterIndex];
  if (laptop && shownOn.integrated && dedicatedWithoutOutputs) {
    tip = T("Portátil en modo híbrido: la gráfica integrada muestra la imagen, así que G-SYNC no puede "
          "funcionar. Actívalo eligiendo «Solo GPU NVIDIA» en el panel de NVIDIA o en la app de tu portátil.",
          "Hybrid-mode laptop: the integrated GPU drives the display, so G-SYNC can't work. Enable it by "
          "choosing “NVIDIA GPU only” in the NVIDIA panel or in your laptop's app.");
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
  const std::string vsyncCapLearned = vsyncCap;
  if (g_limiterKind != 0) vsyncCap = "no";  // el VSync por intervalos es exclusivo del driver de NVIDIA
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
      Log("  G-SYNC (driver de NVIDIA): %s\n", vrrPossible ? T("disponible en esta pantalla", "available on this display") : T("no disponible en esta pantalla", "not available on this display"));
    else
      Log("  VRR: lo detecto por los tiempos de pantalla (sirve para G-SYNC, FreeSync y Adaptive-Sync)\n");
    // G-SYNC no se esta usando? Se recuerda si alguna vez funciono en esta pantalla (por su nombre: en
    // modo hibrido Windows le da otro nombre interno), para avisar sin falsas alarmas en monitores que
    // no lo admiten.
    std::string panelName;
    for (const auto& pth : paths)
      if (pth.gdiName == target->gdiName)
        panelName = Narrow(!pth.edidName.empty() ? pth.edidName : pth.friendlyName);
    const std::string seenKey = "gsync-visto|" + (panelName.empty() ? Narrow(target->gdiName) : panelName);
    if (nvVrrKnown && vrrPossible) WriteKv(L"equipo.txt", seenKey, "si");
    const bool seenBefore = ReadKv(L"equipo.txt", seenKey) == "si";
    const bool hybridNow = laptop && shownOn.integrated && dedicatedWithoutOutputs;
    const int alert = hybridNow && nvOk ? 1 : (nvVrrKnown && !vrrPossible && seenBefore) ? 2 : 0;
    g_vrrMissing = alert != 0;
    if (alert == 1)
      Log("  AVISO: G-SYNC no puede funcionar en modo hibrido%s. Para activarlo: Panel de control de NVIDIA ->\n"
          "  Administrar modo de pantalla -> Solo GPU NVIDIA (o el modo de GPU en la app del portatil), y reiniciar.\n",
          seenBefore ? T(" (en esta pantalla funcionaba antes)", " (it worked on this display before)") : "");
    else if (alert == 2)
      Log("  AVISO: G-SYNC esta desactivado (en esta pantalla funcionaba antes). Para activarlo: Panel de control\n"
          "  de NVIDIA -> Configurar G-SYNC -> Habilitar G-SYNC.\n");
    {
      std::lock_guard<std::mutex> lock(g_ui.m);
      g_ui.vrrAlert = alert;
    }
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
              lfc ? T(" (con LFC: por debajo del minimo, el monitor duplica fotogramas)", " (with LFC: below the minimum, the display repeats frames)")
                  : T(" (sin LFC: por debajo del minimo, mejor un divisor)", " (without LFC: below the minimum, a divisor is better)"));
        }
        std::lock_guard<std::mutex> lock(g_ui.m);
        g_ui.vrrRange = vmin > 0 && vmax > vmin
                            ? Fmt("%d-%d Hz%s%s", vmin, vmax, pth.freeSyncMin > 0 ? ", FreeSync" : "",
                                  vmax >= 2 * vmin ? T(", con LFC", ", with LFC") : "")
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
    g_ui.monitor = Fmt(T("%ld × %ld, %.3f Hz reales", "%ld × %ld, %.3f Hz measured"), target->width, target->height, hz);
    g_ui.hz = hz;
  };
  bool hzFromCache = !hzClean && ReadCachedHz(monitorKey) > 0;
  double lastHzRetry = NowSec();
  Log("  Frecuencia real: %.3f Hz (Windows dice %.3f)\n", hz, nominalHz);
  eco::LowerProcess();

  {
    std::lock_guard<std::mutex> lock(g_ui.m);
    g_ui.gpu = gpuText;
    g_ui.monitor = Fmt(T("%ld × %ld, %.3f Hz reales", "%ld × %ld, %.3f Hz measured"), target->width, target->height, hz);
    g_ui.hz = hz;
    g_ui.tip = tip;
    g_ui.driver = LimiterUiText(nvDriver.error);
    g_ui.driverOk = limiterOk;
  }
  Log("\n=== Driver ===\n  %s\n", nvOk ? T("NVIDIA: disponible", "NVIDIA: available") : (T("NVIDIA: no disponible (", "NVIDIA: not available (") + nvDriver.error + ")").c_str());
  LogLimiter(Narrow(rtssBackend.Dir()));
  if (!limiterOk) g_applyMode = false;
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
  // Vigilancia de PresentMon a mitad de partida: PresentMon 2.6.0 puede dejar de entregar datos (por
  // ejemplo, al desactivar la generacion de fotogramas en un juego que emite eventos de tipo de
  // fotograma; issue 695 de PresentMon). Con el juego en primer plano y sin ningun dato durante mas de
  // 6 s, se reinicia (como mucho una vez por minuto, por si el juego esta de verdad en pausa).
  double lastAnySampleAt = NowSec(), lastPmRestartAt = -1e9;
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
      LimitRequest req;  // lo que se pide al driver (cada fabricante lo traduce)
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
        req.frl = frl;
        req.vsyncOn = true;
      } else if (useVsync) {
        method = Method::DriverVsync;
        targetFps = hz / n;
        shownFps = static_cast<uint32_t>(std::lround(targetFps));
        req.vsyncDivisor = n;
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
          req.frl = frl;
        }
      } else if (intFps > 0) {
        targetFps = intFps;
        shownFps = intFps;
        n = div;
        req.frl = intFps;
      }
      req.maxPerf = g_maxPerf;
      req.lowLatency = g_lowLatency;
      const uint32_t frlWanted = req.vsyncDivisor > 0 ? 0 : req.frl;
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
        if (keepKnown && !session.keepNoted && !req.Empty() && !session.appliedThisSession) {
          session.keepNoted = true;
          Log("[%5.0f s] Mantengo el limite ya aplicado (%s); no lo cambio por una medicion puntual.\n",
              session.T(now), DescribeChange(kc).c_str());
        }
      }
      if (keepKnown) req = LimitRequest{};
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
      if (req.Empty() && !keepKnown) {
        if (!session.noTargetNoted && stable) {
          session.noTargetNoted = true;
          Log("[%5.0f s] No aplico limite: %s\n", session.T(now),
              session.current.valid ? T("tus Hz no tienen un divisor entero y el VSync del driver no funciona aqui.", "your Hz has no whole divisor and the driver's VSync doesn't work here.")
                                    : T("el juego no sostiene ni el limite mas bajo.", "the game can't hold even the lowest limit."));
        }
      } else if (!req.Empty() && !same && stable && spaced) {
        if (known == store.items.end() && st.capped && st.capFits && st.capFps <= targetFps + 0.5 &&
            method == Method::FpsLimit) {
          if (!session.ownCapNoted) {
            session.ownCapNoted = true;
            Log("[%5.0f s] El juego ya tiene un limite adecuado (~%.0f fps).\n", session.T(now), st.capFps);
          }
        } else {
          AppliedChange change = known != store.items.end() ? known->second : AppliedChange{};
          if (nvd.Apply(session.exe, req, change)) {
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
            Log("[%5.0f s] No pude aplicar: %s\n", session.T(now), nvd.Error().c_str());
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
      std::string why = Fmt(T("toda la partida (%.1f min de juego)", "the whole session (%.1f min of play)"), session.gameSecs / 60.0);
      if (TryApply(now, st, true, why.c_str())) {
        auto c = store.items.find(session.exe);
        SaveLimitExplain(session, LimitExplain::WholeSession, 0.0,
                         c->second.newValue / static_cast<double>(std::max(1, c->second.fgFactor)), 0.0);
        result = Fmt(T("Límite para la próxima partida de %s: %s.", "Limit for the next session of %s: %s."),
                     session.label.c_str(), DescribeChange(c->second).c_str());
      }
    } else if (known->second.manual) {
      // Limite manual: no se cambia nunca; solo se informa si no se sostuvo.
      const FailureTracker failT = session.fail.WithoutClosing(session.expectedFps);
      if (failT.Failed())
        Log("\n  Al cerrar: el juego no sostuvo su limite manual durante %.0f s (%.0f%% de la partida).\n"
            "  No lo cambio porque es manual; si quieres, ajustalo o deshazlo.\n",
            failT.failSecs, failT.Percent());
      return;
    } else if (known->second.verified || session.accepted) {
      const AppliedChange& c = known->second;
      // Sin el cierre del juego (los ultimos 15 s y un cuelgue o menu lento al salir): no cuenta como fallo.
      const FailureTracker failT = session.fail.WithoutClosing(session.expectedFps);
      const FailureTracker tailT = session.tailFail.WithoutClosing(session.expectedFps);
      // Subida automatica (con G-SYNC/FreeSync y sin generacion de fotogramas): su estado por juego.
      // Ajustes de limites (incluida la subida automatica) en ambos modos; las sugerencias, segun el modo.
      const bool climbOk = !c.manual && c.vrr && c.fgFactor <= 1 && !FrameGenInvolved(session);
      // Con Estabilidad, tambien falla si el 1 % bajo causado por el juego se hunde de forma sostenida:
      // es lo que promete esa prioridad (sostener el limite incluso en los peores momentos). Con Fluidez
      // solo cuenta la mediana: acepta caidas en los momentos mas exigentes (en Greylock a 125, la
      // mediana se sostuvo el 100 % del tiempo y el 1 % bajo se hundio un 24-48 %).
      // Prioridad por juego: con Fluidez, el 1 % bajo hundido en dos partidas seguidas -> Estabilidad.
      const bool actsAlone = AppActsAlone(session.prioFixed);
      // En modo guiado (AMD e Intel) no hay "GPU al maximo": la GPU se relaja con el limite y el 1 % bajo
      // puede parecer peor de lo que es (la espiral de las primeras versiones). Hasta tener datos de esas
      // graficas, la regla del 1 % bajo no baja limites ni cambia la prioridad; la de la mediana, si.
      const bool tailRuleOk = g_limiterKind == 0;
      // El 1 % mas lento causado por el juego en toda la partida (0.66): la misma medida con la que decide
      // Ritmo constante (antes, el de cada 20 s, que exageraba los tropiezos cortos y creaba una espiral).
      double sessionLow = 0.0;
      if (auto sr = session.sessionReal.find(1); sr != session.sessionReal.end() && session.expectedFps > 0)
        sessionLow = SessionGameLowFps(sr->second, session.sessionWork, 1000.0 / session.expectedFps,
                                       static_cast<double>(failT.trimmed));
      const SteadyCheck steady = CheckSteady(sessionLow, session.expectedFps);
      session.closeSessionLow = sessionLow;
      SaveSessionExplain(session, failT, tailT, c.fgFactor > 1 || FrameGenInvolved(session), sessionLow);
      if (climbOk && tailRuleOk && session.fluid && (actsAlone || g_prioMode == 2 || session.prioFixed)) {
        const AutoPriority before = AutoPriority::Parse(ReadKv(L"equipo.txt", "prioridad|" + session.exe));
        const AutoPriority after =
            UpdateAutoPriority(before, true, SessionSank(sessionLow, session.expectedFps), failT.limPlaySecs);
        WriteKv(L"equipo.txt", "prioridad|" + session.exe, after.Serialize());
        if (after.stability && !actsAlone) {
          // Prioridad elegida a mano: no se cambia; se recomienda en la fila del juego.
          if (!before.stability) {
            WriteKv(L"equipo.txt", "prioridad-sugerida|" + session.exe, "si");
            Log("\n  Al cerrar: el 1 %% bajo se hundio en %d partidas seguidas (en esta, %.0f fps con un limite de %.0f): %s es\n"
                "  inestable con su limite. Te recomiendo Ritmo constante (prioridad elegida por ti: no la cambio).\n",
                kUnstableSessions, sessionLow, session.expectedFps, session.label.c_str());
            std::lock_guard<std::mutex> lock(g_ui.m);
            g_ui.prioSuggest.insert(session.exe);
            if (!g_ui.warnings.count(session.exe)) g_ui.warnings[session.exe] = T(kPrioSuggestText_es, kPrioSuggestText_en);
            g_ui.notice = Fmt(T("%s es inestable con «Más FPS»: te recomiendo «Ritmo constante» (en Juegos).",
                                "%s is unstable with “More FPS”: I recommend “Steady pace” (in Games)."),
                              session.label.c_str());
          }
        } else if (after.stability) {
          session.fluid = false;
          Log("\n  Al cerrar: el 1 %% bajo se hundio en %d partidas seguidas (en esta, %.0f fps con un limite de %.0f): %s es\n"
              "  inestable con su limite. En adelante uso Ritmo constante para este juego.\n",
              kUnstableSessions, sessionLow, session.expectedFps, session.label.c_str());
          std::lock_guard<std::mutex> lock(g_ui.m);
          g_ui.notice = Fmt(T("%s es inestable con su límite: desde ahora uso «Ritmo constante» para este juego.",
                              "%s is unstable with its limit: from now on I'll use “Steady pace” for this game."),
                            session.label.c_str());
        } else if (after.unstableStreak > 0) {
          Log("\n  Al cerrar: el 1 %% bajo de la partida fue de %.0f fps, mas de un 13 %% por debajo del limite (%.0f)\n"
              "  (%d de %d partidas seguidas para pasar este juego a Ritmo constante).\n",
              sessionLow, session.expectedFps, after.unstableStreak, kUnstableSessions);
        }
      }
      const bool tailFailed = climbOk && tailRuleOk && !session.fluid && !failT.Failed() && !steady.holds;
      const bool failed = failT.Failed() || tailFailed;
      ClimbResult cr;
      const double limitBefore = session.expectedFps;
      if (climbOk) {
        const bool plenty = !failed && PlentyWholeSession(session);
        const long long today = static_cast<long long>(std::time(nullptr) / 86400);
        cr = DecideClimb(ClimbState::Parse(ReadKv(L"equipo.txt", "escalada|" + session.exe)), failed, plenty,
                         limitBefore, 1, true, hz, session.fluid, today);
        WriteKv(L"equipo.txt", "escalada|" + session.exe, cr.state.Serialize());
        session.climbHandled = true;
      }
      auto VrrRec = [&](double realFps) {
        Recommendation r;
        r.valid = r.vrr = r.vrrValid = true;
        r.fgFactor = 1;
        r.realFps = r.capFps = r.vrrCapFps = std::floor(realFps + 1e-9);
        return r;
      };
      if (failed) {
        // Una recomendacion de subida pendiente deja de tener sentido si el limite actual ya no se sostiene.
        ClearRecommendation(session.exe);
        const bool revert = climbOk && cr.action == ClimbAction::RevertToPrev;
        // Si solo se hundio el 1 % bajo (la mediana se mantuvo), Ritmo constante baja a lo que dicta su
        // formula con esta partida, como mucho un escalon (un 13 %) por partida: una partida anomala (una
        // carga, la compilacion de shaders) no hunde el limite de golpe; si sigue mal, baja otro escalon.
        Recommendation lower = revert       ? VrrRec(cr.newLimit)
                               : tailFailed ? VrrRec(std::max(steady.target, std::floor(limitBefore / (1.0 + cfg::kJnd))))
                                            : LowerAfterFailure(failT.secRates, c.vrr, c.fgFactor, session.expectedFps, hz);
        if (tailFailed)
          Log("\n  Al cerrar: el 1 %% bajo de la partida fue de %.0f fps: 1,13 x %.0f = %.0f, mas de un 5 %% por debajo\n"
              "  del limite (%.0f), aunque la mediana se mantuvo.\n",
              sessionLow, sessionLow, steady.target, limitBefore);
        else
          Log("\n  Al cerrar: el limite no se sostuvo durante %.0f s (%.0f %% de la partida).\n",
              failT.failSecs, failT.Percent());
        if (climbOk)
          Log("  No volvere a subir hasta %.0f fps durante 30 dias.\n", limitBefore);
        if (lower.valid && !actsAlone) {
          // Prioridad elegida a mano: no se baja; se recomienda (urgente si, sin G-SYNC, el VSync por intervalos
          // hace que un limite que no se sostiene empeore el juego).
          const double to = std::floor(lower.capFps + 1e-9);
          const int kind = c.method == Method::DriverVsync ? 2 : 1;
          if (to > 0 && to < limitBefore) {
            SetRecommendation(session.exe, kind, limitBefore, to);
            Log("  Te recomiendo bajarlo a %.0f fps%s (prioridad elegida por ti: no lo cambio).\n", to,
                kind == 2 ? T(", y cuanto antes: sin G-SYNC, este limite empeora el movimiento", ", and soon: without G-SYNC, this limit makes motion worse") : "");
            result = Fmt(T("%s no sostiene su límite: te recomiendo bajarlo a %.0f FPS (en Juegos).",
                           "%s can't hold its limit: I recommend lowering it to %.0f FPS (in Games)."),
                         session.label.c_str(), to);
            PublishChanges(store);
          }
        } else if (lower.valid) {
          session.current = lower;
          session.lowerPending = true;  // permite cambiar un limite ya comprobado
          session.accepted = false;
          if (TryApply(now, st, true, revert ? T("la vuelta al limite anterior (la subida no se sostuvo)",
                                                 "going back to the previous limit (the raise didn't hold)")
                                             : T("la regla de fallos al cerrar el juego", "the failure rule when the game closed"))) {
            auto nc = store.items.find(session.exe);
            SaveLimitExplain(session, revert ? LimitExplain::Revert : tailFailed ? LimitExplain::FailTail : LimitExplain::FailMedian,
                             limitBefore, nc->second.newValue / static_cast<double>(std::max(1, nc->second.fgFactor)),
                             tailFailed ? 0.0 : failT.Percent(), &failT.secRates, tailFailed ? sessionLow : 0.0);
            result = revert
                ? Fmt(T("%s no sostuvo %.0f FPS: vuelvo a %.0f, que sí sostenía.", "%s didn't hold %.0f FPS: back to %.0f, which it did hold."),
                      session.label.c_str(), limitBefore, cr.newLimit)
                : tailFailed
                ? Fmt(T("El 1 %% más lento de %s fue de %.0f FPS: su límite con «Ritmo constante» es más bajo. En la próxima usaré: %s.",
                        "%s's slowest 1 %% was %.0f FPS: its “Steady pace” limit is lower. Next time I'll use: %s."),
                      session.label.c_str(), sessionLow, DescribeChange(nc->second).c_str())
                : Fmt(T("%s no sostuvo su límite durante el %.0f %% de la partida. En la próxima usaré: %s.",
                        "%s didn't hold its limit for %.0f %% of the session. Next time I'll use: %s."),
                      session.label.c_str(), failT.Percent(), DescribeChange(nc->second).c_str());
          }
        }
      } else {
        // Una recomendacion de bajar pendiente se retira si esta vez el juego sostuvo su limite.
        bool lowerPendingRec = false;
        {
          std::lock_guard<std::mutex> lock(g_ui.m);
          auto k = g_ui.suggestKind.find(session.exe);
          lowerPendingRec = g_ui.suggestions.count(session.exe) && k != g_ui.suggestKind.end() && k->second != 0;
        }
        if (lowerPendingRec && failT.limPlaySecs >= 180.0) {
          ClearRecommendation(session.exe);
          Log("\n  Al cerrar: esta vez sostuvo su limite: retiro la recomendacion de bajarlo.\n");
          PublishChanges(store);
        }
        if (failT.failSecs > 0)
          Log("\n  Al cerrar: bajadas aisladas del limite (%.0f s, %.0f %% de la partida): no cambio nada.\n",
              failT.failSecs, failT.Percent());
        if (climbOk && cr.action == ClimbAction::Raise && !actsAlone) {
          // Prioridad elegida a mano: no se aplica; la subida queda recomendada con un boton en Juegos.
          ClimbState kept = cr.state;
          kept.prevGood = 0.0;
          kept.plentyStreak = 0;
          WriteKv(L"equipo.txt", "escalada|" + session.exe, kept.Serialize());
          SetRecommendation(session.exe, 0, limitBefore, cr.newLimit);
          Log("\n  Al cerrar: el juego fue sobrado toda la partida con %.0f fps: te recomiendo subirlo a %.0f\n"
              "  (prioridad elegida por ti: se aplica con el boton de Juegos).\n", limitBefore, cr.newLimit);
          PublishChanges(store);
        } else if (climbOk && cr.action == ClimbAction::Raise && actsAlone) {
          Log("\n  Al cerrar: el juego fue sobrado toda la partida con %.0f fps: lo subo a %.0f.\n", limitBefore, cr.newLimit);
          session.current = VrrRec(cr.newLimit);
          session.lowerPending = true;  // permite cambiar un limite ya comprobado
          session.accepted = false;
          if (TryApply(now, st, true, T("la subida automatica (el juego iba sobrado)", "the automatic raise (the game had headroom)"))) {
            SaveLimitExplain(session, LimitExplain::Raise, limitBefore, cr.newLimit, 0.0);
            result = Fmt(T("%s iba sobrado: subo su límite de %.0f a %.0f FPS para la próxima partida. Si no lo sostiene, volveré a %.0f.",
                           "%s had headroom: raising its limit from %.0f to %.0f FPS for the next session. If it can't hold it, I'll go back to %.0f."),
                         session.label.c_str(), limitBefore, cr.newLimit, limitBefore);
          }
        } else if (climbOk && cr.action == ClimbAction::BlockedByCeiling) {
          Log("\n  Al cerrar: va sobrado, pero no subo: %.0f fps no se sostuvo hace menos de 30 dias.\n", cr.state.failedCeil);
        } else if (climbOk && cr.action == ClimbAction::AtTop) {
          Log("\n  Al cerrar: va sobrado y ya esta en el maximo para tu pantalla.\n");
        } else if (climbOk && cr.state.plentyStreak > 0) {
          Log("\n  Al cerrar: va sobrado (%d de 2 partidas seguidas, con prioridad Ritmo constante, para subirlo).\n",
              cr.state.plentyStreak);
        }
      }
    }
    PublishChanges(store);  // el desplegable de Juegos muestra la ultima partida y como se decidio el limite
    if (!result.empty()) {
      Log("  %s\n", result.c_str());
      std::lock_guard<std::mutex> lock(g_ui.m);
      g_ui.notice = result;
    }
  };

  // Mide cuanto tarda cada vuelta del motor (se suma a la partida en curso).
  struct TickTimer {
    Session& s;
    double t0 = NowSec();
    ~TickTimer() {
      if (s.start <= 0) return;
      const double ms = (NowSec() - t0) * 1000.0;
      s.engMsSum += ms;
      s.engMsMax = std::max(s.engMsMax, ms);
      ++s.engTicks;
    }
  };
  while (!g_quit) {
    for (int i = 0; i < 10 && !g_quit; ++i) Sleep(100);
    if (g_quit) break;
    TickTimer tick{session};
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
    if (!fresh.empty()) {
      lastAnySampleAt = now;
    } else if (pm.HeaderSeen() && !session.exe.empty() && session.seenForeground && !session.background &&
               now - std::max(lastAnySampleAt, eco::pmLastLineAt.load()) > 6.0 && now - lastPmRestartAt > 60.0) {
      // Desde la 0.65.2 las lineas del propio FlatPace se descartan antes de dar muestras: sin la marca de
      // la ultima linea, una carga de mas de 6 s parecia un PresentMon colgado y se reiniciaba (0.66).
      Log("[%5.0f s] PresentMon dejo de entregar datos con el juego en primer plano: lo reinicio.\n", session.T(now));
      pm.Stop();
      if (!pm.Start(presentMonPath, std::wstring())) Log("  No pude reiniciar PresentMon.\n");
      lastPmRestartAt = now;
      lastAnySampleAt = now;
    }
    bool applyMode = g_applyMode;
    const bool displayRestoreNeeded = switchedRate != 0;
    std::string manualExe;
    {
      std::lock_guard<std::mutex> lock(g_manualMutex);
      manualExe = g_manualExe;
    }
    const bool manual = !manualExe.empty();
    if (applyMode != lastApplyMode) {
      Log("[%5.0f s] Modo: %s\n", session.T(now), applyMode ? T("aplicar limites", "apply limits") : T("solo medir", "measure only"));
      lastApplyMode = applyMode;
    }

    // Limites manuales pedidos desde la ventana.
    {
      // Limites manuales (Fijar...) y sugerencias aplicadas (con las protecciones de la subida automatica).
      // Cambio de limitador pedido desde la ventana: se deshace cada limite con el limitador que lo aplico
      // y se aplica el mismo valor con el nuevo (se notara al abrir cada juego, como siempre).
      if (const int want = g_limiterReq.exchange(-1); want >= 0 && want != g_limiterKind) {
        const bool usable = want == 0 ? nvOk : want == 1 ? g_guidedVendor != 0 : rtssOk;
        if (usable) {
          std::vector<AppliedChange> old;
          for (auto& [exe, c] : store.items) old.push_back(c);
          int undone = 0;
          for (const auto& c : old)
            if (nvd.Revert(c)) ++undone;  // con la pieza que aplico cada limite
          store.items.clear();
          g_limiterKind = want;
          backend.Use(want == 2 ? static_cast<LimitBackend*>(&rtssBackend)
                      : want == 1 ? static_cast<LimitBackend*>(&guidedBackend)
                                  : static_cast<LimitBackend*>(&nvBackend));
          limiterOk = true;
          vsyncCap = want == 0 ? vsyncCapLearned : "no";
          int moved = 0;
          for (const auto& c : old) {
            LimitRequest lr;
            lr.frl = c.method == Method::DriverVsync ? c.newValue : (c.frlValue > 0 ? c.frlValue : c.newValue);
            lr.vsyncOn = want == 0 && c.vrr;
            lr.maxPerf = want == 0 && g_maxPerf;
            lr.lowLatency = want == 0 && g_lowLatency;
            AppliedChange nc;
            if (lr.frl > 0 && nvd.Apply(c.exe, lr, nc)) {
              nc.exe = c.exe;
              nc.method = Method::FpsLimit;  // el VSync por intervalos pasa a limite de FPS con el mismo valor
              nc.divisor = c.divisor;
              nc.newValue = c.newValue;
              nc.fgFactor = c.fgFactor;
              nc.frlValue = lr.frl;
              nc.displayHz = c.displayHz;
              nc.vrr = c.vrr;
              nc.manual = c.manual;
              nc.verified = false;  // se vuelve a comprobar con el limitador nuevo
              store.items[c.exe] = nc;
              ++moved;
            } else {
              Log("  No pude mover el limite de %s al limitador nuevo: %s\n", c.exe.c_str(), nvd.Error().c_str());
            }
          }
          store.Save();
          WriteKv(L"equipo.txt", "limitador", want == 2 ? "rtss" : want == 1 ? "recomendar" : "driver");
          Log("\n[%5.0f s] Limitador cambiado (%d limite%s movido%s; deshechos con el anterior: %d).\n", session.T(now),
              moved, moved == 1 ? "" : "s", moved == 1 ? "" : "s", undone);
          LogLimiter(Narrow(rtssBackend.Dir()));
          {
            std::lock_guard<std::mutex> lock(g_ui.m);
            g_ui.driver = LimiterUiText(nvDriver.error);
            g_ui.driverOk = true;
            g_ui.notice = want == 1
                ? Fmt(T("Limitador: solo recomendar. Te indico cada límite en Juegos (%d juegos).",
                        "Limiter: recommend only. I show each limit in Games (%d games)."), moved)
                : Fmt(T("Limitador cambiado: %d límites movidos. Se aplicarán al abrir cada juego.",
                        "Limiter changed: %d limits moved. They'll apply when you open each game."), moved);
          }
          PublishChanges(store);
        }
      }
      std::vector<std::tuple<std::string, int, bool>> reqs;  // exe, fps en pantalla, manual
      std::map<std::string, int> sugKinds;                   // tipo de cada recomendacion aplicada
      {
        std::lock_guard<std::mutex> lock(g_manualLimitMutex);
        for (const auto& [e, f] : g_manualLimitReq) reqs.emplace_back(e, f, true);
        g_manualLimitReq.clear();
        std::vector<std::string> sug;
        sug.swap(g_applySuggestionReq);
        std::lock_guard<std::mutex> lock2(g_ui.m);
        for (const auto& e : sug) {
          auto it = g_ui.suggestions.find(e);
          if (it != g_ui.suggestions.end()) reqs.emplace_back(e, static_cast<int>(it->second), false);
        }
        sugKinds.clear();
        for (const auto& e : sug) {
          auto k = g_ui.suggestKind.find(e);
          sugKinds[e] = k != g_ui.suggestKind.end() ? k->second : 0;
        }
      }
      for (const auto& [exe, fps, isManual] : reqs) {
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
        LimitRequest req;
        req.frl = frl;
        req.vsyncOn = vrrNow;
        req.maxPerf = g_maxPerf;
        req.lowLatency = g_lowLatency;
        bool ok = limiterOk;
        const double previousReal = known != store.items.end() && known->second.fgFactor <= 1 ? known->second.newValue : 0.0;
        // Se parte de cero: se deshace lo anterior (puede incluir otros ajustes, como el VSync por intervalos).
        if (ok && known != store.items.end()) {
          ok = nvd.Revert(known->second);
          if (ok) store.items.erase(known);
        }
        AppliedChange change;
        if (ok) ok = nvd.Apply(exe, req, change);
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
          change.manual = isManual;
          store.items[exe] = change;
          store.Save();
          {
            LimitExplain ex;
            ex.kind = isManual ? LimitExplain::Manual : LimitExplain::Accepted;
            ex.when = static_cast<long long>(std::time(nullptr));
            ex.from = static_cast<float>(previousReal);
            ex.to = static_cast<float>(screen / std::max(1, k));
            WriteKv(L"equipo.txt", "explica|" + exe, ex.Serialize());
          }
          const bool running = session.exe == exe && session.pid != 0;
          ClearRecommendation(exe);
          if (isManual) {
            Log("\n[%5.0f s] LIMITE MANUAL para %s: %s.\n", session.T(now), name.c_str(), DescribeChange(change).c_str());
            Log("          La app no lo cambiara; se comprueba al abrir el juego%s.\n", running ? T(" (reinicialo)", " (restart it)") : "");
            result = Fmt(running ? T("Límite manual para %s: %s. Reinicia el juego para aplicarlo.",
                                     "Manual limit for %s: %s. Restart the game to apply it.")
                                 : T("Límite manual para %s: %s. Se aplicará al abrir el juego.",
                                     "Manual limit for %s: %s. It will apply when you open the game."),
                         name.c_str(), DescribeChange(change).c_str());
          } else {
            // Recomendacion aplicada. Una subida recuerda el limite anterior: si no se sostiene, se vuelve a
            // el (o, con prioridad elegida a mano, se recomienda volver).
            const bool raise = sugKinds[exe] == 0;
            ClimbState cs = ClimbState::Parse(ReadKv(L"equipo.txt", "escalada|" + exe));
            cs.prevGood = raise ? previousReal : 0.0;
            cs.plentyStreak = 0;
            WriteKv(L"equipo.txt", "escalada|" + exe, cs.Serialize());
            Log("\n[%5.0f s] RECOMENDACION APLICADA para %s: %s (antes %.0f).\n", session.T(now), name.c_str(),
                DescribeChange(change).c_str(), previousReal);
            result = Fmt(running ? (raise ? T("%s: límite subido a %s. Reinicia el juego para aplicarlo.",
                                              "%s: limit raised to %s. Restart the game to apply it.")
                                          : T("%s: límite bajado a %s. Reinicia el juego para aplicarlo.",
                                              "%s: limit lowered to %s. Restart the game to apply it."))
                                 : (raise ? T("%s: límite subido a %s. Se aplicará al abrir el juego.",
                                              "%s: limit raised to %s. It will apply when you open the game.")
                                          : T("%s: límite bajado a %s. Se aplicará al abrir el juego.",
                                              "%s: limit lowered to %s. It will apply when you open the game.")),
                         name.c_str(), DescribeChange(change).c_str());
          }
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
          result = Fmt(T("No pude cambiar el límite de %s: %s", "Couldn't change the limit for %s: %s"),
                       name.c_str(), limiterOk ? nvd.Error().c_str() : T("driver de NVIDIA no disponible", "NVIDIA driver not available"));
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
        if (limiterOk && nvd.Revert(it->second)) {
          store.items.erase(it);
          store.Save();
          WriteKv(L"equipo.txt", "escalada|" + exe, "");  // empezar de cero: sin techo ni subidas pendientes
          WriteKv(L"equipo.txt", "prioridad|" + exe, "");
          WriteKv(L"equipo.txt", "prioridad-sugerida|" + exe, "");
          ClearRecommendation(exe);
          {
            std::lock_guard<std::mutex> lock(g_ui.m);
            g_ui.prioSuggest.erase(exe);
          }
          result = g_limiterKind == 1
                       ? Fmt(T("Deshecho: olvido el límite de %s. Quítalo también en %s.",
                               "Undone: I forget %s's limit. Remove it in %s too."),
                             name.c_str(), GuidedSoftware())
                       : Fmt(T("Deshecho: %s vuelve a como estaba. Si estaba abierto, reinícialo.",
                               "Undone: %s is back to how it was. If it was open, restart it."), name.c_str());
          Log("\n[%5.0f s] Deshecho %s (pedido desde la ventana).\n", session.T(now), exe.c_str());
          WriteKv(L"equipo.txt", "explica|" + exe, "");
          WriteKv(L"equipo.txt", "partida|" + exe, "");
          if (session.exe == exe) {
            session.expectedFps = 0;
            session.verified = session.restartNoted = session.keepNoted = false;
            session.lowerPending = false;
            session.lastApply = now;
          }
          if (exe == session.exe && displayRestoreNeeded) RestoreDisplay();
        } else {
          result = Fmt(T("No pude deshacer %s: %s", "Couldn't undo %s: %s"), name.c_str(), nvd.Error().c_str());
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
      for (const auto& [exe, ch] : store.items) {
        WriteKv(L"equipo.txt", "escalada|" + exe, "");
        WriteKv(L"equipo.txt", "sugerencia|" + exe, "");
        WriteKv(L"equipo.txt", "prioridad|" + exe, "");
        WriteKv(L"equipo.txt", "prioridad-sugerida|" + exe, "");
        WriteKv(L"equipo.txt", "explica|" + exe, "");
        WriteKv(L"equipo.txt", "partida|" + exe, "");
      }
      {
        std::lock_guard<std::mutex> lock(g_ui.m);
        g_ui.suggestions.clear();
      }
      std::string result = UndoAll(nvd, store, limiterOk);
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
    // Una herramienta de captura abierta encima del juego no pausa la medicion.
    if (session.pid != 0 && !session.background && fgPid != session.pid && IsCaptureTool(fgPid)) fgPid = session.pid;

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
        g_ui.monitor = Fmt(T("%ld × %ld, %.3f Hz reales", "%ld × %ld, %.3f Hz measured"), target->width, target->height, hz);
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
      {  // resumen para "Ahora" sin juego abierto
        const auto kc = store.items.find(session.exe);
        std::lock_guard<std::mutex> lock(g_ui.m);
        g_ui.lastGame = session.label;
        g_ui.lastEnd = now;
        g_ui.lastMinutes = session.T(now) / 60.0;
        g_ui.lastFps = g_ui.realFps;
        g_ui.lastHitches = g_ui.hitches;
        g_ui.lastLocked = kc != store.items.end() && kc->second.verified;
      }
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
          std::string b = ReadKv(L"equipo.txt", "ritmo-antes2|" + foundExe);
          if (!b.empty()) session.baseSwing = std::atof(b.c_str());
          std::string bm = ReadKv(L"equipo.txt", "movimiento-antes|" + foundExe);
          if (!bm.empty()) session.baseMotion = std::atof(bm.c_str());
          std::string bl = ReadKv(L"equipo.txt", "latencia-antes|" + foundExe);
          if (!bl.empty()) session.baseLat = std::atof(bl.c_str());
          {
            int mode = 0;
            session.fluid = GameUsesFluid(foundExe, &mode);
            session.prioFixed = mode != 0;
          }
        }
        session.label = label;
        g_gameLabels[foundExe] = label;
        WriteKv(L"equipo.txt", "nombre|" + foundExe, label);
        PublishChanges(store);
        session.start = now;
        session.cpuAtStart = eco::ProcessCpuSec();
        session.pmCpuAtStart = eco::PresentMonCpuTotal();
      session.mainCpuAtStart = eco::ThreadCpuSec(eco::mainThread.load());
      session.engCpuAtStart = eco::ThreadCpuSec(eco::engineThread.load());
      session.rdCpuAtStart = eco::ReaderCpuTotal();
      session.pmLinesAtStart = eco::pmLines.load();
      session.pmOwnAtStart = eco::pmOwnLines.load();
        session.ovMsAtStart = eco::overlayDrawMsSum.load();
        session.ovDrawsAtStart = eco::overlayDraws.load();
        session.ovFullAtStart = eco::overlayFullMsSum.load();
        session.winMsAtStart = eco::windowDrawMsSum.load();
        session.winDrawsAtStart = eco::windowDraws.load();
        session.phaseStart = now;
        session.lastFrameArrival = now;
        gamesSeen++;
        {
          // Los avisos de la ventana hablan de la proxima partida (un limite nuevo, un cambio de prioridad):
          // al empezar una, dejan de tener sentido. Antes no se borraban nunca y quedaban partidas despues.
          std::lock_guard<std::mutex> lock(g_ui.m);
          g_ui.notice.clear();
        }
        Log("\n[Juego detectado] %s  (%s)%s\n", label.c_str(), session.exe.c_str(),
            IsLibraryPath(path) ? T("  [biblioteca de juegos]", "  [game library]") : "");
        if (!foundInFront && !manual)
          Log("  Esta en segundo plano: empiezo a medir cuando vuelvas al juego.\n");
        auto prev = store.items.find(session.exe);
        AppliedChange own;
        if (prev == store.items.end() && nvOk && nvd.FindOwn(session.exe, own)) {
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
      session.cpuAtStart = eco::ProcessCpuSec();
      session.pmCpuAtStart = eco::PresentMonCpuTotal();
      session.mainCpuAtStart = eco::ThreadCpuSec(eco::mainThread.load());
      session.engCpuAtStart = eco::ThreadCpuSec(eco::engineThread.load());
      session.rdCpuAtStart = eco::ReaderCpuTotal();
      session.pmLinesAtStart = eco::pmLines.load();
      session.pmOwnAtStart = eco::pmOwnLines.load();
      session.ovMsAtStart = eco::overlayDrawMsSum.load();
      session.ovDrawsAtStart = eco::overlayDraws.load();
        session.ovFullAtStart = eco::overlayFullMsSum.load();
        session.winMsAtStart = eco::windowDrawMsSum.load();
        session.winDrawsAtStart = eco::windowDraws.load();
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
              session.T(now), (nvVrrKnown && vrrPossible) ? T("segun el driver de NVIDIA", "according to the NVIDIA driver") : T("segun los tiempos de pantalla", "according to the display timings"));
        else
          Log("[%5.0f s] %s: uso los divisores de los Hz.\n", session.T(now),
              detected ? T("Divisores forzados (ajuste avanzado)", "Forced divisors (advanced setting)") : T("El VRR ya no actua con este juego", "VRR no longer works with this game"));
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
          std::string("prioridad: ") + (session.fluid ? "fluidez" : "estabilidad"),
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
      VerifyInput vin;
      vin.expectedFps = exp;
      vin.expectFg = session.expectFg;
      vin.fpsNow = fpsNow;
      vin.fgNow = st.FgFactor();
      vin.presentedFps = st.presentedFps;
      vin.fractionFast = st.FractionRealUnder((1000.0 / exp) * 0.93);
      vin.capped = st.capped;
      vin.capFps = st.capFps;
      // Verificacion directa: el juego va al ritmo exacto esperado (+-1,5 %) y casi ningun
      // fotograma va mas rapido. Reconoce limites que no son divisores (G-SYNC).
      // El limitador del driver deja pasar algun fotograma algo mas rapido: se acepta si el
      // ritmo coincide (+-1,5 %) y menos del 10 % va mas de un 7 % por debajo del periodo.
      if (LimitVerified(vin)) {
        session.verified = true;
        Log("[%5.0f s] LIMITE FUNCIONANDO: el juego va a ~%.3f fps estables.\n", session.T(now), exp);
        {
          auto kc = store.items.find(session.exe);
          if (kc != store.items.end() && !kc->second.verified) {
            kc->second.verified = true;
            store.Save();
            PublishChanges(store);  // la fila deja de mostrar la instruccion del modo guiado
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
      } else if (g_limiterKind == 1 && session.appliedAtStart && !session.restartNoted &&
                 now > session.start + cfg::kVerifySec && fpsNow > exp * 1.10) {
        // Modo guiado: el juego va claramente por encima del limite, asi que aun no se aplico.
        session.restartNoted = true;
        Log("[%5.0f s] Aun no veo el limite de %.0f fps (el juego va a ~%.0f): aplicalo en %s\n"
            "          (instrucciones en Juegos) y reinicia el juego.\n",
            session.T(now), exp, fpsNow, GuidedSoftware());
        std::lock_guard<std::mutex> lock(g_ui.m);
        g_ui.notice = Fmt(T("Aún no veo el límite de %s: aplícalo en %s (instrucciones en Juegos).",
                            "I don't see %s's limit yet: apply it in %s (instructions in Games)."),
                          session.label.c_str(), GuidedSoftware());
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
        if (const double perf = cpuPerf.Sample(); perf > 0) {
          g_cpuPerfPct = perf;
          if (session.cpuPerf.size() < 20000)
            session.cpuPerf.push_back({static_cast<float>(now - session.start), static_cast<float>(perf)});
        }
        session.gameSecs += dt;
        if (session.vrrActive) session.vrrSecs += dt;
        if (st.capped && st.capFps > 0 && session.expectedFps <= 0 && HeldRate(st) > 0) {
          session.ownCapSecs += dt;
          session.ownCapRateSum += st.capFps * dt;
        }
        auto kc = store.items.find(session.exe);
        bool fgSame = kc != store.items.end() && ((kc->second.fgFactor > 1) == (st.FgFactor() > 1));
        if (session.expectedFps > 0 && (session.verified || session.accepted) && fgSame && st.medianMs > 0) {
          session.fail.Add(1000.0 / st.medianMs, session.expectedFps, dt);
          const double gl = GameLowFps(session);
          if (gl > 0) session.tailFail.Add(gl, session.expectedFps, dt);
          if (session.tlFps.size() < 20000) {
            session.tlFps.push_back(static_cast<float>(1000.0 / st.medianMs));
            session.tlLow.push_back(static_cast<float>(gl));
            session.tlSecs += dt;
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
      // Con generacion de fotogramas (vista en esta partida o con la que se aplico el limite), la
      // grafica dibuja las imagenes en pantalla, con la referencia del limite en pantalla.
      const int kFg = known != store.items.end() ? std::max(1, known->second.fgFactor) : std::max(1, session.expectFg);
      const bool screenMode = session.fgEverSeen || kFg > 1;
      overlay::SetScreenMode(screenMode);
      if (session.expectedFps > 0) target = 1000.0 / (session.expectedFps * (screenMode ? kFg : 1));
      if (known != store.items.end()) {
        const AppliedChange& c = known->second;
        limitLabel = std::to_string(c.newValue) + (c.fgFactor > 1 ? T(" en pantalla", " on screen") : " fps");
        state = session.verified ? overlay::LimitState::Locked : overlay::LimitState::Pending;
      }
      overlay::SetGame(session.background || !session.seenForeground ? 0 : session.pid, target, limitLabel, state,
                       session.baseSwing, session.baseLat);
    }
    {
      static int lastOv = -2;
      int ov = overlay::state;
      if (ov != lastOv && session.pid != 0) {
        const char* why[] = {T("visible", "visible"), T("desactivado en la app", "turned off in the app"),
                             T("sin juego detectado", "no game detected"),
                             T("el juego no esta en primer plano", "the game is not in front"),
                             T("aun no hay datos suficientes", "not enough data yet"),
                             T("Windows no pudo dibujarlo", "Windows couldn't draw it")};
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
static ImFont* fontNum = nullptr;      // cifras de "Ahora" (Bahnschrift)
static ImFont* fontVerdict = nullptr;  // veredicto de "Ahora" (Bahnschrift)

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
  // Latin-1, puntuacion tipografica, flechas, la vineta y el signo menos (U+2212, el de la leyenda del
  // umbral: sin el salia como "?", 0.67.2).
  static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2010, 0x2027,
                                   0x2190, 0x2193, 0x2022, 0x2022, 0x2212, 0x2212, 0};
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
  static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2010, 0x2027, 0x2190, 0x2193, 0x2212, 0x2212, 0x2713, 0x2713, 0};
  if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
    fontHead = io.Fonts->AddFontFromFileTTF(path.c_str(), 24.0f * scale, nullptr, ranges);
    fontNum = io.Fonts->AddFontFromFileTTF(path.c_str(), 32.0f * scale, nullptr, ranges);
    fontVerdict = io.Fonts->AddFontFromFileTTF(path.c_str(), 38.0f * scale, nullptr, ranges);
  }
  if (!fontVerdict) fontVerdict = fontBig;
  if (!fontHead) fontHead = fontBig;
  if (!fontNum) fontNum = fontBig;
}

// Copia del estado sin el candado, para dibujar sin bloquear al motor.
struct View {
  UiStatus status;
  std::string statusText, statusDetail, gpu, monitor, tip, driver, game, exe;
  int vrrAlert = 0;
  std::string capText, bottleneck, recText, appliedText, notice, presentText;
  bool driverOk, haveGame;
  double realFps, lowFps, screenFps, steadyPct, jitterMs;
  int hitches;
  std::vector<UiChange> changes;
  std::string currentWarning;
  std::string vrrRange;
  std::string lastGame;
  double lastEnd = 0, lastMinutes = 0, lastFps = 0;
  int lastHitches = 0;
  bool lastLocked = false;
  bool vrrPossible, vrrNvKnown;
  int vrrState;
  std::vector<std::string> log;
  std::wstring logPath;
  double hz;
};

static View Snapshot(bool withLog) {
  std::lock_guard<std::mutex> lock(g_ui.m);
  View v;
  v.status = g_ui.status;
  v.statusText = g_ui.statusText;
  v.statusDetail = g_ui.statusDetail;
  v.gpu = g_ui.gpu;
  v.monitor = g_ui.monitor;
  v.tip = g_ui.tip;
  v.vrrAlert = g_ui.vrrAlert;
  v.driver = g_ui.driver;
  v.driverOk = g_ui.driverOk;
  v.haveGame = g_ui.haveGame;
  v.lastGame = g_ui.lastGame;
  v.lastEnd = g_ui.lastEnd;
  v.lastMinutes = g_ui.lastMinutes;
  v.lastFps = g_ui.lastFps;
  v.lastHitches = g_ui.lastHitches;
  v.lastLocked = g_ui.lastLocked;
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
  // Hasta 600 lineas: solo se copian si se van a mostrar (antes, en cada dibujo de la ventana).
  if (withLog) v.log.assign(g_ui.log.begin(), g_ui.log.end());
  v.logPath = g_ui.logPath;
  v.hz = g_ui.hz;
  return v;
}

// ---- Diseno: estilo SteamOS. El protagonista es la grafica de ritmo ("la linea"). ----
static int page = 0;  // 0 Ahora, 1 Juegos, 2 Equipo, 3 Registro
static bool g_scrollTop = false;

static ImU32 Col(const ImVec4& c, float a = 1.0f) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * a)); }




// Fila etiqueta / valor sin tabla, alineada a una columna.
static void KV(const char* label, const std::string& value, float col, const ImVec4* color = nullptr) {
  ImGui::TextColored(kDim, "%s", label);
  ImGui::SameLine(col);
  if (color) ImGui::TextColored(*color, "%s", value.c_str());
  else ImGui::TextUnformatted(value.c_str());
}

// La grafica de ritmo: linea objetivo y tiempos reales de los ultimos 10 s.
static void PacingGraph(ImVec2 size) {
  bool graphScreen = false;
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
    graphScreen = overlay::feed.screenMode;
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
  // Azul solo con el limite comprobado (el ritmo plano); si no, un tono neutro: medir sin limite no es un error.
  const ImVec4 stateCol = locked ? kBlue : Hex(0xAEB6C3);
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
  std::vector<overlay::PacePoint> pts =
      overlay::BuildTimeSeries(real, display, span, cols, ref, classCache, generation, graphScreen);
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
  bool screenMode = false;
  {
    std::lock_guard<std::mutex> lock(overlay::feed.m);
    screenMode = overlay::feed.screenMode;
  }
  std::string refText =
      screenMode ? Fmt(isTarget ? T("en pantalla %.1f ms ±%.1f", "on screen %.1f ms ±%.1f")
                                : T("en pantalla ~%.1f ms ±%.1f", "on screen ~%.1f ms ±%.1f"),
                       ref, overlay::PaceTolerance(ref))
      : isTarget ? Fmt(T("objetivo %.1f ms ±%.1f", "target %.1f ms ±%.1f"), ref, overlay::PaceTolerance(ref))
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

// Logo (desde la 0.60, variante "2b"): una f trazada como una grafica (de un nivel bajo a uno alto,
// como un ritmo que se estabiliza), cruzada por la linea plana azul, que pasa por delante sin cortarse:
// el trazo blanco se interrumpe a su alrededor. Rejilla de 120; baldosa redondeada de lado `size`.
// Por debajo de 24 px no hay hueco (seria una mancha) y los trazos engordan para seguir leyendose.
static void DrawLogo(ImDrawList* dl, ImVec2 p, float size) {
  const ImU32 tile = Col(Hex(0x12151B));
  dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), tile, size * 0.22f);
  dl->AddRect(p, ImVec2(p.x + size, p.y + size), Col(kLine), size * 0.22f, 0, std::max(1.0f, size / 64.0f));
  const float k = size / 120.0f;
  auto P = [&](float x, float y) { return ImVec2(p.x + x * k, p.y + y * k); };
  const float grow = size >= 96 ? 1.0f : size >= 40 ? 1.1f : size >= 24 ? 1.3f : 1.7f;
  const float w = 10.0f * k * grow;
  const ImU32 stem = Col(Hex(0xD3D7DE)), bar = Col(kBlue);
  // Trazo blanco: cola baja, subida y remate alto (con extremos redondeados).
  dl->PathLineTo(P(20, 103));
  dl->PathLineTo(P(35, 103));
  dl->PathBezierCubicCurveTo(P(43, 103), P(46, 99), P(48, 92));
  dl->PathLineTo(P(62, 38));
  dl->PathBezierCubicCurveTo(P(64, 31), P(69, 27), P(77, 27));
  dl->PathLineTo(P(108, 27));
  dl->PathStroke(stem, 0, w);
  dl->AddCircleFilled(P(20, 103), w * 0.5f, stem);
  dl->AddCircleFilled(P(108, 27), w * 0.5f, stem);
  // Linea azul por delante: primero un hueco del color de la baldosa alrededor, luego la linea.
  // (Como rectangulos redondeados: una linea gruesa con AddLine sale con un escalon en tamanos grandes.)
  const ImVec2 a = P(17, 65), b = P(109, 65);
  if (size >= 24) dl->AddRectFilled(ImVec2(a.x, a.y - w), ImVec2(b.x, b.y + w), tile);
  dl->AddRectFilled(ImVec2(a.x - w * 0.5f, a.y - w * 0.5f), ImVec2(b.x + w * 0.5f, b.y + w * 0.5f), bar, w * 0.5f);
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
  if (ImGui::IsItemClicked() && page != idx) {
    page = idx;
    g_scrollTop = true;  // cada pagina empieza arriba del todo
  }
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

// Ancho comun de los botones de opciones (prioridad, limitador): el mismo en toda la app.
static float OptionW() { return 170 * scale; }

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
                    : v.vrrNvKnown    ? (v.vrrPossible ? T("G-SYNC disponible", "G-SYNC available")
                                                       : T("no disponible", "not available"))
                                      : T("se comprobará al jugar", "checked while playing");
  if (!v.vrrRange.empty()) vrr += std::string(T("; rango de ", "; range ")) + v.vrrRange;
  return vrr;
}

// Etiqueta redondeada con punto y texto, del color indicado (la del estado del limite).


// Abre el Panel de control de NVIDIA: la version clasica si esta instalada, si no la de la Microsoft Store
// (la de los drivers actuales).
static void OpenNvidiaPanel() {
  const wchar_t* classic = L"C:\\Program Files\\NVIDIA Corporation\\Control Panel Client\\nvcplui.exe";
  if (GetFileAttributesW(classic) != INVALID_FILE_ATTRIBUTES) {
    ShellExecuteW(nullptr, L"open", classic, nullptr, nullptr, SW_SHOWNORMAL);
    return;
  }
  ShellExecuteW(nullptr, L"open", L"explorer.exe",
                L"shell:AppsFolder\\NVIDIACorp.NVIDIAControlPanel_56jybvy8sckqj!NVIDIACorp.NVIDIAControlPanel", nullptr,
                SW_SHOWNORMAL);
}

// Aviso cuando G-SYNC no se esta usando, con la causa, los pasos y un boton al panel de NVIDIA.
static void VrrAlertCard(const View& v) {
  if (v.vrrAlert == 0) return;
  const float w = ImGui::GetContentRegionAvail().x;
  const char* title = T("G-SYNC no se está usando", "G-SYNC isn't being used");
  const char* body =
      v.vrrAlert == 1
          ? T("Tu portátil está en modo híbrido: la gráfica integrada controla la pantalla y G-SYNC no puede "
              "funcionar. Sin él, los límites pensados para G-SYNC pueden empeorar el movimiento.\n"
              "Para activarlo: Panel de control de NVIDIA → Administrar modo de pantalla → «Solo GPU NVIDIA». "
              "En algunos portátiles se elige en su propia app (PredatorSense, Armoury Crate, Lenovo Vantage…) "
              "como modo de GPU «solo NVIDIA», «dGPU» o «Ultimate». Después hay que reiniciar.",
              "Your laptop is in hybrid mode: the integrated GPU drives the display and G-SYNC can't work. "
              "Without it, limits meant for G-SYNC can make motion worse.\n"
              "To turn it on: NVIDIA Control Panel → Manage Display Mode → “NVIDIA GPU only”. On some laptops "
              "it's chosen in their own app (PredatorSense, Armoury Crate, Lenovo Vantage…) as the “NVIDIA "
              "only”, “dGPU” or “Ultimate” GPU mode. Then restart.")
          : T("Tu pantalla admite G-SYNC (funcionaba antes), pero ahora está desactivado. Sin él, los límites "
              "pensados para G-SYNC pueden empeorar el movimiento.\n"
              "Para activarlo: Panel de control de NVIDIA → Configurar G-SYNC → «Habilitar G-SYNC».",
              "Your display supports G-SYNC (it worked before), but it's turned off now. Without it, limits "
              "meant for G-SYNC can make motion worse.\n"
              "To turn it on: NVIDIA Control Panel → Set up G-SYNC → “Enable G-SYNC”.");
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float pad = 16 * scale, wrap = w - 2 * pad;
  const float th = ImGui::CalcTextSize(body, nullptr, false, wrap).y;
  const float bh = ImGui::GetFrameHeight();
  const float h = pad + ImGui::GetTextLineHeight() + 6 * scale + th + 10 * scale + bh + pad;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), Col(kCard), 10 * scale);
  dl->AddRectFilled(p, ImVec2(p.x + 4 * scale, p.y + h), Col(kAmber), 10 * scale, ImDrawFlags_RoundCornersLeft);
  ImGui::SetCursorScreenPos(ImVec2(p.x + pad, p.y + pad));
  if (fontStrong) ImGui::PushFont(fontStrong);
  ImGui::TextColored(kAmber, "%s", title);
  if (fontStrong) ImGui::PopFont();
  ImGui::SetCursorScreenPos(ImVec2(p.x + pad, ImGui::GetCursorScreenPos().y + 6 * scale - ImGui::GetStyle().ItemSpacing.y));
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
  ImGui::TextColored(kText, "%s", body);
  ImGui::PopTextWrapPos();
  ImGui::SetCursorScreenPos(ImVec2(p.x + pad, ImGui::GetCursorScreenPos().y + 10 * scale - ImGui::GetStyle().ItemSpacing.y));
  if (ImGui::Button(T("Abrir el panel de NVIDIA", "Open the NVIDIA panel"))) OpenNvidiaPanel();
  ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + 14 * scale));
  ImGui::Dummy(ImVec2(w, 0));
}

// Aceptar la recomendacion de pasar un juego a "Ritmo constante": queda fijado para el (prioridad elegida
// por el usuario). Desde la fila de Juegos y desde Ahora.
static void AcceptPrioSuggestion(const std::string& exe, const std::string& label) {
  WriteKv(L"equipo.txt", "prioridad-fija|" + exe, "estabilidad");
  WriteKv(L"equipo.txt", "prioridad-sugerida|" + exe, "");
  int mode = 0;
  bool autoSt = false;
  const bool fluidNow = GameUsesFluid(exe, &mode, &autoSt);
  std::lock_guard<std::mutex> lock(g_ui.m);
  g_ui.prioSuggest.erase(exe);
  g_ui.warnings.erase(exe);
  for (auto& u : g_ui.changes)
    if (u.exe == exe) {
      u.prioMode = mode;
      u.usesFluid = fluidNow;
      u.autoStability = autoSt;
      u.prioSuggest = false;
      u.warning.clear();
    }
  g_ui.notice = Fmt(T("%s: «Ritmo constante» desde la próxima partida.", "%s: “Steady pace” from the next session."), label.c_str());
}

// Hace cuanto terminó algo, en palabras ("hace un momento", "hace 5 minutos", "hace 2 h").
static std::string AgoText(double seconds) {
  if (seconds < 60) return T("hace un momento", "a moment ago");
  const int m = static_cast<int>(seconds / 60);
  if (m < 60) return m == 1 ? T("hace 1 minuto", "1 minute ago") : Fmt(T("hace %d minutos", "%d minutes ago"), m);
  const int h = m / 60;
  return h == 1 ? T("hace 1 hora", "1 hour ago") : Fmt(T("hace %d horas", "%d hours ago"), h);
}

static void VSpace(float px) { ImGui::Dummy(ImVec2(0, px * scale)); }

// Una cifra de "Ahora": el valor en grande (Bahnschrift) y debajo qué es, en palabras.
static void Figure(const std::string& value, const std::string& label, float width) {
  ImGui::BeginGroup();
  ImGui::PushFont(fontNum);
  ImGui::TextUnformatted(value.c_str());
  ImGui::PopFont();
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width - 16 * scale);
  ImGui::TextColored(kDim, "%s", label.c_str());
  ImGui::PopTextWrapPos();
  ImGui::EndGroup();
}

// Leyenda de la grafica: una muestra de color y su significado.
static void LegendItem(const ImVec4& c, const char* text) {
  ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = ImGui::GetTextLineHeight();
  ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + h * 0.5f - 1.5f * scale),
                                            ImVec2(p.x + 18 * scale, p.y + h * 0.5f + 1.5f * scale), Col(c), 2 * scale);
  ImGui::Dummy(ImVec2(24 * scale, h));
  ImGui::SameLine(0, 0);
  ImGui::TextColored(kDim, "%s", text);
}

// "Ahora" (desde la 0.61): un veredicto en una frase, la grafica de ritmo como protagonista, tres cifras,
// solo la accion que toca y los detalles plegados. Sin juego abierto, que hacer y la ultima partida:
// nunca las cifras de una partida que ya termino.
static void PageNow(const View& v) {
  VrrAlertCard(v);
  const float full = ImGui::GetContentRegionAvail().x;
  ImDrawList* dl = ImGui::GetWindowDrawList();

  // ---------------- Sin juego abierto ----------------
  if (!v.haveGame) {
    const bool err = v.status == UiStatus::Error;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + full);
    ImGui::PushFont(fontVerdict);
    ImGui::TextUnformatted(err ? v.statusText.c_str() : T("Abre un juego.", "Open a game."));
    ImGui::PopFont();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 4 * scale);
    ImGui::TextColored(kDim, "%s", err ? v.statusDetail.c_str()
                                       : T("Lo detecto solo en cuanto pasa a primer plano, y empiezo a medir.",
                                           "I detect it as soon as it comes to the front, and start measuring."));
    ImGui::PopTextWrapPos();
    VSpace(24);
    {  // la linea plana, en reposo
      const ImVec2 p = ImGui::GetCursorScreenPos();
      const float h = 96 * scale, pad = 24 * scale;
      dl->AddRectFilled(p, ImVec2(p.x + full, p.y + h), Col(kCard), 16 * scale);
      for (float x = p.x + pad; x < p.x + full - pad; x += 12 * scale)
        dl->AddCircleFilled(ImVec2(x, p.y + h * 0.5f), 1.6f * scale, Col(kLine));
      ImGui::Dummy(ImVec2(full, h));
    }
    if (!v.lastGame.empty()) {
      VSpace(24);
      const ImVec2 p = ImGui::GetCursorScreenPos();
      const float pad = 24 * scale;
      dl->ChannelsSplit(2);
      dl->ChannelsSetCurrent(1);
      ImGui::SetCursorScreenPos(ImVec2(p.x + pad, p.y + pad * 0.9f));
      ImGui::BeginGroup();
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + full - pad * 2);
      ImGui::PushFont(fontHead);
      ImGui::TextUnformatted(T("Tu última partida", "Your last session"));
      ImGui::PopFont();
      const std::string when = v.lastGame + ", " + AgoText(NowSec() - v.lastEnd);
      ImGui::TextColored(kDim, "%s", when.c_str());
      VSpace(6);
      const int mins = static_cast<int>(std::lround(v.lastMinutes));
      const std::string dur = mins < 1 ? std::string(T("menos de un minuto", "less than a minute"))
                              : mins == 1 ? std::string(T("1 minuto", "1 minute"))
                                          : Fmt(T("%d minutos", "%d minutes"), mins);
      const std::string hit = v.lastHitches == 1 ? std::string(T("1 tirón", "1 stutter"))
                                                 : Fmt(T("%d tirones", "%d stutters"), v.lastHitches);
      const std::string sum =
          v.lastLocked ? Fmt(T("Fue estable a %.0f FPS durante %s, con %s. Su límite está comprobado y aceptado.",
                               "It was steady at %.0f FPS for %s, with %s. Its limit is checked and accepted."),
                             v.lastFps, dur.c_str(), hit.c_str())
                       : Fmt(T("Jugaste %s, a unos %.0f FPS, con %s.", "You played for %s, at about %.0f FPS, with %s."),
                             dur.c_str(), v.lastFps, hit.c_str());
      ImGui::TextUnformatted(sum.c_str());
      ImGui::PopTextWrapPos();
      VSpace(10);
      if (ImGui::Button(T("Ver en Juegos", "See in Games"))) {
        page = 1;
        g_scrollTop = true;
      }
      ImGui::EndGroup();
      const float bottom = ImGui::GetItemRectMax().y + pad * 0.9f;
      dl->ChannelsSetCurrent(0);
      dl->AddRect(p, ImVec2(p.x + full, bottom), Col(kLine), 16 * scale, 0, 1.0f * scale);
      dl->ChannelsMerge();
      ImGui::SetCursorScreenPos(ImVec2(p.x, bottom));
      ImGui::Dummy(ImVec2(full, 0));
    }
    if (!v.notice.empty()) {
      VSpace(16);
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + full);
      ImGui::TextColored(kBlue, "%s", v.notice.c_str());
      ImGui::PopTextWrapPos();
    }
    return;
  }

  // ---------------- Durante una partida ----------------
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
  const UiChange* cur = nullptr;
  for (const auto& c : v.changes)
    if (c.exe == v.exe) cur = &c;

  // Veredicto: una frase con la conclusion, y debajo el porque.
  std::string title, sub;
  if (v.status == UiStatus::Paused) {
    title = T("En pausa.", "Paused.");
    sub = v.statusDetail.empty() ? v.statusText : v.statusDetail;
  } else if (v.status == UiStatus::Restart) {
    title = v.statusText;
    sub = v.statusDetail;
  } else if (locked && cur) {
    const double real = cur->value / static_cast<double>(std::max(1, cur->fg));
    const bool sinking = v.lowFps > 0 && v.lowFps < real / 1.13;  // la regla del 13 %
    title = cur->fg > 1 ? Fmt(sinking ? T("Va a %.0f FPS reales, con caídas en las zonas pesadas.",
                                         "Running at %.0f real FPS, with drops in heavy areas.")
                                       : T("Va estable a %.0f FPS reales.", "Steady at %.0f real FPS."),
                              real)
                        : Fmt(sinking ? T("Va a %.0f FPS, con caídas en las zonas pesadas.", "Running at %.0f FPS, with drops in heavy areas.")
                                      : T("Va estable a %.0f FPS.", "Steady at %.0f FPS."),
                              real);
    sub = std::string(T("Límite de ", "Limit of ")) + cur->desc + T(". Comprobado y aceptado.", ". Checked and accepted.");
  } else if (ls == overlay::LimitState::Pending) {
    title = Fmt(T("Comprobando el límite de %s…", "Checking the %s limit…"), limLabel.c_str());
    sub = cur ? std::string(T("Límite de ", "Limit of ")) + cur->desc + T(". En unos segundos sabré si funciona.", ". I'll know in a few seconds whether it works.")
              : std::string(T("En unos segundos sabré si funciona.", "I'll know in a few seconds whether it works."));
  } else {
    title = T("Midiendo sin límite.", "Measuring without a limit.");
    sub = g_applyMode ? T("Al cerrar el juego decidiré su límite con toda la partida.",
                          "When you close the game, I'll decide its limit using the whole session.")
                      : T("«Aplicar límites» está desactivado: solo mido.", "“Apply limits” is off: I only measure.");
  }
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + full);
  ImGui::PushFont(fontHead);
  ImGui::TextColored(Hex(0xAEB6C3), "%s", v.game.c_str());
  ImGui::PopFont();
  VSpace(2);
  ImGui::PushFont(fontVerdict);
  ImGui::TextUnformatted(title.c_str());
  ImGui::PopFont();
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 4 * scale);
  if (!sub.empty()) ImGui::TextColored(kDim, "%s", sub.c_str());
  if (!v.currentWarning.empty()) {
    VSpace(4);
    ImGui::TextColored(kAmber, "%s", v.currentWarning.c_str());
  }
  ImGui::PopTextWrapPos();
  VSpace(22);

  // La grafica de ritmo, a todo lo ancho: la protagonista.
  ImGui::TextColored(kDim, "%s", T("Ritmo de los últimos 10 segundos", "Pace over the last 10 seconds"));
  VSpace(4);
  PacingGraph(ImVec2(full, 250 * scale));
  VSpace(6);
  LegendItem(locked ? kBlue : Hex(0xAEB6C3), locked ? T("Al ritmo del límite", "At the limit's pace") : T("Ritmo del juego", "Game's pace"));
  ImGui::SameLine(0, 28 * scale);
  LegendItem(kAmber, T("Tirón: un fotograma de más de 2,5 veces lo normal y al menos 8 ms más largo", "Stutter: a frame more than 2.5 times the usual and at least 8 ms longer"));
  VSpace(24);

  // Tres cifras con nombres claros. En pausa, el motor deja los FPS reales a cero: se muestra el ultimo
  // valor de esta partida, para que las tres cifras sean coherentes.
  static double lastReal = 0.0;
  static std::string lastRealExe;
  if (v.realFps > 0) {
    lastReal = v.realFps;
    lastRealExe = v.exe;
  }
  const double shownReal = v.realFps > 0 ? v.realFps : lastRealExe == v.exe ? lastReal : 0.0;
  {
    const float colW = full / 3.0f;
    const float x0 = ImGui::GetCursorPosX();
    Figure(shownReal > 0 ? Fmt("%.0f", shownReal) : "—",
           v.screenFps > 0 ? Fmt(T("FPS reales (%.0f en pantalla)", "real FPS (%.0f on screen)"), v.screenFps)
                           : std::string(T("FPS reales", "real FPS")),
           colW);
    ImGui::SameLine(x0 + colW);
    Figure(v.lowFps > 0 ? Fmt("%.0f", v.lowFps) : "—", T("FPS en el 1 % más lento", "FPS in the slowest 1 %"), colW);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", T("El ritmo del 1 % de fotogramas más lentos (sin contar los tirones).\n"
                                "Cuanto más cerca de los FPS reales, más constante va el juego.",
                                "The pace of the slowest 1 % of frames (not counting stutters).\n"
                                "The closer to the real FPS, the steadier the game."));
    ImGui::SameLine(x0 + colW * 2);
    Figure(Fmt("%d", v.hitches), T("Tirones en esta partida", "Stutters this session"), colW);
  }
  VSpace(24);

  // Solo la accion que toca: la recomendacion pendiente de este juego, o aplicar ya la actual.
  {
    std::string text, button;
    bool prio = false, applyNow = false;
    if (cur && (cur->suggestFps > 0 || cur->prioSuggest)) {
      text = cur->warning;
      prio = cur->suggestFps <= 0;
      button = prio ? T("Usar «Ritmo constante»", "Use “Steady pace”")
                    : Fmt(cur->suggestKind == 0 ? T("Subir a %.0f FPS", "Raise to %.0f FPS") : T("Bajar a %.0f FPS", "Lower to %.0f FPS"),
                          cur->suggestFps);
    } else if (!cur && g_applyMode && !v.recText.empty() && v.recText.find("—") == std::string::npos &&
               v.recText != T("midiendo…", "measuring…")) {
      text = Fmt(T("Límite recomendado ahora: %s. Si no quieres esperar al final de la partida, aplícalo ya y reinicia el juego.",
                   "Limit recommended now: %s. If you don't want to wait for the end of the session, apply it now and restart the game."),
                 v.recText.c_str());
      button = T("Aplicar ya", "Apply now");
      applyNow = true;
    }
    if (!button.empty()) {
      const ImVec2 p = ImGui::GetCursorScreenPos();
      const float pad = 22 * scale, bw = ImGui::CalcTextSize(button.c_str()).x + ImGui::GetStyle().FramePadding.x * 2;
      const bool urgent = cur && cur->suggestFps > 0 && cur->suggestKind == 2;
      dl->ChannelsSplit(2);
      dl->ChannelsSetCurrent(1);
      // Texto y boton centrados en la misma franja, sea el texto de una linea o de varias.
      const float wrapW = full - pad * 3 - bw;
      const float textH = ImGui::CalcTextSize(text.c_str(), nullptr, false, wrapW).y;
      const float rowH = std::max(textH, ImGui::GetFrameHeight());
      ImGui::SetCursorScreenPos(ImVec2(p.x + pad, p.y + pad + (rowH - textH) * 0.5f));
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapW);
      ImGui::TextColored(urgent ? kRed : kText, "%s", text.c_str());
      ImGui::PopTextWrapPos();
      ImGui::SetCursorScreenPos(ImVec2(p.x + full - pad - bw, p.y + pad + (rowH - ImGui::GetFrameHeight()) * 0.5f));
      PrimaryButtonStyle(true);
      if (ImGui::Button(button.c_str())) {
        if (applyNow) {
          g_applyNowRequest = true;
        } else if (prio) {
          AcceptPrioSuggestion(cur->exe, cur->label);
        } else {
          std::lock_guard<std::mutex> lock(g_manualLimitMutex);
          g_applySuggestionReq.push_back(cur->exe);
        }
      }
      PrimaryButtonEnd(true);
      const float bottom = p.y + pad * 2 + rowH;
      dl->ChannelsSetCurrent(0);
      dl->AddRectFilled(p, ImVec2(p.x + full, bottom), Col(kCard), 16 * scale);
      dl->AddRect(p, ImVec2(p.x + full, bottom), Col(urgent ? kRed : kLine, urgent ? 0.6f : 1.0f), 16 * scale, 0, 1.0f * scale);
      dl->ChannelsMerge();
      ImGui::SetCursorScreenPos(ImVec2(p.x, bottom));
      ImGui::Dummy(ImVec2(full, 0));
      VSpace(20);
    }
  }

  // Detalles, plegados: un titulo con flecha, alineado con el resto (una cabecera plegable de la
  // biblioteca se saldria de los margenes de la pagina).
  {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddLine(p, ImVec2(p.x + full, p.y), Col(kLine), 1.0f * scale);
    VSpace(12);
  }
  ImGui::PushFont(fontHead);
  const bool detailsOpen = ImGui::TreeNodeEx(T("Detalles", "Details"), ImGuiTreeNodeFlags_NoTreePushOnOpen);
  ImGui::PopFont();
  if (detailsOpen) {
    VSpace(4);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0, 5 * scale));
    if (ImGui::BeginTable("detalles", 2, ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 230 * scale);
      ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
      auto Row = [](const char* k, const std::string& val) {
        if (val.empty() || val == "—") return;
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextColored(kDim, "%s", k);
        ImGui::TableSetColumnIndex(1);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(val.c_str());
        ImGui::PopTextWrapPos();
      };
      Row("G-SYNC / VRR", VrrDescription(v));
      Row(T("Presentación", "Presentation"), v.presentText);
      Row(T("Qué limita al juego", "What limits the game"), v.bottleneck);
      Row(T("Límite propio del juego", "Game's own limit"), v.capText);
      Row(T("Variación del ritmo", "Pace swing"),
          swing >= 0 ? (baseSwing > 0 ? Fmt(T("±%.1f ms (sin límite: ±%.1f ms)", "±%.1f ms (without a limit: ±%.1f ms)"), swing, baseSwing)
                                      : Fmt("±%.1f ms", swing))
                     : std::string());
      Row(T("Pantalla", "Display"), v.monitor);
      ImGui::EndTable();
    }
    ImGui::PopStyleVar();
  }
  if (!v.notice.empty()) {
    VSpace(16);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + full);
    ImGui::TextColored(kBlue, "%s", v.notice.c_str());
    ImGui::PopTextWrapPos();
  }
}

// ---- Desplegable de cada juego en Juegos (0.63): por que tiene su limite ----
static std::set<std::string> g_expandedGames;  // filas desplegadas (solo la ventana)

static std::string ShortDate(long long t) {
  if (t <= 0) return std::string();
  time_t tt = static_cast<time_t>(t);
  tm lt{};
  localtime_s(&lt, &tt);
  static const char* es[] = {"ene", "feb", "mar", "abr", "may", "jun", "jul", "ago", "sep", "oct", "nov", "dic"};
  static const char* en[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  return g_lang == 1 ? Fmt("%s %d, %02d:%02d", en[lt.tm_mon], lt.tm_mday, lt.tm_hour, lt.tm_min)
                     : Fmt("%d %s, %02d:%02d", lt.tm_mday, es[lt.tm_mon], lt.tm_hour, lt.tm_min);
}

static void DashedLine(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float th, float dash, float gap) {
  const float len = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
  if (len <= 0) return;
  const ImVec2 d((b.x - a.x) / len, (b.y - a.y) / len);
  for (float t = 0; t < len; t += dash + gap) {
    const float e = std::min(len, t + dash);
    dl->AddLine(ImVec2(a.x + d.x * t, a.y + d.y * t), ImVec2(a.x + d.x * e, a.y + d.y * e), col, th);
  }
}

// Leyenda debajo de una grafica (0.67.1): una muestra de cada linea y su significado; pasa a otra linea si
// no cabe.
struct ChartKey {
  int kind;  // 0 linea continua, 1 punteada, 2 zona sombreada
  ImVec4 col;
  std::string text;
};
static void DrawChartKey(const std::vector<ChartKey>& items, float w) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float lh = ImGui::GetTextLineHeight(), sw = 24 * scale;
  float x = p.x + 8 * scale, y = p.y;
  for (const ChartKey& it : items) {
    const float tw = ImGui::CalcTextSize(it.text.c_str()).x;
    const float iw = sw + 8 * scale + tw + 22 * scale;
    if (x > p.x + 8 * scale && x + iw > p.x + w) {
      x = p.x + 8 * scale;
      y += lh + 6 * scale;
    }
    const float cy = y + lh * 0.5f;
    if (it.kind == 0) dl->AddLine(ImVec2(x, cy), ImVec2(x + sw, cy), Col(it.col), 2.5f * scale);
    else if (it.kind == 1) DashedLine(dl, ImVec2(x, cy), ImVec2(x + sw, cy), Col(it.col), 2.0f * scale, 5 * scale, 3 * scale);
    else dl->AddRectFilled(ImVec2(x, y + 2 * scale), ImVec2(x + sw, y + lh - 2 * scale), Col(it.col, 0.12f), 3 * scale);
    dl->AddText(ImVec2(x + sw + 8 * scale, y), Col(kDim), it.text.c_str());
    x += iw;
  }
  ImGui::Dummy(ImVec2(w, y - p.y + lh + 2 * scale));
}

// Curva de la partida que decidio el limite: % de los segundos en que el juego alcanzo cada ritmo.
static void DrawCurveChart(const LimitExplain& e, double limitReal, float w) {
  const float h = 190 * scale;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), Col(kCardHi), 8 * scale);
  const float L = p.x + 52 * scale, R = p.x + w - 16 * scale, Tp = p.y + 14 * scale, B = p.y + h - 28 * scale;
  const float x0 = e.curveLo, x1 = e.curveLo + e.curveStep * (e.curve.size() - 1);
  auto sx = [&](double x) { return L + static_cast<float>((x - x0) / std::max(1e-6, static_cast<double>(x1 - x0))) * (R - L); };
  auto sy = [&](double y) { return B - static_cast<float>(y / 100.0) * (B - Tp); };
  for (int yv : {0, 50, 100}) {
    dl->AddLine(ImVec2(L, sy(yv)), ImVec2(R, sy(yv)), Col(kLine), 1.0f);
    const std::string lab = Fmt("%d %%", yv);
    const ImVec2 ts = ImGui::CalcTextSize(lab.c_str());
    dl->AddText(ImVec2(L - ts.x - 8 * scale, sy(yv) - ts.y * 0.5f), Col(kDim), lab.c_str());
  }
  const double span = x1 - x0, stepT = span > 160 ? 40 : span > 80 ? 20 : span > 40 ? 10 : 5;
  const char* unit = "FPS";
  const float unitX = R - ImGui::CalcTextSize(unit).x;
  for (double xv = std::ceil(x0 / stepT) * stepT; xv <= x1; xv += stepT) {
    const std::string lab = Fmt("%.0f", xv);
    const ImVec2 ts = ImGui::CalcTextSize(lab.c_str());
    if (sx(xv) + ts.x * 0.5f > unitX - 10 * scale) break;  // no montarlo sobre la unidad
    dl->AddText(ImVec2(sx(xv) - ts.x * 0.5f, B + 6 * scale), Col(kDim), lab.c_str());
  }
  dl->AddText(ImVec2(unitX, B + 6 * scale), Col(kDim), unit);
  if (e.fluid || e.curveSeconds)
    DashedLine(dl, ImVec2(L, sy(90)), ImVec2(R, sy(90)), Col(kDim), 1.5f * scale, 4 * scale, 4 * scale);
  for (size_t i = 1; i < e.curve.size(); ++i)
    dl->AddLine(ImVec2(sx(x0 + (i - 1) * e.curveStep), sy(e.curve[i - 1])), ImVec2(sx(x0 + i * e.curveStep), sy(e.curve[i])),
                Col(kBlue), 2.5f * scale);
  auto VMark = [&](double x, const ImVec4& col, bool dashed, const std::string& lab, bool right) {
    if (x < x0 || x > x1) return;
    if (dashed) DashedLine(dl, ImVec2(sx(x), Tp), ImVec2(sx(x), B), Col(col), 2.0f * scale, 5 * scale, 4 * scale);
    else dl->AddLine(ImVec2(sx(x), Tp), ImVec2(sx(x), B), Col(col), 2.0f * scale);
    const ImVec2 ts = ImGui::CalcTextSize(lab.c_str());
    const ImVec2 at(right ? sx(x) + 8 * scale : sx(x) - ts.x - 8 * scale, B - ts.y - 10 * scale);
    dl->AddRectFilled(ImVec2(at.x - 5 * scale, at.y - 3 * scale), ImVec2(at.x + ts.x + 5 * scale, at.y + ts.y + 3 * scale),
                      Col(kCard), 4 * scale);
    dl->AddText(at, Col(col), lab.c_str());
  };
  if (!e.fluid && e.low1 > 0)
    VMark(e.low1, kDim, true, Fmt(T("1 %% más lento: %.0f", "slowest 1 %%: %.0f"), e.low1), false);
  VMark(limitReal, kAmber, false, Fmt(T("límite: %.0f", "limit: %.0f"), limitReal), true);
  ImGui::Dummy(ImVec2(w, h));
  VSpace(6);
  std::vector<ChartKey> leg;
  leg.push_back({0, kBlue, e.curveSeconds ? T("% de los segundos que alcanzaron cada ritmo", "% of seconds that reached each pace")
                                          : T("% de los fotogramas que alcanzaron cada ritmo", "% of frames that reached each pace")});
  if (e.fluid || e.curveSeconds) leg.push_back({1, kDim, T("90 %", "90 %")});
  if (!e.fluid && e.low1 > 0) leg.push_back({1, kDim, T("1 % más lento de la partida", "session's slowest 1 %")});
  leg.push_back({0, kAmber, T("límite", "limit")});
  DrawChartKey(leg, w);
}

// La ultima partida, segundo a segundo, frente al umbral del 13 %.
static void DrawSessionChart(const SessionExplain& x, float w) {
  const float h = 180 * scale;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), Col(kCardHi), 8 * scale);
  const float L = p.x + 52 * scale, R = p.x + w - 58 * scale, Tp = p.y + 14 * scale, B = p.y + h - 28 * scale;
  const double thr = x.limit * (1.0 - cfg::kJnd);
  // Escala fija relativa al limite, del 50 % al 110 % (0.67.3). Antes se ajustaba a la caida mas profunda
  // de cada partida y empezaba cerca de ella: una caida pequena llenaba la grafica y parecia peor de lo que
  // era, y una partida no se podia comparar con otra. Asi el umbral queda siempre a la misma altura; una
  // caida por debajo de la mitad del limite se dibuja en el borde inferior.
  const double lo = x.limit * 0.5, hi = x.limit * 1.10;
  const size_t n = x.fps.size();
  auto sx = [&](double i) { return L + static_cast<float>(i / std::max<double>(1, n - 1)) * (R - L); };
  auto sy = [&](double y) { return B - static_cast<float>((std::clamp(y, lo, hi) - lo) / (hi - lo)) * (B - Tp); };
  // Izquierda, FPS; derecha, cuanto por debajo del limite.
  const double marks[] = {1.0, 1.0 - cfg::kJnd, 0.7, 0.5};
  for (double f : marks) {
    const double yv = x.limit * f;
    if (f < 1.0 - cfg::kJnd - 1e-9)
      dl->AddLine(ImVec2(L, sy(yv)), ImVec2(R, sy(yv)), Col(kLine), 1.0f);
    const std::string lab = Fmt("%.0f", yv);
    const ImVec2 ts = ImGui::CalcTextSize(lab.c_str());
    dl->AddText(ImVec2(L - ts.x - 8 * scale, sy(yv) - ts.y * 0.5f), Col(kDim), lab.c_str());
    const std::string pc = f >= 1.0 ? std::string(T("límite", "limit")) : Fmt("−%.0f %%", 100.0 * (1.0 - f));
    dl->AddText(ImVec2(R + 8 * scale, sy(yv) - ts.y * 0.5f), Col(kDim), pc.c_str());
  }
  // Ultimo minuto: no cuenta para las reglas.
  const double trimPts = x.trimSecs / std::max(1.0f, x.secsPerPoint);
  if (n > trimPts + 2) {
    const float a = sx(n - 1 - trimPts);
    dl->AddRectFilled(ImVec2(a, Tp), ImVec2(R, B), Col(kText, 0.05f));
    const char* lab = T("no cuenta", "doesn't count");
    // Centrada en la zona, pero sin salirse de la grafica (con 15 s la zona es estrecha).
    const float lw = ImGui::CalcTextSize(lab).x;
    const float lx = std::min((a + R - lw) * 0.5f, R - lw - 4 * scale);
    dl->AddText(ImVec2(lx, B - ImGui::GetTextLineHeight() - 4 * scale), Col(kDim), lab);
  }
  DashedLine(dl, ImVec2(L, sy(thr)), ImVec2(R, sy(thr)), Col(kAmber), 2.0f * scale, 6 * scale, 4 * scale);
  for (size_t i = 1; i < n; ++i)
    if (x.fps[i - 1] > 0 && x.fps[i] > 0)
      dl->AddLine(ImVec2(sx(i - 1), sy(x.fps[i - 1])), ImVec2(sx(i), sy(x.fps[i])), Col(kGray), 2.5f * scale);
  for (size_t i = 1; i < n && i < x.low.size(); ++i) {
    if (!(x.low[i - 1] > 0 && x.low[i] > 0)) continue;
    const double y0 = x.low[i - 1], y1 = x.low[i];
    const ImVec2 a(sx(i - 1), sy(y0)), b(sx(i), sy(y1));
    const bool below0 = y0 < thr, below1 = y1 < thr;
    if (below0 == below1) {
      dl->AddLine(a, b, Col(below0 ? kRed : kBlue), 2.0f * scale);
    } else {
      // El tramo cruza el umbral: rojo solo la parte de abajo (0.67.1; antes, todo el tramo en azul).
      const double t = (thr - y0) / (y1 - y0);
      const ImVec2 m(a.x + static_cast<float>(t) * (b.x - a.x), sy(thr));
      dl->AddLine(a, m, Col(below0 ? kRed : kBlue), 2.0f * scale);
      dl->AddLine(m, b, Col(below1 ? kRed : kBlue), 2.0f * scale);
    }
  }
  const double totalMin = n * x.secsPerPoint / 60.0;
  for (int k = 0; k <= 4; ++k) {
    const double m = totalMin * k / 4.0;
    const std::string lab = totalMin >= 4 ? Fmt("%.0f min", m) : Fmt("%.0f s", m * 60);
    const ImVec2 ts = ImGui::CalcTextSize(lab.c_str());
    const float px = sx((n - 1) * k / 4.0);
    dl->AddText(ImVec2(std::clamp(px - ts.x * 0.5f, L, R - ts.x), B + 6 * scale), Col(kDim), lab.c_str());
  }
  ImGui::Dummy(ImVec2(w, h));
  VSpace(6);
  std::vector<ChartKey> leg;
  leg.push_back({0, kGray, x.fg ? T("ritmo real de cada segundo", "each second's real pace") : T("ritmo de cada segundo", "each second's pace")});
  if (!x.fg) {
    leg.push_back({0, kBlue, T("1 % más lento de los últimos 20 s", "slowest 1 % of the last 20 s")});
    leg.push_back({0, kRed, T("por debajo del umbral", "below the threshold")});
  }
  leg.push_back({1, kAmber, Fmt(T("umbral: límite − 13 %% (%.0f)", "threshold: limit − 13 %% (%.0f)"), thr)});
  if (n > trimPts + 2) leg.push_back({2, kText, T("cierre del juego: no cuenta", "game closing: doesn't count")});
  DrawChartKey(leg, w);
}

// Una regla con su resultado: 1 = se cumple, 0 = no, -1 = informativa.
static void RuleLine(int state, const std::string& text, float w) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float lh = ImGui::GetTextLineHeight(), cx = p.x + 8 * scale, cy = p.y + lh * 0.5f;
  if (state == 1) {
    dl->AddLine(ImVec2(cx - 5 * scale, cy), ImVec2(cx - 1.5f * scale, cy + 4 * scale), Col(kBlue), 2.2f * scale);
    dl->AddLine(ImVec2(cx - 1.5f * scale, cy + 4 * scale), ImVec2(cx + 6 * scale, cy - 5 * scale), Col(kBlue), 2.2f * scale);
  } else if (state == 0) {
    dl->AddLine(ImVec2(cx - 4.5f * scale, cy - 4.5f * scale), ImVec2(cx + 4.5f * scale, cy + 4.5f * scale), Col(kAmber), 2.2f * scale);
    dl->AddLine(ImVec2(cx - 4.5f * scale, cy + 4.5f * scale), ImVec2(cx + 4.5f * scale, cy - 4.5f * scale), Col(kAmber), 2.2f * scale);
  } else {
    dl->AddCircleFilled(ImVec2(cx, cy), 2.8f * scale, Col(kDim));
  }
  ImGui::SetCursorScreenPos(ImVec2(p.x + 26 * scale, p.y));
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - 26 * scale);
  ImGui::TextUnformatted(text.c_str());
  ImGui::PopTextWrapPos();
}

static void DrawLimitExplain(const UiChange& c, float w) {
  const LimitExplain e = LimitExplain::Parse(c.explain);
  const SessionExplain x = SessionExplain::Parse(c.lastSession);
  const ClimbState cl = ClimbState::Parse(c.climb);
  const int fg = std::max(1, c.fg);
  const double limitReal = static_cast<double>(c.value) / fg;
  const float inner = w - 2 * 22 * scale;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  dl->ChannelsSplit(2);
  dl->ChannelsSetCurrent(1);
  ImGui::SetCursorScreenPos(ImVec2(p0.x + 22 * scale, p0.y + 18 * scale));
  ImGui::BeginGroup();
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
  // 1. La formula.
  std::string rule, formula, why;
  const std::string when = ShortDate(e.when);
  switch (e.kind) {
    case LimitExplain::WholeSession:
      if (!c.vrr && c.divisor > 0) {
        rule = T("Sin G-SYNC: un divisor exacto de los Hz de la pantalla", "Without G-SYNC: an exact divisor of the display's Hz");
        formula = Fmt(T("%.0f Hz ÷ %d = %.0f FPS", "%.0f Hz ÷ %d = %.0f FPS"), e.to * c.divisor, c.divisor, e.to);
      } else if (e.fluid) {
        rule = T("Más FPS: el ritmo que alcanza el 90 % de los fotogramas", "More FPS: the pace 90 % of frames reach");
        formula = Fmt(T("90 %% de los fotogramas a %.0f FPS o más", "90 %% of frames at %.0f FPS or more"), e.to);
      } else {
        rule = T("Ritmo constante: el 1 % de fotogramas más lentos puede llegar como mucho un 13 % tarde",
                 "Steady pace: the slowest 1 % of frames can arrive at most 13 % late");
        formula = Fmt(T("1,13 × el 1 %% más lento → %.0f FPS", "1.13 × the slowest 1 %% → %.0f FPS"), e.to);
      }
      why = Fmt(T("Decidido al cerrar la partida del %s, con toda la partida (%.0f min de juego).",
                  "Decided when the %s session closed, with the whole session (%.0f min of play)."), when.c_str(), e.minutes);
      why += T(" No cuentan los tirones (fotogramas de más de 2,5 veces lo normal).",
               " Stutters (frames more than 2.5 times the usual) don't count.");
      if (!e.fluid && e.low1 > 0)
        why += Fmt(T(" El 1 %% más lento medido en esa partida fue de %.0f FPS.", " The slowest 1 %% measured in that session was %.0f FPS."), e.low1);
      break;
    case LimitExplain::FailMedian:
      rule = Fmt(T("Regla de fallos: con %.0f, el juego quedó más de un 13 %% por debajo el %.0f %% de la partida",
                   "Failure rule: with %.0f, the game stayed more than 13 %% below it %.0f %% of the session"), e.from, e.pct);
      formula = Fmt(T("90 %% de los segundos a %.0f FPS o más", "90 %% of the seconds at %.0f FPS or more"), e.to);
      why = Fmt(T("Bajó al ritmo que el juego superó el 90 %% de los segundos de la partida del %s.",
                  "Lowered to the pace the game exceeded in 90 %% of the seconds of the %s session."), when.c_str());
      break;
    case LimitExplain::FailTail:
      if (e.version >= 3 && e.low1 > 0) {
        // 0.66: la formula de Ritmo constante con la partida, como mucho un escalon por partida
        const double t = std::floor(1.13 * e.low1 + 1e-9);
        rule = Fmt(T("Ritmo constante: con %.0f, el 1 %% más lento de la partida fue de %.0f FPS",
                     "Steady pace: with %.0f, the session's slowest 1 %% was %.0f FPS"), e.from, e.low1);
        if (std::fabs(t - e.to) < 1.0) {
          formula = Fmt(T("1,13 × %.0f = %.0f FPS", "1.13 × %.0f = %.0f FPS"), e.low1, e.to);
          why = Fmt(T("Bajó a lo que dice la fórmula de «Ritmo constante» con la partida del %s.",
                      "Lowered to what the “Steady pace” formula gives with the %s session."), when.c_str());
        } else {
          formula = Fmt(T("%.0f ÷ 1,13 = %.0f FPS", "%.0f ÷ 1.13 = %.0f FPS"), e.from, e.to);
          why = Fmt(T("La fórmula daba %.0f (1,13 × %.0f), pero el límite baja como mucho un escalón del 13 %% por partida, por si fue una partida anómala (partida del %s).",
                      "The formula gave %.0f (1.13 × %.0f), but the limit drops at most one 13 %% step per session, in case it was an unusual session (the %s session)."),
                    t, e.low1, when.c_str());
        }
      } else {
        rule = Fmt(T("Regla del 1 %% más lento: con %.0f, se hundió más de un 13 %% el %.0f %% de la partida",
                     "Slowest 1 %% rule: with %.0f, it sank more than 13 %% for %.0f %% of the session"), e.from, e.pct);
        formula = Fmt(T("%.0f ÷ 1,13 = %.0f FPS", "%.0f ÷ 1.13 = %.0f FPS"), e.from, e.to);
        why = Fmt(T("Bajó un escalón del 13 %%, la diferencia perceptible, al cerrar la partida del %s.",
                    "Lowered one 13 %% step, the perceptible difference, when the %s session closed."), when.c_str());
      }
      break;
    case LimitExplain::Raise:
      rule = T("Iba sobrado: más del 98 % de los fotogramas esperando al limitador", "It had headroom: over 98 % of frames waiting on the limiter");
      formula = Fmt(T("%.0f × 1,13 = %.0f FPS", "%.0f × 1.13 = %.0f FPS"), e.from, e.to);
      why = Fmt(T("Subió un escalón del 13 %% al cerrar la partida del %s. Si no lo sostiene, volverá a %.0f.",
                  "Raised one 13 %% step when the %s session closed. If it can't hold it, it goes back to %.0f."), when.c_str(), e.from);
      break;
    case LimitExplain::Revert:
      rule = Fmt(T("La subida a %.0f no se sostuvo", "The raise to %.0f didn't hold"), e.from);
      formula = Fmt(T("vuelta a %.0f FPS", "back to %.0f FPS"), e.to);
      why = Fmt(T("Volvió al límite que sí sostenía, al cerrar la partida del %s.", "Went back to the limit it did hold, when the %s session closed."), when.c_str());
      break;
    case LimitExplain::Manual:
      rule = T("Límite puesto a mano", "Limit set by hand");
      formula = Fmt("%.0f FPS", e.to);
      why = T("La app no lo cambia: solo comprueba que funciona.", "The app doesn't change it: it only checks that it works.");
      break;
    case LimitExplain::Accepted:
      rule = T("Recomendación aplicada con su botón", "Recommendation applied with its button");
      formula = e.from > 0 ? Fmt("%.0f → %.0f FPS", e.from, e.to) : Fmt("%.0f FPS", e.to);
      why = Fmt(T("Aplicada el %s.", "Applied on %s."), when.c_str());
      break;
    default:
      rule = T("Límite actual", "Current limit");
      formula = Fmt("%.0f FPS", limitReal);
      why = T("Se decidió en una versión anterior: el detalle de su fórmula aparecerá en la próxima decisión.",
              "It was decided in an earlier version: the details of its formula will appear with the next decision.");
      break;
  }
  if (fg > 1)
    why += Fmt(T(" Con generación de fotogramas: %d × %.0f reales = %d FPS en pantalla. La app no lo sube ni lo baja por su cuenta.",
                 " With frame generation: %d × %.0f real = %d FPS on screen. The app doesn't raise or lower it on its own."),
               fg, limitReal, c.value);
  ImGui::TextColored(kDim, "%s", rule.c_str());
  ImGui::PushFont(fontHead);
  ImGui::TextUnformatted(formula.c_str());
  ImGui::PopFont();
  ImGui::TextColored(kDim, "%s", why.c_str());
  // 2. De donde sale: la curva de la partida que lo decidio.
  if (!e.curve.empty() && (e.kind == LimitExplain::WholeSession || e.kind == LimitExplain::FailMedian)) {
    VSpace(14);
    ImGui::PushFont(fontStrong);
    const bool bySeconds = e.curveSeconds;
    ImGui::TextUnformatted(bySeconds ? T("De dónde sale: cuántos segundos alcanzaron cada ritmo", "Where it comes from: how many seconds reached each pace")
                                     : T("De dónde sale: cuántos fotogramas alcanzaron cada ritmo", "Where it comes from: how many frames reached each pace"));
    ImGui::PopFont();
    ImGui::TextColored(kDim, "%s", bySeconds ? T("Porcentaje de los segundos de esa partida en que el juego llegó a cada ritmo o más. Donde la curva cruza el 90 % está el límite.",
                                                 "Share of that session's seconds in which the game reached each pace or more. The limit is where the curve crosses 90 %.")
                                   : e.fluid ? T("Porcentaje de los fotogramas de esa partida (sin tirones) que llegaron a cada ritmo o más. Donde la curva cruza el 90 % está el límite.",
                                                 "Share of that session's frames (without stutters) that reached each pace or more. The limit is where the curve crosses 90 %.")
                                             : T("Porcentaje de los fotogramas de esa partida (sin tirones) que llegaron a cada ritmo o más. La línea punteada es su 1 % más lento; el límite, 1,13 veces ese ritmo.",
                                                 "Share of that session's frames (without stutters) that reached each pace or more. The dotted line is their slowest 1 %; the limit, 1.13 times that pace."));
    if (bySeconds && e.kind != LimitExplain::FailMedian)
      ImGui::TextColored(kDim, "%s", T("Gráfica guardada con la 0.63, por segundos: la app decide con los fotogramas, así que el límite puede quedar un poco a la izquierda del cruce. Desde la próxima decisión será por fotogramas.",
                                       "Chart saved with 0.63, by seconds: the app decides with frames, so the limit can sit a little left of the crossing. From the next decision it will be by frames."));
    if (fg > 1)
      ImGui::TextColored(kDim, "%s", T("Con generación de fotogramas, la curva es de los fotogramas reales (los que calcula el juego), solo de los tramos con la generación más habitual: los menús, que muchos juegos muestran sin generación, quedan fuera.",
                                       "With frame generation, the curve is of the real frames (the ones the game computes), only from the stretches with the most common frame generation: menus, which many games show without it, are left out."));
    VSpace(6);
    DrawCurveChart(e, e.to > 0 ? e.to : limitReal, inner);
  }
  // 3. Comprobacion: la ultima partida.
  if (x.limit > 0 && x.fps.size() >= 10) {
    VSpace(14);
    ImGui::PushFont(fontStrong);
    ImGui::TextUnformatted(Fmt(T("Comprobación: la última partida (%s, %.0f min), con %.0f", "Check: the last session (%s, %.0f min), with %.0f"),
                               ShortDate(x.when).c_str(), x.minutes, x.limit).c_str());
    ImGui::PopFont();
    ImGui::TextColored(kDim, "%s", x.fg ? T("Ritmo real de cada segundo (gris), frente al umbral del 13 %. Con generación de fotogramas las reglas no ajustan el límite: la gráfica es solo informativa.",
                                            "Each second's real pace (gray), against the 13 % threshold. With frame generation the rules don't adjust the limit: the chart is for information only.")
                                       : T("Ritmo de cada segundo (gris) y 1 % más lento de los últimos 20 segundos (azul; rojo bajo el umbral), frente al umbral del 13 %. La línea azul muestra dónde están los tropiezos; la regla usa el 1 % más lento de toda la partida (abajo).",
                                           "Each second's pace (gray) and the slowest 1 % of the last 20 seconds (blue; red below the threshold), against the 13 % threshold. The blue line shows where the dips are; the rule uses the whole session's slowest 1 % (below)."));
    VSpace(6);
    DrawSessionChart(x, inner);
  }
  // 4. Las reglas.
  VSpace(14);
  ImGui::PushFont(fontStrong);
  ImGui::TextUnformatted(T("Reglas", "Rules"));
  ImGui::PopFont();
  VSpace(4);
  if (x.limit <= 0 && !c.manual && fg <= 1)
    RuleLine(-1, T("La comprobación aparecerá después de la próxima partida con este límite.",
                   "The check will appear after the next session with this limit."), inner);
  if (x.limit > 0) {
    RuleLine(x.verified ? 1 : 0, x.verified ? Fmt(T("Límite comprobado en la última partida: %.0f FPS (tolerancia ±1,5 %%).", "Limit checked in the last session: %.0f FPS (±1.5 %% tolerance)."), x.limit)
                                            : std::string(T("En la última partida no se pudo comprobar el límite.", "The limit couldn't be checked in the last session.")), inner);
    if (!x.fg) {
      const bool mFail = x.failSecs >= 60 && x.failPct >= 10;
      RuleLine(mFail ? 0 : 1, Fmt(T("Ritmo por debajo del umbral: %.0f s, el %.0f %% de la partida (falla con 60 s y el 10 %%).",
                                    "Pace below the threshold: %.0f s, %.0f %% of the session (it fails at 60 s and 10 %%)."), x.failSecs, x.failPct), inner);
      if (x.sessionLow > 0) {
        // 0.66: el 1 % mas lento de toda la partida, la medida de la regla
        if (c.usesFluid) {
          const bool sank = x.sessionLow < x.limit * 0.87;
          RuleLine(sank ? 0 : 1, Fmt(T("1 %% más lento de la partida: %.0f FPS (umbral: %.0f)%s.",
                                       "Session's slowest 1 %%: %.0f FPS (threshold: %.0f)%s."), x.sessionLow, x.limit * 0.87,
                                     sank ? T(": con «Más FPS» no baja el límite, pero si se repite dos partidas seguidas, el juego pasa a «Ritmo constante»",
                                              ": with “More FPS” it doesn't lower the limit, but if it repeats for two sessions in a row, the game moves to “Steady pace”")
                                          : ""), inner);
        } else {
          const double t = std::floor(1.13 * x.sessionLow + 1e-9);
          const bool holds = t >= x.limit * 0.95;
          RuleLine(holds ? 1 : 0, Fmt(T("1 %% más lento de la partida: %.0f FPS. 1,13 × %.0f = %.0f: %s (se sostiene si llega al 95 %% del límite).",
                                        "Session's slowest 1 %%: %.0f FPS. 1.13 × %.0f = %.0f: %s (it holds if it reaches 95 %% of the limit)."),
                                      x.sessionLow, x.sessionLow, t,
                                      holds ? T("el límite se sostiene", "the limit holds") : T("el límite no se sostiene", "the limit doesn't hold")),
                   inner);
        }
      } else {
        const bool tFail = x.tailSecs >= 60 && x.tailPct >= 10;
        RuleLine(tFail ? 0 : 1, Fmt(T("1 %% más lento bajo el umbral: %.0f s, el %.0f %% de la partida (regla hasta la 0.65).",
                                      "Slowest 1 %% below the threshold: %.0f s, %.0f %% of the session (rule up to 0.65)."),
                                    x.tailSecs, x.tailPct), inner);
      }
      if (x.waitPct >= 0)
        RuleLine(x.waitPct > 98 ? 1 : -1, x.waitPct > 98 ? Fmt(T("Va sobrado: el %.0f %% de los fotogramas esperó al limitador.", "It has headroom: %.0f %% of frames waited on the limiter."), x.waitPct)
                                                         : Fmt(T("No va sobrado: el %.0f %% de los fotogramas esperó al limitador (para subir hace falta más del 98 %%).",
                                                                 "No headroom: %.0f %% of frames waited on the limiter (raising needs more than 98 %%)."), x.waitPct), inner);
    }
  }
  if (c.manual) {
    RuleLine(-1, T("Límite manual: la app no lo sube ni lo baja.", "Manual limit: the app doesn't raise or lower it."), inner);
  } else if (fg > 1 || x.fg) {
    RuleLine(-1, T("Con generación de fotogramas, las reglas de ajuste no cambian este límite: los fotogramas reales cambian de ritmo con ella.",
                   "With frame generation, the adjustment rules don't change this limit: real frames change pace with it."), inner);
  } else if (!c.vrr) {
    RuleLine(-1, T("Sin G-SYNC, el límite usa divisores de los Hz y no se sube solo.", "Without G-SYNC, the limit uses divisors of the Hz and isn't raised on its own."), inner);
  } else {
    if (!c.usesFluid)
      RuleLine(-1, Fmt(T("Subida: %d de 2 partidas seguidas yendo sobrado.", "Raise: %d of 2 sessions in a row with headroom."), cl.plentyStreak), inner);
    const long long today = static_cast<long long>(std::time(nullptr) / 86400);
    if (cl.failedCeil > 0 && today - cl.failedDay < 30)
      RuleLine(-1, Fmt(T("%.0f FPS bloqueado hasta el %s: no se sostuvo.", "%.0f FPS blocked until %s: it didn't hold."), cl.failedCeil,
                       ShortDate((cl.failedDay + 30) * 86400).substr(0, ShortDate((cl.failedDay + 30) * 86400).find(',')).c_str()), inner);
  }
  ImGui::PopTextWrapPos();
  ImGui::EndGroup();
  const float bottom = ImGui::GetItemRectMax().y + 20 * scale;
  dl->ChannelsSetCurrent(0);
  dl->AddRectFilled(p0, ImVec2(p0.x + w, bottom), Col(kCard), 10 * scale);
  dl->AddLine(ImVec2(p0.x + 16 * scale, p0.y), ImVec2(p0.x + w - 16 * scale, p0.y), Col(kLine), 1.0f);
  dl->ChannelsMerge();
  ImGui::SetCursorScreenPos(ImVec2(p0.x, bottom + 8 * scale));
}

static void PageGames(const View& v) {
  VrrAlertCard(v);
  // Limitador: como se aplican los limites. Solo se muestra si hay algo que elegir (AMD o Intel, o RTSS).
  const int lk = g_limiterKind;
  const bool nvAvail = g_nvidiaAvailable, rtssAvail = g_rtssAvailable;
  if (!nvAvail && (rtssAvail || g_guidedVendor != 0)) {  // con NVIDIA todo es automatico: no hay nada que elegir
    Section(T("Limitador", "Limiter"));
    const int first = nvAvail ? 0 : 1;  // driver de NVIDIA, o "Solo recomendar" con AMD/Intel
    const char* firstName = nvAvail ? T("Driver de NVIDIA", "NVIDIA driver") : T("Solo recomendar", "Recommend only");
    const bool canFirst = nvAvail || g_guidedVendor != 0;
    if (canFirst) {
      const bool sel = lk == first;
      PrimaryButtonStyle(sel);
      if (ImGui::Button(firstName, ImVec2(OptionW(), 0)) && !sel) g_limiterReq = first;
      PrimaryButtonEnd(sel);
    }
    if (rtssAvail) {
      if (canFirst) ImGui::SameLine(0, 4 * scale);
      const bool sel = lk == 2;
      PrimaryButtonStyle(sel);
      if (ImGui::Button(T("RivaTuner (RTSS)", "RivaTuner (RTSS)"), ImVec2(OptionW(), 0)) && !sel) g_limiterReq = 2;
      PrimaryButtonEnd(sel);
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x * 0.85f);
    ImGui::TextColored(kDim, "%s",
        lk == 2 ? (nvAvail ? T("RivaTuner (RTSS): la app aplica cada límite en el perfil del juego en RTSS. RTSS tiene que "
                               "estar abierto mientras juegas. Sin «GPU al máximo» ni la baja latencia del driver; con "
                               "G-SYNC, activa tú el VSync del driver.",
                               "RivaTuner (RTSS): the app applies each limit in the game's RTSS profile. RTSS has to be "
                               "running while you play. Without “GPU at max” or the driver's low latency; with G-SYNC, "
                               "turn on the driver's VSync yourself.")
                             : T("RivaTuner (RTSS): la app aplica cada límite en el perfil del juego en RTSS. RTSS tiene que "
                                 "estar abierto mientras juegas. Con FreeSync, activa también el VSync del driver.",
                                 "RivaTuner (RTSS): the app applies each limit in the game's RTSS profile. RTSS has to be "
                                 "running while you play. With FreeSync, also turn on the driver's VSync."))
        : lk == 1 ? (rtssAvail ? T("Solo recomendar: la app te indica cada límite en la fila del juego, lo aplicas tú y ella "
                                   "comprueba que funciona. Con RivaTuner (RTSS), la app los aplicaría sola.",
                                   "Recommend only: the app shows each limit on the game's row, you apply it and it checks "
                                   "that it works. With RivaTuner (RTSS), the app would apply them itself.")
                               : T("Solo recomendar: la app te indica cada límite en la fila del juego, lo aplicas tú y ella "
                                   "comprueba que funciona. Si instalas RivaTuner (RTSS), podrás elegirlo aquí para que la "
                                   "app los aplique sola.",
                                   "Recommend only: the app shows each limit on the game's row, you apply it and it checks "
                                   "that it works. If you install RivaTuner (RTSS), you'll be able to choose it here so the "
                                   "app applies them itself."))
                  : T("Driver de NVIDIA (recomendado): la app aplica cada límite en el perfil del juego del driver, con "
                      "«GPU al máximo» y la baja latencia.",
                      "NVIDIA driver (recommended): the app applies each limit in the game's driver profile, with "
                      "“GPU at max” and low latency."));
    if (rtssAvail && canFirst)  // solo si hay a que cambiar
      ImGui::TextColored(kDim, "%s", T("Al cambiar de limitador, los límites de tus juegos se mueven al nuevo.",
                                       "When you change the limiter, your games' limits move to the new one."));
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, 14 * scale));
  }
  Section(T("Prioridad", "Priority"));
  {
    const int pm = g_prioMode;
    const char* names[3] = {T("Automática", "Automatic"), T("Ritmo constante", "Steady pace"), T("Más FPS", "More FPS")};
    for (int i = 0; i < 3; ++i) {
      if (i > 0) ImGui::SameLine(0, 4 * scale);
      const bool sel = pm == i;
      PrimaryButtonStyle(sel);
      if (ImGui::Button(names[i], ImVec2(OptionW(), 0)) && !sel) {
        SetPrioMode(i);
        WriteKv(L"equipo.txt", "prioridad", i == 0 ? "auto" : i == 1 ? "estabilidad" : "fluidez");
      }
      PrimaryButtonEnd(sel);
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x * 0.85f);
    ImGui::TextColored(kDim, "%s",
        pm == 0 ? T("Automática (recomendada): la app ajusta cada juego por su cuenta. Empieza con «Más FPS», "
                    "pasa a «Ritmo constante» los juegos que resultan inestables, y sube o baja cada límite según "
                    "lo que el juego sostiene.",
                    "Automatic (recommended): the app adjusts each game on its own. It starts with “More FPS”, "
                    "moves games that turn out to be unstable to “Steady pace”, and raises or lowers each limit "
                    "based on what the game holds.")
        : pm == 1 ? T("Ritmo constante: un límite que el juego sostiene incluso en sus peores momentos (el ritmo "
                      "más regular, a veces con algunos FPS menos). La app no cambia nada por su cuenta: si un "
                      "juego va sobrado o no sostiene su límite, te lo recomienda en su fila.",
                      "Steady pace: a limit the game holds even in its worst moments (the steadiest pace, sometimes "
                      "with a few fewer FPS). The app doesn't change anything on its own: if a game has headroom or "
                      "can't hold its limit, it recommends it on its row.")
                  : T("Más FPS: el límite más alto que el juego alcanza la mayor parte del tiempo, aceptando caídas "
                      "breves en los momentos más exigentes. La app no cambia nada por su cuenta: si un juego va "
                      "sobrado, no sostiene su límite o le conviene «Ritmo constante», te lo recomienda en su fila.",
                      "More FPS: the highest limit the game reaches most of the time, accepting brief drops in the "
                      "most demanding moments. The app doesn't change anything on its own: if a game has headroom, "
                      "can't hold its limit or would do better with “Steady pace”, it recommends it on its row."));
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, 4 * scale));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x * 0.85f);
    if (lk == 1)
      ImGui::TextColored(kDim, "%s", Fmt(T("Con «Solo recomendar», la prioridad decide qué límite te recomiendo; lo aplicas tú en %s.",
                                           "With “Recommend only”, the priority decides which limit I recommend; you apply it in %s."),
                                         GuidedSoftware()).c_str());
    ImGui::TextColored(kDim, "%s", T("Cada juego puede tener la suya con el botón de prioridad de su fila. Al cambiar "
                                     "la prioridad, los límites ya aplicados no se recalculan: usa el botón «Deshacer» "
                                     "del juego.",
                                     "Each game can have its own with the priority button on its row. Changing the "
                                     "priority doesn't recalculate limits already applied: use the game's “Undo” "
                                     "button."));
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
      const float pad2 = ImGui::GetStyle().FramePadding.x * 2;
      const float bw = ImGui::CalcTextSize(T("Deshacer", "Undo")).x + pad2;
      const float mw = ImGui::CalcTextSize(T("Fijar…", "Set…")).x + pad2;
      // Boton de la prioridad del juego: ancho fijo (el de la etiqueta mas larga) para que no baile.
      // Ancho de la etiqueta mas larga de las cuatro posibles (en ingles, "Auto · Smoothness" es mas larga
      // que las de Stability; hasta la 0.53 la flecha tapaba su ultima letra).
      const float prw = std::max({ImGui::CalcTextSize(T("Auto · Ritmo constante", "Auto · Steady pace")).x,
                                  ImGui::CalcTextSize(T("Auto · Más FPS", "Auto · More FPS")).x}) + pad2 + 22 * scale;
      // Recomendacion pendiente (solo con prioridad elegida a mano): un boton para aplicarla.
      const bool recPrio = c.suggestFps <= 0 && c.prioSuggest;
      const std::string sugLabel =
          c.suggestFps > 0 ? Fmt(c.suggestKind == 0 ? T("Subir a %.0f FPS", "Raise to %.0f FPS") : T("Bajar a %.0f FPS", "Lower to %.0f FPS"),
                                 c.suggestFps)
          : recPrio ? std::string(T("Usar «Ritmo constante»", "Use “Steady pace”"))
                    : std::string();
      const float sw = !sugLabel.empty() ? ImGui::CalcTextSize(sugLabel.c_str()).x + pad2 : 0.0f;
      float wrapW = w - (bw + mw + prw + (sw > 0 ? sw + 8 * scale : 0.0f) + 92 * scale);
      float warnH = c.warning.empty() ? 0.0f
                    : ImGui::CalcTextSize(c.warning.c_str(), nullptr, false, wrapW).y + 6 * scale;
      // La descripcion tambien se ajusta al espacio que dejan los botones (con cuatro, una linea larga
      // pasaba por debajo de ellos).
      const float descH = ImGui::CalcTextSize(c.desc.c_str(), nullptr, false, wrapW).y;
      float h = line + descH + ImGui::GetStyle().ItemSpacing.y + warnH + 22 * scale;
      ImDrawList* dl = ImGui::GetWindowDrawList();
      dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), Col(kCard), 10 * scale);
      // Aviso de la fila: ambar; rojo si es urgente (un limite que, sin G-SYNC, empeora el juego).
      const bool urgent = c.suggestFps > 0 && c.suggestKind == 2;
      if (!c.warning.empty())
        dl->AddRectFilled(ImVec2(p.x, p.y + 10 * scale), ImVec2(p.x + 3 * scale, p.y + h - 10 * scale),
                          Col(urgent ? kRed : kAmber), 2 * scale);
      // Desplegable (0.63): la zona del nombre y la descripcion abre el porque del limite.
      const bool expanded = g_expandedGames.count(c.exe) > 0;
      ImGui::SetCursorScreenPos(p);
      ImGui::InvisibleButton("expand", ImVec2(std::max(1.0f, wrapW + 44 * scale), h));
      const bool rowHover = ImGui::IsItemHovered();
      if (ImGui::IsItemClicked()) {
        if (expanded) g_expandedGames.erase(c.exe);
        else g_expandedGames.insert(c.exe);
      }
      if (rowHover) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("%s", expanded ? T("Ocultar el porqué de este límite", "Hide why this game has its limit")
                                         : T("Ver de dónde sale este límite y cómo le fue en la última partida",
                                             "See where this limit comes from and how the last session went"));
      }
      {
        const float cx = p.x + 20 * scale, cy = p.y + 11 * scale + ImGui::GetTextLineHeight() * 0.5f + 2 * scale, r = 4.5f * scale;
        const ImU32 cc = Col(rowHover ? kText : kDim);
        if (expanded) {
          dl->AddLine(ImVec2(cx - r, cy - r * 0.5f), ImVec2(cx, cy + r * 0.5f), cc, 2.0f * scale);
          dl->AddLine(ImVec2(cx, cy + r * 0.5f), ImVec2(cx + r, cy - r * 0.5f), cc, 2.0f * scale);
        } else {
          dl->AddLine(ImVec2(cx - r * 0.5f, cy - r), ImVec2(cx + r * 0.5f, cy), cc, 2.0f * scale);
          dl->AddLine(ImVec2(cx + r * 0.5f, cy), ImVec2(cx - r * 0.5f, cy + r), cc, 2.0f * scale);
        }
      }
      ImGui::SetCursorScreenPos(ImVec2(p.x + 38 * scale, p.y + 11 * scale));
      ImGui::BeginGroup();
      ImGui::PushFont(fontStrong);
      ImGui::TextUnformatted(c.label.c_str());
      ImGui::PopFont();
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapW);
      ImGui::TextColored(kDim, "%s", c.desc.c_str());
      ImGui::PopTextWrapPos();
      if (!c.warning.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapW);
        ImGui::TextColored(urgent ? kRed : kAmber, "%s", c.warning.c_str());
        ImGui::PopTextWrapPos();
      }
      ImGui::EndGroup();
      const float by = p.y + (h - ImGui::GetFrameHeight()) * 0.5f;
      if (!sugLabel.empty()) {
        ImGui::SetCursorScreenPos(ImVec2(p.x + w - bw - mw - prw - sw - 38 * scale, by));
        PrimaryButtonStyle(true);
        if (ImGui::Button(sugLabel.c_str())) {
          if (recPrio) {
            AcceptPrioSuggestion(c.exe, c.label);
          } else {
            std::lock_guard<std::mutex> lock(g_manualLimitMutex);
            g_applySuggestionReq.push_back(c.exe);
          }
        }
        PrimaryButtonEnd(true);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("%s", recPrio ? T("Aplica «Ritmo constante» a este juego desde la próxima partida.",
                                              "Applies “Steady pace” to this game from the next session.")
                                  : c.suggestKind == 0
                                  ? T("Sube el límite al valor recomendado. Si el juego no lo sostiene,\n"
                                      "la app te recomendará volver al actual.",
                                      "Raises the limit to the recommended value. If the game can't hold it,\n"
                                      "the app will recommend going back to the current one.")
                                  : T("Baja el límite al valor recomendado, que el juego sí sostiene.",
                                      "Lowers the limit to the recommended value, which the game does hold."));
      }
      // Prioridad del juego: automatica (la global; la app puede pasarlo a Estabilidad) o fija.
      if (!c.manual) {
        ImGui::SetCursorScreenPos(ImVec2(p.x + w - bw - mw - prw - 30 * scale, by));
        const char* applied = c.usesFluid ? T("Más FPS", "More FPS") : T("Ritmo constante", "Steady pace");
        const std::string pl = c.prioMode == 0 && g_prioMode == 0 ? std::string("Auto · ") + applied : std::string(applied);
        // Texto alineado a la izquierda y un triangulo dibujado a la derecha (indica que despliega un menu;
        // el caracter del triangulo no esta en la fuente de la ventana).
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
        if (ImGui::Button((pl + "##prio").c_str(), ImVec2(prw, 0))) ImGui::OpenPopup("prio");
        ImGui::PopStyleVar();
        {
          const ImVec2 bmax = ImGui::GetItemRectMax(), bmin = ImGui::GetItemRectMin();
          const float cx = bmax.x - 12 * scale, cy = (bmin.y + bmax.y) * 0.5f, r = 3.5f * scale;
          ImGui::GetWindowDrawList()->AddTriangleFilled(ImVec2(cx - r, cy - r * 0.6f), ImVec2(cx + r, cy - r * 0.6f),
                                                        ImVec2(cx, cy + r * 0.7f), ImGui::GetColorU32(kDim));
        }
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("%s", c.prioMode != 0
              ? T("Prioridad elegida por ti para este juego: la app no cambia nada por su cuenta, solo te recomienda.",
                  "Priority you chose for this game: the app doesn't change anything on its own, it only recommends.")
              : g_prioMode != 0
              ? T("Usa el ajuste general de prioridad.", "Uses the general priority setting.")
              : c.autoStability
              ? T("Automática: la app pasó este juego a «Ritmo constante» porque es inestable con su límite.",
                  "Automatic: the app moved this game to “Steady pace” because it's unstable with its limit.")
              : T("Automática: empieza con «Más FPS»; la app lo pasa a «Ritmo constante» si es inestable.",
                  "Automatic: starts with “More FPS”; the app moves it to “Steady pace” if it's unstable."));
        if (ImGui::BeginPopup("prio")) {
          int choice = -1;
          if (ImGui::Selectable(g_prioMode == 0 ? T("Automática", "Automatic") : T("Como el ajuste general", "Same as the general setting"),
                                c.prioMode == 0))
            choice = 0;
          if (ImGui::IsItemHovered() && g_prioMode == 0)
            ImGui::SetTooltip("%s", T("Empieza con «Más FPS»; si el juego resulta inestable con su límite,\n"
                                      "la app lo pasa a «Ritmo constante».",
                                      "Starts with “More FPS”; if the game turns out to be unstable with its\n"
                                      "limit, the app moves it to “Steady pace”."));
          if (ImGui::Selectable(T("Siempre ritmo constante", "Always steady pace"), c.prioMode == 2)) choice = 2;
          if (ImGui::Selectable(T("Siempre más FPS", "Always more FPS"), c.prioMode == 1)) choice = 1;
          ImGui::Separator();
          ImGui::TextColored(kDim, "%s", T("Se aplica desde la próxima partida.", "Applies from the next session."));
          ImGui::EndPopup();
          if (choice >= 0 && choice != c.prioMode) {
            WriteKv(L"equipo.txt", "prioridad-fija|" + c.exe, choice == 1 ? "fluidez" : choice == 2 ? "estabilidad" : "");
            WriteKv(L"equipo.txt", "prioridad-sugerida|" + c.exe, "");
            int mode = 0;
            bool autoSt = false;
            const bool fluidNow = GameUsesFluid(c.exe, &mode, &autoSt);
            std::lock_guard<std::mutex> lock(g_ui.m);
            if (g_ui.prioSuggest.erase(c.exe) && !g_ui.suggestions.count(c.exe)) g_ui.warnings.erase(c.exe);
            for (auto& u : g_ui.changes)
              if (u.exe == c.exe) {
                u.prioMode = mode;
                u.usesFluid = fluidNow;
                u.autoStability = autoSt;
                u.prioSuggest = false;
              }
            g_ui.notice = Fmt(T("%s: prioridad %s desde la próxima partida.", "%s: %s priority from the next session."),
                              c.label.c_str(),
                              choice == 0 ? (g_prioMode == 0 ? T("automática", "automatic") : T("la general", "the general one"))
                                          : choice == 1 ? T("«Más FPS»", "“More FPS”") : T("«Ritmo constante»", "“Steady pace”"));
          }
        }
      }
      ImGui::SetCursorScreenPos(ImVec2(p.x + w - bw - mw - 22 * scale, by));
      if (ImGui::Button(T("Fijar…", "Set…"))) RequestManual(c.exe, c.label, c.value, c.fg);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Pon tú el límite de este juego. La app no lo cambiará.",
                                  "Set this game's limit yourself. The app won't change it."));
      ImGui::SetCursorScreenPos(ImVec2(p.x + w - bw - 14 * scale, by));
      // Deshacer se destaca con un aviso que pide revisar el limite; con una recomendacion, el boton
      // importante es el de la recomendacion.
      const bool undoHi = !c.warning.empty() && c.suggestFps <= 0 && !c.prioSuggest && !c.guidedPending;
      PrimaryButtonStyle(undoHi);
      if (ImGui::Button(T("Deshacer", "Undo"))) {
        std::lock_guard<std::mutex> lock(g_undoOneMutex);
        g_undoOne.push_back(c.exe);
      }
      PrimaryButtonEnd(undoHi);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Devuelve este juego a como estaba. Si «Aplicar límites» está activado,\n"
                                  "la app lo recalculará la próxima vez que juegues.",
                                  "Returns this game to how it was. If “Apply limits” is on,\n"
                                  "the app will recalculate it the next time you play."));
      ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + 8 * scale));
      if (expanded) {
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h - 4 * scale));
        DrawLimitExplain(c, w);
      }
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
  // Interlineado de texto, no el espacio entre elementos de la interfaz (dejaba el registro a doble espacio).
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 3 * scale));
  for (const auto& line : v.log) ImGui::TextColored(kDim, "%s", line.c_str());
  if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
  ImGui::PopStyleVar();
  ImGui::EndChild();
  ImGui::PopStyleColor();
}

static void Draw() {
  View v = Snapshot(page == 3);
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
  if (g_scrollTop) {
    ImGui::SetScrollY(0.0f);
    g_scrollTop = false;
  }
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
    // Prioridad global. Hasta la 0.53, "fluidez" significaba en realidad "automatica" (empezar con mas FPS
    // y pasar a ritmo constante los juegos inestables), salvo con prioridad-auto=no, que la dejaba fija;
    // se convierte una sola vez. Sin prioridad guardada (instalacion nueva): automatica.
    {
      std::string pr = ReadKv(L"equipo.txt", "prioridad");
      const bool autoOff = ReadKv(L"equipo.txt", "prioridad-auto") == "no";
      if (pr.empty() || (pr == "fluidez" && !autoOff && ReadKv(L"equipo.txt", "prioridad-v3") != "1")) pr = "auto";
      if (ReadKv(L"equipo.txt", "prioridad-v3") != "1") {
        WriteKv(L"equipo.txt", "prioridad", pr);
        WriteKv(L"equipo.txt", "prioridad-auto", "");
        WriteKv(L"equipo.txt", "prioridad-v3", "1");
      }
      SetPrioMode(pr == "estabilidad" ? 1 : pr == "fluidez" ? 2 : 0);
    }
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
    NvDriver nvDriver;
    NvidiaBackend nvBackend(nvDriver);
    RtssBackend rtssBackend;
    const bool rtssOk = rtssBackend.Init();
    bool ok = nvDriver.Init() || rtssOk;
    BackendSelector nvd(&nvBackend);
    if (rtssOk) nvd.SetRtss(&rtssBackend);
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

  if (ReadKv(L"equipo.txt", "overlay") == "clasico") g_overlayClassic = true;
  overlay::Window ov;
  ov.Create(gui::scale, gui::dev, gui::ctx);
  eco::ExemptCurrentThread();  // ventana y overlay
  std::thread engine(RunEngine, presentMonPath);

  bool running = true;
  double lastUiDraw = 0.0;
  double lastInput = NowSec();
  // Temporizador de alta precision (Windows 10 1803 o posterior; si no, uno normal): el normal despierta
  // cada 15,6 ms y dejaba el overlay a ~18 dibujos por segundo, irregulares.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
  HANDLE waitTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!waitTimer) waitTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
  eco::mainThread = eco::OpenCurrentThread();
  while (running) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
      // Raton, teclado, cambios de tamano o de foco: hay que redibujar enseguida.
      if ((msg.message >= WM_MOUSEFIRST && msg.message <= WM_MOUSELAST) ||
          (msg.message >= WM_KEYFIRST && msg.message <= WM_KEYLAST) || msg.message == WM_MOUSELEAVE ||
          msg.message == WM_NCMOUSEMOVE)
        lastInput = NowSec();
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
      if (msg.message == WM_QUIT) running = false;
    }
    if (!running) break;
    ov.Update();
    ImGui::SetCurrentContext(mainCtx);
    // La ventana se dibuja solo cuando hace falta (hasta la 0.63: unas 60 veces por segundo en primer
    // plano y 10 detras, aunque no cambiara nada ni se viera):
    //  - justo despues de mover el raton o pulsar algo (medio segundo a ~60 fps, para que los botones
    //    respondan y se vean los cambios);
    //  - en Ahora con un juego en marcha, a ~30 fps (la grafica de ritmo avanza);
    //  - si no, una vez por segundo, para recoger lo que publica el motor (que lo hace una vez por
    //    segundo);
    //  - detras de un juego a pantalla completa que la tapa, cada 2 segundos.
    // El overlay se sigue dibujando a su ritmo aunque la ventana este minimizada o tapada.
    const bool mineFront = GetForegroundWindow() == hwnd;
    const double tNow = NowSec();
    {
      static bool wasFront = false;
      if (mineFront && !wasFront) lastInput = tNow;  // al volver a la ventana, redibujar enseguida
      wasFront = mineFront;
    }
    if (gui::resizeW != 0 && gui::resizeH != 0) lastInput = tNow;  // cambio de tamano: redibujar ya
    bool haveGame = false;
    {
      std::lock_guard<std::mutex> lock(g_ui.m);
      haveGame = g_ui.haveGame;
    }
    double interval;
    if (mineFront && tNow - lastInput < 0.5) interval = 1.0 / 60.0;
    else if (gui::page == 0 && haveGame) interval = mineFront ? 1.0 / 30.0 : 0.1;
    else interval = 1.0;
    if (!mineFront) {
      HWND fg = GetForegroundWindow();
      if (fg && fg != hwnd && WindowCoversMonitor(fg) &&
          MonitorFromWindow(fg, MONITOR_DEFAULTTONULL) == MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL))
        interval = 2.0;
    }
    if (IsIconic(hwnd) || tNow - lastUiDraw < interval) {
      // Esperar a la siguiente cosa que hacer: un mensaje (raton, teclado...), el overlay o el siguiente
      // dibujo de la ventana. Antes se despertaba cada 4 ms con el overlay visible.
      double wait = IsIconic(hwnd) ? 0.25 : interval - (tNow - lastUiDraw);
      if (ov.Visible()) wait = std::min(wait, std::max(0.0, ov.NextDrawAt() - NowSec()));
      wait = std::clamp(wait, 0.0005, 0.25);
      if (waitTimer) {
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(wait * 1e7);  // relativo, en unidades de 100 ns
        SetWaitableTimer(waitTimer, &due, 0, nullptr, nullptr, FALSE);
        MsgWaitForMultipleObjects(1, &waitTimer, FALSE, static_cast<DWORD>(wait * 1000.0) + 20, QS_ALLINPUT);
      } else {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>(wait * 1000.0) + 1, QS_ALLINPUT);
      }
      continue;
    }
    lastUiDraw = tNow;
    const double winT0 = NowSec();
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
    // Sin la espera de la sincronizacion vertical (Present con 1 espera al refresco de la pantalla).
    eco::windowDrawMsSum = eco::windowDrawMsSum.load() + (NowSec() - winT0) * 1000.0;
    ++eco::windowDraws;
    gui::swap->Present(mineFront ? 1 : 0, 0);
  }

  if (waitTimer) CloseHandle(waitTimer);
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
