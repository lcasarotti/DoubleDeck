/* Unica unità di traduzione che compila dr_wav (public domain / MIT-0).
   Sta a sé, e in C, per gli stessi motivi per cui lo fa Fundamental: il file è
   9000 righe di C, e compilarlo insieme al resto rallenterebbe ogni build del
   plugin. */
#define DR_WAV_IMPLEMENTATION
#include "dep/dr_wav.h"
