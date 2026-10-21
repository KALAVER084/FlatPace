# Guía de uso de FlatPace 0.44

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

- **Ahora:** el juego actual y la etiqueta de estado de su límite (*FIJADO* en azul; *SIN CONFIRMAR* o *SIN LÍMITE* en rojo); una tarjeta con los FPS reales, cuatro métricas (1 % bajo, consistencia, tirones y variación del ritmo) y la **gráfica de ritmo**; y dos tarjetas: *Límite* (aplicado, recomendado y estado, con *Aplicar ya*, *Límite manual…* y *Deshacer*) y *Detalles* (VRR, presentación, límite del juego, cuello de botella y pantalla). La gráfica se conserva cuando cambias a la app.
- **Juegos:** la prioridad, los juegos con límite (cada uno con sus botones *Fijar…* y *Deshacer*), *Deshacer todos los cambios* y *Elegir juego*.
- **Equipo:** gráfica, pantalla, driver y estado del VRR con el rango de frecuencias del monitor.
- **Registro:** todo lo que la app hace y mide, con el botón para abrir el archivo de registro y la casilla **Grabar los datos de cada fotograma**, que guarda un archivo CSV por partida en la carpeta `grabaciones` (botón *Abrir grabaciones*), útil para analizar el ritmo con detalle.

Al pie de la barra: **Aplicar límites** (si está apagado, la app solo mide), **Overlay en el juego** (la app recuerda tu elección) e idioma (**ES | EN**).

## 4. El ciclo de un juego

| Partida | Qué hace FlatPace | Qué haces tú |
|---|---|---|
| **Primera** | Mide toda la partida. Al cerrar el juego, decide y guarda el límite: *"Límite para la próxima partida de …"* | Jugar con normalidad |
| **Segunda** | Comprueba que el juego va al ritmo del límite: **"Límite funcionando"**. El límite queda **aceptado** | Nada |
| **Siguientes** | Solo mide y vigila. Al cerrar, si el juego no sostuvo el límite de forma continuada, lo baja para la próxima vez | Nada |

**¿Lo quieres antes?** En *Ahora*, el botón **Aplicar ya** guarda la recomendación del momento. Reinicia el juego para que el driver lo aplique. Al cerrar, la app lo afina con toda la partida.

**¿Cuándo se baja un límite aceptado?** Solo si el juego fue más de un 13 % por debajo del límite, en tramos de al menos 5 segundos, durante al menos el 10 % de la partida y un minuto en total. Las bajadas más cortas se tratan como eventos aislados. La app **nunca sube un límite sola**: para recalcularlo, usa *Deshacer*.

## 5. Prioridad: Estabilidad o Fluidez

En *Juegos*:

- **Estabilidad** (por defecto, recomendada): un límite que el juego sostiene incluso en sus peores momentos **sin bajar de forma perceptible** (su 1 % más lento llega como mucho un 13 % tarde, una diferencia que no se nota). Es el ritmo más regular y la línea más plana en la gráfica.
- **Fluidez:** el límite que el juego alcanza **el 90 % del tiempo**, aceptando caídas breves en los momentos más exigentes. Da más FPS; en las pruebas, las caídas perceptibles fueron de alrededor del 1 % del tiempo.

Con G-SYNC/FreeSync, ninguna de las dos cuenta los **tirones** (fotogramas de más de 2,5 veces lo normal): ningún límite los arregla. Sin VRR, por ahora se mantienen las reglas anteriores.

Los límites ya aceptados no cambian al cambiar la prioridad: deshaz el juego para recalcularlo con el criterio nuevo.

## 6. Límite manual

Si prefieres un valor concreto (por ejemplo, 120 en un juego que la app pondría más alto, o 220 en uno que sabes que tu equipo sostiene), puedes ponerlo tú:

- **Juego de la lista:** botón *Fijar…* en su fila, en *Juegos*.
- **Juego abierto:** botón *Límite manual…* en *Ahora*.

