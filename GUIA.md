# Guía de uso de FlatPace 0.66.1

Esta guía describe la versión actual. Los cambios entre versiones están en el [historial](CHANGELOG.md).

## 1. Instalación

1. Descomprime la carpeta donde quieras. Contiene:
   - `FlatPace.exe`: el programa.
   - `PresentMon-2.6.0-x64.exe`: la herramienta de Intel que mide los fotogramas (debe estar en la misma carpeta).
   - `Deshacer cambios.bat`: quita todos los límites aplicados, sin abrir la app.
2. Abre `FlatPace.exe`. Windows pedirá permisos de administrador.
3. Si aparece "Windows protegió su PC", pulsa *Más información* → *Ejecutar de todas formas*.

No hace falta instalar nada ni cambiar ajustes de Windows. Para desinstalar, borra la carpeta y, si quieres, `%LOCALAPPDATA%\FlatPace` (pulsa antes "Deshacer todos los cambios").

## 2. Qué pasa al abrir la app

1. **Mide la frecuencia real de tu pantalla** durante 5 segundos (por ejemplo, 239,986 Hz aunque Windows diga 240). Si el equipo está ocupado y la medición sale ruidosa, usa la última medición buena.
2. **Revisa tu equipo:** gráficas, pantalla, driver de NVIDIA, frecuencias disponibles y si G-SYNC está disponible.
3. **Espera a que abras un juego.** Lo detecta solo, aunque ya estuviera abierto, y empieza a medir cuando lo pones en primer plano.

## 3. La ventana

La barra lateral tiene cuatro secciones:

- **Ahora:** durante una partida, el juego y un **veredicto en una frase** (*"Va estable a 114 FPS."*, *"Midiendo sin límite."*, *"Comprobando el límite…"*), con el límite explicado debajo; la **gráfica de ritmo** a todo lo ancho (azul cuando el límite está comprobado, tirones en ámbar); tres cifras (FPS reales, FPS en el 1 % más lento y tirones); solo la acción que toca (una recomendación pendiente con su botón, o *Aplicar ya*); y los *Detalles* plegados (G-SYNC, presentación, qué limita al juego, variación del ritmo y pantalla). Sin juego abierto, qué hacer y un resumen de tu última partida. La gráfica se conserva cuando cambias a la app.
- **Juegos:** con AMD o Intel, el limitador (*Solo recomendar* o RivaTuner, si está instalado; con NVIDIA todo es automático y no aparece), la prioridad general (sección 5), los juegos con límite, *Deshacer todos los cambios* y *Elegir juego*. Cada juego muestra su límite y sus botones: el de su **prioridad**, *Fijar…*, *Deshacer* y, si la app te recomienda algo, el botón para aplicarlo. **Pulsando el nombre de un juego se despliega el porqué de su límite:** la regla y la fórmula con sus números, una gráfica de cuánto tiempo alcanzó cada ritmo la partida que lo decidió, la última partida segundo a segundo frente al umbral del 13 % (el cierre del juego, que no cuenta, aparece sombreado) y el resultado de cada regla (si se comprobó, los tramos por debajo del umbral, si va sobrado y las subidas bloqueadas). Los límites decididos antes de la 0.63 lo muestran desde su próxima decisión.
- **Equipo:** gráfica, pantalla, driver y estado del VRR con el rango de frecuencias del monitor.
- **Registro:** todo lo que la app hace y mide, con el botón para abrir el archivo de registro y la casilla **Grabar los datos de cada fotograma**, que guarda un archivo CSV por partida en la carpeta `grabaciones` (botón *Abrir grabaciones*), útil para analizar el ritmo con detalle. Si el juego entrega los datos necesarios (algunos juegos con Reflex), el resumen de cada partida incluye además la **latencia del PC** (desde que el juego lee tu acción hasta que la imagen sale hacia la pantalla), con el límite y sin él. En los juegos probados hasta ahora todavía no ha aparecido. El resumen también dice **qué limita los momentos lentos** (la CPU o la GPU), con el consejo que corresponde: si es la CPU, bajar los gráficos no ayudará, pero sí los ajustes que la cargan (población, física, distancia de visión). Y avisa si la CPU baja de frecuencia justo en esos momentos, algo frecuente en portátiles por calor o energía.

Al pie de la barra: **Aplicar límites** (si está apagado, la app solo mide), **Overlay en el juego** (la app recuerda tu elección) e idioma (**ES | EN**).

## 4. El ciclo de un juego

