# Documentación técnica de FlatPace 0.41.1

Este documento explica cómo funciona FlatPace por dentro: arquitectura, algoritmos, fórmulas, formatos de datos y las fuentes en que se basan sus decisiones.

## 1. Arquitectura

| Componente | Hilo | Función |
|---|---|---|
| Ventana (Dear ImGui 1.91.5 + Direct3D 11) | Principal | Interfaz, ~60 FPS en primer plano y ~10 en segundo plano o minimizada |

**Prioridad:** desde la 0.41, el proceso de FlatPace tiene prioridad baja (`BELOW_NORMAL_PRIORITY_CLASS`) y modo de eficiencia de Windows 11 (EcoQoS), para no competir con el juego. Excepciones: el hilo lector de PresentMon (sin modo de eficiencia, prioridad equivalente a la normal) y el hilo de la ventana y el overlay (sin modo de eficiencia, prioridad baja). PresentMon se lanza con prioridad normal. El resumen de cada partida indica el coste de FlatPace (motor, proceso completo, PresentMon y overlay).
| Motor | Propio | Detección del juego, estadísticas, decisiones, driver; una vuelta por segundo |
| Lector de PresentMon | Propio | Lanza PresentMon 2.6.0 como proceso hijo (`--output_stdout`) y analiza su CSV línea a línea |
| Overlay | Principal | Ventana propia, transparente, siempre encima y que el ratón atraviesa (dibujo propio con suavizado + `UpdateLayeredWindow`), hasta 60 actualizaciones por segundo |

Los hilos se comunican con estructuras protegidas por mutex (estado de la ventana, datos del overlay, peticiones de deshacer). El motor nunca bloquea la interfaz.

**Pila:** C++20, Win32, Dear ImGui, Direct3D 11, NVAPI (cargada en tiempo de ejecución desde `nvapi64.dll`), PresentMon. Código en dos partes: `main.cpp` (~5.200 líneas: Windows, interfaz, motor, driver y overlay) y `src/core/` (~660 líneas: la lógica pura, sin dependencias de Windows, en varios módulos: parámetros, estadísticas, análisis y recomendaciones, ciclo de decisión (`decision.h`), modelos estadísticos, EDID y gráfica de ritmo). `main.cpp` incluye cada módulo en el mismo punto donde estaba su código. Se compila con MinGW-w64 (compilación cruzada) o con CMake + MSVC.

## 2. Medición

### 2.1 Frecuencia real de la pantalla
`IDXGIOutput::WaitForVBlank` durante 5 s. Se calcula la frecuencia media y la dispersión de los intervalos; una medición con refrescos perdidos o dispersión alta se repite hasta 3 veces. Si ninguna es válida, se usa la última buena guardada en `pantalla.txt`.

### 2.2 Fotogramas reales y generados
- **Tipo de fotograma** (`FrameType` de PresentMon): `Application` es real; el resto, generado.
- **Marcadores de simulación** (`MsBetweenSimulationStart`, de NVIDIA Reflex / PC Latency): si un juego los emite durante 60 fotogramas seguidos, se confía en ellos; un fotograma sin marcador es generado y el intervalo real es el de simulación.
- Sin marcadores, el **intervalo real** suma las presentaciones desde el último fotograma real. El trabajo de GPU de los generados se suma al real que los acompaña.
- **Factor de generación** *k* = presentaciones / fotogramas reales, redondeado entre 2 y 4 si más del 5 % son generados. Un cambio de estado de la generación solo cuenta si dura 3 s seguidos.

