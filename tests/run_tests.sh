#!/bin/sh
# Compila y ejecuta las pruebas de la logica pura de FlatPace (src/core).
set -e
cd "$(dirname "$0")/.."
${CXX:-g++} -std=c++20 -O1 -Wall -Wextra tests/test_core.cpp -o tests/test_core
./tests/test_core
