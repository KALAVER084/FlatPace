// FlatPace - lectura del EDID (nombre y rangos de frecuencia)
// Logica pura, sin dependencias de Windows: se incluye desde main.cpp (en el mismo punto donde
// estaba) y desde las pruebas automaticas (tests/). Requiere las cabeceras estandar ya incluidas y display_info.h.
#pragma once

// Nombre del monitor y rangos de frecuencia segun el EDID (descriptor de limites de rango
// y, si existe, el bloque de AMD con el rango FreeSync).
static void ParseEdid(const std::vector<uint8_t>& e, DisplayPathInfo& info) {
  if (e.size() < 128) return;
  for (int d = 54; d <= 108; d += 18) {
    const uint8_t* b = &e[d];
    if (b[0] != 0 || b[1] != 0 || b[2] != 0) continue;
    if (b[3] == 0xFC) {  // nombre
      std::wstring n;
      for (int i = 5; i < 18 && b[i] != 0x0A; ++i) n += static_cast<wchar_t>(b[i]);
      while (!n.empty() && n.back() == L' ') n.pop_back();
      info.edidName = n;
    } else if (b[3] == 0xFD) {  // limites de rango
      info.rangeMin = b[5] + ((b[4] & 0x01) ? 255 : 0);
      info.rangeMax = b[6] + ((b[4] & 0x02) ? 255 : 0);
    }
  }
  if (info.edidName.empty()) {  // fabricante + producto
    uint16_t m = static_cast<uint16_t>((e[8] << 8) | e[9]);
    wchar_t id[4] = {static_cast<wchar_t>(L'A' - 1 + ((m >> 10) & 31)),
                     static_cast<wchar_t>(L'A' - 1 + ((m >> 5) & 31)),
                     static_cast<wchar_t>(L'A' - 1 + (m & 31)), 0};
    wchar_t buf[32];
    swprintf(buf, 32, L"%ls %04X", id, static_cast<unsigned>(e[10] | (e[11] << 8)));
    info.edidName = buf;
  }
  // Extensiones CTA-861: bloque de AMD (OUI 00-00-1A) con el rango FreeSync
  for (size_t ext = 128; ext + 128 <= e.size(); ext += 128) {
    const uint8_t* x = &e[ext];
    if (x[0] != 0x02) continue;
    size_t end = std::min<size_t>(x[2], 127), i = 4;
    while (i < end) {
      int tag = x[i] >> 5, len = x[i] & 31;
      if (tag == 3 && len >= 7 && i + 7 < 128 && x[i + 1] == 0x1A && x[i + 2] == 0x00 && x[i + 3] == 0x00) {
        int mn = x[i + 6], mx = x[i + 7];
        if (mn >= 20 && mx > mn && mx <= 500) {
          info.freeSyncMin = mn;
          info.freeSyncMax = mx;
        }
      }
      i += 1 + len;
    }
  }
}
