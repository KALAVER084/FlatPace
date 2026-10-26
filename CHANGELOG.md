# Historial de cambios

Formato basado en [Keep a Changelog](https://keepachangelog.com/es-ES/1.1.0/). La versión más reciente va primero.

## 0.67.3
- **La gráfica de la comprobación ya no exagera las caídas.** Su escala vertical se ajustaba a la caída más profunda de cada partida y empezaba cerca de ella: una bajada del 1 % más lento ocupaba casi toda la altura y parecía peor de lo que era, y una partida no se podía comparar con otra. Ahora la escala es fija respecto al límite, del 50 % al 110 % (con 126, de 63 a 139): el umbral del 13 % queda siempre a la misma altura, y a la derecha se lee cuánto cae cada tramo (*"−13 %"*, *"−30 %"*, *"−50 %"*). No se empieza en 0 para no aplastar la zona entre el límite y el umbral, que es la que deciden las reglas.
- **La línea gris (el ritmo de cada segundo, lo habitual) es más gruesa,** y la del 1 % más lento (lo peor de cada momento), algo más fina, para que la vista no se vaya solo a lo peor.

## 0.67.2
- **Corregido:** en la leyenda de la comprobación, el signo menos de *"umbral: límite − 13 %"* salía como *"?"*: la fuente de la ventana no cargaba ese carácter. Ahora sí.

## 0.67.1
- **Las gráficas del desplegable de *Juegos* llevan leyenda:** una muestra de cada línea (continua, punteada o zona sombreada) y lo que significa, para no tener que leer el texto de encima. En la curva: la curva de la partida, la línea del 90 % o del 1 % más lento y el límite. En la comprobación: el ritmo de cada segundo, el 1 % más lento de los últimos 20 s, su parte bajo el umbral, el umbral (con su valor) y el cierre del juego, que no cuenta.
- **Corregido: el rojo de la comprobación empezaba tarde.** Un tramo de la línea solo se pintaba en rojo si sus dos extremos quedaban bajo el umbral, y el tramo que lo cruzaba salía entero en azul, también por debajo. Como la gráfica reduce la partida a unos 240 puntos, en las caídas bruscas ese tramo era largo. Ahora se parte en el punto de cruce.
- **La leyenda de *Ahora*** define el tirón completo: un fotograma de más de 2,5 veces lo normal **y al menos 8 ms más largo**.

## 0.67
- **El desplegable de *Juegos*, también con generación de fotogramas.** Hasta ahora, en esos juegos solo mostraba la fórmula. Ahora muestra la curva de *"de dónde sale"*, con los fotogramas reales de los tramos con la generación más habitual en la partida (los mismos con los que se decide el límite; los menús, que muchos juegos muestran sin generación, quedan fuera), y la gráfica de la comprobación con el ritmo real de cada segundo, marcada como informativa: con generación, las reglas no ajustan el límite.
- **El aviso de la generación que se activa y desactiva sola** dice ahora que lo más probable es que sean los menús (y que entonces no hay que hacer nada), antes que un modo dinámico del juego. En Control Resonant eran los menús, y el aviso anterior llevaba a pensar otra cosa.
- **Corregido:** la etiqueta *"no cuenta"* de la gráfica de la comprobación se salía por la derecha cuando la zona del cierre era estrecha (15 s).
- **README y guías:** cómo trata la app la generación de fotogramas y por qué aplica las fórmulas a los fotogramas reales. Aplicadas a las imágenes de la pantalla no darían lo mismo: el juego entrega el fotograma real y el generado casi a la vez (en Control Resonant, con 0,4 ms de separación) y, sin límite, aparecen en pantalla de forma muy irregular (unas a los 5 ms de la anterior, otras a los 22 ms); las fórmulas medirían esa irregularidad y habrían dado 45 en pantalla en lugar de 63. Y la latencia del PC ya no es *"aún sin ver en ningún juego"*: Control Resonant la entrega.

## 0.66.1
- **Corregido: PresentMon se reiniciaba al empezar algunas partidas.** La vigilancia lo da por colgado si nadie le envía datos durante 6 s. Hasta la 0.65.1, las imágenes del propio overlay y de la ventana contaban como datos; desde la 0.65.2 esas líneas se descartan antes de analizarlas (para ahorrar), y dejaron de contar: una carga de más de 6 s parecía un PresentMon colgado. Ahora la vigilancia mira si PresentMon sigue enviando líneas, de cualquier programa. El reinicio era casi inofensivo (unos segundos de datos al empezar), pero no tenía que ocurrir.
- **Corregido: el consumo del lector de PresentMon podía salir negativo** (y faltar el de PresentMon) si este se reiniciaba durante la partida: el hilo y el proceso nuevos restaban el tiempo de los antiguos. Ahora se suma lo que consumieron los anteriores.
- **El resumen dice de qué periodo es cada 1 % más lento:** *"1 % bajo con el límite (últimos 3 min)"*, y una línea nueva con el de toda la partida, el que usan las reglas desde la 0.66. Antes podían no coincidir (111 frente a 103 en una partida de Greylock) sin que se supiera por qué.
- **README:** al decidir, los tirones son los fotogramas de más de 2,5 veces lo normal (decía también *"y al menos 8 ms más largos"*, que es la definición de los tirones que se muestran); el tope de un escalón es para las bajadas por la fórmula de *Ritmo constante*; y se añaden el overlay a tirones sin límite (en *Limitaciones*) y que desactivar el overlay es lo que más ahorra.
- **README:** captura nueva de *Juegos*, con el desplegable de Greylock.
- **README y guías:** la regla de las dos partidas seguidas, explicada del todo: qué borra la racha (una partida limpia de al menos 3 minutos), que solo actúa con la prioridad *Automática* y un ejemplo con cifras.
- **Confirmado con datos: los tirones del overlay en partidas sin límite** venían de la GPU saturada por el juego (0,90 ms por dibujo sin límite, 0,19 ms con él). Solo ocurre en la primera partida de un juego o tras *Deshacer*; con el límite puesto va fluido.

## 0.66
- **Corregido: la regla del 1 % más lento creaba una espiral de bajadas.** Contaba los segundos en los que el 1 % más lento de los últimos 20 s quedaba más de un 13 % por debajo del límite, y esa ventana exageraba los tropiezos cortos: unos segundos lentos en una zona pesada dejaban el 1 % más lento hundido durante 20. En Greylock con 107, el juego estuvo bajo el umbral 36 s de 497 (un 7 %, que no llega a fallar), pero la ventana los convirtió en 79 y la regla lo bajó a 94; con 94 habría vuelto a pasar, y así hasta perseguir los peores segundos del juego (129 → 114 → 121 → 107 → 94). Además contradecía a la propia fórmula de *Ritmo constante*, que con esa misma partida daba 109: es decir, que 107 se sostenía. El fallo estaba en la regla desde la 0.48.1, pero solo se nota cuando una partida tiene varios tropiezos cortos repartidos; el cambio de la 0.65 (15 s al cerrar en lugar de un minuto) lo hizo más probable, porque dejó de descartar alguno de esos tropiezos.
- **Ahora se comprueba con la misma medida con la que se decide:** el 1 % más lento causado por el juego en toda la partida (sin el cierre ni los tirones). Con *Ritmo constante*, el límite se sostiene si 1,13 veces ese ritmo llega al 95 % del límite; si no, baja a lo que dice la fórmula, como mucho un escalón del 13 % por partida (una partida anómala, como una carga o la compilación de shaders, no hunde el límite de golpe). Con *Más FPS*, la partida cuenta como hundida si ese 1 % más lento queda más de un 13 % por debajo del límite; dos seguidas y el juego pasa a *Ritmo constante*, como antes.
- **Validado con las 26 partidas de Greylock con límite** (`tools/replay_steady.cpp`, con el mismo código que la app): las subidas malas a 125-130 de las primeras versiones se siguen detectando (bajarían a 102-113), y los límites de 107-114 se sostienen.
- **El desplegable de *Juegos*** muestra la regla nueva: el 1 % más lento de la partida, la fórmula y si el límite se sostiene. La línea azul de la gráfica (el 1 % más lento de cada 20 s) se queda como referencia de dónde están los tropiezos. Los registros guardados con versiones anteriores se siguen mostrando con su explicación de entonces.
- 558 comprobaciones automáticas.

## 0.65.2
- **El lector de PresentMon trabaja menos.** Con la medición completa de la 0.65.1, el overlay (1,3 % de un núcleo), la ventana (0,02 %) y el motor (1,7 %) sumaban un 3 %, y el proceso completo un 6,3 %: faltaba un 3,3 % sin medir. El candidato es el hilo que lee PresentMon. PresentMon vigila todos los programas del equipo (es lo que permite detectar los juegos), y el lector analizaba cada línea de cualquiera de ellos: la cortaba moviendo todo el texto que quedaba detrás, la dividía en unas 30 columnas creando un texto nuevo para cada una, y luego la mayoría se descartaban. Entre ellas, las 60 imágenes por segundo del propio overlay: por eso el consumo subió al pasar a 60. Ahora las líneas del propio FlatPace se descartan sin analizarlas (basta leer la columna del programa), el texto se corta una vez por bloque y las columnas reutilizan la memoria de una línea a la siguiente. Los datos que se obtienen son los mismos.
- **El resumen mide el consumo real de cada hilo** con los tiempos de procesador de Windows (ventana y overlay, motor y lector de PresentMon), y cuántas líneas por segundo envía PresentMon, para confirmar dónde estaba el resto.

## 0.65.1
- **El overlay sigue a 60 dibujos por segundo, con dos ahorros.** Con la 0.65 se veía muy fluido (56 dibujos por segundo, 0,29 ms cada uno), pero el proceso completo volvió a un 6,3 % de un núcleo: la medida de cada dibujo no incluía la preparación de los datos, y con 56 dibujos por segundo esa parte pesa. Ahora reafirma que está *"siempre encima"* una vez por segundo (antes, en cada dibujo, lo que obliga a Windows a revisar el orden de las ventanas), y en cada dibujo copia solo los fotogramas que dibuja la gráfica (sus últimos 3,3 s, con margen: 4 s), y los 11 s que necesita la oscilación solo al recalcular las cifras, 4 veces por segundo. Lo que se ve no cambia.
- **El resumen mide ahora todo el coste del dibujo:** el overlay, con y sin la preparación de los datos, y la ventana principal, con sus dibujos por segundo. Así se sabrá exactamente dónde se va el consumo, en lugar de estimarlo.

## 0.65
- **El cierre del juego ya no descuenta un minuto entero.** La regla de fallos y la del 1 % más lento dejaban fuera el último minuto de cada partida, para no contar los menús, la salida o un cuelgue al cerrar. Pero en 41 grabaciones, salir del juego llevó 8 segundos o menos en 9 de cada 10 partidas (la mediana fue 0, porque la medición ya se pausa al salir al escritorio): casi siempre se descartaba juego real. Ahora se descartan los últimos 15 segundos y, antes de ellos, el tramo final que vaya muy por debajo del límite (menos del 70 %), dure lo que dure: así se sigue descartando un cuelgue al salir, como los 70 s de Greylock a 52 fps con un límite de 110 que motivaron el minuto. Una zona pesada va algo por debajo del límite (un 75-87 %) y sí cuenta, aunque dure hasta el cierre. En la gráfica de la comprobación, la zona sombreada es ahora lo que se descartó de verdad.
- **El overlay, a 60 dibujos por segundo y regulares.** En la 0.64 debían ser 30, pero el temporizador normal de Windows, que despierta cada 15,6 ms, los dejaba en unos 18 e irregulares (31 y 47 ms alternados). Ahora un temporizador de alta precisión los espacia cada 16,7 ms. Para que cueste menos: las cifras (FPS, 1 % más lento, oscilación) se recalculan 4 veces por segundo en lugar de en cada dibujo, y la gráfica (también la de *Ahora*) solo calcula los tramos nuevos y reutiliza la memoria: preparar sus datos es unas 5 veces más barato (43 → 8,5 microsegundos por dibujo), con una prueba automática que comprueba que dibuja exactamente lo mismo que antes. El resto del coste de cada dibujo es entregar la imagen a Windows, que no cambia: el overlay costará algo más que en la 0.64, pero mucho menos que en la 0.63.
- 536 comprobaciones automáticas.

## 0.64
- **Menos consumo, sin cambiar ninguna decisión.** Al cerrar cada partida, el registro anotaba que FlatPace usaba un 7-9 % de un núcleo del procesador (el proceso completo, con la ventana y el overlay). Cuatro cambios:
  - **La ventana se dibuja solo cuando hace falta.** Antes se redibujaba unas 60 veces por segundo en primer plano y 10 detrás del juego, aunque no cambiara nada ni se viera. Ahora: a ~60 por segundo solo durante medio segundo tras usar el ratón o el teclado; a ~30 en *Ahora* con un juego en marcha (la gráfica avanza); una vez por segundo el resto del tiempo, que es lo que tarda el motor en publicar datos nuevos; y cada 2 segundos detrás de un juego a pantalla completa que la tapa. Entre dibujos, espera a que pase algo en lugar de despertarse cada 4 ms.
  - **El registro se copia solo con la página *Registro* abierta.** Antes, sus hasta 600 líneas se copiaban en cada dibujo de la ventana.
  - **El overlay, a 30 dibujos por segundo** (64 antes): la gráfica sigue viéndose fluida, y cada dibujo cuesta 0,4-0,7 ms.
  - **El 1 % más lento causado por el juego se calcula una vez por cada lote de fotogramas.** Se calculaba tres veces por segundo con los mismos datos (para el registro, la ventana y el overlay); ahora se guarda el resultado, que es idéntico.
- El efecto se puede comprobar en la línea *"Coste de FlatPace"* del resumen de cada partida.

## 0.63.2
- **Corregido: la gráfica de la comprobación solo mostraba el principio de la partida.** Los ajustes se leían línea a línea con un espacio fijo de 512 caracteres, y el registro de la última partida ocupa más (unos 1.700 en una partida de 3,5 minutos): se cortaba al leerlo, y también al guardar cualquier otro ajuste, que reescribe el archivo con lo leído. En Greylock se veían 53 segundos de 212, sin las caídas del 1 % más lento de los segundos 155 a 205. Ahora se leen las líneas completas, sea cual sea su longitud. Solo afectaba a los dos registros nuevos de la 0.63: los demás ajustes son mucho más cortos. El registro recortado se sustituye en la próxima partida de cada juego.
- **El eje de tiempo de esa gráfica usa la duración real de cada punto** (cada vuelta del motor dura algo más de un segundo), no un segundo supuesto.
- **Las etiquetas de la curva** se colocan junto al pie de su línea, donde no tapan la curva, y el último número del eje ya no se monta sobre *"FPS"*.

## 0.63.1
- **Corregido: la curva del desplegable no era el cálculo de la decisión.** Contaba segundos y quitaba los que quedan por debajo del rango de G-SYNC, pero la decisión con la partida completa usa los fotogramas reales sin los tirones (más de 2,5 veces lo normal): *Más FPS* toma el ritmo que alcanza el 90 % de ellos, y *Ritmo constante*, 1,13 veces el de su 1 % más lento. Por eso el límite no caía justo donde la curva cruzaba el 90 % (en Greylock, 118 frente a unos 121). Ahora la curva usa esos fotogramas y cruza el 90 % en el límite. Con la regla de fallos, que sí usa segundos, la curva es la de esos segundos. Los registros guardados con la 0.63 se siguen mostrando por segundos, con una nota.
- **Corregido el texto que lo explicaba:** decía que no cuentan los segundos por debajo del rango de G-SYNC; lo que no cuenta son los tirones. La misma imprecisión estaba en el README y en las guías (*Más FPS*: *"el 90 % del tiempo"*): es el 90 % de los fotogramas, sin contar los tirones.
- **Corregido:** el signo *≥* aparecía como *?* (la fuente de la ventana no lo tiene): ahora *"90 % de los fotogramas a 118 FPS o más"*. Las etiquetas de las líneas de la gráfica llevan fondo y no se montan sobre la curva. Y el apartado *Reglas* ya no sale vacío tras una decisión: dice que la comprobación aparecerá después de la próxima partida con ese límite.
- **README:** el desplegable también aparece en la lista detallada de *Cómo funciona*.
- 528 comprobaciones automáticas.

## 0.63
- **Nuevo: el porqué de cada límite, en *Juegos*.** Pulsando el nombre de un juego se despliega: la regla con la que se decidió su límite y la fórmula con sus números (por ejemplo, *"129 ÷ 1,13 = 114 FPS"* tras la regla del 1 % más lento); una gráfica de cuánto tiempo alcanzó cada ritmo la partida que lo decidió (el porcentaje de los segundos que llegaron a cada ritmo o más; con *Más FPS*, el límite está donde la curva cruza el 90 %); la última partida segundo a segundo, con el ritmo y el 1 % más lento frente al umbral del 13 % (el último minuto, que no cuenta, sombreado); y el resultado de cada regla (comprobado, tramos por debajo del umbral, si va sobrado, subidas bloqueadas 30 días). Con generación de fotogramas, sin G-SYNC o con un límite manual explica por qué las reglas de ajuste no actúan. Los límites decididos antes de esta versión muestran su detalle desde su próxima decisión.
- El motor guarda dos registros por juego (`explica|` y `partida|` en `equipo.txt`; formato en `src/core/explain.h`) que solo lee el desplegable: **ninguna decisión cambia en esta versión.**
- **Memoria de límites, preparada pero sin usar** (`src/core/limit_memory.h`, con sus pruebas, y `tools/replay_memory.cpp`, que la reproduce con grabaciones reales). Se estudió decidir cada límite con varias partidas en lugar de una: con las grabaciones de Greylock dio límites más estables, pero con *Ritmo constante* los bajaría unos 12 FPS para cubrir las zonas más pesadas, una diferencia por debajo de lo perceptible frente a unas caídas que la app actual ya corrige en una partida. Con un solo juego medido, no se cambia el núcleo de las decisiones; se activará si lo confirman más juegos.
- **README:** las bajadas por el 1 % más lento son de un escalón del 13 % (el texto decía que bajaban al ritmo del 90 % de los segundos, que es la otra regla), y se describe el desplegable de *Juegos*.
- 525 comprobaciones automáticas (antes 417).

## 0.62.3
- **Corregido: el aviso azul de la ventana no desaparecía nunca.** Por ejemplo, *"Greylock no sostuvo su límite el 60 % de la partida. La próxima vez usaré: 114 FPS…"* seguía visible dos partidas después de que el 114 quedara comprobado. Ningún sitio borraba el aviso; solo lo sustituía otro. Como todos hablan de la próxima partida (un límite nuevo, un cambio de prioridad, una instrucción para AMD o Intel), ahora se borra al empezar una partida.

## 0.62.2
- **La portada de GitHub, en inglés:** `README.md` es ahora el README en inglés, y el español pasa a `README.es.md` (antes, `README.md` era el español y `README.en.md` el inglés). Enlaces actualizados en los dos README y en las guías.
- **Fuentes citadas en el README:** la ley de Weber (Weber, 1846), el estudio del 13-15 % (Naylor-Lovell, Chalmers y Debattista, 2025) y el de la frecuencia de fotogramas (Denes, Jindal, Mikhailiuk y Mantiuk, 2020), con cita de autor y año donde se usa cada una y una lista de referencias con enlaces.
- **RTSS marcado como experimental y aún sin probar** en los README y las guías: el limitador con RivaTuner (0.57-0.59) está implementado, pero nunca se ha probado con RTSS en una gráfica AMD o Intel. En la tabla de compatibilidad, todo el soporte de AMD e Intel figura como aún sin probar.
- **README reorganizado para quien llega por primera vez:** la presentación, la imagen del overlay y un enlace a la descarga arriba; después el inicio rápido, un *Qué hace* corto (seis puntos de una línea), las capturas, los requisitos y las limitaciones. Toda la teoría (la regla del 13 % con sus referencias, cómo se elige, comprueba y ajusta el límite, los datos censurados, la lista detallada de funciones y por qué el modo automático solo funciona con NVIDIA) pasa al final, en secciones plegables, sin cambiar su texto.
- **Datos censurados explicados en el README,** en un párrafo propio: qué son (con la comparación del coche en una calle limitada a 50), y sus dos usos en la app: decidir si el juego va sobrado para subir el límite, y estimar su capacidad sin el límite con un modelo Tobit (solo informativa). Antes solo se explicaban en la documentación técnica.
- **Quitada la comparación con la Steam Deck** de la presentación del README: la Steam Deck ofrece un limitador y el ajuste de la frecuencia de la pantalla, pero el valor lo elige el usuario; FlatPace mide el juego, decide el límite, comprueba que funciona y lo ajusta.
- **README con capturas:** la comparación del overlay en Greylock sin límite (225 FPS irregulares, ±0,9 ms) y con el límite que eligió la app (129 FPS planos, ±0,1 ms), y una sección *Así se ve* con una partida, *Ahora* y *Juegos* (en `docs/img`, también en el paquete). Además: la interfaz y el registro están en dos idiomas, Metal Eden entre los juegos probados y la limitación de *Impr Pant*. Y la regla del 13 % explicada con la ley de Weber y el estudio del que sale (Naylor-Lovell, Chalmers y Debattista, *Virtual Reality*, 2025: un cambio del 13-15 % en la frecuencia de actualización de un objeto en movimiento se nota la mitad de las veces), con sus matices; la documentación técnica lo describía como un estudio de frecuencia de fotogramas, y se corrige. Revisadas además contra el código todas las cifras del README: coincidían salvo dos, que se corrigen. Las bajadas no son de un escalón del 13 % (solo las subidas): con G-SYNC, el límite baja al ritmo que el juego superó en el 90 % de los segundos, y sin VRR, al divisor siguiente. Y un tirón es un fotograma de más de 2,5 veces lo normal y, además, al menos 8 ms más largo. La sección *Cómo funciona* separa ahora qué dice la ley de Weber, cómo la aplica la app (siempre un 13 % del ritmo de ese momento, nunca milisegundos fijos; también en la banda de la gráfica) y qué cifras no salen de ella; antes decía que "todo FlatPace" se apoyaba en ella, lo que no era cierto.
- **Retirado: el arreglo de *Impr Pant* de la 0.61.1 no funcionaba.** Abrir la herramienta de recortes desde FlatPace tampoco funciona, probablemente porque Windows tampoco deja abrirla desde un programa ejecutado como administrador. Se quita para no dejar en el código una corrección que no corrige nada. *Win + Impr Pant* y *Win + Shift + S* sí funcionan con FlatPace delante, y se explica en el README.

## 0.62.1
- **Corregido (0.62):** con la app en inglés, cinco líneas del registro seguían en español: las columnas de PresentMon y el tipo de fotograma visto, que escribe el lector de PresentMon por otra vía, y los tres avisos del overlay al arrancar. Revisadas también las demás vías por las que un texto llega al registro.

## 0.62
- **Nuevo: el registro se escribe en el idioma de la app.** Hasta ahora, los mensajes del registro (en la sección *Registro* y en el archivo) estaban siempre en español. Los 192 mensajes con texto tienen ahora su versión en inglés, en una tabla aparte (`src/core/log_text.h`) que el registro consulta al escribir cada línea si la app está en inglés; si faltara alguna traducción, se escribe el mensaje en español, nunca se pierde una línea. También se traducen los fragmentos que se insertan en los mensajes (el tipo de equipo, el modo de presentación, el cuello de botella, el estado del overlay, los motivos de cada decisión, los errores del driver de NVIDIA, los avisos de los marcadores de Reflex). Una prueba automática nueva comprueba que cada traducción tiene los mismos huecos que su original y en el mismo orden (417 comprobaciones). Al cambiar de idioma, las líneas siguientes salen en el nuevo; las anteriores no cambian. El encabezado de las grabaciones de fotogramas sigue en español, porque lo leen los programas de análisis.
- **Corregido:** en *Detalles*, con la app en inglés, el cuello de botella aparecía en español (*"mixto"*).

## 0.61.2
- **Corregido: el overlay desaparecía al hacer una captura de pantalla.** La herramienta de recortes pasa a primer plano al abrirse, y la app entendía que el usuario había salido del juego: ocultaba el overlay y pausaba la medición. Ahora, las herramientas de captura conocidas (la de recortes de Windows, la Xbox Game Bar, ShareX, Greenshot y Lightshot) no cuentan como salir del juego, si el juego estaba delante cuando se abrieron.
- **Corregido (0.61):** en pausa, *Ahora* dejaba vacíos los FPS reales pero mostraba el 1 % más lento y los tirones; ahora muestra las tres cifras de la partida en curso. En *Equipo*, el VRR mezclaba idiomas y paréntesis (*"available (G-SYNC) (range 60-240 Hz, con LFC)"*): ahora se lee *"G-SYNC available; range 60-240 Hz, with LFC"*. El veredicto y la frase de debajo quedan más juntos, como en la maqueta.
- **El registro, a interlineado sencillo:** cada línea ocupaba el doble de lo necesario, porque usaba el espacio entre elementos de la interfaz.

## 0.61.1
- **Corregido: la tecla *Impr Pant* no hacía nada con FlatPace en primer plano.** En Windows 11 abre la herramienta de recortes, pero la gestiona el Explorador, que se ejecuta sin permisos de administrador, y Windows no le deja ver el teclado mientras una ventana de administrador tiene el foco (FlatPace lo necesita por PresentMon y el driver de NVIDIA). La ventana de FlatPace sí recibe la tecla, así que ahora abre ella la herramienta de recortes. *Win + Shift + S* funcionaba ya.

## 0.61
- **Rediseño de *Ahora*.** Durante una partida: el nombre del juego y un **veredicto en una frase** (*"Va estable a 114 FPS."*, o *"…con caídas en las zonas pesadas"* si el 1 % más lento queda más de un 13 % por debajo; *"Midiendo sin límite."*; *"Comprobando el límite…"*), con el límite explicado debajo; la **gráfica de ritmo a todo lo ancho**, con su leyenda; tres cifras con nombres claros (FPS reales, FPS en el 1 % más lento, tirones en esta partida) en lugar del número gigante y las cuatro métricas; **solo la acción que toca** (la recomendación pendiente del juego con su botón, también la urgente en rojo, o *Aplicar ya*; *Límite manual…* y *Deshacer* quedan en *Juegos*); y los ***Detalles* plegados**, ahora con contenido real (G-SYNC, presentación, qué limita al juego, variación del ritmo y pantalla).
- **Nuevo: *Ahora* sin juego abierto** dice qué hacer y resume la última partida (*"Fue estable a 114 FPS durante 4 minutos, con 2 tirones…"*), con un botón para verla en *Juegos*.
- **Corregido:** sin juego abierto, *Ahora* mostraba las cifras y la gráfica de la última partida como si siguiera en marcha; *"Consistencia"* se cortaba contra la gráfica; *Detalles* salía casi vacía (sus valores se colocaban fuera de su sitio, por mezclar coordenadas de pantalla y de ventana); los botones del límite quedaban fuera de la ventana; y al cambiar de sección, la página podía empezar desplazada hacia abajo, con el título cortado.
- **Coherencia visual en toda la app:** el límite se describe con palabras (*"114 FPS en el driver de NVIDIA, con G-SYNC, GPU al máximo y baja latencia"*), sin la cadena de puntos medios, también en *Juegos* y en el registro; los botones de opciones (prioridad, limitador) tienen el mismo ancho en todas las páginas; las etiquetas de estado en mayúsculas desaparecen de la ventana (se mantienen en el overlay del juego, donde funcionan como etiqueta compacta); y el azul queda reservado al ritmo plano: sin un límite comprobado, la gráfica usa un tono neutro en lugar del rojo, porque medir sin límite no es un error.
- La propuesta de diseño (maquetas y logo) está en el lienzo *FlatPace — rediseño*.

## 0.60
- **Nuevo logo**, con la misma idea: una *f* trazada como una gráfica (de un nivel bajo a uno alto, como un ritmo que se estabiliza), cruzada por la línea plana azul, que ahora **pasa por delante sin cortarse**: el trazo blanco se interrumpe a su alrededor. El ritmo plano nunca se corta. En la barra lateral y en el icono (`flatpace.ico`, con cada tamaño, de 16 a 256 píxeles, dibujado por separado: por debajo de 24 píxeles no hay hueco y los trazos son más gruesos, para que se siga leyendo).

## 0.59
- **Cambiado: con NVIDIA, todo vuelve a ser automático, sin opciones de limitador.** La sección *Limitador* solo aparece con AMD o Intel (*Solo recomendar* o RivaTuner). Con NVIDIA, la app usa siempre el driver, con G-SYNC, "GPU al máximo" y la baja latencia, como hasta la 0.56. Usar RTSS con NVIDIA obligaba a perder esos ajustes del driver o a construir un modo combinado, y no aportaba nada que el driver no haga ya. Si quedaban límites en RTSS de la 0.58, la app los devuelve al driver al arrancar.

## 0.58
- **Nuevo: el limitador se elige en *Juegos*,** encima de la prioridad, cuando hay algo que elegir: *Driver de NVIDIA* o *Solo recomendar* (AMD e Intel, el modo guiado), y *RivaTuner (RTSS)* si está instalado. Con AMD e Intel, la app empieza en *Solo recomendar* y **nunca usa RTSS sin que lo elijas** (en la 0.57 lo usaba en cuanto lo encontraba): RTSS entra en los juegos, y hay quien lo tiene para otras cosas. Sustituye al ajuste oculto `limitador=rtss`, que sigue funcionando.
- **Al cambiar de limitador, los límites se mueven:** cada uno se deshace con el limitador que lo aplicó y se aplica el mismo valor con el nuevo, para comprobarlo de nuevo en la siguiente partida. El VSync por intervalos, que solo existe en el driver de NVIDIA, pasa a un límite de FPS del mismo valor.
- **Con *Solo recomendar*, la prioridad explica su papel:** decide qué límite se recomienda; lo aplica el usuario. Los botones de prioridad siguen activos, porque *Ritmo constante* y *Más FPS* recomiendan límites distintos.
- **Corregido (0.57):** con AMD o Intel y RTSS en uso, la app mostraba las instrucciones del modo guiado y lo trataba como tal en *Deshacer* y en la comprobación del límite: dependía de que la gráfica fuera AMD o Intel, no del limitador en uso.

## 0.57
- **Nuevo: límites automáticos con RivaTuner (RTSS) para AMD e Intel.** Sin NVIDIA, si RivaTuner Statistics Server está instalado, FlatPace aplica los límites ella misma en el perfil de cada juego de RTSS, con su biblioteca `RTSSHooks64.dll` (cargar el perfil, cambiar `FramerateLimit`, guardar y pedir a RTSS que recargue), como hacen otras herramientas de código abierto. No se incluye ningún archivo de RTSS ni se usa ningún kit con licencia restrictiva. *Deshacer* restaura el límite que tenía el perfil del juego o borra el que creó la app, y cada cambio se deshace con la pieza que lo aplicó (también en *Deshacer cambios.bat*). La app avisa si RTSS no está abierto, y recuerda activar el VSync del driver con G-SYNC/FreeSync. Sin RTSS, se usa el modo guiado, que ahora sugiere instalarlo. Con `limitador=rtss` en `equipo.txt` se puede usar también con NVIDIA, para probarlo. Sin "GPU al máximo", la regla del 1 % bajo no actúa, como en el modo guiado. Aún sin probar.

## 0.56.1
- **Modo guiado más completo:** la instrucción de cada juego recuerda también activar el VSync del driver cuando hay FreeSync o Adaptive-Sync (en AMD Software, *Esperar a la actualización vertical: siempre activado*), que es la combinación recomendada para no salirse del rango del VRR y la que la app activa sola con NVIDIA; y la opción de baja latencia de cada fabricante (Radeon Anti-Lag en AMD), en lugar del modo de baja latencia de NVIDIA.
- **Más prudencia en modo guiado:** sin "GPU al máximo", la GPU se relaja con el límite puesto y el 1 % bajo puede parecer peor de lo que es, que es lo que causaba la espiral de bajadas de las primeras versiones. Hasta tener datos de AMD e Intel, en modo guiado la regla del 1 % bajo no baja límites ni cambia la prioridad; la regla de la mediana, que no depende de cómo trabaje la GPU, sigue actuando.
- *Deshacer* ya no se destaca en azul cuando la fila solo muestra la instrucción del modo guiado.

## 0.56
- **Nuevo: modo guiado para AMD e Intel.** Sin NVIDIA, con una gráfica AMD o Intel, *Aplicar límites* ya no se desactiva: la app mide el juego, decide el límite y, en lugar de aplicarlo en el driver, muestra en la fila del juego qué poner y dónde (AMD Software: Radeon Chill con mínimo y máximo iguales; Intel Graphics Software: su limitador de FPS; o el limitador del propio juego). Al abrir el juego, comprueba que funciona midiendo el ritmo, como con NVIDIA: si el juego va claramente por encima del límite, avisa de que aún no se ha aplicado; con *"Límite funcionando"*, la instrucción desaparece. *Deshacer* recuerda quitar el límite también en el software de la gráfica. Sin VSync por intervalos, "GPU al máximo" ni baja latencia, que son ajustes del driver de NVIDIA. Aún sin probar en esas gráficas. Es una pieza más del limitador común de la 0.53 (`GuidedBackend`).
- **Decidido: el modo automático solo con NVIDIA.** Los kits oficiales de AMD (ADLX) e Intel (IGCL) tienen licencias que no encajan con un proyecto MIT de código abierto (ADLX prohíbe redistribuir sus archivos y exige un contrato de usuario que prohíba copiar el programa; IGCL limita su uso a plataformas Intel e incluye una cláusula de indemnización). El README lo explica.
- **README reescrito:** sin el lema, con una sección *Cómo funciona* que explica la regla del 13 %, cómo se elige, comprueba y ajusta cada límite, con sus tolerancias, y otra sobre por qué el modo automático solo funciona con NVIDIA.
- **Corregido:** al confirmarse un límite (*"Límite funcionando"*), la fila del juego no se actualizaba hasta el siguiente cambio. Y *Deshacer* no limpiaba del todo las recomendaciones de la 0.55 en la ventana.

## 0.55.1
- **Corregido: el registro anunciaba una y otra vez el límite de la propia app.** El detector de límites ajenos (el de un juego, por ejemplo) veía el límite que había aplicado FlatPace y, como en las zonas pesadas aparece y desaparece, llenaba el registro de *"Nuevo límite activo: ~120 fps"* y *"Ya no hay un límite activo"*, a veces con dos explicaciones distintas. Ahora, si el límite detectado coincide con el de la app (con un margen del 5 %, porque el detector lo ve redondeado), no se anuncia ni aparece en las líneas de estado. Los límites ajenos se siguen anunciando igual. No cambia ninguna decisión.
- **Comprobado en el portátil de pruebas:** la reorganización del limitador para AMD e Intel (0.53) mantiene el funcionamiento con NVIDIA: un límite decidido al cerrar una partida se aplica y se confirma (*"LÍMITE FUNCIONANDO"*). Y las medidas de la 0.51 aparecen en un resumen real: los momentos lentos de Greylock los limita la CPU, y la CPU no baja de frecuencia en ellos.

## 0.55
- **Cambiado: la app solo hace cambios por su cuenta en modo automático.** Con la prioridad general *Automática* (la predeterminada) y en los juegos sin prioridad propia, la app sube y baja los límites y pasa a *Ritmo constante* los juegos inestables, como hasta ahora. Si eliges tú la prioridad (*Ritmo constante* o *Más FPS*, general o de un juego), la app no cambia nada: cada cambio que convendría se muestra como **recomendación** en la fila del juego, con su motivo y un botón para aplicarlo (*Subir a…*, *Bajar a…* o *Usar «Ritmo constante»*). Antes, las bajadas eran siempre automáticas. Una recomendación de bajar aparece en rojo si es urgente: con el VSync por intervalos (sin G-SYNC), un límite que no se sostiene cae al escalón siguiente y empeora el movimiento. Con G-SYNC, un límite que el juego no alcanza deja de actuar en las zonas pesadas sin empeorar nada, así que esperar a que decidas es seguro. Las recomendaciones se conservan al reiniciar la app, con su texto (antes solo se recuperaba el botón), y la de bajar se retira sola si en una partida posterior el juego sostiene su límite. La primera decisión de un juego nuevo se sigue aplicando en todos los modos.
- **Eliminado: la sección *Ajuste de límites*** (*Automático* / *Solo sugerir*): su función queda cubierta por la prioridad. Tenía dos botones "Automático" en la misma página que significaban cosas distintas.
- *Deshacer* ya no se destaca en azul cuando la fila tiene una recomendación: el botón importante es el de la recomendación.

## 0.54
- **Cambiado: las prioridades tienen nombres nuevos y la general tiene tres opciones.** *Fluidez* pasa a llamarse **Más FPS** y *Estabilidad*, **Ritmo constante** (en inglés, *More FPS* y *Steady pace*): "Fluidez" era engañoso en una app cuya idea central es que un ritmo constante se siente más fluido que unos FPS más altos pero irregulares. La prioridad general ofrece ahora **Automática** (por defecto: cada juego empieza con *Más FPS* y la app pasa a *Ritmo constante* los inestables), **Ritmo constante** y **Más FPS** (las dos últimas, para todos los juegos y sin cambios automáticos), igual que el botón de cada juego. Hasta ahora, elegir *Fluidez* significaba en realidad el modo automático, y la versión fija solo existía como ajuste oculto (`prioridad-auto=no`). Las preferencias guardadas se convierten solas: *Fluidez* pasa a *Automática* (o a *Más FPS* si tenía `prioridad-auto=no`), y *Estabilidad* a *Ritmo constante*. Los valores internos de `equipo.txt` no cambian. En el historial anterior se conservan los nombres de su momento.

## 0.53.1
- **Corregido:** en inglés, la flecha del botón de prioridad de cada juego tapaba la última letra de *"Auto · Smoothness"*: el ancho del botón solo tenía en cuenta las etiquetas de *Stability*. Ahora usa la más larga de las cuatro posibles.

## 0.53
- **Reorganizado, sin cambios de comportamiento:** primer paso para el soporte de límites en AMD e Intel. El motor ya no construye los ajustes del driver de NVIDIA: pide el límite de forma neutra (`LimitRequest`: el valor del limitador, el VSync, el VSync por intervalos, "GPU al máximo" y la baja latencia), y una pieza por fabricante lo traduce a su driver a través de una interfaz común (`LimitBackend`). Por ahora solo existe la de NVIDIA, que envuelve el código de siempre. Una prueba automática nueva comprueba, en todas las combinaciones, que la traducción produce exactamente los mismos ajustes que las versiones anteriores (222 comprobaciones). Las declaraciones de NVAPI compilan también fuera de Windows, para las pruebas.

## 0.52
- **Nuevo: aviso cuando G-SYNC no se está usando**, en *Ahora* y en *Juegos*, con la causa, los pasos para activarlo y un botón que abre el Panel de control de NVIDIA. Cubre dos casos: **portátil en modo híbrido** (la gráfica integrada controla la pantalla y G-SYNC no puede funcionar; en el portátil de pruebas, la opción *Automatic Selection* de PredatorSense arrancó así tras una actualización del driver, y la app siguió decidiendo límites sin G-SYNC sin que se notara) y **G-SYNC desactivado en el panel** en una pantalla donde funcionaba antes. Para no dar falsas alarmas en monitores que no lo admiten, la app recuerda en qué pantallas lo ha visto funcionar (`gsync-visto|pantalla`, por el nombre de la pantalla, que no cambia entre modos). La app no cambia el modo por su cuenta: no hay una forma pública de hacerlo, depende de cada fabricante, requiere reiniciar y afecta a todo el equipo.
- **Nuevo: los juegos cuyo límite se decidió con G-SYNC lo indican en su fila** mientras G-SYNC no esté activo: en una pantalla de refresco fijo, ese límite puede empeorar el movimiento en lugar de mejorarlo.
- El aviso antiguo del modo híbrido, en *Equipo*, ahora dice que se pierde G-SYNC (antes hablaba solo de "mejor pacing").

## 0.51.3
- **Nuevo: vigilancia de PresentMon a mitad de partida.** El PresentMon 2.6.0 que distribuye FlatPace tiene fallos conocidos con las opciones que usa (`--track_frame_type`, `--track_pc_latency`): entre otros, puede dejar de entregar datos al desactivar la generación de fotogramas en un juego que emite eventos de tipo de fotograma (issue 695 de PresentMon; CapFrameX distribuye una versión propia corregida, y Intel aún no ha publicado una oficial). Hasta ahora, FlatPace solo reiniciaba PresentMon si no entregaba datos al arrancar, y si se bloqueaba a mitad de partida se quedaba en pausa indefinidamente. Ahora, con el juego en primer plano y sin ningún dato durante más de 6 segundos, lo reinicia (como mucho una vez por minuto) y lo anota en el registro.
- **Investigado: por qué la latencia del PC no aparece en Space Marine 2.** PresentMon calcula la latencia del PC con los eventos *PC Latency Stats* del SDK de Reflex, que incluyen el tramo desde la entrada hasta el inicio del fotograma. Space Marine 2 emite los marcadores de inicio de simulación (los que FlatPace usa para separar fotogramas reales), pero en las grabaciones la latencia llega siempre vacía. Pendiente de comprobar si falta el tramo de la entrada.

## 0.51.2
- **Nuevo, solo en el registro:** en las partidas con generación de fotogramas, si el movimiento con ritmo plano está descompasado en un 10 % o más, el resumen explica la causa: la generación de fotogramas, no el límite (en Space Marine 2, en la misma partida, un 37 % con ella y un 0,5 % sin ella), y sugiere probar el juego sin ella si el movimiento se nota irregular. No cambia ningún ajuste ni ninguna decisión.

## 0.51.1
- **Corregido: al desactivar la generación de fotogramas en mitad de una partida, todos los fotogramas se contaban como generados.** FlatPace distingue los fotogramas reales por los marcadores de Reflex que emiten juegos como Space Marine 2, y una vez que confiaba en ellos lo hacía para siempre; al desactivar la generación, el juego dejó de emitirlos y la app perdió todos los fotogramas reales. Ahora, tras más de 4 fotogramas seguidos sin marcador (con generación nunca hay más de 3 generados seguidos), deja de confiar en ellos y vuelve a hacerlo cuando reaparecen. Arregla también los tramos de menús, cargas y escenas de las partidas con generación, donde el juego tampoco los emite.
- **Medido: el movimiento descompasado de Space Marine 2 lo causa la generación de fotogramas.** En una misma partida, alternando: con generación, el 37,3 % de los fotogramas de ritmo plano tienen el movimiento descompasado (con 56 reales, por el límite de 112 en pantalla); sin ella, el 0,5 % (con 77 reales).
- **Medido: en el portátil de pruebas, "GPU al máximo" le quita energía a la CPU** (en Greylock, con el mismo límite, la CPU rinde un 10 % menos con él: 123-129 % de su frecuencia nominal frente a 136-138 %). Pero quitarlo no compensa: separando bien las zonas (con una ventana centrada; un primer análisis con una ventana hacia atrás mezclaba las zonas y daba una conclusión errónea), sin él el 1 % bajo mejora un 5 % en las zonas pesadas (de 84,8 a 88,9 FPS) y empeora un 9 % en las ligeras (de 112,7 a 103,0), porque la GPU se relaja. Se descartó decidirlo por juego: "GPU al máximo" se mantiene siempre.

## 0.51
- **Nuevo: diagnóstico de los momentos lentos en el resumen.** Con el trabajo de CPU y GPU de cada fotograma, el resumen dice qué limita los momentos lentos (con límite, los fotogramas más de un 13 % por encima de su tiempo; sin él, el 10 % más lento) y da el consejo que corresponde: si los limita la CPU (en el 70 % o más), *"bajar los ajustes gráficos no los acelerará; los que cargan la CPU sí (población, física, distancia de visión…)"*; si los limita la GPU, bajar la resolución o los gráficos. En las grabaciones, los momentos lentos de Greylock, Megabonk y Space Marine 2 los limita la CPU (82-100 %); los de Control Resonant, ambas (sin consejo).
- **Nuevo: rendimiento de la CPU durante la partida** (contador de Windows *% Processor Performance*: el porcentaje de su frecuencia nominal, una vez por segundo), en el resumen (mediana, mínimo, y en los segundos lentos frente al resto) y en la grabación (`cpu_rendimiento_pct`). Si la CPU limita los momentos lentos y baja de frecuencia justo en ellos más de un 10 %, el resumen lo avisa: posible calor o límite de energía. En un portátil, CPU y GPU comparten energía y temperatura.
- La app usa una librería más de Windows (`pdh`, contadores de rendimiento); actualizados CMake y las instrucciones de compilación.

## 0.50.3
- **Nuevo: el error de animación se compara con la partida sin límite**, igual que la variación del ritmo: el resumen muestra *"X % de fotogramas con más de 1 ms (sin límite: Y %)"*. Es una segunda medida, independiente de la gráfica, de que el límite mejora el movimiento (en Greylock, del 39 % sin límite al 7 % con él). La referencia se guarda de las partidas sin ningún límite y sin generación de fotogramas (`movimiento-antes|juego.exe`).
- **Nuevo: movimiento con ritmo plano** en el resumen: el porcentaje de fotogramas de ritmo perfectamente plano cuyo movimiento está descompasado de forma perceptible (error de animación mayor que el 13 % del fotograma). Es algo que la gráfica no puede mostrar. En las grabaciones: Greylock a 130 y Megabonk a 225, ~2 %; Greylock sin límite, 16 %; **Space Marine 2 y Control Resonant con generación de fotogramas, 17-18 %**, aunque su gráfica sea plana. Un umbral del 5 % se descartó: a 225 FPS marcaba errores de 0,2 ms, imperceptibles.
- **Medido: el modo de baja latencia no cambia nada en Greylock**, porque sus zonas pesadas las limita la CPU (8,8 ms por fotograma frente a 4,7 de la GPU) y la cola de render no llega a llenarse (sin el ajuste, la espera sube solo a ~1,9 ms). Se mantiene activado: su efecto se espera en los juegos limitados por la GPU.

## 0.50.2
- **Corregido: `baja-latencia=no` y `rendimiento-maximo=no` no desactivaban nada en los límites existentes.** Solo impedían añadir esos ajustes a los límites nuevos; los que ya los tenían en el perfil del driver los conservaban. Ahora, al arrancar, la app los devuelve a su valor original (como *Deshacer*) en los límites que los tenían, y lo anota en el registro. Con el VSync por intervalos, la cola de un fotograma forma parte del método y se mantiene.
- **Primer dato real de la espera en la cola de la GPU** (Greylock a 130 FPS, 3 minutos sin zonas pesadas): mientras el límite se sostiene, la cola está vacía (mediana de 0,18 ms), como predecía la simulación.

## 0.50.1
- **Corregido: los ajustes avanzados escritos como `clave=valor` se ignoraban.** La documentación los mostraba así (`baja-latencia=no`, `overlay=clasico`…), pero la app solo leía la clave, una tabulación y el valor. Ahora acepta ambas formas, con o sin espacios alrededor del `=`, y al guardar normaliza las líneas al formato de la app.
- **Nuevo: limpieza de ajustes antiguos.** Al arrancar, la app quita de `equipo.txt` las claves que ya no usa ninguna versión (`memoria|`, `memoria2|`, `ritmo-antes|`, `prioridad-v2`, `aprendizaje-generacion` y `vrr`) y lo anota en el registro.

## 0.50
- **Nuevo: modo de baja latencia con cada límite** (cola de render de un fotograma, *"Modo de baja latencia: Activado"* del panel de NVIDIA). Una simulación de la cadena de latencia con los tiempos de Greylock mostró que, mientras el juego sostiene el límite, FlatPace ya reduce mucho la latencia del PC (de 24 a 13,7 ms, porque vacía la cola de render), pero en las zonas donde el juego no llega al límite la GPU se satura, la cola se llena y la latencia se triplica (40 ms). Con la cola de un fotograma baja a 20 ms en esas zonas, sin coste en FPS. Hasta ahora FlatPace solo lo aplicaba con el VSync por intervalos. Se añade al arrancar a los límites existentes, se deshace con *Deshacer*, aparece en *Juegos* como *"· baja latencia"* y se desactiva con `baja-latencia=no`. En los juegos con Reflex, Reflex tiene prioridad (su limitador, dentro del juego, sigue siendo el de menor latencia).
- **Nuevo: espera en la cola de la GPU** (`MsGPULatency` de PresentMon) en la grabación (`espera_gpu_ms`) y en el resumen de cada partida: mide cuánto espera cada fotograma antes de que la GPU lo empiece, también en juegos sin Reflex, para comprobar con datos reales el efecto del modo de baja latencia.

## 0.49.1
- **Nuevo: prioridad por juego elegible en su fila de *Juegos*.** Un botón muestra la prioridad que se aplica (*"Auto · Fluidez"*, *"Auto · Estabilidad"*, *"Fluidez"* o *"Estabilidad"*) y despliega tres opciones: **Automática** (la global como punto de partida; la app puede pasar el juego a *Estabilidad* si resulta inestable), **Siempre Fluidez** y **Siempre Estabilidad** (la app no las cambia). Se aplica desde la siguiente partida y no recalcula el límite actual; *Deshacer* sigue sirviendo para empezar de cero (y conserva la elección fija). Los límites manuales no lo muestran.
- **Corregido:** la descripción del límite de cada juego no se ajustaba al ancho disponible y, con varios botones en la fila, pasaba por debajo de ellos. Comprobado dibujando las filas reales con Dear ImGui en ambos idiomas.
- **Corregido:** la ventana y el motor escriben en el mismo archivo de ajustes; dos escrituras a la vez podían perder una. Ahora están protegidas.

## 0.49
- **Nuevo: prioridad por juego.** La prioridad global es el punto de partida de cada juego; con *Fluidez*, si un juego resulta inestable con su límite (el 1 % bajo causado por el juego se hunde de forma sostenida en dos partidas seguidas, con la regla de fallos aplicada al 1 % bajo), la app lo pasa a *Estabilidad*, lo anota en el registro y lo muestra en su fila de *Juegos* (*"· Estabilidad (elegida por la app)"*). Desde ese cierre actúa la regla de *Estabilidad*, que baja el límite partida a partida hasta uno que el juego sostenga también en sus zonas pesadas. Solo en esa dirección: con un límite que ya se sostiene, el juego parece estable precisamente porque el límite es el correcto, y volver a *Fluidez* lo subiría otra vez; con *Estabilidad*, un juego que va sobrado ya sube solo. La inestabilidad se mide con el límite puesto, porque sin límite se exagera (Megabonk varía mucho sin límite y es el juego más estable con él). Comprobado con las grabaciones: Greylock falla en sus tres partidas con *Fluidez* (16-33 % del tiempo), y Megabonk nunca. *Deshacer* lo olvida; `prioridad-auto=no` en `equipo.txt` lo desactiva. 193 pruebas automáticas.
- **Cambiado:** un cambio de la prioridad global se aplica desde la siguiente partida de cada juego (antes, también a la recomendación en directo de la partida en curso).

## 0.48.3
- **Cambiado: *Fluidez* es la prioridad por defecto y la recomendada** (antes, *Estabilidad*). Con las reglas validadas con partidas reales, *Fluidez* da límites más altos con caídas perceptibles alrededor del 1 % del tiempo, y con las subidas automáticas llega antes a lo que el juego sostiene. Solo afecta a instalaciones nuevas: una prioridad ya guardada se respeta. En la ventana, *Fluidez* aparece primero.

## 0.48.2
- **Corregido: la regla del 1 % bajo (0.48.1) solo baja límites con *Estabilidad*.** Aplicada también con *Fluidez*, convertía esa prioridad en *Estabilidad* partida a partida (en Greylock: 125 → 110 → 97), aunque *Fluidez* promete justo lo contrario: el ritmo que el juego alcanza la mayor parte del tiempo, aceptando caídas en los momentos más exigentes (a 125, la mediana se sostuvo el 100 % del tiempo). Con *Fluidez*, un límite falla solo si cae la mediana. En ambas prioridades, "va sobrado" sigue exigiendo que el 1 % bajo no se haya hundido.
- **Nuevo: el último minuto antes de cerrar el juego no cuenta para la regla de fallos.** Ahí suelen estar los menús, la salida o, como en una partida de Greylock, un cuelgue en el que el juego siguió presentando imágenes a ~52 FPS durante 50 segundos: para FlatPace es indistinguible de una zona lenta. (En esa partida no cambió la decisión, pero en una más corta podría haberlo hecho.)

## 0.48.1
Correcciones a partir de la primera subida automática real (Greylock, 111 → 125 FPS).
- **Corregido: una subida que no se sostiene ya se detecta aunque la mediana se mantenga.** La regla de fallos solo vigilaba la mediana del ritmo, y en Greylock la mediana nunca cayó a 125 FPS, pero en las zonas pesadas el 1 % bajo causado por el juego se hundió más de un 13 % por debajo del límite un 34-48 % del tiempo (a 111, un 2,5 %). La subida venía de una partida que no pasó por esas zonas. Ahora, en los límites que la app ajusta sola (con G-SYNC/FreeSync y sin generación de fotogramas), el mismo criterio se aplica también al 1 % bajo causado por el juego: si se hunde de forma sostenida (tramos de al menos 5 s que sumen un minuto y el 10 % de la partida), la app vuelve al límite anterior, o baja un escalón si no lo hay, y anota el techo. "Va sobrado" exige además que el 1 % bajo no se haya hundido. Comprobado con las grabaciones: a 111 se sostiene; las dos partidas a 125 fallan.
- **Corregido: el modo *Solo sugerir* no llegaba a mostrar el botón *Aplicar sugerencia*** (0.46): la lógica de las subidas solo se activaba en modo *Automático*. Ahora funciona en ambos modos, y solo la subida en sí depende del modo.
- **Corregido: la referencia "antes" de la variación se guardaba también en partidas con un límite ya aceptado** de una sesión anterior (en Greylock, ±0,07 ms con el límite de 111). Ahora solo se guarda sin ningún límite, y las referencias guardadas hasta ahora se descartan (se vuelven a tomar en la próxima partida sin límite).

## 0.48
- **Nuevo: la latencia del PC en el overlay,** en la fila de cifras, junto al 1 % bajo: *"latencia 24 ms"*. Con un límite fijado y sin generación de fotogramas, se compara con la latencia del juego sin límite, con el mismo estilo que la variación: *"latencia 18 ms (antes 22)"*. La referencia "antes" se guarda de las partidas sin límite y sin generación (`latencia-antes|juego.exe`). Solo en juegos con marcadores de Reflex/PC Latency; en los demás no aparece.
- **Cambiado:** la cifra *"pantalla X"* solo se muestra cuando aporta: sin límite fijado, o si el ritmo en pantalla se aleja más de un 13 % del límite; si no, repetía lo que ya dice la etiqueta del límite.
- **La fila de cifras se ajusta al espacio:** si no cabe junto a la etiqueta del límite, se quitan elementos por orden de importancia (el "(antes)", "pantalla" y la latencia); el 1 % bajo se mantiene siempre. Comprobado dibujando el overlay real en cuatro situaciones, incluida la etiqueta más larga.

## 0.47
- **Nuevo: medición de la latencia del PC** en los juegos con marcadores de Reflex/PC Latency (como Space Marine 2 o Control Resonant), con la medida `MsPCLatency` de PresentMon: desde que el juego lee la entrada hasta que la imagen sale hacia la pantalla. El resumen de cada partida da su mediana y su 1 % peor, por separado con el límite funcionando y sin él, y la grabación de fotogramas incluye la columna `latencia_pc_ms`. Sirve para comprobar si el límite de un juego mejora o empeora su latencia; el ratón y el monitor añaden lo mismo en ambos casos, así que no hace falta hardware para comparar.

## 0.46
- **Nuevo: interruptor "Ajuste de límites"** en *Juegos*, debajo de la prioridad: **Automático** (por defecto: los límites suben solos cuando el juego va sobrado) o **Solo sugerir** (la app lo avisa y la fila del juego muestra el botón **Aplicar sugerencia**). Antes, la única forma de elegir era editar `equipo.txt`.
- **Aplicar sugerencia** sube el límite aunque el juego no esté abierto (por el mismo camino que *Fijar…*), pero lo deja como límite automático y con las mismas protecciones que las subidas automáticas: si no se sostiene, vuelve al límite anterior y guarda el techo. La sugerencia se conserva al reiniciar la app y se descarta si el límite actual falla o se deshace.
- **En ambos modos, las bajadas son automáticas:** un límite que el juego no sostiene siempre empeora el ritmo. Para control total sobre un juego sigue estando *Fijar…*.
- **Corregido:** el texto de la sección de prioridad decía que los límites aplicados "no cambian solos", algo que ya no era cierto; ahora dice que cambiar la prioridad no los recalcula.

## 0.45
- **Nuevo: los límites suben solos cuando el juego va sobrado.** Las reglas eligen el límite con una partida sin límite, y eso exagera lo que el juego necesita: muchos juegos rinden más con un límite puesto (Megabonk sostenía 225 FPS cuando las reglas daban 189-215). Ahora, con G-SYNC/FreeSync y sin generación de fotogramas, si el juego fue sobrado **toda la partida** (al menos 3 minutos con el límite, ni un tramo por debajo y más del 98 % de los fotogramas esperando al limitador), el límite sube al siguiente escalón: un 13 % más, sin pasar del máximo de la pantalla. Con *Fluidez* basta una partida así; con *Estabilidad*, dos seguidas. El usuario no tiene que hacer nada.
- **Protecciones contra el ping-pong:** si una subida no se sostiene, la app vuelve al límite anterior (que sí se sostenía) y guarda el que falló como techo durante 30 días, sin volver a subir hasta él. *Deshacer* borra ese estado; `subir-solo=no` en `equipo.txt` desactiva las subidas. Con límites manuales, o fuera de su alcance, se mantiene la sugerencia.
- **Validado con las grabaciones:** Megabonk a 195 FPS (sobrado: sube a 220) y Greylock a 83, 90 y 100 (sin subida: el juego va justo en sus zonas pesadas). Simulación de dos meses alternando zonas ligeras y pesadas: como mucho una partida fallida por cada caducidad del techo, sin bucle de subir y bajar.
- 182 pruebas automáticas, incluidas las de las protecciones (se comprobó que detectan cuatro errores introducidos a propósito).

## 0.44
- **Nuevo: overlay dibujado con la GPU (DirectComposition).** El overlay clásico se dibujaba con el procesador, píxel a píxel, y se entregaba con `UpdateLayeredWindow`, que obliga a Windows a copiar todos los píxeles en cada actualización (en Space Marine 2, 1,28 ms por dibujo, ~5 % de un núcleo). Ahora lo dibuja la GPU con Dear ImGui (un contexto propio, en el mismo dispositivo que la ventana principal) en una ventana sin superficie propia (`WS_EX_NOREDIRECTIONBITMAP`), cuya imagen, con transparencia premultiplicada, compone Windows directamente. Mismo diseño, colores, posiciones y comportamiento; sigue sin recibir clics.
- **Respaldo automático:** si el overlay de la GPU no se puede iniciar, o falla durante la partida (por ejemplo, al reiniciarse el driver), la app vuelve sola al overlay clásico y lo anota en el registro. Con `overlay=clasico` en `equipo.txt` se usa siempre el clásico.
- **Comprobado fuera de Windows:** el código de dibujo del overlay nuevo, extraído tal cual, se ejecutó con datos de partidas reales (Space Marine 2 y Control Resonant) y se rasterizó a imagen: mismo diseño que el clásico, incluidos el relleno degradado, los tirones en ámbar y la banda de tolerancia. La parte de DirectComposition solo se puede comprobar en Windows.

## 0.43.4
Revisión de autoría del código, sin cambios de comportamiento (el programa compilado es idéntico byte a byte, salvo el número de versión).
- **Cambiado:** las declaraciones de NVAPI que usa FlatPace (tipos, estructuras, códigos de estado e identificadores de ajustes y funciones del driver, reproducidos de los encabezados públicos de NVIDIA, licencia MIT) estaban dentro de `main.cpp`, mezcladas con código propio. Ahora están en `third_party/nvapi/nvapi_min.h`, con su aviso de autoría, igual que Dear ImGui en `third_party/imgui`: todo el código de terceros queda en `third_party/`. Los identificadores de las funciones del driver tienen ahora nombre (por ejemplo `nv::fn::DRS_SetSetting`) en lugar de números sueltos.
- **Documentación:** la técnica y los README indican dónde está cada parte de terceros, y los tamaños del código están al día.

## 0.43.3
Revisión del código y la documentación, sin cambios de comportamiento.
- **Código:** eliminados restos sin uso (la lista de horas de llegada de las imágenes, que dejó de usarse con la corrección de la cifra en pantalla; dos métodos sin uso; un parámetro que ya siempre valía lo mismo); corregidos tres comentarios que describían valores anteriores (el reloj de reproducción, la prioridad de PresentMon y el criterio de la memoria); un nombre de variable más claro.
- **Documentación:** la guía explica la generación intermitente y que, con generación, no se calculan la capacidad ni las sugerencias, y responde cuánto consume FlatPace (con lo medido en las pruebas). Los README mencionan "GPU al máximo", la grabación de fotogramas y el bajo consumo, y el inglés vuelve a coincidir con el español (le faltaban dos líneas y tenía otro orden de secciones).

## 0.43.2
- **Corregido (borde derecho de la gráfica):** el tramo más reciente se dibujaba antes de estar completo, con una clasificación provisional que se recalculaba en cada dibujo: podía aparecer como pico con los primeros fotogramas y volver a la línea al completarse (en modo en pantalla, los 3 últimos tramos, que esperan a sus vecinos para la media). Ahora solo se dibujan los tramos completos: la línea termina unos píxeles antes (como mucho ~75 ms de datos), pero nada de lo que se dibuja cambia después.
- **Corregido (borde izquierdo):** el valor de los tramos que se salen de la línea se recalculaba mientras sus fotogramas más antiguos salían de la ventana de datos, así que un tirón podía encoger justo antes de desaparecer. Ahora el valor se fija al completarse el tramo, junto con su clasificación.
- **Añadido:** dos pruebas automáticas que comprueban que nada de lo dibujado cambia en ninguno de los dos bordes (166 comprobaciones).

## 0.43.1
- **Corregido:** la cifra de imágenes en pantalla del overlay (*"pantalla 118"*) parpadeaba con cada tanda de datos: contaba las imágenes **llegadas** en los últimos 5 s, y como llegan en tandas de ~1 s, la cuenta saltaba al entrar o salir una tanda de la ventana (en Space Marine 2, entre 93 y 132 con un límite de 112). Ahora se calcula con la duración real de las imágenes, como los FPS reales: entre 111,7 y 112,0.

## 0.43
Los picos que se veían en la gráfica con generación de fotogramas, al ritmo de los refrescos, venían del propio dibujo, no del juego. Se encontraron simulando el overlay completo con su código real y las horas de llegada de una partida grabada de Space Marine 2.
- **Corregido (desplazamiento):** el reloj de reproducción dejaba muy poco margen: antes de cada tanda de datos llegaba casi al último dato, frenaba, y al llegar la tanda aceleraba. El avance por dibujo variaba entre 12,4 y 20,8 ms (debería ser constante: 16,7 ms a 60 dibujos por segundo). Ahora deja 0,6 s de margen (antes 0,35; la gráfica va ~0,25 s más retrasada) y corrige con suavidad (entre 0,95 y 1,05×): de 15,8 a 17,5 ms.
- **Corregido (modo en pantalla):** con generación, cada tramo de la gráfica (~25 ms) tiene solo ~3 imágenes, y es normal que alguna dure algo más que otra. Con las reglas de los fotogramas reales, un 11 % de los tramos se marcaban como tirón (una sola imagen de más de 1,5×) y la línea solo era recta un 73 % del tiempo. Ahora, en pantalla, cada tramo vale la duración media de las imágenes en ~125 ms, y solo es tirón una imagen de más de 2,5× (como los TIRONES del registro): línea recta un 94 % del tiempo y un 0,2 % de tirones, sin esconder caídas reales.

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
