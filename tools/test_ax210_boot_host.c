#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/net/ax210.h"
#include "driver/net/ax210_boot.h"
#include "driver/system/pci.h"
#include "knet.h"
#include "memory/paging.h"
#include "smp_core.h"

#define TEST_BAR 0xdc800000u
#define TEST_MEM 2u
#define TEST_MASTER 4u
#define TEST_EXP_CAP 0x40u
#define TEST_PENDING (1u << 21)
#define TEST_PM_CAP 0xc8u
#define TEST_MSI_CAP 0xd0u
#define TEST_MSIX_CAP 0x80u
#define TEST_MSIX_ENABLE (1u << 31)

static _Alignas(PAGE_SIZE) uint32_t registers[3 * PAGE_SIZE / 4];
static uint32_t config[256 / 4];
static uint64_t now_us;
static void *allocations[4];
static unsigned alloc_count, free_count;
static bool msix;
static unsigned irq_wakes;
static pci_device_t board = {
    .busNo = 3, .vendor = 0x8086, .device = 0x2725, .subvendor = 0x8086,
    .subdevice = 0x0024, .revisionID = 0x1a, .baseAdd = { TEST_BAR | 4 },
};

uint16_t kPCIDeviceCount = 1, kPCIFunctionCount;
pci_device_t *kPCIDeviceHeaders = &board, *kPCIDeviceFunctions;
pt_entry_t kKernelPML4v;
uint64_t kHHDMOffset;
cpu_t kCPUs[MAX_CPUS];
doorbell_t kNetDoorbell;

void paging_map_pages(pt_entry_t *pml4, uint64_t vaddr, uint64_t paddr,
                      uint64_t pages, uint64_t flags)
{
    assert(pml4 == (pt_entry_t *)kKernelPML4v && vaddr == (uintptr_t)registers);
    assert(paddr == TEST_BAR && pages == (msix ? 3u : 2u));
    assert(flags == (PAGE_PRESENT | PAGE_WRITE | PAGE_PCD | PAGE_NO_EXECUTE));
}

uint32_t readPCIRegister(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset)
{
    assert(bus == 3 && device == 0 && function == 0 && offset < sizeof(config));
    return config[offset / 4];
}

void writePCIRegister(uint8_t bus, uint8_t device, uint8_t function,
                      uint16_t offset, uint32_t value)
{
    assert(bus == 3 && device == 0 && function == 0 && offset < sizeof(config));
    if (offset == 4) {
        assert((value & 0xffff0000u) == 0);
        if ((value & TEST_MASTER) != 0)
            assert(registers[0x128 / 4] == 13944 && alloc_count == 3 && free_count == 0);
    }
    config[offset / 4] = value;
}

uint8_t pci_find_capability(uint8_t bus, uint8_t device, uint8_t function, uint8_t id)
{
    assert(bus == 3 && device == 0 && function == 0);
    switch (id) {
    case 0x01: return TEST_PM_CAP;
    case 0x10: return TEST_EXP_CAP;
    case PCI_CAP_MSI: return TEST_MSI_CAP;
    case PCI_CAP_MSIX: return TEST_MSIX_CAP;
    default: return 0;
    }
}

pci_msix_result_t pci_msix_enable_one(uint8_t bus, uint8_t device, uint8_t function,
                                      const uint32_t bars[6], uint8_t vector, uint8_t apic_id)
{
    assert(msix && bus == 3 && device == 0 && function == 0 && bars[0] == (TEST_BAR | 4));
    assert(vector == AX210_MSIX_VECTOR && apic_id == 2);
    assert(config[4 / 4] == (TEST_MEM | PCI_COMMAND_INTX_DISABLE));
    assert(registers[0x2804 / 4] == UINT32_MAX && registers[0x280c / 4] == UINT32_MAX);
    const uint8_t *mmio = (const uint8_t *)registers;
    assert(mmio[0x2880] == 0x80 && mmio[0x2881] == 0x80);
    assert(mmio[0x2890 + 0x10] == 0x80 && mmio[0x2890 + 0x29] == 0x80);
    config[TEST_MSIX_CAP / 4] |= TEST_MSIX_ENABLE;
    return (pci_msix_result_t) { .took = true, .cap = TEST_MSIX_CAP, .entries = 16,
                                 .offset = 0x2000, .data = vector, .control_after = 0x800f };
}

