# Documentación técnica de FlatPace 0.66.1

Este documento explica cómo funciona FlatPace por dentro: arquitectura, algoritmos, fórmulas, formatos de datos y las fuentes en que se basan sus decisiones.

## 1. Arquitectura

| Componente | Hilo | Función |
|---|---|---|
| Ventana (Dear ImGui 1.91.5 + Direct3D 11) | Principal | Interfaz. Desde la 0.64 se dibuja solo cuando hace falta: ~60 FPS durante medio segundo tras usar el ratón o el teclado, ~30 en *Ahora* con un juego en marcha, una vez por segundo el resto del tiempo (el motor publica una vez por segundo) y cada 2 s detrás de un juego a pantalla completa; entre dibujos espera mensajes (`MsgWaitForMultipleObjects`) en lugar de despertar cada pocos milisegundos. Hasta la 0.63: ~60 FPS en primer plano y ~10 detrás |
| Motor | Propio | Detección del juego, estadísticas, decisiones, driver; una vuelta por segundo. Lee también el rendimiento de la CPU (contador de Windows *% Processor Performance*) y vigila que PresentMon siga entregando datos |
| Lector de PresentMon | Propio | Lanza PresentMon 2.6.0 como proceso hijo (`--output_stdout`) y analiza su CSV línea a línea |
| Overlay | Principal | Ventana propia, transparente, siempre encima y que el ratón atraviesa (dibujo propio con suavizado + `UpdateLayeredWindow`), 60 actualizaciones por segundo desde la 0.65, espaciadas con un temporizador de alta precisión de Windows (en la 0.64, 30 en teoría y ~18 irregulares en la práctica, por el temporizador normal de 15,6 ms; 64 antes); las cifras se recalculan 4 veces por segundo y la gráfica solo calcula los tramos nuevos; fondos fijos y textos guardados entre fotogramas, mezcla de píxeles con enteros. Desde la 0.44 lo dibuja la GPU: Dear ImGui (contexto propio, mismo dispositivo de Direct3D 11) en una cadena de imágenes para composición (`CreateSwapChainForComposition`, alfa premultiplicado) mostrada con DirectComposition en una ventana `WS_EX_NOREDIRECTIONBITMAP` por capas y transparente a los clics; si falla, vuelve al dibujo con el procesador (`overlay=clasico` lo fuerza) |

**Prioridad:** desde la 0.41, el proceso de FlatPace tiene prioridad baja (`BELOW_NORMAL_PRIORITY_CLASS`) y modo de eficiencia de Windows 11 (EcoQoS), para no competir con el juego. Excepciones: el hilo lector de PresentMon (sin modo de eficiencia, prioridad baja; la tubería con PresentMon tiene 1 MB de búfer para que no se bloquee) y el hilo de la ventana y el overlay (sin modo de eficiencia, prioridad baja). PresentMon se lanza también con prioridad baja y modo de eficiencia, y sin seguimiento de entrada (`--no_track_input`), que FlatPace no usa. El resumen de cada partida indica el coste de FlatPace (motor, proceso completo, PresentMon y overlay).

Los hilos se comunican con estructuras protegidas por mutex (estado de la ventana, datos del overlay, peticiones de deshacer). El motor nunca bloquea la interfaz.

**Pila:** C++20, Win32, Dear ImGui, Direct3D 11, NVAPI (cargada en tiempo de ejecución desde `nvapi64.dll`), PresentMon. Código en dos partes: `main.cpp` (~6.700 líneas: Windows, interfaz, motor, driver y overlay) y `src/core/` (~1.500 líneas: la lógica pura, sin dependencias de Windows, en varios módulos: parámetros, estadísticas, análisis y recomendaciones, ciclo de decisión (`decision.h`), modelos estadísticos, EDID y gráfica de ritmo). `main.cpp` incluye cada módulo en el mismo punto donde estaba su código. Se compila con MinGW-w64 (compilación cruzada) o con CMake + MSVC. El límite se pide al driver de forma neutra (`LimitRequest`, en `src/core/limit_request.h`) y cada fabricante lo traduce a su driver a través de una interfaz común (`LimitBackend` en `main.cpp`); hay tres: la de NVIDIA (automática, con NVAPI); la de RivaTuner (`RtssBackend`, desde la 0.57: con la biblioteca `RTSSHooks64.dll` de RTSS, carga el perfil `juego.exe`, cambia `FramerateLimit`, lo guarda y pide a RTSS que recargue los perfiles; para deshacer, restaura el límite que tenía el perfil o borra el que creó), que se usa si el usuario la elige como limitador en *Juegos*, solo con AMD o Intel (desde la 0.59; nunca sin elegirla); y la guiada (`GuidedBackend`), que se usa sin NVIDIA con una gráfica AMD o Intel: registra el límite sin tocar ningún driver, la ventana muestra al usuario qué poner y dónde, y la comprobación habitual (midiendo el juego) confirma que funciona. Los mensajes del registro se escriben en español en el código y se traducen al escribirse, según el idioma de la app, con la tabla de `src/core/log_text.h`.

