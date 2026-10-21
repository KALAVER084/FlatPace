// FlatPace - un fotograma medido por PresentMon
// Logica pura, sin dependencias de Windows: se incluye desde main.cpp (en el mismo punto donde
// estaba) y desde las pruebas automaticas (tests/). Requiere las cabeceras estandar ya incluidas (y DWORD, que en Windows da windows.h).
#pragma once

struct FrameSample {
  double t = 0.0;            // momento de llegada (reloj QPC de esta app)
  double presentMs = 0.0;    // tiempo desde la presentacion anterior (real o generada)
  double realMs = -1.0;      // tiempo desde el fotograma REAL anterior (-1 si es generado)
  // Lo mismo, medido entre presentaciones (el punto que regula el limitador). Con marcadores de
  // Reflex, realMs usa el inicio de la simulacion, que varia mas; la grafica usa este valor.
  double presentRealMs = -1.0;
  bool generated = false;    // fotograma creado por generacion de fotogramas
  double cpuBusyMs = -1.0;
  double gpuBusyMs = -1.0;
  std::string swapchain;      // "pid:direccion" (unico entre procesos)
  DWORD pid = 0;
  std::string app;            // nombre del .exe en minusculas
  std::string presentMode;    // como lo muestra Windows (flip directo o compuesto)
  double displayMs = -1.0;    // tiempo entre cambios reales de imagen en pantalla
  // Error de animacion (PresentMon): diferencia entre el ritmo de la simulacion del juego y el
  // de la pantalla. NAN si no esta disponible.
  double animErrMs = std::numeric_limits<double>::quiet_NaN();
  // Latencia del PC (PresentMon, MsPCLatency): desde que el juego lee la entrada hasta que la imagen
  // sale hacia la pantalla. Solo en juegos con marcadores de Reflex/PC Latency; NAN si no esta disponible.
  double pcLatencyMs = std::numeric_limits<double>::quiet_NaN();
};
