# FlatPace

*La única línea plana que quieres ver.*

**FlatPace mide tus juegos y aplica el límite de FPS que hace que vayan lo más estables posible.** Hace en Windows lo que la Steam Deck hace en su sistema: un ritmo constante se siente más fluido que unos FPS más altos pero irregulares.

Versión actual: **0.40.1** · Windows 10 (1803 o posterior) y Windows 11 · [Guía de uso](GUIA.md) · [Documentación técnica](TECNICO.md) · [Historial de cambios](CHANGELOG.md) · [Licencia](LICENSE) · [English](README.en.md)

## Qué hace

- **Mide cada juego** mientras juegas, fotograma a fotograma (con PresentMon de Intel), separando los fotogramas reales de los generados por DLSS Frame Generation.
- **Decide el límite con toda la partida** al cerrar el juego, según tu prioridad (fluidez o estabilidad), tu pantalla y si hay G-SYNC/FreeSync.
- **Lo aplica en el driver de NVIDIA** (perfil del juego, sin tocar el juego), **comprueba que funciona** en la siguiente partida y lo mantiene.
- **Lo vigila:** si un día el juego deja de sostenerlo de forma continuada, lo baja; los tropiezos aislados no cuentan.
- **Todo es reversible:** cada juego se puede deshacer por separado, y vuelve exactamente a como estaba.
- **Diagnóstico:** tirones, cuello de botella (CPU o GPU), generación de fotogramas, modo de presentación, VRR activo, overlay en el juego con la gráfica de ritmo.

## Inicio rápido

1. Descomprime la carpeta donde quieras (no hace falta instalar nada).
2. Abre `FlatPace.exe` y acepta los permisos de administrador (los necesitan PresentMon y el driver de NVIDIA).
3. Juega con normalidad. Al cerrar el juego, FlatPace guarda el límite para la próxima partida.
4. En la siguiente partida, la app comprueba que funciona ("Límite funcionando") y queda **aceptado**.

Si Windows muestra "Windows protegió su PC", pulsa *Más información* → *Ejecutar de todas formas*: aparece porque el programa no está firmado digitalmente.

## Requisitos y compatibilidad

| | Medir y recomendar | Aplicar límites |
|---|---|---|
| NVIDIA | Sí | Sí |
| AMD / Intel | Sí | Todavía no |

- **Pantallas sin VRR:** límites que dividen exactamente los Hz reales (por ejemplo 240, 120, 80, 60 en una de 240 Hz).
- **G-SYNC / FreeSync / Adaptive-Sync:** límites al ritmo exacto que el juego sostiene, por debajo del máximo de la pantalla.
- **Portátiles en modo híbrido:** funcionan (sin G-SYNC, con divisores).
- **Juegos con anticheat:** FlatPace no entra en el juego; solo usa el perfil del driver, como el Panel de control de NVIDIA.

## Limitaciones

- El driver aplica el límite **al abrir el juego**: los cambios se notan en la partida siguiente.
- No elimina los tirones propios del juego (compilación de shaders, cargas): los detecta y los muestra.
- Probado sobre todo en un portátil con RTX 4080 y pantalla de 240 Hz, con unos pocos juegos.

## Privacidad

Todo funciona en tu equipo. FlatPace no se conecta a internet. Guarda sus datos en `%LOCALAPPDATA%\FlatPace` y los registros junto al programa.

## Compilar desde el código

Necesitas Windows con Visual Studio 2022 (o CMake con MSVC), o MinGW-w64 desde Linux. Dear ImGui ya está incluido en `third_party/imgui`.

```
cmake -B build
cmake --build build --config Release
```

Para ejecutarlo, coloca junto a `FlatPace.exe` el archivo `PresentMon-2.6.0-x64.exe` (descárgalo de las [releases de PresentMon](https://github.com/GameTechDev/PresentMon/releases)) o usa el paquete de la sección *Releases* de este repositorio, que ya lo incluye. Las pruebas automáticas se ejecutan con `sh tests/run_tests.sh` o `ctest`. Más detalles en la [documentación técnica](TECNICO.md#12-compilación).

## Licencia

FlatPace (© 2026 KALAVER084) se distribuye con la [licencia MIT](LICENSE): puedes usarlo, modificarlo y redistribuirlo libremente, conservando el aviso de autoría.

## Créditos y licencias de terceros

- [PresentMon](https://github.com/GameTechDev/PresentMon) (Intel, licencia MIT): medición de fotogramas.
- [NVAPI](https://github.com/NVIDIA/nvapi) (NVIDIA, licencia MIT): perfiles del driver y estado de G-SYNC.
- [Dear ImGui](https://github.com/ocornut/imgui) (licencia MIT): interfaz.