## 2. Medición

### 2.1 Frecuencia real de la pantalla
`IDXGIOutput::WaitForVBlank` durante 5 s. Se calcula la frecuencia media y la dispersión de los intervalos; una medición con refrescos perdidos o dispersión alta se repite hasta 3 veces. Si ninguna es válida, se usa la última buena guardada en `pantalla.txt`.

### 2.2 Fotogramas reales y generados
- **Tipo de fotograma** (`FrameType` de PresentMon): `Application` es real; el resto, generado.
- **Marcadores de simulación** (`MsBetweenSimulationStart`, de NVIDIA Reflex / PC Latency): si un juego los emite durante 60 fotogramas seguidos, se confía en ellos; un fotograma sin marcador es generado y el intervalo real es el de simulación. Desde la 0.51.1, si llegan más de 4 fotogramas seguidos sin marcador (con generación nunca hay más de 3 generados seguidos), se deja de confiar en ellos hasta que vuelvan: algunos juegos (Space Marine 2) dejan de emitirlos al desactivar la generación, y también en menús y cargas.
- Sin marcadores, el **intervalo real** suma las presentaciones desde el último fotograma real. El trabajo de GPU de los generados se suma al real que los acompaña.
- **Factor de generación** *k* = presentaciones / fotogramas reales, redondeado entre 2 y 4 si más del 5 % son generados. Un cambio de estado de la generación solo cuenta si dura 3 s seguidos.

### 2.3 Estadísticas
Ventana móvil de 20 s. Mediana, percentiles 5, 95 y 99, trabajo de CPU y GPU.
- **Trabajo por fotograma** = min(max(CPU ocupada, GPU ocupada), intervalo real). El recorte corrige Reflex y los limitadores internos, que cuentan sus esperas como CPU ocupada.
- **Tirón:** fotograma real de más de 2,5 veces la mediana y al menos 8 ms más largo. Por encima de 250 ms se considera pausa (carga, menú) y no cuenta.
- **Error de animación:** columna `MsAnimationError` de PresentMon (diferencia entre el ritmo de la simulación y el de la pantalla); media de su valor absoluto, porcentaje de fotogramas por encima de 1 ms (comparado con el de la primera partida sin límite y sin generación de fotogramas, guardado como `movimiento-antes|juego.exe`) y máximo. Solo está disponible en los juegos en que PresentMon puede calcularlo.
- **Movimiento con ritmo plano:** porcentaje de fotogramas reales que duran lo mismo que el anterior (±0,3 ms), es decir, perfectos en la gráfica, cuyo error de animación supera el 13 % del fotograma. Mide un descompás del movimiento que la gráfica no puede mostrar. En las grabaciones: Greylock y Megabonk con límite, ~2 %; Greylock sin límite, 16 %; Space Marine 2 con generación de fotogramas, 17-37 %, y sin ella, 0,5 % en la misma partida. Con generación y un 10 % o más, el resumen lo atribuye a la generación (no al límite). Solo informa: no interviene en ninguna decisión, porque en los juegos sin generación sube y baja junto al 1 % bajo, que ya se usa.
- **Dato de la gráfica:** el intervalo entre presentaciones de cada fotograma real (suma de `MsBetweenPresents` desde el real anterior), el punto que regula el limitador; los marcadores de simulación de Reflex se usan para el análisis, no para la gráfica.
- **Gráfica de desviaciones:** los tramos con |mediana − referencia| ≤ tolerancia se dibujan sobre la referencia; tolerancia = ±13 % del tiempo de fotograma, entre 0,5 y 6 ms (misma regla con y sin límite; hasta la 0.39.1 el máximo era 2 ms, más estricto que el 13 % por debajo de ~65 FPS). Con generación de fotogramas (vista en la partida o con la que se aplicó el límite), la gráfica dibuja el intervalo en pantalla de cada imagen (`MsBetweenDisplayChange`), con la referencia del límite en pantalla. En ese modo, cada tramo vale la duración media de las imágenes en ~125 ms (5 tramos) y solo es tirón una imagen de más de 2,5× (con fotogramas reales: mediana del tramo y tirón a 1,5×).
- **Línea de tiempo de la gráfica:** cada fotograma recibe su instante al llegar (el anterior + su duración) y no cambia. Un reloj de reproducción avanza a velocidad real hacia "último dato, extrapolado al reloj actual, menos el búfer" (corrección suave, entre 0,95× y 1,05×; frena a menos de 0,25 s del último dato). El búfer sigue al mayor intervalo reciente entre tandas de PresentMon (+0,6 s; baja 0,05 s por segundo). Hasta la 0.42.1, con +0,35 s y una corrección de 0,8-1,25×, el reloj frenaba antes de cada tanda y aceleraba después: el avance por dibujo variaba entre 12,4 y 20,8 ms (ahora, entre 15,8 y 17,5). Cada tramo de 3 píxeles (fijo en el tiempo) vale la mediana de sus fotogramas, o su máximo si contiene un tirón, y se clasifica una sola vez al completarse, fijando también su valor; solo se dibujan los tramos completos. Sin límite, la referencia es la mediana de los últimos 10 s, suavizada en ~1,5 s. Escala vertical de objetivo − r a objetivo + 1,6 r, con r = máx(2 ms, 50 % del objetivo).
- **Variación del ritmo:** desviación media de las medianas de ventanas consecutivas de 100 ms respecto a su mediana; mide las oscilaciones lentas, que son las que corrige un límite. La de la primera partida sin ningún límite de cada juego se guarda en `equipo.txt` (`ritmo-antes2|juego.exe`) como referencia.
- **Consistencia:** media de |Δt| entre fotogramas reales consecutivos y proporción de cambios menores de 2 ms (la misma métrica de variación consecutiva de CapFrameX). Se calcula también sobre todo lo presentado (incluidos los generados) y se acumula para el resumen de la partida.
- **Cambio de régimen:** si la mediana cambia bruscamente (ajustes, zona, generación), la medición se reinicia tras 3 s de estabilización.