### 2.3 Estadísticas
Ventana móvil de 20 s. Mediana, percentiles 5, 95 y 99, trabajo de CPU y GPU.
- **Trabajo por fotograma** = min(max(CPU ocupada, GPU ocupada), intervalo real). El recorte corrige Reflex y los limitadores internos, que cuentan sus esperas como CPU ocupada.
- **Tirón:** fotograma real de más de 2,5 veces la mediana y al menos 8 ms más largo. Por encima de 250 ms se considera pausa (carga, menú) y no cuenta.
- **Error de animación:** columna `MsAnimationError` de PresentMon (diferencia entre el ritmo de la simulación y el de la pantalla); media de su valor absoluto, porcentaje de fotogramas por encima de 1 ms y máximo. Solo está disponible en los juegos en que PresentMon puede calcularlo.
- **Dato de la gráfica:** el intervalo entre presentaciones de cada fotograma real (suma de `MsBetweenPresents` desde el real anterior), el punto que regula el limitador; los marcadores de simulación de Reflex se usan para el análisis, no para la gráfica.
- **Gráfica de desviaciones:** los tramos con |mediana − referencia| ≤ tolerancia se dibujan sobre la referencia; tolerancia = ±13 % del tiempo de fotograma, entre 0,5 y 6 ms (misma regla con y sin límite; hasta la 0.39.1 el máximo era 2 ms, más estricto que el 13 % por debajo de ~65 FPS). Con generación de fotogramas (vista en la partida o con la que se aplicó el límite), la gráfica dibuja el intervalo en pantalla de cada imagen (`MsBetweenDisplayChange`), con la referencia del límite en pantalla.
- **Línea de tiempo de la gráfica:** cada fotograma recibe su instante al llegar (el anterior + su duración) y no cambia. Un reloj de reproducción avanza a velocidad real hacia "último dato, extrapolado al reloj actual, menos el búfer" (corrección entre 0,8× y 1,25×; frena con suavidad a menos de 0,4 s del último dato). El búfer sigue al mayor intervalo reciente entre tandas de PresentMon (+0,35 s; baja 0,05 s por segundo). Cada tramo de 3 píxeles (fijo en el tiempo) vale la mediana de sus fotogramas, o su máximo si contiene un tirón, y se clasifica una sola vez al completarse. Sin límite, la referencia es la mediana de los últimos 10 s, suavizada en ~1,5 s. Escala vertical de objetivo − r a objetivo + 1,6 r, con r = máx(2 ms, 50 % del objetivo).
- **Variación del ritmo:** desviación media de las medianas de ventanas consecutivas de 100 ms respecto a su mediana; mide las oscilaciones lentas, que son las que corrige un límite. La de la primera partida sin límite de cada juego se guarda en `equipo.txt` (`ritmo-antes|juego.exe`) como referencia.
- **Consistencia:** media de |Δt| entre fotogramas reales consecutivos y proporción de cambios menores de 2 ms (la misma métrica de variación consecutiva de CapFrameX). Se calcula también sobre todo lo presentado (incluidos los generados) y se acumula para el resumen de la partida.
- **Cambio de régimen:** si la mediana cambia bruscamente (ajustes, zona, generación), la medición se reinicia tras 3 s de estabilización.

### 2.4 Detector de límites
Hay un límite activo si la GPU no está ocupada todo el tiempo (menos del 90 % del intervalo), los fotogramas casi nunca son más rápidos que la mediana (p5 ≥ 0,9 × mediana, o 0,8 si la cifra coincide al 0,5 % con un valor típico) y la mediana coincide con un divisor de los Hz (1 %) o con un límite habitual (1,5 %).

## 3. Recomendación

Sea *P* = 1000 / ritmo real objetivo (ms) y *k* el factor de generación.

### 3.1 Sin VRR: divisores
Candidatos: ritmo en pantalla = Hz / *n* (*n* = 1…8·*k*), ritmo real = ritmo en pantalla / *k* ≥ 30.
- **Estabilidad:** el mayor candidato con *P* ≥ trabajo p99 × 1,05.
- **Ajustado:** si ninguno cumple, el mayor en el que como mucho el 2 % de los fotogramas supera *P* (sin límite, el ritmo sería irregular, que es peor).
- **Fluidez:** el mayor candidato que cumple, con los tiempos reales de los últimos 3 minutos: p70 ≤ *P* × 1,08 y menos del 3 % por encima de *P* × 1,5.
- **Con un límite activo** (del juego o de la app), el trabajo de la GPU engaña (baja frecuencias): si el juego cumple su ritmo con como mucho el 2 % de fotogramas por encima de *P* × 1,25, ese límite es sostenible y no se recomienda menos.