void doorbell_ring(doorbell_t *db)
{
    assert(db == &kNetDoorbell);
    ++irq_wakes;
}

uint64_t micros_now(void)
{
    return now_us;
}

uint64_t allocate_memory_try(uint64_t length)
{
    assert(length % PAGE_SIZE == 0 && alloc_count < 4);
    void *memory = aligned_alloc(PAGE_SIZE, length);
    assert(memory != NULL);
    memset(memory, 0, length);
    allocations[alloc_count++] = memory;
    return (uintptr_t)memory - kHHDMOffset;
}

uint64_t free_memory(uint64_t address)
{
    assert((config[4 / 4] & TEST_MASTER) == 0);
    assert((config[(TEST_EXP_CAP + 8) / 4] & TEST_PENDING) == 0);
    assert((config[TEST_MSIX_CAP / 4] & TEST_MSIX_ENABLE) == 0);
    void *memory = (void *)(address + kHHDMOffset);
    for (unsigned i = 0; i < alloc_count; ++i) {
        if (allocations[i] == memory) {
            allocations[i] = NULL;
            free(memory);
            ++free_count;
            return 0;
        }
    }
    abort();
}

void printd(__uint128_t level, const char *fmt, ...)
{
    (void)level;
    (void)fmt;
}

static void advance(uint64_t microseconds)
{
    now_us += microseconds;
    ax210_service();
}

static void post_msix(uint32_t fh, uint32_t hw)
{
    assert((config[TEST_MSIX_CAP / 4] & TEST_MSIX_ENABLE) != 0);
    registers[0x2800 / 4] = fh;
    registers[0x2808 / 4] = hw;
    registers[0x2810 / 4] = 0;
    ax210_isr();
    assert(registers[0x2810 / 4] == 1);
    assert(registers[0x2800 / 4] == fh && registers[0x2808 / 4] == hw);
    // Model the W1C effects of the ISR's MMIO writes.
    registers[0x2800 / 4] = 0;
    registers[0x2808 / 4] = 0;
}

static void test_alive_bounds(void)
{
    uint8_t notification[AX210_BOOT_PAGE] = {0};
    ax210_notification_t packet;
    ax210_alive_t info;
    assert(ax210_decode_rx(notification, 7, &packet) == AX210_RX_MALFORMED);
    assert(ax210_decode_rx(notification, sizeof(notification), &packet) == AX210_RX_EMPTY);
    notification[0] = 148;
    notification[4] = 1;
    notification[8] = 0xfe;
    notification[9] = 0xca;
    assert(ax210_decode_rx(notification, 151, &packet) == AX210_RX_MALFORMED);
    assert(ax210_decode_rx(notification, 152, &packet) == AX210_RX_PACKET);
    assert(ax210_decode_alive(&packet, &info) && info.status == 0xcafe);
    --packet.payload.length;
    assert(!ax210_decode_alive(&packet, &info));
}

static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = value >> (8u * i);
}

static uint8_t *post_packet(ax210_boot_dma_t *dma, uint8_t group, uint8_t op,
                            uint8_t queue, uint8_t index, unsigned payload_bytes)
{
    uint16_t cursor = dma->completion_head;
    assert(cursor < AX210_RX_COUNT);
    uint8_t *p = (uint8_t *)allocations[1] + cursor * PAGE_SIZE;
    put32(p, payload_bytes + 4);
    p[4] = op;
    p[5] = group;
    p[6] = index;
    p[7] = queue;
    dma->rx_used[cursor].token = cursor;
    dma->completion_head = cursor + 1;
    registers[0x08 / 4] = 0;
    if (msix)
        post_msix(1, 0);
    return p + 8;
}

static void check_command(ax210_boot_dma_t *dma, uint8_t index, uint8_t group,
                          uint8_t opcode, const void *data, size_t length)
{
    const ax210_command_tfd_t *tfd = &dma->command_ring[index];
    const ax210_command_packet_t *packet = &dma->command_payloads[index];
    assert(tfd->count == 1 && tfd->length == 8 + length);
    assert(tfd->address + kHHDMOffset == (uintptr_t)packet);
    assert(packet->queue == 0 && packet->index == index);
    assert(packet->group == group && packet->opcode == opcode);
    assert(packet->length == length && packet->version == 0);
    assert(memcmp(packet->payload, data, length) == 0);
    assert(registers[0x460 / 4] == (uint32_t)index + 1);
}