| Partida | Qué hace FlatPace | Qué haces tú |
|---|---|---|
| **Primera** | Mide toda la partida. Al cerrar el juego, decide y guarda el límite: *"Límite para la próxima partida de …"* | Jugar con normalidad |
| **Segunda** | Comprueba que el juego va al ritmo del límite: **"Límite funcionando"**. El límite queda **aceptado** | Nada |
| **Siguientes** | Mide y vigila. Al cerrar, si el juego no sostuvo el límite de forma continuada, lo baja; si fue **sobrado** toda la partida, lo **sube** un 13 % (sin pasar del máximo de tu pantalla). Con una prioridad elegida a mano, en lugar de cambiarlo te lo recomienda | Nada, o aplicar las recomendaciones |

**¿Lo quieres antes?** En *Ahora*, el botón **Aplicar ya** guarda la recomendación del momento. Reinicia el juego para que el driver lo aplique. Al cerrar, la app lo afina con toda la partida.

**¿Cuándo se baja un límite aceptado?** Solo si el juego fue más de un 13 % por debajo del límite, en tramos de al menos 5 segundos, durante al menos el 10 % de la partida y un minuto en total. Las bajadas más cortas se tratan como eventos aislados. El cierre del juego no cuenta: ni los últimos 15 segundos ni, antes de ellos, un tramo final muy por debajo del límite (menos del 70 %), que es un cuelgue o un menú lento al salir.

**¿Cuándo se sube?** Con G-SYNC/FreeSync y sin generación de fotogramas, si el juego fue **sobrado** toda la partida: al menos 3 minutos con el límite, ni un tramo por debajo (ni de la mediana ni del 1 % bajo) y casi todos los fotogramas esperando al limitador. Con *Más FPS* basta una partida así; con *Ritmo constante* hacen falta dos seguidas. Las reglas eligen el límite con prudencia, y muchos juegos rinden más con un límite puesto que sin él, así que la app va subiendo poco a poco hasta lo que el juego sostiene de verdad. Si una subida no se sostiene (con *Ritmo constante*, aunque solo sea en las zonas pesadas), vuelve al límite anterior y no lo vuelve a intentar durante 30 días. **¿Y si prefieres decidir tú?** La app solo cambia los límites por su cuenta con la prioridad **Automática** (sección 5) y en los juegos sin prioridad propia. Si eliges tú la prioridad, general o de un juego, la app no cambia nada: cuando un juego va sobrado, no sostiene su límite o le conviene *Ritmo constante*, te lo **recomienda** en su fila, con el motivo y un botón para aplicarlo (*Subir a…*, *Bajar a…* o *Usar «Ritmo constante»*). Si una recomendación de bajar se debe a que, sin G-SYNC, el límite está empeorando el juego, aparece en rojo. Y si quieres control total sobre un juego, usa *Fijar…*.

## 5. Prioridad: Automática, Ritmo constante o Más FPS

En *Juegos* hay dos formas de decidir el límite, y la prioridad elige entre ellas:

- **Más FPS:** el límite que alcanzan **el 90 % de los fotogramas** (sin contar los tirones), aceptando caídas breves en los momentos más exigentes. En las pruebas, las caídas perceptibles fueron de alrededor del 1 % del tiempo.
- **Ritmo constante:** un límite que el juego sostiene incluso en sus peores momentos **sin bajar de forma perceptible** (su 1 % más lento llega como mucho un 13 % tarde, una diferencia que no se nota). Es el ritmo más regular y la línea más plana en la gráfica, a veces con algunos FPS menos.

La prioridad general tiene tres opciones:

- **Automática** (por defecto, recomendada): la app ajusta cada juego por su cuenta. Empieza con *Más FPS* y, si el juego resulta inestable con su límite (su 1 % más lento queda más de un 13 % por debajo del límite en dos partidas seguidas; una partida limpia de al menos 3 minutos borra la racha, así que una sola zona pesada no basta), lo pasa a *Ritmo constante*; además, sube o baja cada límite según lo que el juego sostiene (sección 4). Así, los juegos estables aprovechan al máximo tu equipo y los inestables mantienen un ritmo constante. La prioridad solo cambia en esa dirección, para no alternar entre las dos; *Deshacer* lo olvida.
- **Ritmo constante** o **Más FPS:** la misma para todos los juegos, y tú tienes el control: la app no cambia nada por su cuenta y te **recomienda** en la fila de cada juego lo que convendría cambiar.