### 2.4 Detector de límites
Hay un límite activo si la GPU no está ocupada todo el tiempo (menos del 90 % del intervalo), los fotogramas casi nunca son más rápidos que la mediana (p5 ≥ 0,9 × mediana, o 0,8 si la cifra coincide al 0,5 % con un valor típico) y la mediana coincide con un divisor de los Hz (1 %) o con un límite habitual (1,5 %).

### 2.5 Diagnóstico del resumen de cada partida
- **Momentos lentos:** fotogramas más de un 13 % por encima del tiempo del límite (sin límite, el 10 % más lento). Se clasifican como limitados por la CPU si su trabajo de CPU supera en un 10 % al de GPU, y al revés. Con un 70 % o más de una de las dos, el resumen da el consejo correspondiente (CPU: bajar los gráficos no ayudará; GPU: bajar resolución o gráficos sí). En las grabaciones del portátil de pruebas, la CPU limita los momentos lentos de Greylock, Megabonk y Space Marine 2 (82-100 %); en Control Resonant, ambas.
- **Rendimiento de la CPU:** contador de Windows *% Processor Performance* (100 = frecuencia nominal), una vez por segundo: mediana, mínimo, y en los segundos lentos frente al resto. Si la CPU limita y baja de frecuencia más de un 10 % justo en los segundos lentos, se avisa de un posible problema de calor o energía.
- **Espera en la cola de la GPU** (`MsGPULatency`): con el límite funcionando, mediana y 1 % peor.
- **Latencia del PC** (`MsPCLatency`): mediana y 1 % peor, con el límite y sin él. PresentMon la calcula con los eventos *PC Latency Stats* del SDK de Reflex; aunque PresentMon ofrece la columna, en los juegos probados ha llegado siempre vacía (en Space Marine 2, que emite los marcadores de simulación, también).

## 3. Recomendación

Sea *P* = 1000 / ritmo real objetivo (ms) y *k* el factor de generación.

### 3.1 Sin VRR: divisores
Candidatos: ritmo en pantalla = Hz / *n* (*n* = 1…8·*k*), ritmo real = ritmo en pantalla / *k* ≥ 30.
- **Ritmo constante:** el mayor candidato con *P* ≥ trabajo p99 × 1,05.
- **Ajustado:** si ninguno cumple, el mayor en el que como mucho el 2 % de los fotogramas supera *P* (sin límite, el ritmo sería irregular, que es peor).
- **Más FPS:** el mayor candidato que cumple, con los tiempos reales de los últimos 3 minutos: p70 ≤ *P* × 1,08 y menos del 3 % por encima de *P* × 1,5.
- **Con un límite activo** (del juego o de la app), el trabajo de la GPU engaña (baja frecuencias): si el juego cumple su ritmo con como mucho el 2 % de fotogramas por encima de *P* × 1,25, ese límite es sostenible y no se recomienda menos.