static void run_init_commands(ax210_boot_dma_t *dma, const char *scenario)
{
    bool p5_pnvm = strcmp(scenario, "p5-pnvm") == 0;
    bool bad_pnvm = strcmp(scenario, "bad-pnvm") == 0;
    uint8_t *pnvm = post_packet(dma, 0x0c, 0xfe, 0xc0, 2, bad_pnvm ? 3 : 4);
    put32(pnvm, p5_pnvm ? 0x00020001 : 0);
    advance(10000);
    if (bad_pnvm) {
        assert(registers[0x460 / 4] == 0);
        return;
    }
    const uint32_t nvm_flag = 2, zero = 0;
    check_command(dma, 0, 2, 3, &nvm_flag, sizeof(nvm_flag));

    bool reject = strcmp(scenario, "command-reject") == 0;
    if (strcmp(scenario, "stale-reply") == 0) {
        post_packet(dma, 2, 3, 0, 1, 0);
        advance(10000);
        assert(registers[0x460 / 4] == 1);
    }
    post_packet(dma, reject ? 0x42 : 2, 3, 0, 0, 0);
    advance(10000);
    if (reject) {
        assert(registers[0x460 / 4] == 1);
        return;
    }
    check_command(dma, 1, 0x0c, 0, &zero, sizeof(zero));

    bool early = strcmp(scenario, "early-init") == 0;
    if (early) {
        post_packet(dma, 0, 4, 0x80, 0, 0);
        advance(10000);
        assert(registers[0x460 / 4] == 2);
    }
    post_packet(dma, 0x0c, 0, 0, 1, 0);
    advance(10000);
    if (!early) {
        assert(registers[0x460 / 4] == 2);
        post_packet(dma, 0, 4, 0x80, 0, 0);
        advance(10000);
    }
    check_command(dma, 2, 0x0c, 2, &zero, sizeof(zero));

    // Strap is empty in this fixture. The address comes from OTP.
    registers[0x380 / 4] = 0x02112233;
    registers[0x384 / 4] = 0x4455;
    uint8_t *nvm = post_packet(dma, 0x0c, 2, 0, 2, 468);
    put32(nvm + 4, 0x102);
    nvm[7] = 1;
    put32(nvm + 8, 0x1f);
    put32(nvm + 12, 3);
    put32(nvm + 16, 3);
    put32(nvm + 20, 1);
    put32(nvm + 24, 110);
    put32(nvm + 28, 1);
    advance(10000);

    const uint32_t coex[] = { 3, 0 };
    check_command(dma, 3, 1, 0x9b, coex, sizeof(coex));
    assert(!ax210_shutdown_waiting() && free_count == 0);
    if (strcmp(scenario, "coex-timeout") == 0) {
        advance(1000000);
        return;
    }
    if (strcmp(scenario, "coex-reject") == 0) {
        post_packet(dma, 0x41, 0x9b, 0, 3, 0);
    } else {
        // Firmware can reply with a legacy or a long-group header.
        post_packet(dma, p5_pnvm ? 0 : 1, 0x9b, 0, 3, 0);
    }
    advance(10000);
}

