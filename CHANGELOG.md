# Historial de cambios

Formato basado en [Keep a Changelog](https://keepachangelog.com/es-ES/1.1.0/). La versión más reciente va primero.

## 0.42.1
Con las mediciones de la 0.42 en Space Marine 2: el proceso de FlatPace bajó del 15,9 % al 8,5 % de un núcleo y el overlay de 3,04 a 1,43 ms por dibujo; al principio de cada tanda de datos seguía habiendo 1,5 veces más imágenes lentas que en el resto (2,0 en la 0.40, 1,7 en la 0.41.1).
- **Cambiado:** PresentMon se lanza **sin seguimiento de entrada** (`--no_track_input`): FlatPace no usa esos datos, y un ratón de juego genera cientos o miles de eventos por segundo que PresentMon tenía que procesar.
- **Cambiado:** PresentMon también funciona con **prioridad baja y modo de eficiencia**. Windows le entrega los eventos en ráfagas cada ~1 s, y procesarlas con prioridad normal competía con el juego justo en ese instante. Mientras espera, los eventos quedan en los búferes de Windows; si se perdieran, PresentMon lo avisaría en el registro.

## 0.42
Con las mediciones de la 0.41.1 en Space Marine 2: el overlay era lo que más gastaba (3 ms por dibujo, ~13 % de un núcleo); PresentMon, solo un 1,2 %; el motor, ~2,5 % de CPU real.
- **Mejorado (overlay, unas 5 veces más rápido sin cambiar lo que se ve):** los fondos fijos (tarjeta y área de la gráfica) se dibujan una sola vez y se copian en cada fotograma; los textos se dibujan con Windows solo la primera vez y después se reutilizan; la mezcla de píxeles usa enteros en lugar de coma flotante (diferencia de color de como mucho 2 niveles sobre 255); y las esquinas redondeadas solo calculan distancias en los bordes. En una réplica del dibujo, de 5,4 a 1,1 ms por fotograma, sin contar el ahorro de los textos.
- **Mejorado (al llegar cada tanda de datos):** el búfer entre PresentMon y FlatPace pasa de 4 KB a 1 MB, así que el hilo lector ya no necesita prioridad elevada y el juego puede adelantarlo siempre. Con la 0.41.1, al principio de cada tanda seguía habiendo 1,7 veces más imágenes lentas que en el resto (frente a 2 veces en la 0.40).

## 0.41.1
- **Añadido:** el resumen de cada partida mide también el coste de **PresentMon** (uso de CPU de su proceso) y el del **overlay** (tiempo por dibujo y dibujos por segundo), además del motor y del proceso de FlatPace. Sirve para saber dónde está realmente el coste antes de optimizar.

## 0.41
- **Mejorado: FlatPace ya no compite con el juego por el procesador.** Las grabaciones mostraron que los fotogramas que el juego calcula justo después de cada tanda de datos de PresentMon (cada ~1 s) salían más lentos (Greylock y Megabonk: 1,6-2 veces más fotogramas lentos; en Space Marine 2, limitado por la CPU, la generación de fotogramas se saltaba imágenes: un 44 % de generadas frente al 48 %, lo que se veía como picos al actualizarse la gráfica). Ahora:
  - FlatPace funciona con **prioridad baja** y en el **modo de eficiencia de Windows 11** (núcleos de eficiencia, frecuencias bajas), salvo el hilo que lee PresentMon (no puede retrasarse) y el que dibuja la ventana y el overlay (sin modo de eficiencia, para que no vaya a tirones). PresentMon se lanza con prioridad normal.
  - El **modelo de capacidad** solo se calcula en el resumen de la partida: cada 5 s mientras se jugaba añadía un pico de trabajo que coincidía con fotogramas lentos (2-3 veces más en una de cada cinco tandas, con límite).
  - Los últimos minutos de fotogramas se ordenan como mucho cada 5 s (antes, cada segundo).
- **Añadido:** el resumen de cada partida indica el **coste de FlatPace** (tiempo del motor por segundo y uso de CPU del proceso completo), para comprobar el efecto con datos.

## 0.40.1
- **Corregido:** en juegos con generación de fotogramas, la app podía sugerir que el juego "va sobrado" y subir el límite cuando no era así. Los modelos de capacidad suponen que el límite rige siempre sobre los fotogramas reales; con generación intermitente no es cierto (en Control Resonant, sin generación los fotogramas reales iban a 60-72 FPS con un límite de 36 reales, y el modelo los tomaba por fotogramas esperando en el límite). Ahora, con generación, no se calculan la capacidad, el 1 % bajo causado por el juego, la memoria ni las sugerencias; el resumen lo indica y la sugerencia errónea se retira.
- **Corregido:** las comillas tipográficas aparecían como "?" en la interfaz (*Click ?Undo?*): la fuente solo cargaba la raya y los puntos suspensivos de ese bloque de signos. Ahora carga todo el bloque de puntuación (comillas, apóstrofos y guiones).

## 0.40
Cambios a partir de las pruebas con CONTROL Resonant (generación de fotogramas que el juego activa y desactiva sola).
- **Cambiado:** en los juegos con generación de fotogramas, la gráfica dibuja las **imágenes en pantalla** (lo que se ve), con la referencia del límite en pantalla, durante toda la partida. Antes dibujaba los fotogramas reales, que en Control Resonant saltaban entre 36 FPS (con generación) y 60-68 (sin ella), aunque en pantalla la diferencia era mucho menor. La etiqueta lo indica (*"en pantalla 13,9 ms ±1,8"*). La cifra grande y el 1 % bajo siguen siendo de fotogramas reales, que marcan la respuesta del juego.
- **Corregido:** la tolerancia de la gráfica tenía un máximo de 2 ms, que a FPS bajos la hacía más estricta que el propio criterio del 13 % (a 36 FPS, un 7 %) y mostraba como picos variaciones imperceptibles. Ahora es el 13 % a cualquier FPS (mínimo 0,5 ms; el máximo de 6 ms solo actúa por debajo de ~22 FPS). Por encima de ~65 FPS no cambia nada.
- **Añadido:** reconocimiento de la generación intermitente: tras 4 cambios de estado en una partida, la app avisa una vez en el registro y en *Juegos* (probablemente un modo dinámico del juego o del driver; fijarlo da un ritmo más constante) y deja de anotar cada cambio.
- **Comprobado:** el overlay no causa los cambios de generación (con el overlay apagado siguieron igual).

## 0.39.1
- **Mejorado:** el ciclo de decisión (verificar el límite, la regla de fallos, el valor al que se baja tras un fallo y la decisión al cerrar el juego) pasa de `main.cpp` al módulo `src/core/decision.h`, con pruebas automáticas, incluidas simulaciones del ciclo completo a lo largo de varias partidas (158 comprobaciones). Se comprobó que detectan cinco errores introducidos a propósito.
- **Corregido (encontrado por las pruebas):** con un límite puesto, la decisión al cerrar podía bajarlo 1-2 FPS en cada partida por el temblor del limitador (en la simulación, de 150 a 87 en diez partidas): una espiral descendente como la de versiones antiguas. Ahora, con un límite activo, solo se baja si el juego queda por debajo de forma perceptible (más de un 13 %), igual que en la regla de fallos.
- **Corregido (encontrado por las pruebas):** un juego clavado por su propio límite (por ejemplo, a 144) recibía con la *Estabilidad* de la 0.39 un límite mayor (162), aunque su capacidad real es desconocida. Ahora se usa ese ritmo demostrado, ni más ni menos.

## 0.39
Reglas nuevas para elegir el límite con G-SYNC/FreeSync, validadas con partidas reales grabadas (Greylock y Megabonk, con y sin límite).
- **Cambiado (Fluidez):** el ritmo que el juego alcanza el **90 %** del tiempo, sin contar los tirones (antes, el 70 %). Con la regla anterior, en Greylock el juego quedaba por debajo del límite un 22-34 % de los segundos, y un 29 % de forma perceptible. Validado: con el límite que da la regla nueva (100 FPS), el 1 % bajo causado por el juego fue de 96 FPS y solo un 1 % de los segundos bajó de forma perceptible.
- **Cambiado (Estabilidad):** el 1 % más lento, sin tirones, puede llegar **como mucho un 13 % tarde** (la diferencia perceptible): 1,13 × 1000 / p99. Antes era 1000 / (p99 × 1,05) **con** los tirones incluidos, que inflaban el p99: Greylock pasa de 78-85 FPS a 91-108 sin caídas perceptibles. En Megabonk, la regla nueva (189) sigue siendo prudente: con 225 el juego no bajó nunca del límite, porque con un límite puesto su CPU trabaja mucho menos que sin él; para eso está la sugerencia de subir.
- **Añadido (protección contra la espiral):** con un límite activo, la recomendación nunca sube (los datos están censurados; eso queda para la sugerencia) y usa los tiempos causados por el juego, sin los retrasos del limitador.
- **Cambiado:** la memoria del juego usa el mismo criterio que la Estabilidad nueva y empieza de nuevo (clave `memoria3|`). Con un límite, solo aprende si el juego baja de él de forma perceptible.
- **Corregido:** el resumen de una partida con el límite ya aceptado mostraba una recomendación sin sentido (con el formato de las pantallas sin G-SYNC); ahora indica que el límite aceptado no se recalcula.
- Sin G-SYNC/FreeSync (divisores), las reglas no cambian hasta validarlas con una pantalla sin VRR.

## 0.38
- **Añadido:** con cada límite (automático o manual), FlatPace pide al driver de NVIDIA que **no baje las frecuencias de la GPU** en ese juego (*Modo de administración de energía: Preferir rendimiento máximo*), en el mismo perfil y con el mismo *Deshacer*. Con un límite, el driver bajaba las frecuencias hasta que la GPU iba justa y tardaba en subirlas en las escenas pesadas, que se pasaban del límite: en Greylock con 90 FPS, la GPU tardaba casi el doble por fotograma (mediana de 9,6 ms frente a 5,1 sin límite) con el mismo 85 % de uso, y el 1 % bajo causado por el juego quedaba en 83. Es la combinación que recomienda NVIDIA para su limitador. La GPU sigue protegiéndose sola por temperatura y potencia: el ajuste no cambia esos límites.
- **Añadido:** al abrir la app, el ajuste se añade también a los juegos que ya tenían límite (actúa la próxima vez que se abra el juego). Se muestra como *"· GPU al máximo"* en *Juegos*.
- **Añadido:** ajuste avanzado `rendimiento-maximo=no` en `equipo.txt` para no aplicarlo.

## 0.37.2
- **Corregido:** con un límite puesto, el **1 % bajo** caía por debajo del propio límite (Megabonk con 195 FPS: 176). En tus grabaciones, el 96-97 % de esos fotogramas tardíos no se debían al juego (su CPU y GPU habían terminado de sobra, ~2,2 ms en Megabonk), sino al temporizador del limitador del driver, que a veces despierta tarde y compensa adelantando el siguiente fotograma (en el 78-89 % de los casos). Ahora, con un límite activo, un fotograma tardío solo cuenta como lento si su trabajo de CPU/GPU también superó el 95 % del límite. En la ventana, el overlay y el registro se muestra ese **1 % bajo causado por el juego** (Megabonk: 195 FPS; Greylock con 90: 83, porque ahí sí hay escenas pesadas en las que no llega), y el resumen muestra ambos valores.
- **Añadido (protección contra la espiral):** con un límite puesto, la memoria del juego solo aprende si el juego **no** lo sostuvo. Si lo sostuvo, solo se sabe que da "al menos eso", y anotar el límite como capacidad haría bajarlo partida a partida (como la espiral 231 → 226 → 222 de versiones antiguas). Las decisiones de *Estabilidad* ya eran inmunes: usan el trabajo de CPU/GPU, que el limitador no distorsiona.

## 0.37.1
Primera validación de los modelos con partidas reales grabadas (Megabonk y Greylock, con y sin límite).
- **Corregido (datos censurados):** con el limitador del driver, los tiempos quedan repartidos de forma simétrica alrededor del límite (±0,2 ms en Megabonk, ±0,4 ms en Greylock). El modelo trataba como "lento" todo lo que superaba el límite en un 3 % y confundía esa variación del limitador con falta de capacidad (Megabonk con 195 FPS: estimaba ~205 FPS de mediana con un juego capaz de ~300). Ahora mide la variación del limitador en cada partida y solo cuenta como lento lo que la supera en más de tres veces. Con más del 98 % de fotogramas en el límite, indica que el juego **va sobrado** (Megabonk); la mediana solo se muestra con un 90 % de censura o menos, porque por encima es pura extrapolación (error del 9-20 % en las partidas reales); el p99 sí es fiable (Greylock con 90: 74 FPS estimados frente a 76 medidos).
- **Corregido (memoria por juego):** las memorias guardadas con la 0.37 se calcularon con el modelo anterior y se descartan (se empieza de nuevo con la clave `memoria2|`).
- **Comprobado (fases):** con las grabaciones, el detector encuentra los cambios reales de Greylock (inicio de la partida y zona más pesada) y ninguno en Megabonk ni con el límite puesto. Limitación conocida: una bajada gradual (en Greylock, de ~218 a ~147 FPS en 50 s) cuenta como una sola fase.
- **Corregido:** los mensajes de cambio de recomendación decían siempre "prioridad: fluidez"; ahora indican la prioridad real.
- **Corregido:** con G-SYNC/FreeSync actuando, un límite dentro del rango (como 90 FPS) ya no se anuncia como "NO encaja con tu monitor".
- **Mejorado:** las fases se anotan en el registro al confirmarse, indicando desde qué segundo empezaron.
- **Añadido:** pruebas que reproducen el comportamiento real del limitador (101 comprobaciones).

## 0.37
- **Añadido:** tres modelos estadísticos, en módulos de `src/core` con sus pruebas. Por ahora informan y sugieren; no cambian las decisiones hasta validarlos con grabaciones reales.
  - **Datos censurados (modelo Tobit log-normal):** con un límite puesto, estima la capacidad real del juego (mediana y p99) aunque la mayoría de los fotogramas esperen en el límite. Aparece en el registro cada 5 s (*"sin límite ~X fps"*) y en el resumen. En simulación, con el 85-96 % de fotogramas en el límite, recupera la mediana real con menos de un 1 % de error. Si casi todo está en el límite (> 98 %), indica que el juego va sobrado.
  - **Memoria por juego:** combina las partidas (ponderadas por duración, con olvido gradual de las antiguas) en una estimación del ritmo que el juego sostiene, con un límite inferior al 80 % de probabilidad. Aparece en el resumen.
  - **Fases de juego (BOCPD, Adams y MacKay 2007):** detecta cambios de zona o de ajustes con un dato por segundo; una fase nueva cuenta si, tras 10 s, su ritmo difiere al menos un 8 %. Un filtro de mediana de 3 s evita que un segundo atípico reinicie la fase. Aparece en el registro (*"Fase nueva"*) y en el resumen.
  - **Sugerencia de subir el límite:** con el límite confirmado, si la capacidad estimada alcanza el siguiente escalón (con VRR, un 13 % más sin pasar del techo; sin VRR, el divisor siguiente) y la memoria del juego no lo contradice, la app lo sugiere en *Juegos*. Nunca lo cambia sola.
- **Corregido:** en el resumen, el peor tirón aparecía al final de la línea del error de animación.

## 0.36.3
- **Añadido:** grabación de los datos de cada fotograma. Casilla *Grabar los datos de cada fotograma* en *Registro* (desactivada por defecto; la app recuerda la elección): guarda un archivo CSV por partida en la carpeta `grabaciones`, junto al programa, con una cabecera de contexto (juego, pantalla, rango VRR, prioridad y límite aplicado) y una fila por fotograma (tiempos, CPU, GPU, pantalla, error de animación, estado del VRR y del límite). Sirve para analizar y validar con datos reales las mejoras estadísticas previstas (datos censurados, memoria por juego y detección de fases). Unos 10 MB por cada 10 minutos a 240 FPS.

## 0.36.2
- **Añadido:** la app usa el **rango de VRR del monitor** (el rango FreeSync del EDID si lo declara, si no su rango de frecuencias). Si el VRR del monitor no llega a su frecuencia máxima, el límite queda por debajo del máximo del rango VRR. Si un juego va por debajo del mínimo del rango y el monitor no puede duplicar fotogramas (LFC, cuando el máximo es menor que el doble del mínimo), la app usa un divisor de los Hz en lugar del límite con VRR. La fila *VRR* indica el rango y si hay LFC.
- **Cambiado:** la sección *Ahora* adopta la identidad visual: cabecera con la etiqueta de estado del límite (la misma que el overlay: *FIJADO* en azul, *SIN CONFIRMAR* o *SIN LÍMITE* en rojo), tarjeta principal con la cifra de FPS, cuatro métricas (1 % bajo, consistencia, tirones y variación) y la gráfica, y dos tarjetas de la misma altura, *Límite* (con *Aplicar ya*, *Límite manual…* y *Deshacer*) y *Detalles* (VRR, presentación, límite del juego, cuello de botella y pantalla).
- **Cambiado:** la app recuerda si el overlay está activado, y el registro ya no anota *"Overlay: sin juego detectado"* al empezar cada partida.
- **Añadido:** licencia MIT y guía de uso y documentación técnica en inglés (`GUIDE.md`, `TECHNICAL.md`).
- **Mejorado:** la lógica pura (parámetros, estadísticas, análisis y recomendaciones, EDID y gráfica de ritmo) pasa de `main.cpp` a ocho módulos en `src/core/`, sin cambiar el programa: el ejecutable compilado es idéntico byte a byte salvo la fecha de compilación.
- **Añadido:** pruebas automáticas (`tests/`, 56 comprobaciones) que fijan el comportamiento validado de esa lógica, ejecutables sin Windows.

## 0.36.1
- **Corregido:** la gráfica se desplazaba a tirones y algunos picos aparecían o desaparecían en medio. Tres causas: la línea de tiempo se recalculaba en cada dibujo (los fotogramas cambiaban de tramo al llegar cada tanda de datos), el retraso de dibujo cambiaba a saltos y, sin límite, la referencia se movía en cada dibujo. Ahora:
  - cada fotograma recibe su instante una sola vez, al llegar, y no cambia;
  - un **reloj de reproducción** con búfer (como un reproductor de vídeo) avanza a velocidad real y se corrige suavemente, frenando con suavidad si una tanda se retrasa;
  - la referencia sin límite es la mediana de los últimos 10 s, suavizada;
  - cada tramo se clasifica (dentro de la tolerancia, fuera o tirón) una sola vez, al completarse.
  En una simulación con tandas irregulares de 1 a 2 s, la variación de la velocidad de desplazamiento baja de ±54,6 % a ±6,7 %, y los picos que aparecen o desaparecen, de 740 a 10 en 45 s.

## 0.36
- **Cambiado:** la gráfica de ritmo pasa a ser una **gráfica de desviaciones**: los tramos dentro de la tolerancia se dibujan como una línea recta sobre la referencia, y solo se dibuja lo que se sale. La regla es la misma con y sin límite, para que la comparación sea justa, y las métricas (variación, consistencia) siguen contando todos los fotogramas.
- **Cambiado:** la tolerancia (y la banda azul) es proporcional al tiempo de fotograma: ±13 %, el umbral de diferencia perceptible que ya usan las decisiones, entre 0,5 y 2 ms. Los 2 ms fijos eran demasiado permisivos a FPS altos (un 38 % a 190 FPS) y dejaban también plana la gráfica sin límite. En una simulación con datos de Greylock, los tramos fuera de la línea recta son un 29,5 % sin límite frente a un 16,3 % con límite de 120 (con 2 ms fijos eran un 4,9 % y un 2,2 %: ambas casi planas). La etiqueta indica la tolerancia: *"objetivo 8,3 ms ±1,1"*.

## 0.35
- **Cambiado:** la prioridad predeterminada pasa a ser **Estabilidad**: un límite que el juego sostiene siempre, incluso en sus peores momentos. Con *Fluidez*, el límite es el ritmo que el juego alcanza el 70 % del tiempo, y en juegos exigentes (como Greylock, a 165-185 FPS con un límite de 188) eso deja al limitador sin margen y el ritmo irregular. Al actualizar, la prioridad pasa una vez a Estabilidad; se puede volver a Fluidez en *Juegos*. Los límites ya aplicados no cambian: *Deshacer* los recalcula con el criterio nuevo.
- **Corregido:** el error de animación ignora las cargas y pausas (antes el máximo podía mostrar miles de milisegundos).

## 0.34
- **Cambiado:** la gráfica de ritmo muestra el intervalo entre **presentaciones** de los fotogramas reales, el punto que regula el limitador. En los juegos con Reflex usaba el inicio de la simulación, que varía más por naturaleza aunque el limitador entregue los fotogramas a un ritmo perfecto. El análisis (decisión, verificación, consistencia) no cambia y sigue usando los marcadores de Reflex donde aportan.

## 0.33.2
- **Cambiado:** la gráfica vuelve a dibujar los fotogramas reales (la mediana de cada tramo, sin promedios de 100 ms ni halo). El "ritmo percibido" de la 0.33 suavizaba también la variación sin límite, y ambas gráficas parecían igual de planas.
- **Corregido:** se quita el reloj de PresentMon de la 0.33.1 y la línea de tiempo se reconstruye hacia atrás desde el fotograma más reciente, que termina cuando llegaron sus datos. Así una tanda de datos nunca se amontona en un punto. El retraso de dibujo sigue al intervalo real entre tandas. En una simulación con tandas de 0,15 a 2 s, la gráfica queda cubierta en torno al 99 % y los tramos no cambian entre dibujos.
- **Cambiado:** zoom vertical alrededor del objetivo (±50 % del tiempo de fotograma, mínimo ±2 ms), para que las diferencias reales de estabilidad se vean; lo que se sale se dibuja contra el borde. Los tramos con tirones (más de un 50 % por encima del objetivo) se pintan en ámbar.
- Se mantienen la variación del ritmo (±ms) y la comparación con la partida sin límite.

## 0.33.1
- **Corregido:** la gráfica de ritmo podía mostrar solo un tramo recto con huecos a los lados, sobre todo con generación de fotogramas. PresentMon entrega los datos por tandas (con generación, cada 1-2 s), y la línea de tiempo reconstruida sumando duraciones amontonaba cada tanda en un solo punto. Ahora cada fotograma se coloca en su instante real, según el reloj del propio PresentMon (columna de tiempo de cada presentación, con sus unidades deducidas automáticamente), y el retraso de dibujo se adapta a la latencia real de las tandas para que el borde derecho no quede vacío. En una simulación con tandas de 2 s, la parte de la gráfica con datos reales pasa del 5 % al 100 %.

## 0.33
- **Cambiado:** la gráfica de ritmo (overlay y ventana) muestra el **ritmo percibido**: la mediana de los fotogramas en ventanas de 100 ms, aproximadamente como lo integra la vista. La variación de un fotograma a otro se dibuja como un **halo** tenue alrededor de la línea (percentiles 10 a 90), y los tirones se siguen marcando aparte, en ámbar. Así se distingue lo que estabiliza un límite (las oscilaciones lentas de los FPS con la escena) de la variación fina, que es parecida con y sin límite e imperceptible.
- **Añadido:** variación del ritmo percibido (±ms) en la gráfica, en el registro y en el resumen de cada partida. La primera partida sin límite de cada juego se guarda como referencia, y con el límite confirmado la gráfica la compara: *"variación ±0,1 ms · antes ±0,4"*.
- **Corregido:** la gráfica podía quedarse con el borde derecho vacío tras una carga o un tirón largo, porque la línea de tiempo tardaba en reanclarse (ahora lo hace a los 0,6 s, antes de que se note).
- **Cambiado:** las etiquetas de la gráfica pasan a la parte inferior, donde no tapan la curva.

## 0.32.1
- **Añadido:** banda de tolerancia en la gráfica de ritmo (overlay y ventana): una franja de ±2 ms alrededor del objetivo, el mismo umbral que usa la consistencia. Solo aparece con el límite confirmado (azul). Si la línea ondula dentro de la banda, el ritmo es estable; las variaciones que salen de ella son las que se pueden notar.

## 0.32
- **Añadido:** identidad visual. Logo: una *f* trazada como una gráfica, cruzada por la línea plana en azul. Es el icono del ejecutable (de 16 a 256 píxeles), de la ventana y de la barra de tareas, y aparece con el nombre en la barra lateral.
- **Cambiado:** paleta nueva (fondo casi negro, tarjetas oscuras, azul de marca #2FA8FF). El estado confirmado pasa de verde a **azul**, también en el overlay; el rojo sigue indicando "sin límite" o "sin confirmar".
- **Añadido:** iconos en la navegación (Ahora, Juegos, Equipo, Registro), tarjetas más redondeadas y, en Windows 11, barra de título del color de la ventana.

## 0.31
- **Añadido:** límite manual. Botón *Fijar…* en cada juego de la lista y *Límite manual…* en *Ahora*. Se aplica en el driver como los automáticos (con G-SYNC, junto al VSync del driver), se comprueba al abrir el juego (el overlay pasa a verde) y **la app nunca lo cambia**: solo avisa al cerrar si el juego no lo sostuvo. La ventana avisa si el valor supera el máximo recomendado con G-SYNC o, sin G-SYNC, si no divide los Hz (con los valores que encajan). Con generación de fotogramas, el valor es lo que se ve en pantalla. *Deshacer* devuelve el juego al modo automático.

## 0.30.1
- **Cambiado:** el uso de G-SYNC / FreeSync es totalmente automático: se quita el interruptor de la ventana. Para pantallas que parpadean con VRR queda un ajuste avanzado, `divisores-siempre=si` en `equipo.txt` (ver la documentación técnica).

## 0.30
- **Cambiado:** nuevo diseño del overlay. **Verde** cuando el límite está confirmado ("FIJADO") y **rojo** cuando no hay límite o aún no está confirmado ("SIN LÍMITE", "SIN CONFIRMAR"). Curva con bordes suavizados, relleno degradado, rejilla de un segundo y esquinas redondeadas; los tirones forman parte de la curva, en ámbar.
- **Mejorado:** el overlay se actualiza hasta 60 veces por segundo (antes 15), también con la ventana de FlatPace minimizada, y se desplaza de forma continua: dibuja con un segundo de retraso sobre una línea de tiempo reconstruida con la duración de los fotogramas, en lugar de avanzar a saltos con cada tanda de datos de PresentMon. Los tramos de la gráfica son fijos en el tiempo, así que no "tiemblan" al desplazarse.
- **Cambiado:** la gráfica de la ventana usa el mismo estilo y colores de estado.
- **Añadido:** error de animación (métrica de PresentMon que compara el ritmo de la simulación del juego con el de la pantalla), en el registro cada 5 s y en el resumen de cada partida, cuando el juego lo permite.
- **Mejorado:** el texto del overlay solo se procesa en su propio rectángulo.

## 0.29.6
- **Corregido:** la app podía volver a detectar un juego en el instante en que se cerraba (su proceso aún presentaba algún fotograma); ahora ignora el mismo juego durante unos segundos tras cerrarse.
- **Corregido:** la app se excluye a sí misma de la detección de juegos por su nombre.
- **Mejorado:** limpieza del código (cabecera actualizada y comentarios de fases de desarrollo antiguas).

## 0.29.5
- **Cambiado:** la gráfica de ritmo muestra unos 3,3 segundos (cada fotograma, el triple de ancho) y la escala vertical se amplía un 50 %.
- **Corregido:** un límite con G-SYNC que funcionaba (por ejemplo, 225 clavado) podía no darse por aceptado, porque el limitador del driver deja pasar algún fotograma algo más rápido. Ahora se acepta si el ritmo coincide (±1,5 %) y menos del 10 % de los fotogramas va más de un 7 % por debajo del periodo.
- **Comprobado:** en la prueba A/B con Megabonk, el límite no redondo de 225 con G-SYNC dio una consistencia del 98,3 % (variación media de 0,33 ms), frente al 98,0 % (0,66 ms) del VSync propio del juego a 240.

## 0.29.4
- **Cambiado:** gráficas de ritmo más fieles a lo que se percibe: la línea muestra el valor típico de cada momento (mediana por columna) y los tirones reales se marcan aparte en rojo; la escala vertical tiene al menos 12 ms por encima de la referencia, para que la variación fina a FPS altos no parezca enorme.
- **Corregido:** sin límite aplicado, la línea de referencia es el ritmo actual del juego ("ritmo actual"), no el límite recomendado.

## 0.29.3
- **Corregido:** PresentMon podía no entregar datos (sin medición ni overlay) si quedaba abierta una sesión de captura de Windows (ETW) de una ejecución anterior. Ahora la app cierra su sesión al arrancar y al salir y, si PresentMon no entrega datos en 8 segundos, lo reinicia una vez.

## 0.29.2
- **Añadido:** el registro anota cuándo y por qué el overlay se muestra u oculta, para diagnosticar casos en que no aparece.

## 0.29.1
- **Corregido:** las gráficas de ritmo (overlay y ventana) colocaban los fotogramas según cuándo llegaban los datos de PresentMon, que vienen por tandas, y eso dibujaba marcas cada segundo que parecían picos. Ahora la línea de tiempo se reconstruye con la duración de los propios fotogramas: un ritmo perfecto se ve como una línea completamente plana.

## 0.29
- **Añadido:** identidad del proyecto: **FlatPace**. El nombre viene de la línea plana (*flat*) que forma la gráfica de tiempos de fotograma con un ritmo perfecto y del *frame pacing* (*pace*). Lema: *"La única línea plana que quieres ver"*.

## 0.28
- **Añadido:** métrica de consistencia: variación entre fotogramas reales consecutivos (porcentaje de cambios menores de 2 ms y variación media), como CapFrameX. En la ventana (*Ahora*), en el registro cada 5 s y en el resumen de cada partida.

## 0.27.1
- **Cambiado:** techo con G-SYNC/FreeSync proporcional a la frecuencia: `min(Hz − 3, Hz − Hz²/4096)` (240 Hz → 225, 144 Hz → 138).
- **Añadido:** documentación nueva (README, guía de uso, documentación técnica e historial).

## 0.27
- **Añadido:** interruptor "Usar G-SYNC / FreeSync cuando esté disponible".
- **Añadido:** detección de VRR para cualquier marca a partir de los tiempos de pantalla de PresentMon.
- **Añadido:** lectura del EDID: nombre del monitor, rango de frecuencias y rango FreeSync. Fila "VRR" en *Equipo*.

## 0.26
- **Eliminado:** prueba de nitidez (BFI), experimento del límite en vivo, regla de parada estadística y código sin uso (−15 % de código).
- **Mejorado:** los datos de los últimos minutos se ordenan una sola vez por segundo.

## 0.25.1
- **Corregido:** si el juego estuvo limitado por sí mismo la mayor parte de la partida, ese ritmo cuenta como demostrado en la decisión al cerrar.
- **Corregido:** ya no pide reiniciar si el límite ya estaba puesto al abrir el juego.

## 0.25
- **Cambiado:** el límite se decide **al cerrar el juego, con toda la partida**. Ya no hay que reiniciar a mitad de partida.
- **Añadido:** botón "Aplicar ya".
- **Añadido:** regla de fallos para límites aceptados (13 %, tramos de 5 s, 10 % de la partida, 1 minuto).

## 0.24.1
- **Corregido:** reconoce límites que no son divisores (G-SYNC) y evita la espiral descendente al medir con el propio límite puesto.

## 0.24
- **Añadido:** decisión con regla de parada estadística y umbral perceptible del 13 % (sustituida en 0.25).

## 0.23 – 0.23.1
- **Añadido:** detección de G-SYNC con NVAPI y límites sin divisores cuando actúa.
- **Añadido:** decisión más rápida con refinamiento silencioso del límite pendiente (sustituido en 0.25).

## 0.22
- **Añadido:** experimento del límite en vivo con la frecuencia de la pantalla (eliminado en 0.26).

## 0.21 – 0.21.1
- **Añadido:** prioridad Fluidez / Estabilidad.
- **Añadido:** límites aceptados: tras comprobarse, la app deja de recomendar para ese juego.

## 0.20 – 0.20.1
- **Añadido:** deshacer por juego y avisos para revisar límites.
- **Cambiado:** una sola decisión por partida.

## 0.19 – 0.19.3
- **Cambiado:** rediseño de la ventana al estilo SteamOS, con barra lateral.
- **Corregido:** aprendizaje del limitador con generación de fotogramas, alturas según la escala de pantalla y límites comprobados que se recalculaban.

## 0.18 – 0.18.1
- **Añadido:** interfaz en español e inglés.
- **Corregido:** los límites activos se juzgan por los tiempos reales; sin bajadas en cascada por tirones sueltos.

## 0.17
- **Añadido:** ajuste de la frecuencia de la pantalla por juego (monitores con varias frecuencias).

## 0.16 – 0.16.1
- **Añadido:** overlay en el juego con FPS, 1 % bajo, límite y gráfica de ritmo.
- **Corregido:** el trabajo de CPU inflado por Reflex.

## 0.15 – 0.15.1
- **Añadido:** límites con generación de fotogramas, sobre el ritmo en pantalla.
- **Añadido:** límite "ajustado" cuando no hay margen cómodo.

## 0.13 – 0.14
- **Añadido:** VSync del driver a fracciones del refresco y registro del modo de presentación.

## 0.10 – 0.12
- **Añadido:** banco de pruebas de BFI (eliminado en 0.26).

## 0.8 – 0.9.3
- **Añadido:** aplicación del límite en el perfil del driver de NVIDIA, verificación y deshacer.

## 0.2 – 0.7
- Medición con PresentMon, frecuencia real de la pantalla, recomendación de límites por divisores y detección de generación de fotogramas.
