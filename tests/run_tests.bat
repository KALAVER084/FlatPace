@echo off
rem Compila y ejecuta las pruebas de la logica pura de FlatPace (src/core) con MSVC.
cd /d "%~dp0.."
cl /nologo /std:c++20 /EHsc /O1 /W4 tests\test_core.cpp /Fe:tests\test_core.exe /Fo:tests\ || exit /b 1
tests\test_core.exe