### 3.2 Con VRR (G-SYNC, FreeSync, Adaptive-Sync)
- **Techo:** `floor(min(Hz − 3, Hz − Hz²/4096))`; si el rango VRR del monitor acaba por debajo de su frecuencia, se aplica la misma fórmula a ese máximo. A frecuencias altas manda la fórmula (240 Hz → 225; 144 Hz → 138); a bajas, el margen clásico de 3 FPS (60 Hz → 57).
- Sobre los tiempos de fotograma reales **sin tirones** (> 2,5 × mediana) de los últimos minutos (o de toda la partida al decidir al cerrar): **Fluidez:** ritmo real = 1000 / p90; **Estabilidad:** (1 + 0,13) × 1000 / p99, es decir, el 1 % más lento llega como mucho un 13 % tarde. Validado en 0.39 con partidas reales: en Greylock la regla anterior de Fluidez (p70) dejaba el juego por debajo del límite de forma perceptible un 29 % de los segundos, y la de Estabilidad (1000 / (p99 × 1,05) con tirones) daba 78-85 FPS; con las nuevas, 100 y 91, y con 100 el 1 % bajo causado por el juego fue de 96 FPS.
- **Con un límite activo** (datos censurados), la recomendación nunca supera el límite, usa los tiempos causados por el juego y solo baja si el juego queda por debajo de forma perceptible (> 13 %); sin esa histéresis, el temblor del limitador lo bajaba 1-2 FPS por partida. Con un ritmo demostrado (el juego clavado por su propio límite o VSync), se usa exactamente ese ritmo.
- Ritmo en pantalla = min(techo, ritmo real × *k*), redondeado hacia abajo a un múltiplo entero de *k*.
- **Ritmo demostrado:** si el juego sostiene un límite (con pocos fotogramas tardíos), la recomendación nunca baja de ese ritmo, y si el límite es el máximo de la pantalla, se usa el techo. Esto evita el sesgo de medir con un límite puesto (ningún fotograma puede ir más rápido que el límite, pero algunos sí más lentos).

### 3.3 Frecuencia de pantalla alternativa (sin VRR)
Si el monitor ofrece otras frecuencias en la resolución actual y alguna permite al menos un 10 % más de FPS estables, se planea para la próxima partida (cambio temporal con `ChangeDisplaySettingsEx` y `CDS_FULLSCREEN`, restaurado al cerrar el juego, la app o al deshacer).

## 4. Decisión, verificación y aceptación

1. **Decisión al cerrar el juego** con toda la partida: tiempos reales del estado de generación en el que más se jugó, VRR si actuó más de la mitad del tiempo, ritmo demostrado si el juego estuvo limitado por sí mismo la mayor parte del tiempo. "Aplicar ya" decide al momento con los datos disponibles.
2. **Verificación** en la siguiente partida: el ritmo real coincide con el esperado (±1,5 %) y menos del 5 % de los fotogramas es más rápido que el límite; con generación, reales ±2 % y en pantalla ±4 %.
3. **Aceptación:** un límite verificado queda aceptado (se guarda) y ya no se recalcula.
4. **Límites manuales** (puestos por el usuario): se aplican igual (con VRR, límite + VSync del driver) y se verifican, pero no pasan por la decisión al cerrar ni por la regla de fallos; solo se informa si el juego no los sostuvo.
5. **Regla de fallos** (al cerrar cada partida con límite aceptado): se baja solo si el juego fue más de un 13 % por debajo, en tramos de al menos 5 s, durante al menos el 10 % del tiempo y 60 s en total. Con VRR, al ritmo superado el 90 % del tiempo (percentil 10 de los ritmos por segundo); sin VRR, al siguiente divisor. Nunca se sube automáticamente.

## 5. Aplicación en el driver de NVIDIA (NVAPI)