### 3.2 Con VRR (G-SYNC, FreeSync, Adaptive-Sync)
- **Techo:** `floor(min(Hz − 3, Hz − Hz²/4096))`; si el rango VRR del monitor acaba por debajo de su frecuencia, se aplica la misma fórmula a ese máximo. A frecuencias altas manda la fórmula (240 Hz → 225; 144 Hz → 138); a bajas, el margen clásico de 3 FPS (60 Hz → 57).
- Sobre los tiempos de fotograma reales **sin tirones** (> 2,5 × mediana) de los últimos minutos (o de toda la partida al decidir al cerrar): **Más FPS:** ritmo real = 1000 / p90; **Ritmo constante:** (1 + 0,13) × 1000 / p99, es decir, el 1 % más lento llega como mucho un 13 % tarde. Validado en 0.39 con partidas reales: en Greylock la regla anterior de Más FPS (p70) dejaba el juego por debajo del límite de forma perceptible un 29 % de los segundos, y la de Ritmo constante (1000 / (p99 × 1,05) con tirones) daba 78-85 FPS; con las nuevas, 100 y 91, y con 100 el 1 % bajo causado por el juego fue de 96 FPS.
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
5. **Regla de fallos** (al cerrar cada partida con límite aceptado, sin contar el cierre del juego: desde la 0.65, los últimos 15 s y, antes de ellos, el tramo final por debajo del 70 % del límite (hasta la 0.64, el último minuto entero), que suele ser de menús, salida o cuelgues): se baja solo si el juego fue más de un 13 % por debajo, en tramos de al menos 5 s, durante al menos el 10 % del tiempo y 60 s en total. Con VRR, al ritmo superado el 90 % del tiempo (percentil 10 de los ritmos por segundo); sin VRR, al siguiente divisor. La regla de fallos solo baja; las subidas se describen en el punto siguiente. **Con *Ritmo constante*** (desde la 0.66; `SessionGameLowFps` y `CheckSteady` en `src/core/decision.h`), además, se aplica su fórmula a la partida: el 1 % más lento causado por el juego en toda la partida (sin el cierre ni los tirones, como al decidir). Si 1,13 veces ese ritmo no llega al 95 % del límite, baja a lo que dice la fórmula, como mucho un escalón del 13 % por partida. Hasta la 0.65 se contaban los segundos en que el 1 % más lento de los últimos 20 s quedaba un 13 % por debajo: la ventana exageraba los tropiezos cortos (en Greylock con 107, 36 s reales bajo el umbral se convertían en 79) y creaba una espiral (129 → 114 → 121 → 107 → 94) que contradecía a la propia fórmula (109 con esa partida). Validado con las 26 partidas de Greylock con límite (`tools/replay_steady.cpp`): las subidas malas a 125-130 se siguen detectando y los límites de 107-114 se sostienen.
6. **Subida automática** (`DecideClimb` en `src/core/decision.h`; desde la 0.45, con VRR y sin generación de fotogramas, límites no manuales): al cerrar, si el juego fue sobrado **toda la partida** (≥ 3 min con el límite funcionando, sin tramos por debajo, ≤ 1 % de segundos sueltos más de un 13 % por debajo, y > 98 % de los fotogramas de la partida en el límite según el modelo de datos censurados), el límite sube al siguiente escalón: mín(límite × 1,13, techo VRR). Con *Más FPS* basta una partida; con *Ritmo constante*, dos seguidas. En estos límites, con prioridad *Ritmo constante*, la regla de fallos se aplica también al 1 % bajo causado por el juego (ventana de 20 s; con *Más FPS* solo cuenta la mediana, desde la 0.48.2), porque una subida puede mantener la mediana y hundir solo las zonas pesadas (Greylock a 125: 34-48 % del tiempo más de un 13 % por debajo; a 111, 2,5 %); si solo falla el 1 % bajo y no hay límite anterior guardado, se baja un escalón (límite / 1,13), y "va sobrado" exige que no se haya hundido. Protecciones contra el ping-pong: si una subida falla, se vuelve al límite anterior en lugar de al ritmo superado el 90 % del tiempo; el límite que falló se guarda como techo 30 días y no se sube hasta él. Estado por juego en `equipo.txt` (`escalada|juego.exe`: techo;día;anterior;racha); *Deshacer* lo borra. Validado con las grabaciones: Megabonk a 195 (sobrado, sube a 220) y Greylock a 83, 90 y 100 (sin subida: 97-98 % en el límite). Desde la 0.55, la app solo cambia los límites y la prioridad por su cuenta con la prioridad general *Automática* y en los juegos sin prioridad propia (`AppActsAlone`); si no, cada subida, bajada (incluida la regla de fallos) o cambio a *Ritmo constante* se guarda como **recomendación** y se muestra en la fila del juego con un botón para aplicarla. Una recomendación de bajar es urgente (en rojo) si el límite usa el VSync por intervalos: sin G-SYNC, un límite que no se sostiene cae al escalón siguiente y empeora el movimiento. Se retira sola si en una partida posterior el juego sostiene su límite. La primera decisión de un juego nuevo se aplica en todos los modos, porque es para lo que está *Aplicar límites*.
7. **Prioridad por juego** (`UpdateAutoPriority` en `src/core/decision.h`; desde la 0.49, en los mismos límites que la subida automática): la prioridad global es el punto de partida; con *Más FPS*, si el 1 % bajo causado por el juego en toda la partida queda más de un 13 % por debajo del límite (`SessionSank`, desde la 0.66; antes, la regla de fallos aplicada al 1 % bajo de cada 20 s) en dos partidas seguidas, el juego pasa a *Ritmo constante*; una partida limpia de al menos 3 minutos corta la racha. Solo en esa dirección (con un límite que se sostiene, el juego parece estable por el propio límite). Se mide con el límite puesto: sin límite la inestabilidad se exagera. Estado en `equipo.txt` (`prioridad|juego.exe`: elegida;racha); *Deshacer* lo borra; con la prioridad general fija (*Ritmo constante* o *Más FPS*, desde la 0.54) no actúa. El usuario puede fijar la prioridad de cada juego desde su fila (`prioridad-fija|juego.exe`: fluidez o estabilidad); una prioridad fija tiene precedencia y la app no la cambia.