Cada juego puede tener la suya con el botón de prioridad de su fila, que muestra la que se aplica (*"Auto · Ritmo constante"*, por ejemplo) y permite elegir entre la general, **Siempre ritmo constante** o **Siempre más FPS**. Con una prioridad propia, ese juego queda también bajo tu control: la app solo te recomienda.

Con G-SYNC/FreeSync, ninguna cuenta los **tirones** (fotogramas de más de 2,5 veces lo normal y al menos 8 ms más largos): ningún límite los arregla. Sin VRR, por ahora se mantienen las reglas anteriores.

Los límites ya aceptados no cambian al cambiar la prioridad: deshaz el juego para recalcularlo con el criterio nuevo.

## 6. Límite manual

Si prefieres un valor concreto (por ejemplo, 120 en un juego que la app pondría más alto, o 220 en uno que sabes que tu equipo sostiene), puedes ponerlo tú:

- **Juego de la lista:** botón *Fijar…* en su fila, en *Juegos*.
- **Juego abierto:** botón *Límite manual…* en *Ahora*.

Escribe el valor y pulsa *Aplicar*; luego reinicia el juego si estaba abierto. La ventana te avisa si el valor no encaja: con G-SYNC, si supera el máximo recomendado de tu pantalla; sin G-SYNC, si no divide sus Hz (y te sugiere los valores que sí encajan). Con generación de fotogramas, el valor es lo que se ve en pantalla.

La app **no cambiará nunca** un límite manual: solo comprobará que funciona (el overlay se pone azul) y, al cerrar el juego, te avisará si no lo sostuvo. Para volver al modo automático, pulsa *Deshacer*.

## 7. G-SYNC, FreeSync y pantallas sin VRR

- **Con NVIDIA, cada límite va acompañado de "GPU al máximo"** (*Preferir rendimiento máximo* en el perfil del juego): con un límite, el driver bajaría las frecuencias de la GPU hasta ir justa y no llegaría a tiempo en las escenas pesadas, que empeorarían el 1 % bajo. La GPU sigue protegiéndose sola por temperatura y potencia. En los portátiles le resta algo de energía a la CPU, pero en las pruebas compensó. Se deshace junto con el límite.
- **Y del modo de baja latencia** (*Modo de baja latencia: Activado*): si el juego no llega al límite y la GPU no da abasto, evita que los fotogramas se acumulen esperando y añadan retraso. Cuando el límite se sostiene no cambia nada.
- **Con VRR actuando**, el límite se ajusta al ritmo exacto que el juego sostiene (por ejemplo 116 o 122), siempre por debajo del techo de tu pantalla (225 en una de 240 Hz, 138 en una de 144 Hz). La app activa además el VSync del driver, la combinación recomendada con G-SYNC.
- **Se tiene en cuenta el rango de VRR del monitor** (leído de su EDID): si su rango VRR acaba por debajo de su frecuencia, el límite queda por debajo del final del rango; y si un juego va por debajo del mínimo del rango en un monitor que no puede duplicar fotogramas (sin LFC), la app usa un divisor de los Hz. *Equipo* muestra el rango y si hay LFC.
- **Sin VRR**, el límite divide exactamente los Hz reales (240, 120, 80, 60…), para que cada imagen dure lo mismo en pantalla.
- **Todo es automático:** no hay que activar nada. Si tu pantalla parpadea con VRR (pasa en algunas OLED y VA) y prefieres los divisores siempre, hay un ajuste avanzado descrito en la documentación técnica.
- La app detecta si el VRR **actúa de verdad** con cada juego: con NVIDIA preguntando al driver; con otras marcas, analizando cuándo aparece cada imagen en pantalla.

## 8. Generación de fotogramas (DLSS Frame Generation)

- La app separa los fotogramas reales de los generados. El límite se expresa como *"122 en pantalla (61 reales)"*.
- Los fotogramas reales nunca bajan de 30: por debajo, la latencia es mala.
- Si la generación se activa o desactiva (muchos juegos lo hacen en menús), la app vuelve a medir. Si el juego la activa y desactiva solo con frecuencia (un modo dinámico o automático), la app lo avisa una vez en *Juegos* y deja de anotar cada cambio: fijar ese modo en el juego da un ritmo más constante.
- Con generación, la gráfica muestra las imágenes **en pantalla** (sección 9), y la app no calcula la capacidad del juego ni sugiere subir el límite, porque los fotogramas reales cambian de ritmo con la generación.
- La primera vez, aprende sobre qué actúa el limitador del driver en tu equipo (lo que se ve en pantalla o los fotogramas reales).
- **La generación puede descompasar el movimiento aunque la gráfica esté plana:** en Space Marine 2, más de un tercio de los fotogramas con ritmo perfecto tenían el movimiento descompasado con generación, y casi ninguno sin ella. Ningún límite lo corrige. El resumen de cada partida lo mide (*"Movimiento con ritmo plano"*) y, si es alto, lo explica; si notas el movimiento irregular, prueba el juego sin generación.

