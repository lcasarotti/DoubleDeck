// Header iniettato in ogni unità di traduzione con -include.
//
// Il firmware conta su include transitivi che la toolchain arm-none-eabi
// fornisce e MinGW/libstdc++ no: <cstdint>, <cstddef>, <cmath>, <algorithm>
// mancano in una trentina di file di src/core.
//
// Risolverlo qui invece che nei sorgenti tiene il diff verso upstream vicino
// allo zero, così `git pull` sul submodule resta indolore. Se un giorno le
// mancanze venissero corrette upstream, questo file diventa semplicemente
// ridondante.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
