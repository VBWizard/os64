#include "src/dsp/cpu.h"
static int cpu_features(CPUFeature feature)
{
#ifdef OS64_WEBP_SCALAR
    (void)feature;
    return 0;
#else
    return feature == kSSE2;
#endif
}
/* The callback identity is fixed before any lazy DSP initialization. */
VP8CPUInfo VP8GetCPUInfo = cpu_features;