## 9. Overlay en el juego

En una esquina del juego: FPS, 1 % bajo, una etiqueta con el estado del límite y la gráfica de ritmo de los últimos 3 segundos.

- **Azul ("FIJADO 225 fps"):** el límite está confirmado; el juego va a su ritmo.
- Si el juego entrega los datos necesarios (algunos juegos con Reflex; en los probados hasta ahora, todavía no), la fila de cifras incluye la **latencia del PC** (*"latencia 24 ms"*). Con un límite fijado y sin generación de fotogramas, la compara con la del juego sin límite: *"latencia 18 ms (antes 22)"*. Si la fila no cabe, se quitan primero los datos secundarios.
- **Rojo ("SIN CONFIRMAR" o "SIN LÍMITE"):** el juego aún no tiene límite, o lo tiene pero todavía no se ha comprobado.
- La gráfica muestra las **desviaciones** del ritmo: mientras el tiempo de los fotogramas está dentro de la tolerancia (±13 % del objetivo, indicada en la etiqueta: *"objetivo 8,3 ms ±1,1"*), la línea es recta; solo se dibuja lo que se sale. Se aplica igual con y sin límite, así que la diferencia que ves es real. Los **tirones** aparecen en ámbar.
- **Con generación de fotogramas**, la gráfica dibuja las imágenes **en pantalla** (lo que ves), y la etiqueta lo indica (*"en pantalla 13,9 ms ±1,8"*). Si el juego activa y desactiva la generación por su cuenta, la app lo avisa en *Juegos*.
- Abajo a la izquierda, la **variación** del ritmo (±ms). La primera partida sin límite de cada juego queda como referencia, y con el límite confirmado verás la comparación: *"variación ±0,1 ms · antes ±0,4"*.
- Con el límite confirmado, una **banda azul** muestra esa tolerancia alrededor del objetivo.
- Con un límite activo, el **1 % bajo** descuenta los retrasos del propio limitador (fotogramas que salen algo tarde aunque el juego ya los tenía listos): muestra lo que causa el juego. El resumen de cada partida en *Registro* muestra los dos valores. Si la línea queda por encima del objetivo, el juego no llega del todo al límite: un límite algo más bajo dará al limitador margen para aplanarla.
- Se actualiza hasta 60 veces por segundo y se desplaza de forma continua, con un pequeño retraso que se adapta solo (los datos de PresentMon llegan por tandas; con generación de fotogramas, el retraso puede llegar a unos 2 segundos).