## 5. Aplicación en el driver de NVIDIA (NVAPI)

- Sesión DRS → perfil del juego (el existente o uno propio llamado `FlatPace - juego.exe`).
- Ajustes usados:
  - `FRL_FPS` (0x10835002): limitador de FPS (enteros).
  - `VSYNCMODE` (0x00A879CF): `FORCEON` con VRR; intervalos 1/2, 1/3 o 1/4 del refresco sin VRR cuando la presentación es directa y el equipo lo admite.
  - `PREFERRED_PSTATE` (0x1057EB71) = 1, *Preferir rendimiento máximo*, con cada límite: evita que el driver baje las frecuencias de la GPU hasta ir justa (en Greylock con 90 FPS, el trabajo por fotograma casi se duplicaba con el mismo 85 % de uso, y las escenas pesadas se pasaban del límite). Al abrir la app se añade a los cambios que no lo tenían. Desactivable con `rendimiento-maximo=no`. En portátiles, CPU y GPU comparten energía: en Greylock, con él la CPU rinde un 10 % menos (123-129 % de su frecuencia nominal frente a 136-138 %), pero sin él la GPU se relaja y, con las zonas bien separadas, el 1 % bajo empeora un 9 % en las zonas ligeras y mejora solo un 5 % en las pesadas (limitadas por la CPU). Además, una GPU relajada falsea las mediciones con las que se decide el límite (la espiral de las primeras versiones). Por eso se mantiene siempre; decidirlo por juego se estudió y se descartó.
  - `PRERENDERLIMIT` (0x007BA09E) = 1, *Modo de baja latencia: Activado* (cola de render de un fotograma), con cada límite desde la 0.50 (antes solo con el VSync por intervalos, del que forma parte): cuando el juego no llega al límite y la GPU se satura, los fotogramas no se acumulan en la cola. En una simulación con una GPU saturada, la latencia del PC en esas zonas bajaba de 40 a 20 ms, sin coste en FPS; con el límite sostenido no cambia nada. En Greylock no tuvo efecto medible, porque sus zonas pesadas las limita la CPU y la cola no llega a llenarse. En los juegos con Reflex, Reflex tiene prioridad. Se añade al arrancar a los límites que no lo tenían; desactivable con `baja-latencia=no`.
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
| `equipo.txt` | `%LOCALAPPDATA%\FlatPace` | Preferencias (`idioma`, `prioridad`: `auto`, `estabilidad` (ritmo constante) o `fluidez` (más FPS), `overlay`, `grabar-fotogramas` y los ajustes avanzados de la sección 8) y estado por juego: `nombre|exe`, referencias sin límite (`ritmo-antes2|`, `latencia-antes|`, `movimiento-antes|`), memoria (`memoria3|`), subida automática (`escalada|`), recomendaciones pendientes (`sugerencia|`: límite;tipo;límite actual, con tipo 0 subir, 1 bajar y 2 bajar urgente; `prioridad-sugerida|`), prioridad (`prioridad|`, `prioridad-fija|`); aprendizajes del equipo (`vsync-driver|`, `limite-con-generacion|`, `gsync-visto|pantalla`: G-SYNC visto funcionando en esa pantalla, para avisar si deja de estarlo) y pantalla pendiente de restaurar. Al arrancar, la app quita las claves de versiones anteriores que ya no se usan |
| `pantalla.txt` | `%LOCALAPPDATA%\FlatPace` | Última frecuencia real medida por monitor y modo |
| `registro-AAAAMMDD-HHMMSS.txt` | Carpeta del programa | Registro completo de cada ejecución |
| `grabaciones/grabacion-juego.exe-AAAAMMDD-HHMMSS.csv` | Carpeta del programa | Opcional: un fotograma por fila (ver formato) |

**Por qué cada juego tiene su límite (0.63):** en `equipo.txt`, `explica|<juego>` guarda cómo se decidió el límite actual (la regla, sus números y la curva de la partida que lo decidió: el porcentaje de los segundos que alcanzaron cada ritmo, sin los que quedan por debajo del rango del VRR) y `partida|<juego>`, la última partida con el límite (el ritmo y el 1 % más lento de cada segundo, reducidos a 240 puntos conservando las caídas, y el resultado de cada regla). Solo los lee el desplegable de *Juegos*: ninguna decisión. Formato y funciones en `src/core/explain.h`.

**Formato de `cambios-aplicados.txt` (v2), separado por tabuladores:**
`v2  exe  creado(0/1)  método(0 límite / 1 VSync)  divisor  fps  n  [id_hex  tenía(0/1)  original_hex]×n` seguido de campos opcionales: `fg k frl`, `hz frecuencia`, `ok 1` (aceptado), `base fps_reales`, `vrr 1`, `manual 1` (puesto por el usuario: la app no lo cambia).

