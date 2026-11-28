#ifndef AX210_H
#define AX210_H
#include <stdbool.h>

#define AX210_MODE_SIZE 16
#define AX210_MSIX_VECTOR 0x48
extern char kWifiMode[AX210_MODE_SIZE];

// Validate the image and map the boot-time PCI function.
void ax210_probe(void);
// Advance the probe in knet without waiting for hardware.
void ax210_service(void);
void ax210_isr(void);
const char *ax210_probe_report(void);
void ax210_shutdown_begin(void);
bool ax210_shutdown_waiting(void);
bool ax210_shutdown_safe(void);

#endif