Es una ventana propia y transparente: no toca el juego. La dibuja la GPU, con un coste casi nulo para el procesador; si en tu equipo no funcionara, la app vuelve sola al overlay clásico (y con `overlay=clasico` en `equipo.txt`, dentro de `%LOCALAPPDATA%\FlatPace`, se usa siempre el clásico; ver los ajustes avanzados en la [documentación técnica](TECNICO.md#8-ajustes-avanzados)).

## 10. Avisos y deshacer

- **Avisos en ámbar** (en *Juegos* y en *Ahora*): el juego no sostiene su límite, va claramente mejor que cuando se decidió (por ejemplo, tras bajar ajustes), la generación de fotogramas está al revés que cuando se aplicó, o, con un límite **manual**, **podría sostener uno más alto** (*"Va sobrado con su límite…"*): la app no cambia los límites manuales, así que solo te lo sugiere. Los límites automáticos suben solos (sección 4).
- **En el registro** verás además la capacidad estimada con el límite puesto (*"sin límite ~X fps"*), las **fases** de la partida (cambios de zona o de ajustes) y, en el resumen, la **memoria del juego**: lo que sostiene según todas tus partidas.
- **Deshacer un juego:** botón *Deshacer* en su fila. Vuelve exactamente a como estaba; si "Aplicar límites" está activado, se recalcula en la próxima partida.
- **Deshacer todo:** *Deshacer todos los cambios* (la app pasa a solo medir) o doble clic en `Deshacer cambios.bat`.

## 11. Otras funciones

- **Elegir juego:** para juegos en ventana que no se detectan solos, escribe el nombre del `.exe`.
- **Ajustar la frecuencia de la pantalla (monitores sin VRR):** si tu monitor ofrece varias frecuencias (por ejemplo 100, 120 y 144 Hz) y otra permite claramente más FPS estables, la app la pone al abrir ese juego y la restaura al cerrarlo. Es un cambio temporal que no modifica la configuración de Windows.

## 12. Comparar límites con la consistencia

Para saber qué límite da el ritmo más regular en un juego, compara partidas de unos minutos en la misma zona:
1. Juega con un límite y anota, del resumen de la partida en *Registro*, la línea *"consistencia: X % de cambios < 2 ms"*.
2. En *Juegos*, pulsa *Deshacer* para ese juego, cambia el criterio (por ejemplo, la prioridad) y repite.
3. El límite con mayor porcentaje, y menos tirones, es el más regular.

## 13. Preguntas frecuentes

**La app dice "reinicia el juego".** El driver aplica los límites al abrir el juego. Ciérralo y ábrelo; la app lo comprueba sola. A veces basta con cambiar una opción gráfica que reinicie el renderizado (por ejemplo, el modo de DLSS).

**Un juego va peor que antes.** En *Juegos*, pulsa *Deshacer* en su fila y reinicia el juego. Envía el registro si quieres que se investigue.

**¿Puede banearme un anticheat?** FlatPace no entra en los juegos: solo cambia el perfil del juego en el driver de NVIDIA, igual que el Panel de control de NVIDIA.

**¿Consume muchos recursos?** No debería notarse en tus juegos. FlatPace y PresentMon funcionan con prioridad baja y en el modo de eficiencia de Windows 11, así que el juego siempre tiene preferencia y, en procesadores con núcleos de eficiencia, FlatPace usa esos. En las pruebas con un portátil, entre los dos usaron alrededor de un 10 % de un núcleo, casi todo en los de eficiencia. El resumen de cada partida en el *Registro* indica el coste real en tu equipo.

**¿Por qué el límite no es igual a mis FPS?** La app elige el ritmo que el juego sostiene en sus momentos exigentes, no en los tranquilos, para que el ritmo sea constante.

**¿Dónde están los registros?** Junto al programa, como `registro-fecha-hora.txt`, y en la sección *Registro*.

**La app dice que G-SYNC no se está usando.** En un portátil, lo más habitual es que esté en **modo híbrido**: la gráfica integrada controla la pantalla y G-SYNC no puede funcionar. Pasa con opciones como *Automatic Selection*, que eligen el modo en cada arranque, o tras actualizar el driver. Elige «Solo GPU NVIDIA» en el Panel de control de NVIDIA (*Administrar modo de pantalla*) o en la app de tu portátil, y reinicia. Después, pulsa *Deshacer* en los juegos cuyo límite se decidiera mientras G-SYNC no funcionaba, para que se recalculen.

**Tengo una gráfica AMD o Intel.** FlatPace empieza en ***Solo recomendar*** (el modo guiado). Si tienes **RivaTuner Statistics Server (RTSS)** instalado (viene con MSI Afterburner), puedes elegirlo como limitador en *Juegos*, encima de la prioridad (es experimental: está implementado, pero aún no se ha probado con una gráfica AMD o Intel): la app aplicará los límites ella misma en el perfil de cada juego de RTSS (déjalo abierto mientras juegas, y con FreeSync activa también el VSync del driver). Al cambiar de limitador, los límites de tus juegos se mueven al nuevo. En *Solo recomendar*: mide el juego, decide el límite y lo comprueba, pero lo aplicas tú. En la fila de cada juego, en *Juegos*, verás exactamente qué poner y dónde (en AMD Software, Radeon Chill con mínimo y máximo iguales; en Intel Graphics Software, su limitador de FPS; o el limitador del propio juego), y si conviene activar también el VSync del driver (con FreeSync) y la opción de baja latencia. Al abrir el juego, la app comprueba que funciona: si aún no lo has aplicado, te lo recuerda, y cuando aparece *"Límite funcionando"*, la instrucción desaparece. Si la app recomienda otro límite más adelante, la fila mostrará el valor nuevo. El modo automático solo existe con NVIDIA por las licencias de los kits de AMD e Intel (en el [README](README.es.md) se explica el motivo).

**Voy a actualizar el driver de NVIDIA.** Una actualización normal conserva los perfiles de los juegos, y con ellos los límites. Una *instalación limpia* los borra: pulsa antes *Deshacer todos los cambios*, y la app volverá a medir cada juego.
