// FlatPace - datos de un monitor
// Logica pura, sin dependencias de Windows: se incluye desde main.cpp (en el mismo punto donde
// estaba) y desde las pruebas automaticas (tests/). Requiere las cabeceras estandar ya incluidas.
#pragma once

struct DisplayPathInfo {
  std::wstring gdiName;
  std::wstring friendlyName;
  std::wstring edidName;     // nombre segun el EDID del monitor
  int rangeMin = 0, rangeMax = 0;      // rango de frecuencias declarado (EDID)
  int freeSyncMin = 0, freeSyncMax = 0;  // rango FreeSync declarado por AMD (EDID), si existe
  double nominalHz = 0.0;
  double signalHz = 0.0;  // frecuencia exacta de la senal de video, si Windows la da
};