**Formato de las grabaciones (CSV):** líneas de cabecera que empiezan por `#` (versión, juego, frecuencia real, rango VRR, prioridad y límite al empezar) y una fila por fotograma del juego con estas columnas (los valores no disponibles quedan vacíos):

| Columna | Contenido |
|---|---|
| `t_llegada_s` | Cuándo llegó el dato (PresentMon los entrega por tandas) |
| `t_acumulado_s` | Suma de las presentaciones: el tiempo de la partida |
| `presentacion_ms` | Intervalo entre presentaciones (`MsBetweenPresents`) |
| `real_ms` | Intervalo del fotograma real (con marcadores de Reflex, el de simulación); vacío en los generados |
| `real_presentacion_ms` | Intervalo entre presentaciones de fotogramas reales |
| `generado` | 1 si el fotograma es generado |
| `cpu_ms`, `gpu_ms` | Trabajo de CPU y GPU (`MsCPUBusy`, `MsGPUBusy`) |
| `pantalla_ms` | Intervalo entre cambios de imagen en pantalla (`MsBetweenDisplayChange`) |
| `error_animacion_ms` | `MsAnimationError` |
| `vrr`, `limite_confirmado` | Estado en ese momento (0/1) |
| `latencia_pc_ms` | `MsPCLatency` (0.47; ver 2.5) |
| `espera_gpu_ms` | `MsGPULatency`: desde que el juego entrega el fotograma hasta que la GPU lo empieza (0.50) |
| `cpu_rendimiento_pct` | Rendimiento de la CPU en ese segundo, en % de su frecuencia nominal (0.51) |

## 8. Ajustes avanzados

Sin interfaz, en `%LOCALAPPDATA%\FlatPace\equipo.txt` (para abrir la carpeta: Windows + R, escribe `%LOCALAPPDATA%\FlatPace` e Intro), con la app cerrada. Cada ajuste es una línea `clave=valor`; la app escribe las suyas con una tabulación en lugar del `=`, y desde la 0.50.1 acepta ambas formas (antes, una línea con `=` se ignoraba). Si una clave no está, se usa su valor por defecto:

| Clave | Efecto |
|---|---|
| `limitador=` | El limitador elegido en *Juegos*, solo con AMD o Intel: `recomendar` (modo guiado) o `rtss`. Con NVIDIA se usa siempre el driver (`driver`); si quedaban límites en RTSS de la 0.58, se devuelven al driver al arrancar. Al cambiarlo desde la ventana, cada límite se deshace con el limitador que lo aplicó y se aplica el mismo valor con el nuevo (el VSync por intervalos pasa a límite de FPS). Con RTSS, sin "GPU al máximo" ni baja latencia del driver, y sin la regla del 1 % bajo. |
| `baja-latencia=no` | No acompañar los límites del *Modo de baja latencia* (cola de render de un fotograma); al arrancar, la app lo quita también de los límites que ya lo tenían (salvo con el VSync por intervalos, donde forma parte del método). |
| `rendimiento-maximo=no` | No acompañar los límites de *Preferir rendimiento máximo*; al arrancar, la app lo quita también de los límites que ya lo tenían. |
| `overlay=clasico` | Usar siempre el overlay clásico (dibujado con el procesador) en lugar del de la GPU. |
| `divisores-siempre=si` | Usa siempre divisores de los Hz, aunque G-SYNC/FreeSync actúe. Para pantallas que parpadean con VRR (algunas OLED y VA). La sección *Equipo* lo indica en la fila *VRR*. |

Los límites ya aplicados no cambian: para recalcular un juego con el ajuste nuevo, usa *Deshacer*.

## 9. Modelos estadísticos (0.37)

El de datos censurados ya interviene en una decisión: decide si el juego va sobrado para la subida automática (punto 6 de la sección 4). El resto informa y sugiere.