static void run_firmware(const char *scenario)
{
    registers[0x24 / 4] |= 1u;
    advance(1);
    assert((config[4 / 4] & TEST_MASTER) == 0 && alloc_count == 3);
    if (strcmp(scenario, "early-stop") == 0) {
        ax210_shutdown_begin();
        assert(ax210_shutdown_safe());
        advance(2);
        assert(free_count == 3 && (config[4 / 4] & TEST_MASTER) == 0);
        return;
    }
    advance(1);
    assert((config[4 / 4] & TEST_MASTER) == 0);
    advance(1);
    assert((config[4 / 4] & TEST_MASTER) != 0);
    assert(strstr(ax210_probe_report(), "phase: waiting-alive") != NULL);
    if (msix) {
        assert(strstr(ax210_probe_report(), "interrupt_mode: msix\n") != NULL);
        assert(registers[0x2804 / 4] == ~(uint32_t)0x002b0003);
        assert(registers[0x280c / 4] == ~(uint32_t)0x22000001);
    }

    uint64_t context = registers[0x118 / 4] | (uint64_t)registers[0x11c / 4] << 32;
    assert((uintptr_t)allocations[0] == context + kHHDMOffset);
    ax210_boot_dma_t *dma = allocations[0];
    assert(dma->context.scratch == context + offsetof(ax210_boot_dma_t, scratch));
    assert(dma->context.transfer_ring_size == 5 && dma->context.completion_ring_size == 9);
    assert(dma->scratch.mac == 0x420 && dma->scratch.size_dwords == 415);
    assert(dma->scratch.umac[0] && dma->scratch.lmac[0] && dma->scratch.paging[0]);
    assert(dma->scratch.umac[16] == 0 && dma->scratch.lmac[15] == 0 && dma->scratch.paging[23] == 0);
    assert(dma->rx_free[0].address + kHHDMOffset == (uintptr_t)allocations[1]);
    assert(dma->rx_free[AX210_RX_COUNT - 1].token == AX210_RX_COUNT - 1);
    assert(registers[0x444 / 4] == 0x03d05c44 && registers[0x44c / 4] == 1);
    assert(registers[0x1c80 / 4] == 0);
    registers[0x08 / 4] = 1;
    registers[0x10 / 4] = 0;
    if (msix) {
        advance(10000);
        assert(registers[0x1c80 / 4] == 0); // A legacy cause cannot start MSI-X RX.
        post_msix(0, 1);
    }
    advance(10000);
    assert(registers[0x1c80 / 4] == 8);

    bool interrupt = strcmp(scenario, "shutdown") == 0;
    bool msix_fault = strcmp(scenario, "msix-fault") == 0;
    bool bad_token = strcmp(scenario, "bad-token") == 0;
    bool unsafe = strcmp(scenario, "unsafe-stop") == 0;
    if (msix_fault) {
        post_msix(1u << 21, 0); // FH error while the loader owns DMA.
        advance(1);
        assert(dma->completion_head == 0 && registers[0x460 / 4] == 0);
    } else if (interrupt) {
        ax210_shutdown_begin();
        advance(1);
    } else {
        uint8_t *notify = post_packet(dma, 0, 1, 0x80, 0, 144);
        put32(notify, 0xcafe);
        put32(notify + 4, 77);
        put32(notify + 8, 0xf92b5fed);
        put32(notify + 116, 0x510d0);
        dma->rx_used[0].token = bad_token ? AX210_RX_COUNT : 0;
        advance(10000);
        if (!bad_token) {
            assert(alloc_count == 4 && dma->scratch.pnvm_address == 0);
            advance(2);
            assert(dma->scratch.pnvm_address + kHHDMOffset == (uintptr_t)allocations[3]);
            assert(dma->scratch.pnvm_bytes == 13704);
            assert(registers[0x444 / 4] == 0x03d05c04 && registers[0x44c / 4] == (1u << 20));
            if (unsafe) {
                ax210_shutdown_begin();
                advance(1);
            } else {
                run_init_commands(dma, scenario);
            }
        }
    }
    assert(ax210_shutdown_waiting() && !ax210_shutdown_safe() && free_count == 0);
    if (msix) {
        assert((config[TEST_MSIX_CAP / 4] & TEST_MSIX_ENABLE) == 0);
        assert(registers[0x2804 / 4] == UINT32_MAX && registers[0x280c / 4] == UINT32_MAX);
        registers[0x2810 / 4] = 0;
        ax210_isr();
        assert(registers[0x2810 / 4] == 0 && irq_wakes == (msix_fault ? 2u : 8u));
    }
    if (unsafe) {
        advance(150000);
        assert(!ax210_shutdown_waiting() && !ax210_shutdown_safe() && free_count == 0);
        assert(strstr(ax210_probe_report(), "phase: unsafe-stop") != NULL);
        // The simulated controller cannot DMA after this process exits.
        for (unsigned i = 0; i < alloc_count; ++i)
            free(allocations[i]);
        return;
    }
    registers[0x20 / 4] |= 1u << 8; // Device acknowledged Stop Master.
    advance(1);
    assert((config[4 / 4] & TEST_MASTER) == 0 && free_count == 0);
    advance(4999);
    assert(free_count == 0 && ax210_shutdown_waiting());
    advance(1);
    assert(free_count == alloc_count && ax210_shutdown_safe());
    const char *report = ax210_probe_report();
    if (interrupt)
        assert(strstr(report, "phase: stopped") != NULL);
    else if (msix_fault) {
        assert(strstr(report, "phase: failed\nreason: device fault during firmware initialization") != NULL);
        assert(strstr(report, "msix: irqs=2 rx_irqs=0\n") != NULL);
    }
    else if (bad_token)
        assert(strstr(report, "invalid RX completion token") != NULL);
    else if (strcmp(scenario, "bad-pnvm") == 0)
        assert(strstr(report, "invalid PNVM_INIT_COMPLETE notification") != NULL);
    else if (strcmp(scenario, "command-reject") == 0 || strcmp(scenario, "coex-reject") == 0)
        assert(strstr(report, "firmware rejected the current command") != NULL);
    else if (strcmp(scenario, "coex-timeout") == 0)
        assert(strstr(report, "timed out waiting for BT_CONFIG reply") != NULL);
    else {
        assert(strstr(report, "phase: nvm-ready") != NULL);
        assert(strstr(report, "status=0xcafe lmac=77.f92b5fed") != NULL);
        assert(strstr(report, "pnvm_match=ce1a5094") != NULL);
        if (strcmp(scenario, "p5-pnvm") == 0)
            assert(strstr(report, "pnvm: bytes=13704 status=0x00020001\n") != NULL);
        assert(strstr(report, "replies=4") != NULL);
        assert(strstr(report, "bt_config=1") != NULL);
        if (msix)
            assert(strstr(report, "msix: irqs=8 rx_irqs=7\n") != NULL);
        assert(strstr(report, "channels=110 valid=1") != NULL);
        assert(strstr(report, "mac: 02:11:22:33:44:55") != NULL);
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    test_alive_bounds();
    bool d3 = strcmp(argv[1], "d3") == 0;
    bool pending = strcmp(argv[1], "pending") == 0;
    bool timeout = strcmp(argv[1], "clock-timeout") == 0;
    msix = strncmp(argv[1], "msix", 4) == 0;
    kCPUs[BSP_CPU].apic.id = 2;
    kHHDMOffset = (uintptr_t)registers - TEST_BAR;
    config[4 / 4] = 0xffff0000u | TEST_MEM | TEST_MASTER;
    config[0x10 / 4] = TEST_BAR | 4;
    config[TEST_MSI_CAP / 4] = 1u << 16;
    config[TEST_MSIX_CAP / 4] = TEST_MSIX_ENABLE | (15u << 16);
    config[TEST_MSIX_CAP / 4 + 1] = 0x2000;
    config[(TEST_PM_CAP + 4) / 4] = d3 ? 3 : 0;
    config[(TEST_EXP_CAP + 8) / 4] = pending ? TEST_PENDING : 0;
    registers[0x28 / 4] = 0x420;
    registers[0x9c / 4] = 0x10d000;
    strcpy(kWifiMode, msix ? "probe-msix" : "probe");

    ax210_probe();
    assert(strcmp(ax210_probe_report(), "phase: disabled\n") != 0);
    assert((config[4 / 4] & (TEST_MEM | TEST_MASTER)) == 0);
    advance(0);
    if (d3) {
        advance(9999);
        assert(config[(TEST_PM_CAP + 4) / 4] == 0);
        assert((config[4 / 4] & TEST_MEM) == 0);
        advance(1);
    }
    assert((config[4 / 4] & (TEST_MEM | TEST_MASTER)) == TEST_MEM);
    assert(config[TEST_MSI_CAP / 4] == 0);
    assert(config[TEST_MSIX_CAP / 4] == (15u << 16));
    advance(0);
    if (pending) {
        advance(100000);
        assert(registers[0x20 / 4] == 0);
        assert(strstr(ax210_probe_report(), "PCI transactions did not drain") != NULL);
    } else {
        advance(0);
        assert((registers[0x20 / 4] & (1u << 7)) != 0);
        advance(4999);
        assert(registers[0x24 / 4] == 0);
        advance(1);
        assert((registers[0x24 / 4] & (1u << 2)) != 0);
        if (timeout) {
            advance(25000);
            assert(strstr(ax210_probe_report(), "MAC clock did not become ready") != NULL);
        } else {
            run_firmware(argv[1]);
        }
    }
    assert((config[4 / 4] & TEST_MASTER) == 0);
    printf("AX210 bootstrap %s passed\n", argv[1]);
    return 0;
}
