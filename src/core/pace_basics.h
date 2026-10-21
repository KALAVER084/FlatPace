// FlatPace - grafica de ritmo: punto y tolerancia (dentro de namespace overlay)
// Logica pura, sin dependencias de Windows: se incluye desde main.cpp (en el mismo punto donde
// estaba) y desde las pruebas automaticas (tests/). Requiere las cabeceras estandar ya incluidas.
#pragma once

// Grafica de ritmo. Los fotogramas se agrupan en tramos fijos en el tiempo (no en columnas de
// pantalla), asi cada tramo conserva su valor mientras la grafica se desplaza y no "tiembla".
// Cada tramo vale la mediana de sus fotogramas, o su maximo si contiene un pico real (mas de un
// 50 % por encima de la referencia), para que los tirones formen parte de la curva.
struct PacePoint {
  float x;      // 0 = borde izquierdo, 1 = borde derecho
  float ms;     // mediana del tramo, o su maximo si contiene un tiron
  bool spike;   // el tramo contiene un tiron (mas de un 50 % por encima de la referencia)
};

// Tolerancia de la grafica: +-13 % del tiempo de fotograma (la diferencia perceptible, ley de
// Weber, el mismo umbral que usan las decisiones), con un minimo de 0,5 ms. Proporcional porque el
// ritmo se percibe en proporcion. Hasta la 0.39.1 tenia ademas un maximo de 2 ms, que a FPS bajos la
// hacia mas estricta que el propio criterio (a 36 fps, un 7 % en vez de un 13 %), y mostraba como
// picos variaciones imperceptibles. El maximo de 6 ms solo actua por debajo de ~22 fps.
static double PaceTolerance(double ref) { return std::clamp(0.13 * ref, 0.5, 6.0); }
