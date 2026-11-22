// CPU-side constant setup for FSR 1 (EASU + RCAS), using AMD's reference headers.
#include <cstdint>
#include <cmath>
#define A_CPU 1
#include "ffx_a.h"
#include "ffx_fsr1.h"

void fsrEasuConstants(uint32_t con[16], float inW, float inH, float outW, float outH) {
    FsrEasuCon(con, con + 4, con + 8, con + 12, inW, inH, inW, inH, outW, outH);
}

void fsrRcasConstants(uint32_t con[4], float sharpnessStops) {
    FsrRcasCon(con, sharpnessStops);
}