Escribe el valor y pulsa *Aplicar*; luego reinicia el juego si estaba abierto. La ventana te avisa si el valor no encaja: con G-SYNC, si supera el máximo recomendado de tu pantalla; sin G-SYNC, si no divide sus Hz (y te sugiere los valores que sí encajan). Con generación de fotogramas, el valor es lo que se ve en pantalla.

La app **no cambiará nunca** un límite manual: solo comprobará que funciona (el overlay se pone azul) y, al cerrar el juego, te avisará si no lo sostuvo. Para volver al modo automático, pulsa *Deshacer*.

## 7. G-SYNC, FreeSync y pantallas sin VRR

- **Con NVIDIA, cada límite va acompañado de "GPU al máximo"** (*Preferir rendimiento máximo* en el perfil del juego): con un límite, el driver bajaría las frecuencias de la GPU hasta ir justa y no llegaría a tiempo en las escenas pesadas, que empeorarían el 1 % bajo. La GPU sigue protegiéndose sola por temperatura y potencia. Se deshace junto con el límite.
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

## 9. Overlay en el juego

En una esquina del juego: FPS, 1 % bajo, una etiqueta con el estado del límite y la gráfica de ritmo de los últimos 3 segundos.

- **Azul ("FIJADO 225 fps"):** el límite está confirmado; el juego va a su ritmo.
- **Rojo ("SIN CONFIRMAR" o "SIN LÍMITE"):** el juego aún no tiene límite, o lo tiene pero todavía no se ha comprobado.
- La gráfica muestra las **desviaciones** del ritmo: mientras el tiempo de los fotogramas está dentro de la tolerancia (±13 % del objetivo, indicada en la etiqueta: *"objetivo 8,3 ms ±1,1"*), la línea es recta; solo se dibuja lo que se sale. Se aplica igual con y sin límite, así que la diferencia que ves es real. Los **tirones** aparecen en ámbar.
- **Con generación de fotogramas**, la gráfica dibuja las imágenes **en pantalla** (lo que ves), y la etiqueta lo indica (*"en pantalla 13,9 ms ±1,8"*). Si el juego activa y desactiva la generación por su cuenta, la app lo avisa en *Juegos*.
- Abajo a la izquierda, la **variación** del ritmo (±ms). La primera partida sin límite de cada juego queda como referencia, y con el límite confirmado verás la comparación: *"variación ±0,1 ms · antes ±0,4"*.
- Con el límite confirmado, una **banda azul** muestra esa tolerancia alrededor del objetivo.
- Con un límite activo, el **1 % bajo** descuenta los retrasos del propio limitador (fotogramas que salen algo tarde aunque el juego ya los tenía listos): muestra lo que causa el juego. El resumen de cada partida en *Registro* muestra los dos valores. Si la línea queda por encima del objetivo, el juego no llega del todo al límite: un límite algo más bajo dará al limitador margen para aplanarla.
- Se actualiza hasta 60 veces por segundo y se desplaza de forma continua, con un pequeño retraso que se adapta solo (los datos de PresentMon llegan por tandas; con generación de fotogramas, el retraso puede llegar a unos 2 segundos).

Es una ventana propia y transparente: no toca el juego. Desde la 0.44 la dibuja la GPU, con un coste casi nulo para el procesador; si en tu equipo no funcionara, la app vuelve sola al overlay clásico (y con `overlay=clasico` en `equipo.txt`, dentro de `%LOCALAPPDATA%\FlatPace`, se usa siempre el clásico).

## 10. Avisos y deshacer

- **Avisos en ámbar** (en *Juegos* y en *Ahora*): el juego no sostiene su límite, va claramente mejor que cuando se decidió (por ejemplo, tras bajar ajustes), la generación de fotogramas está al revés que cuando se aplicó, o **podría sostener un límite más alto**: con el límite puesto, la app estima la capacidad real del juego y, si llega al siguiente escalón, te lo sugiere (*"Podría sostener ~X FPS reales"*). Nunca lo sube sola: para aplicarlo, pulsa *Deshacer* y la app lo recalculará.
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