- **Con generación de fotogramas** (vista en la partida o con la que se aplicó el límite), no se calculan la capacidad, el 1 % bajo causado por el juego, la memoria ni las sugerencias: suponen que el límite rige siempre sobre los fotogramas reales, y con generación intermitente no es así.
- **Datos censurados** (`src/core/censored.h`): con un límite de periodo *c*, se mide la variación del limitador en cada partida (desviación absoluta mediana × 1,4826 de los fotogramas entre 0,85 *c* y 1,15 *c*), y los fotogramas ≤ *c* + máx(3 % de *c*, 3 × variación) se tratan como censurados ("tardarían como mucho *c*"); los demás, como medidas. Se ajusta una log-normal por máxima verosimilitud (Nelder-Mead), sin pausas (> 250 ms) ni tirones (> 2,5 × mediana). Requiere ≥ 200 fotogramas, entre un 5 % y un 98 % de censura y ≥ 30 fotogramas visibles; con > 98 % el juego "va sobrado". El p99 es fiable; la mediana, solo con ≤ 90 % de censura. **Validación con partidas reales (0.37.1):** con el limitador del driver, los tiempos quedan repartidos de forma simétrica alrededor del límite (±0,2 ms en Megabonk, ±0,4 ms en Greylock); con el umbral anterior (+3 % fijo), Megabonk con 195 FPS parecía capaz de ~205 FPS de mediana cuando daba ~300; ahora sale "sobrado", y en Greylock con 90 FPS el p99 estimado (74 FPS) coincide con el medido (76).
- **1 % bajo causado por el juego** (`GameCausedPercentileMs`, en `src/core/analysis.h`): con un límite activo de periodo *c*, un fotograma más lento que *c* cuenta como tal solo si su trabajo (máx. de CPU y GPU ocupadas) supera 0,95 × *c*; si no, se cuenta como *c*, porque el retraso fue del temporizador del limitador. Validado con partidas reales: Megabonk con 195 FPS, 176 medidos → 195 del juego; Greylock con 90, 76 → 83. Se usa para el 1 % bajo de la ventana, el overlay y el registro.
- **Memoria por juego** (`src/core/game_memory.h`): media y varianza ponderadas (Welford) del logaritmo del ritmo que el juego sostiene en cada partida (el criterio de Ritmo constante: 1,13 × 1000 / p99 sin tirones; con límite, solo si el 1 % más lento causado por el juego queda más de un 13 % por debajo del límite, como protección contra la espiral descendente), con peso = minutos jugados (máximo 10) y olvido ×0,8 por partida. Incertidumbre = √(varianza entre partidas + 0,08² / partidas); límite inferior al 80 % = exp(media − 0,84 × incertidumbre). Se guarda en `equipo.txt` (`memoria3|juego.exe`).
- **Fases** (`src/core/bocpd.h`): BOCPD con prior normal-gamma (κ₀ = 0,02, α₀ = 2, desviación típica a priori 0,03 en escala logarítmica), riesgo de cambio 1/60 por segundo y rachas truncadas a 300 s, sobre el logaritmo de la mediana de cada segundo filtrado con una mediana de 3 s. Se propone un cambio cuando la racha más probable vuelve a ≤ 3 s durante dos pasos tras haber sido ≥ 8 s; la fase se confirma si, tras 10 s, su mediana difiere ≥ 8 % de la anterior.
- **Sugerencia** (`src/core/suggest.h`): siguiente escalón = con VRR, mín(límite × 1,13, techo); sin VRR, el divisor siguiente. Se sugiere si el juego va sobrado, o si la capacidad estimada llega al escalón y, con dos o más partidas en memoria, su límite inferior llega al 95 % del escalón.

## 10. Fundamentos y fuentes

