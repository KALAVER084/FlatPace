// FlatPace - parametros (cfg)
// Logica pura, sin dependencias de Windows: se incluye desde main.cpp (en el mismo punto donde
// estaba) y desde las pruebas automaticas (tests/). Requiere las cabeceras estandar ya incluidas.
#pragma once

// ---------------------------------------------------------------------------
// Parametros (todo lo "inteligente" se ajusta aqui)
// ---------------------------------------------------------------------------
namespace cfg {
constexpr double kRefreshMeasureSec = 5.0;  // medicion de Hz reales
constexpr double kQuickMinSec = 8.0;        // fase rapida: minimo de tiempo...
constexpr size_t kQuickMinFrames = 400;     // ...y de fotogramas
constexpr double kQuickMaxSec = 20.0;       // ...o como maximo esto
constexpr double kWindowSec = 20.0;         // ventana movil del monitoreo
constexpr double kSpikeMs = 250.0;          // mas = carga, alt-tab o pausa
constexpr double kMarginInitial = 0.05;     // margen sobre el percentil 99
constexpr double kMarginRaise = 0.15;       // subir exige mas margen
constexpr double kDownTolerance = 1.03;     // tolerancia antes de bajar
constexpr int kDownChecks = 3;              // segundos seguidos para bajar
constexpr int kUpChecks = 30;               // segundos seguidos para subir
constexpr double kMinCapFps = 30.0;
constexpr int kMaxDivisor = 8;
}  // namespace cfg

namespace cfg {
constexpr double kVrrMinFps = 48.0;        // por debajo, el VRR no suele actuar
constexpr double kHitchFactor = 2.5;       // tiron: > 2.5 veces la mediana...
constexpr double kHitchMinExtraMs = 8.0;   // ...y al menos 8 ms mas que lo normal
constexpr double kRegimeShortSec = 3.0;    // ventana corta para detectar cambios
constexpr double kRegimeRatio = 1.4;       // cambio brusco: +/- 40 % en la mediana
constexpr int kRegimeChecks = 3;           // segundos seguidos para confirmarlo
constexpr double kFgOnFraction = 0.05;     // > 5 % generados = generacion activa
constexpr double kFgMinBaseFps = 40.0;     // base real minima razonable para generar
}  // namespace cfg

namespace cfg {
constexpr double kGapSec = 3.0;              // sin fotogramas: pausa (minimizado, carga)
constexpr double kSettleSec = 3.0;           // espera tras una pausa o un cambio brusco
constexpr double kRateChangeFraction = 0.3;  // > 30 % lentos en 1 s = cambio de ritmo
constexpr int kCapChangeChecks = 5;          // segundos para confirmar un limite nuevo
}  // namespace cfg

namespace cfg {
constexpr int kDetectMinPresents = 20;  // fotogramas por segundo minimos
constexpr int kDetectConfirmSec = 3;    // segundos seguidos para confirmar
constexpr double kJnd = 0.13;  // diferencia perceptible (13-15 % segun la ley de Weber)
constexpr double kVerifySec = 15.0;        // espera para ver el limite funcionando
}  // namespace cfg