- Sesión DRS → perfil del juego (el existente o uno propio llamado `FlatPace - juego.exe`).
- Ajustes usados:
  - `FRL_FPS` (0x10835002): limitador de FPS (enteros).
  - `VSYNCMODE` (0x00A879CF): `FORCEON` con VRR; intervalos 1/2, 1/3 o 1/4 del refresco sin VRR cuando la presentación es directa y el equipo lo admite.
  - `PRERENDERLIMIT` (0x007BA09E) = 1, junto con el VSync por intervalos.
  - `PREFERRED_PSTATE` (0x1057EB71) = 1, *Preferir rendimiento máximo*, con cada límite: evita que el driver baje las frecuencias de la GPU hasta ir justa (en Greylock con 90 FPS, el trabajo por fotograma casi se duplicaba con el mismo 85 % de uso, y las escenas pesadas se pasaban del límite). Al abrir la app se añade a los cambios que no lo tenían. Desactivable con `rendimiento-maximo=no`.
- Antes de cambiar un ajuste se guarda su valor original; **deshacer** lo restaura o borra el perfil propio.
- **Aprendizajes por equipo:** si el driver impone su VSync (comprobado al reabrir el juego) y si, con generación de fotogramas, el limitador actúa sobre lo que se ve en pantalla o sobre los fotogramas reales (comprobado con la generación estable al menos 8 s).

## 6. Detección de VRR

- **NVIDIA:** `NvAPI_Disp_GetVRRInfo` cada 2 s; cambia de estado tras 3 lecturas iguales.
- **Cualquier marca:** tiempos de PresentMon. Para cada intervalo *x* entre presentaciones y entre cambios en pantalla (`MsBetweenDisplayChange`) se calcula su distancia al múltiplo del refresco más cercano. Si más del 30 % de las presentaciones no son múltiplos y la pantalla las muestra igual (más del 30 % tampoco), hay VRR; si la pantalla las redondea (menos del 10 %), no lo hay. Si el juego ya va a un múltiplo exacto, no se decide.
- **Rango VRR usado:** el rango FreeSync del EDID si existe; si no, el rango de frecuencias. Por debajo del mínimo, si el monitor no tiene LFC (máximo < 2 × mínimo), la recomendación con VRR se descarta y se usa un divisor.
- **EDID** (registro de Windows, a partir de la ruta del monitor de `QueryDisplayConfig`): nombre, rango de frecuencias (descriptor 0xFD) y, si existe, rango FreeSync del bloque de AMD (OUI 00-00-1A).

## 7. Archivos

| Archivo | Ubicación | Contenido |
|---|---|---|
| `cambios-aplicados.txt` | `%LOCALAPPDATA%\FlatPace` | Un juego por línea (ver formato) |
| `equipo.txt` | `%LOCALAPPDATA%\FlatPace` | `idioma`, `prioridad`, `overlay`, `divisores-siempre` (ajuste avanzado), nombres de juegos (`nombre|exe`), variación sin límite de cada juego (`ritmo-antes|exe`), aprendizajes del VSync del driver y del limitador con generación, pantalla pendiente de restaurar |
| `pantalla.txt` | `%LOCALAPPDATA%\FlatPace` | Última frecuencia real medida por monitor y modo |
| `registro-AAAAMMDD-HHMMSS.txt` | Carpeta del programa | Registro completo de cada ejecución |
| `grabaciones/grabacion-juego.exe-AAAAMMDD-HHMMSS.csv` | Carpeta del programa | Opcional: un fotograma por fila (ver formato) |

**Formato de `cambios-aplicados.txt` (v2), separado por tabuladores:**
`v2  exe  creado(0/1)  método(0 límite / 1 VSync)  divisor  fps  n  [id_hex  tenía(0/1)  original_hex]×n` seguido de campos opcionales: `fg k frl`, `hz frecuencia`, `ok 1` (aceptado), `base fps_reales`, `vrr 1`, `manual 1` (puesto por el usuario: la app no lo cambia).

**Formato de las grabaciones (CSV):** líneas de cabecera que empiezan por `#` (versión, juego, frecuencia real, rango VRR, prioridad y límite al empezar) y una fila por fotograma del juego con: `t_llegada_s` (cuándo llegó el dato, por tandas), `t_acumulado_s` (suma de presentaciones), `presentacion_ms`, `real_ms` (con marcadores de Reflex, intervalo de simulación), `real_presentacion_ms`, `generado` (0/1), `cpu_ms`, `gpu_ms`, `pantalla_ms`, `error_animacion_ms`, `vrr` y `limite_confirmado` (estado en ese momento). Los valores no disponibles quedan vacíos.

