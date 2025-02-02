/* 
 * File:   apic.h
 * Author: yogi
 *
 * Created on May 21, 2016, 4:00 PM
 */

#ifndef APIC_H
#define	APIC_H

#include <stdint.h>
#include <stdbool.h>

#define IOAPIC_REGSEL_OFFSET 0x00
#define IOAPIC_WIN_OFFSET 0x10

#define IA32_APIC_BASE_MSR              0x1B
#define IA32_APIC_BASE_MSR_BSP          0x100 // Processor is a BSP
#define APIC_SW_ENABLE                  0x100
#define IA32_APIC_BASE_MSR_ENABLE       0x800
#define APIC_REGISTER_APIC_ID_OFFSET    0x20
#define APIC_REGISTER_VERSION           0x30
#define APIC_REGISTER_SPURIOUS          0x00f0
#define APIC_REGISTER_LVT_CMCI          0x02f0
#define APIC_REGISTER_LVT_TIMER         0x0320
#define APIC_REGISTER_LVT_THERM         0x0330
#define APIC_REGISTER_LVT_PERF          0x0340
#define APIC_REGISTER_LVT_LINT0         0x0350
#define APIC_REGISTER_LVT_LINT1         0x0360
#define APIC_REGISTER_LVT_ERROR         0x0370
#define APIC_REGISTER_TIMER_INITIAL     0x0380
#define APIC_REGISTER_TIMER_CURRENT     0x0390
#define APIC_REGISTER_TIMER_DIV         0x03e0
#define APIC_DELIVERY_MODE_FIXED        0x0
#define APIC_DELIVERY_MODE_SMI          0x2
#define APIC_TIMER_MODE_NMI             0x4<<8
#define APIC_TIMER_MODE_INIT            0x5<<8;
#define APIC_TIMER_MODE_EXTINT          0x7<<8
#define APIC_TIMER_STATUS_IDLE       0x0
#define APIC_TIMER_STATUS_PENDING    0x1
#define APIC_INT_INPUT_PIN_POL_AHIGH    0x0
#define APIC_INT_INPUT_PIN_POL_ALOW     0x0
#define APIC_TIMER_MODE_ONESHOT         0x0
#define APIC_TIMER_INT_DISABLE          0x10000
#define APIC_TIMER_MODE_PERIODIC        0x20000
#define APIC_TIMER_MODE_TSCDEADLINE     0x60000

bool check_for_apic();
uint8_t acpiGetAPICVersion();
uint64_t apicGetAPICBase(void);
uint32_t apicReadRegister(uint32_t reg);
void apicWriteRegister(uint64_t reg, uint32_t value);
void apicSetAPICBase(uintptr_t apic);
uint32_t apicGetHZ();
void apicDisable();
void apicEnable();
bool apicIsEnabled();
int tscGetTicksPerSecond();
void remap_irq0_to_apic(uint32_t vector);

#endif	/* APIC_H */
