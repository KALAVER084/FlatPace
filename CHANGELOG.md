# Historial de cambios

Formato basado en [Keep a Changelog](https://keepachangelog.com/es-ES/1.1.0/). La versión más reciente va primero.

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