## 8. Ajustes avanzados

Sin interfaz, en `%LOCALAPPDATA%\FlatPace\equipo.txt` (una línea `clave=valor`), con la app cerrada:

| Clave | Efecto |
|---|---|
| `rendimiento-maximo=no` | No acompañar los límites de *Preferir rendimiento máximo* (los cambios ya aplicados lo pierden al reaplicarse o al deshacerlos). |
| `divisores-siempre=si` | Usa siempre divisores de los Hz, aunque G-SYNC/FreeSync actúe. Para pantallas que parpadean con VRR (algunas OLED y VA). La sección *Equipo* lo indica en la fila *VRR*. |

Los límites ya aplicados no cambian: para recalcular un juego con el ajuste nuevo, usa *Deshacer*.

## 9. Modelos estadísticos (0.37)

Por ahora informan y sugieren; su efecto en las decisiones se activará cuando se validen con grabaciones reales.

- **Con generación de fotogramas** (vista en la partida o con la que se aplicó el límite), no se calculan la capacidad, el 1 % bajo causado por el juego, la memoria ni las sugerencias: suponen que el límite rige siempre sobre los fotogramas reales, y con generación intermitente no es así.
- **Datos censurados** (`src/core/censored.h`): con un límite de periodo *c*, se mide la variación del limitador en cada partida (desviación absoluta mediana × 1,4826 de los fotogramas entre 0,85 *c* y 1,15 *c*), y los fotogramas ≤ *c* + máx(3 % de *c*, 3 × variación) se tratan como censurados ("tardarían como mucho *c*"); los demás, como medidas. Se ajusta una log-normal por máxima verosimilitud (Nelder-Mead), sin pausas (> 250 ms) ni tirones (> 2,5 × mediana). Requiere ≥ 200 fotogramas, entre un 5 % y un 98 % de censura y ≥ 30 fotogramas visibles; con > 98 % el juego "va sobrado". El p99 es fiable; la mediana, solo con ≤ 90 % de censura. **Validación con partidas reales (0.37.1):** con el limitador del driver, los tiempos quedan repartidos de forma simétrica alrededor del límite (±0,2 ms en Megabonk, ±0,4 ms en Greylock); con el umbral anterior (+3 % fijo), Megabonk con 195 FPS parecía capaz de ~205 FPS de mediana cuando daba ~300; ahora sale "sobrado", y en Greylock con 90 FPS el p99 estimado (74 FPS) coincide con el medido (76).
- **1 % bajo causado por el juego** (`GameCausedPercentileMs`, en `src/core/analysis.h`): con un límite activo de periodo *c*, un fotograma más lento que *c* cuenta como tal solo si su trabajo (máx. de CPU y GPU ocupadas) supera 0,95 × *c*; si no, se cuenta como *c*, porque el retraso fue del temporizador del limitador. Validado con partidas reales: Megabonk con 195 FPS, 176 medidos → 195 del juego; Greylock con 90, 76 → 83. Se usa para el 1 % bajo de la ventana, el overlay y el registro.
- **Memoria por juego** (`src/core/game_memory.h`): media y varianza ponderadas (Welford) del logaritmo del ritmo que el juego sostiene en cada partida (el criterio de Estabilidad: 1,13 × 1000 / p99 sin tirones; con límite, solo si el 1 % más lento causado por el juego queda más de un 13 % por debajo del límite, como protección contra la espiral descendente), con peso = minutos jugados (máximo 10) y olvido ×0,8 por partida. Incertidumbre = √(varianza entre partidas + 0,08² / partidas); límite inferior al 80 % = exp(media − 0,84 × incertidumbre). Se guarda en `equipo.txt` (`memoria3|juego.exe`).
- **Fases** (`src/core/bocpd.h`): BOCPD con prior normal-gamma (κ₀ = 0,02, α₀ = 2, desviación típica a priori 0,03 en escala logarítmica), riesgo de cambio 1/60 por segundo y rachas truncadas a 300 s, sobre el logaritmo de la mediana de cada segundo filtrado con una mediana de 3 s. Se propone un cambio cuando la racha más probable vuelve a ≤ 3 s durante dos pasos tras haber sido ≥ 8 s; la fase se confirma si, tras 10 s, su mediana difiere ≥ 8 % de la anterior.
- **Sugerencia** (`src/core/suggest.h`): siguiente escalón = con VRR, mín(límite × 1,13, techo); sin VRR, el divisor siguiente. Se sugiere si el juego va sobrado, o si la capacidad estimada llega al escalón y, con dos o más partidas en memoria, su límite inferior llega al 95 % del escalón.