- **Divisores del refresco:** sin VRR, cada imagen ocupa un número entero de refrescos; solo los divisores dan un ritmo uniforme. Google aplica la misma regla en el Game Mode de Android.
- **Variación frente a media:** la media de FPS por sí sola no predice la calidad percibida cuando hay variación en los tiempos de fotograma (WPI, CHI 2023).
- **Umbral perceptible del 13 %:** la percepción de la frecuencia es aproximadamente logarítmica (ley de Weber). Naylor-Lovell, Chalmers y Debattista ([Virtual Reality, 2025](https://doi.org/10.1007/s10055-025-01227-9)) midieron la diferencia apenas perceptible de la frecuencia de actualización (*tick rate*) de un objeto en movimiento, en VR y en escritorio con G-SYNC a 90 Hz: k = 0,133 y 0,155, es decir, un cambio del 13-15 % para notarlo la mitad de las veces, constante en unidades logarítmicas. Es la frecuencia de actualización de un objeto, no la duración de los fotogramas de un juego, pero un fenómeno muy cercano; los estudios de frecuencia de fotogramas (Denes y otros, 2020) dan saltos del mismo orden. El umbral crece mucho cuando la pantalla no es un múltiplo entero de la frecuencia del movimiento, lo que respalda usar divisores exactos de los Hz sin VRR.
- **Techo con VRR:** regla de 3 FPS de Blur Busters (G-SYNC 101) y fórmula Hz − Hz²/4096, inspirada en Special K y coincidente con el límite automático de Reflex.
- **Medición:** PresentMon (Intel) y la metodología de marcadores de Reflex de NVIDIA.

## 11. Limitaciones conocidas y líneas futuras

- **Sin cambio en vivo:** el límite del driver se lee al abrir el juego. Un limitador propio dentro del juego (DLL) lo permitiría, con riesgo en juegos con anticheat.
- **AMD e Intel: *Solo recomendar*** (modo guiado, desde la 0.56) por defecto, o **límites con RTSS** (desde la 0.57) si el usuario lo elige como limitador. La app mide, decide y comprueba el límite, pero lo aplica el usuario (en AMD Software, con Radeon Chill; en Intel Graphics Software, con su limitador de FPS; o con el limitador del juego), siguiendo la instrucción que aparece en la fila del juego. El modo automático no es posible con los kits de AMD (ADLX: prohíbe redistribuir sus archivos y exige un contrato de usuario que prohíba copiar y estudiar el programa) ni de Intel (IGCL: solo para plataformas Intel y con una cláusula de indemnización), porque sus licencias no encajan con un proyecto MIT de código abierto (ver el README). Sin "GPU al máximo", baja latencia ni VSync por intervalos, que son ajustes del driver de NVIDIA: la instrucción recuerda activar el VSync del driver con FreeSync o Adaptive-Sync, y la opción de baja latencia del fabricante. Sin "GPU al máximo", la GPU se relaja con el límite y el 1 % bajo puede parecer peor de lo que es (la espiral de las primeras versiones), así que en modo guiado la regla del 1 % bajo no baja límites ni cambia la prioridad (desde la 0.56.1). Aún sin probar en esas gráficas.
- **PresentMon 2.6.0** tiene fallos conocidos con las opciones que usa FlatPace (issue 695 de PresentMon): puede acumular datos internos y dejar de entregar fotogramas, por ejemplo al desactivar la generación de fotogramas en algunos juegos. Desde la 0.51.3, FlatPace lo reinicia si deja de entregar datos con el juego en primer plano durante más de 6 s. Conviene actualizarlo cuando Intel publique una versión corregida.
- **Latencia del PC:** la medida está integrada, pero en los juegos probados PresentMon no la ha entregado (sección 2.5).
- **Generación de fotogramas:** descompasa el movimiento aunque el ritmo sea plano (sección 2.3); ningún límite lo corrige, y FlatPace solo lo informa.
- **Memoria de límites (preparada, sin usar):** `src/core/limit_memory.h` decide con varias partidas en lugar de una (la cota prudente del 1 % más lento para *Ritmo constante*, la media del ritmo del 90 % para *Más FPS*, con las partidas censuradas como cota inferior que solo puede subir la estimación), y `tools/replay_memory.cpp` la reproduce con grabaciones reales. En Greylock dio límites más estables (ningún salto de más de un 13 %, frente a 8 con una sola partida), pero con *Ritmo constante* bajaría el límite unos 12 FPS para cubrir las zonas más pesadas, una diferencia por debajo de lo perceptible. Con un solo juego medido no basta para cambiar el núcleo de las decisiones: se activará si lo confirman más juegos y equipos.
- **Validar los modelos estadísticos con grabaciones reales** y, después, dejar que influyan en las decisiones (subir límites con la capacidad estimada, decidir con la memoria del juego y con la fase más exigente).
- Validación en más equipos (sobremesa, sin VRR, AMD), monitores y juegos: todo lo medido procede de un portátil con RTX 4080 y pantalla de 240 Hz con G-SYNC, y el equipo puede cambiar las conclusiones (como pasó con "GPU al máximo").

## 12. Compilación

MinGW-w64 (desde Linux):
```
x86_64-w64-mingw32-windres app.rc -O coff -o app.res
x86_64-w64-mingw32-g++-posix -std=c++20 -municode -mwindows -O2 -DNDEBUG -Ithird_party/imgui \
  -Ithird_party/imgui/backends main.cpp imgui*.o app.res -o FlatPace.exe \
  -ld3d11 -ldxgi -ld3dcompiler_47 -ldwmapi -ldcomp -lgdi32 -lshell32 -luser32 -ladvapi32 -lversion \
  -limm32 -lwinmm -lpdh -static -s
```
Windows: `cmake -B build && cmake --build build --config Release`. Dear ImGui (con los backends Win32 y DX11) va en `third_party/imgui`, y las declaraciones de NVAPI que usa FlatPace (de NVIDIA, licencia MIT) en `third_party/nvapi/nvapi_min.h`: todo el código de terceros está en `third_party/`.

## 13. Pruebas automáticas

`tests/test_core.cpp` prueba la lógica de `src/core` sin necesitar Windows (559 comprobaciones, entre ellas las de los registros del desplegable de *Juegos* y las de la memoria de límites; incluidas las de que cada traducción del registro conserva los huecos de su original; entre ellas, que la traducción a NVIDIA produce exactamente los ajustes de versiones anteriores): percentiles y variación del ritmo, consistencia, detector de límites, recomendaciones con y sin VRR (techo, rango VRR del monitor y LFC, Más FPS), lectura del EDID, gráfica de ritmo (tolerancia, escala, tirones, modo en pantalla y estabilidad de los tramos al desplazarse y en ambos bordes), los modelos estadísticos (recuperación de la capacidad con datos censurados, detección de fases y su resistencia a picos aislados, memoria por juego y reglas de sugerencia) y el ciclo de decisión (verificación, regla de fallos, bajada tras un fallo, decisión al cerrar y simulaciones de varias partidas seguidas que comprueban que no hay espiral descendente). Cada prueba fija un comportamiento ya validado con partidas reales o en simulación.

- Linux/macOS: `sh tests/run_tests.sh`
- Windows (MSVC): `tests\run_tests.bat`, o `ctest` tras compilar con CMake.

Se comprobó que detectan errores introducidos a propósito, parecidos a regresiones reales del proyecto (tolerancia de la gráfica, techo con VRR y clasificación de tramos).