## 10. Fundamentos y fuentes

- **Divisores del refresco:** sin VRR, cada imagen ocupa un número entero de refrescos; solo los divisores dan un ritmo uniforme. Google aplica la misma regla en el Game Mode de Android.
- **Variación frente a media:** la media de FPS por sí sola no predice la calidad percibida cuando hay variación en los tiempos de fotograma (WPI, CHI 2023).
- **Umbral perceptible del 13 %:** la percepción de la frecuencia es aproximadamente logarítmica (ley de Weber); se necesita un cambio del 13-15 % para notarlo la mitad de las veces (estudio de JND de frecuencia en VR y escritorio, 2025).
- **Techo con VRR:** regla de 3 FPS de Blur Busters (G-SYNC 101) y fórmula Hz − Hz²/4096, inspirada en Special K y coincidente con el límite automático de Reflex.
- **Medición:** PresentMon (Intel) y la metodología de marcadores de Reflex de NVIDIA.

## 11. Limitaciones conocidas y líneas futuras

- **Sin cambio en vivo:** el límite del driver se lee al abrir el juego. Un limitador propio dentro del juego (DLL) lo permitiría, con riesgo en juegos con anticheat.
- **AMD:** detección y recomendación sí; aplicación pendiente (ADLX: FRTC o Radeon Chill).
- **Validar los modelos estadísticos con grabaciones reales** y, después, dejar que influyan en las decisiones (subir límites con la capacidad estimada, decidir con la memoria del juego y con la fase más exigente).
- Validación en más equipos, monitores y juegos.

## 12. Compilación

MinGW-w64 (desde Linux):
```
x86_64-w64-mingw32-windres app.rc -O coff -o app.res
x86_64-w64-mingw32-g++-posix -std=c++20 -municode -mwindows -O2 -DNDEBUG -Ithird_party/imgui \
  -Ithird_party/imgui/backends main.cpp imgui*.o app.res -o FlatPace.exe \
  -ld3d11 -ldxgi -ld3dcompiler_47 -ldwmapi -lgdi32 -lshell32 -luser32 -ladvapi32 -lversion \
  -limm32 -lwinmm -static -s
```
Windows: `cmake -B build && cmake --build build --config Release`. Dear ImGui (con los backends Win32 y DX11) va en `third_party/imgui`.

## 13. Pruebas automáticas

`tests/test_core.cpp` prueba la lógica de `src/core` sin necesitar Windows (159 comprobaciones): percentiles y variación del ritmo, consistencia, detector de límites, recomendaciones con y sin VRR (techo, rango VRR del monitor y LFC, Fluidez), lectura del EDID, gráfica de ritmo (tolerancia, escala, tirones y estabilidad de los tramos al desplazarse) los modelos estadísticos (recuperación de la capacidad con datos censurados, detección de fases y su resistencia a picos aislados, memoria por juego y reglas de sugerencia) y el ciclo de decisión (verificación, regla de fallos, bajada tras un fallo, decisión al cerrar y simulaciones de varias partidas seguidas que comprueban que no hay espiral descendente). Cada prueba fija un comportamiento ya validado con partidas reales o en simulación.

- Linux/macOS: `sh tests/run_tests.sh`
- Windows (MSVC): `tests\run_tests.bat`, o `ctest` tras compilar con CMake.

Se comprobó que detectan errores introducidos a propósito, parecidos a regresiones reales del proyecto (tolerancia de la gráfica, techo con VRR y clasificación de tramos).
