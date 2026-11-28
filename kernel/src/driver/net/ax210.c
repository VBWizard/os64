#include "BasicRenderer.h"
#include "driver/net/ax210.h"
#include "driver/net/ax210_boot.h"
#include "driver/system/pci.h"
#include "knet.h"
#include "memcmp.h"
#include "memory/allocator.h"
#include "memory/paging.h"
#include "memcpy.h"
#include "micros.h"
#include "printd.h"
#include "smp_core.h"
#include "strings/sprintf.h"
#include "strings/strings.h"

// First supported board. Match the PCI identity independently of its slot.
#define AX210_VENDOR_INTEL 0x8086u
#define AX210_DEVICE       0x2725u
#define AX210_SUBDEVICE    0x0024u
#define PCI_COMMAND_MEM    0x0002u
#define PCI_COMMAND_MASTER 0x0004u
#define AX210_PCI_CAP_PM   0x01u
#define AX210_PCI_CAP_EXP  0x10u
#define PCI_MSI_ENABLE     (1u << 16)
#define PCI_MSIX_ENABLE    (1u << 31)
#define PCI_PMCSR_OFFSET   4u
#define PCI_PMCSR_STATE    3u
#define PCI_EXP_DEVCTL     8u
#define PCI_EXP_PENDING    (1u << 21)  // Device Status, high half of DevCtl.

// CSR offsets and bits for the discrete AX210 from the pinned iwx reference.
#define AX210_CSR_HW_IF       0x000u
#define AX210_CSR_INT          0x008u
#define AX210_CSR_INT_MASK    0x00cu
#define AX210_CSR_FH_INT      0x010u
#define AX210_CSR_RESET       0x020u
#define AX210_CSR_GP_CTRL     0x024u
#define AX210_CSR_HW_REV      0x028u
#define AX210_CSR_GIO         0x03cu
#define AX210_CSR_UCODE_CLR   0x05cu
#define AX210_CSR_MBOX_SET    0x088u
#define AX210_CSR_HW_RF_ID    0x09cu
#define AX210_CSR_SHADOW      0x0a8u
#define AX210_CSR_LTR         0x0d4u
#define AX210_CSR_GIO_CHICKEN 0x100u
#define AX210_CSR_CONTEXT     0x118u
#define AX210_CSR_IML         0x120u
#define AX210_CSR_IML_SIZE    0x128u
#define AX210_CSR_IML_REPLY   0x12cu
#define AX210_CSR_HPET       0x240u
#define AX210_CSR_LINK_POWER 0x250u
#define AX210_CSR_MAC_OTP    0x380u
#define AX210_CSR_MAC_STRAP  0x388u
#define AX210_HBUS_PRPH_WADDR 0x444u
#define AX210_HBUS_PRPH_WDATA 0x44cu
#define AX210_CMD_DOORBELL    0x460u
#define AX210_RX_DOORBELL     0x1c80u
#define AX210_MSIX_FH_CAUSE  0x2800u
#define AX210_MSIX_FH_MASK   0x2804u
#define AX210_MSIX_HW_CAUSE  0x2808u
#define AX210_MSIX_HW_MASK   0x280cu
#define AX210_MSIX_AUTOMASK  0x2810u
#define AX210_MSIX_RX_IVAR   0x2880u
#define AX210_MSIX_IVAR      0x2890u
#define AX210_MSIX_EXPLICIT_ACK (1u << 7) // IVAR routes to table entry zero.
#define AX210_MSIX_RX         ((1u << 0) | (1u << 1))
#define AX210_MSIX_FH_ERROR   (1u << 21)
#define AX210_MSIX_FH_EVENTS  (AX210_MSIX_RX | (1u << 16) | (1u << 17) | (1u << 19) | AX210_MSIX_FH_ERROR)

#define AX210_CONTEXT_AUTO_BOOT (1u << 1)
#define AX210_INT_ALIVE        (1u << 0)
#define AX210_INT_FW_ERROR     (1u << 25)
#define AX210_INT_HW_ERROR     (1u << 29)
#define AX210_FH_ERROR         (1u << 31)
#define AX210_HW_IF_READY      (1u << 22)
#define AX210_HW_IF_PREPARE    (1u << 27)
#define AX210_HW_IF_WAKE_L1A   (1u << 19)
#define AX210_OS_ALIVE        (1u << 5)  // Host ownership bit, distinct from firmware ALIVE.
#define AX210_LINK_PWR_OFF     (1u << 31)
#define AX210_SW_RESET         (1u << 7)
#define AX210_MASTER_DISABLED  (1u << 8)
#define AX210_STOP_MASTER      (1u << 9)
#define AX210_INIT_DONE        (1u << 2)
#define AX210_MAC_CLOCK_READY  (1u << 0)
#define AX210_MAC_ACCESS       (1u << 3)
#define AX210_MAC_SLEEPING     (1u << 4)
#define AX210_L1A_NO_L0S_RX    (1u << 23)
#define AX210_L0S_DISABLED    (1u << 1)
#define AX210_HPET_THRESHOLD  0xffff0000u
#define AX210_MAC_TYPE_TY     0x42u
#define AX210_RF_TYPE_GF      0x10du
#define AX210_UMAC_OFFSET    0x300000u
#define AX210_UREG_CHICK     0xa05c00u
#define AX210_UREG_ISR6      0xa05c04u
#define AX210_UREG_INIT_RUN  0xa05c44u
#define AX210_CHICK_MSI      (1u << 24)
#define AX210_CHICK_MSIX     (1u << 25)
#define AX210_ISR6_PNVM      (1u << 20)
#define AX210_BOOT_LTR       0x88fa88fau
#define AX210_BAR_PAGES      2u
#define AX210_BAR_MSIX_PAGES 3u
#define AX210_BAR_BYTES      (4u * PAGE_SIZE)
#define AX210_FIRST_RX       8u
#define AX210_RX_BUDGET      16u
#define AX210_IMAGE_LIMIT   (8u * 1024u * 1024u)
#define AX210_PNVM_LIMIT    (128u * 1024u)

// API 77 boot commands and notifications on the queue established by the Gen3 context.
#define AX210_LEGACY_GROUP         0x00u
#define AX210_LONG_GROUP           0x01u
#define AX210_SYSTEM_GROUP         0x02u
#define AX210_REGULATORY_GROUP     0x0cu
#define AX210_NOTIF_ALIVE          0x01u
#define AX210_NOTIF_ERROR          0x02u
#define AX210_NOTIF_INIT_COMPLETE  0x04u
#define AX210_NOTIF_PNVM_COMPLETE  0xfeu
#define AX210_INIT_EXTENDED_CFG    0x03u
#define AX210_NVM_ACCESS_COMPLETE  0x00u
#define AX210_NVM_GET_INFO         0x02u
#define AX210_BT_CONFIG            0x9bu
#define AX210_BT_COEX_WIFI         0x03u
#define AX210_INIT_NVM             (1u << 1)
#define AX210_RX_UNSOLICITED       (1u << 7)

_Static_assert(PAGE_SIZE == AX210_BOOT_PAGE, "AX210 DMA page alignment");

typedef enum {
    AX210_IDLE,
    AX210_PCI_SETUP,
    AX210_D0_WAIT,
    AX210_WAIT_PCI_IDLE,
    AX210_WAIT_OWNERSHIP,
    AX210_WAKE_LINK_WAIT,
    AX210_RETRY_OWNERSHIP,
    AX210_RESET_WAIT,
    AX210_WAIT_CLOCK,
    AX210_WAIT_ACCESS,
    AX210_WAIT_ALIVE,
    AX210_WAIT_PNVM_ACCESS,
    AX210_WAIT_PNVM,
    AX210_WAIT_INIT_CONFIG,
    AX210_WAIT_NVM_ACK,
    AX210_WAIT_INIT_COMPLETE,
    AX210_WAIT_NVM_INFO,
    AX210_WAIT_BT_CONFIG,
    AX210_WAIT_STOP,
    AX210_WAIT_STOP_RESET,
} ax210_step_t;

// The network worker owns the device. Shutdown closes this gate before
// waiting, so a late startup cannot enable DMA after shutdown observes safe.
typedef enum {
    AX210_DMA_OPEN,
    AX210_DMA_ACTIVE,
    AX210_DMA_STOP_REQUESTED,
    AX210_DMA_SAFE,
    AX210_DMA_UNSAFE,
} ax210_dma_gate_t;

char kWifiMode[AX210_MODE_SIZE];
static ax210_fw_image_t s_firmware;
static const pci_device_t *s_board;
static volatile uint32_t *s_mmio;
static uint8_t s_pm_cap, s_exp_cap;
static uint8_t s_entry_pm = UINT8_MAX;
static uint16_t s_entry_command;
static uint32_t s_hw, s_rf;
static uint64_t s_deadline, s_access_earliest;
static ax210_step_t s_step;
static ax210_dma_gate_t s_dma_gate;
static uint64_t s_control_phys, s_rx_phys, s_image_phys, s_pnvm_phys;
static ax210_boot_dma_t *s_dma;
static uint16_t s_rx_cursor, s_rx_offered;
static uint64_t s_rx_seen[AX210_RX_COUNT / 64];
static uint32_t s_last_int, s_last_fh, s_iml_reply, s_boot_stage, s_ipc_status;
static bool s_msix_requested, s_msix_configured, s_msix_armed;
static uint32_t s_irq_hw, s_irq_fh, s_irqs, s_rx_irqs;
static ax210_alive_t s_alive;
static ax210_pnvm_t s_selected_pnvm;
static uint32_t s_pnvm_version, s_pnvm_bytes, s_pnvm_status = UINT32_MAX;
static uint8_t s_next_command, s_expected_group, s_expected_opcode, s_command_replies;
static uint8_t s_last_rx_group, s_last_rx_opcode, s_last_rx_queue, s_last_rx_index;
static bool s_init_complete_seen, s_bt_config_ack;
static uint8_t s_mac[6];
static ax210_nvm_info_t s_nvm;
static const char *s_stop_phase, *s_stop_reason;

// Each report uses its own buffer and is complete before sysfs can observe
// its pointer. Knet publishes the firmware and terminal snapshots once.
static char s_boot_report[1024] = "phase: disabled\n";
static char s_dma_report[1024];
static char s_terminal_report[1024];
static const char *s_report = s_boot_report;

static const pci_device_t *find_board(const pci_device_t *devices, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        const pci_device_t *dev = &devices[i];
        if (dev->vendor == AX210_VENDOR_INTEL && dev->device == AX210_DEVICE &&
            dev->subvendor == AX210_VENDOR_INTEL && dev->subdevice == AX210_SUBDEVICE)
            return dev;
    }
    return NULL;
}

static uint64_t bar_address(uint32_t low, uint32_t high)
{
    if ((low & 7) != 4)
        return 0;
    uint64_t bar = ((uint64_t)high << 32) | (low & ~0xfull);
    if (bar % PAGE_SIZE != 0 || bar > UINT64_MAX - kHHDMOffset - (AX210_BAR_BYTES - 1))
        return 0;
    return bar;
}

static uint32_t pci_read(uint16_t offset)
{
    return readPCIRegister(s_board->busNo, s_board->deviceNo, s_board->funcNo, offset);
}

static void pci_write(uint16_t offset, uint32_t value)
{
    writePCIRegister(s_board->busNo, s_board->deviceNo, s_board->funcNo, offset, value);
}

static uint8_t pci_cap(uint8_t id)
{
    return pci_find_capability(s_board->busNo, s_board->deviceNo, s_board->funcNo, id);
}

static bool disable_message_irq(uint8_t cap_id, uint32_t enable_bit)
{
    uint8_t cap = pci_cap(cap_id);
    if (cap == 0)
        return true;
    uint32_t value = pci_read(cap);
    if (value == UINT32_MAX)
        return false;
    if ((value & enable_bit) != 0)
        pci_write(cap, value & ~enable_bit);
    return (pci_read(cap) & enable_bit) == 0;
}

static uint32_t csr_read(unsigned offset)
{
    return s_mmio[offset / sizeof(uint32_t)];
}

static void csr_write(unsigned offset, uint32_t value)
{
    s_mmio[offset / sizeof(uint32_t)] = value;
}

static void csr_write_byte(unsigned offset, uint8_t value)
{
    ((volatile uint8_t *)s_mmio)[offset] = value;
}

static bool csr_set(unsigned offset, uint32_t flags)
{
    uint32_t value = csr_read(offset);
    if (value == UINT32_MAX)
        return false;
    csr_write(offset, value | flags);
    return true;
}

static void write_report(char *report, const char *phase, const char *reason)
{
    const pci_device_t *dev = s_board;
    snprintf(report, sizeof(s_boot_report),
             "phase: %s\n"
             "reason: %s\n"
             "pci: %02x:%02x.%u 8086:2725 subsystem 8086:0024 rev %02x\n"
             "pci_entry: command=0x%04x pm_state=%u\n"
             "registers: hw_rev=0x%08x rf_id=0x%08x\n"
             "embedded_firmware: %u.%08x.%u\n"
             "sections: lmac=%lu umac=%lu paging=%lu iml=%lu\n"
             "interrupt_mode: %s\n"
             "msix: irqs=%u rx_irqs=%u\n"
             "boot_status: csr=0x%08x fh=0x%08x iml=0x%08x stage=0x%08x ipc=0x%08x rx=%u\n"
             "alive: status=0x%04x lmac=%u.%08x umac=%u.%08x sku=%08x/%08x/%08x pnvm_match=%08x\n"
             "pnvm: bytes=%u status=0x%08x\n"
             "commands: replies=%u last_group=0x%02x last_opcode=0x%02x init_complete=%u bt_config=%u\n"
             "last_rx: group=0x%02x opcode=0x%02x queue=0x%02x index=%u\n"
             "nvm: version=0x%04x sku=0x%08x channels=%u valid=%u lar=%u tx=0x%x rx=0x%x\n"
             "mac: %02x:%02x:%02x:%02x:%02x:%02x\n",
             phase, reason, dev->busNo, dev->deviceNo, dev->funcNo, dev->revisionID,
             s_entry_command, s_entry_pm, s_hw, s_rf, s_firmware.api, s_firmware.build,
             s_firmware.revision,
             (unsigned long)s_firmware.groups[AX210_FW_LMAC].count,
             (unsigned long)s_firmware.groups[AX210_FW_UMAC].count,
             (unsigned long)s_firmware.groups[AX210_FW_PAGING].count,
             (unsigned long)s_firmware.loader.length,
             s_msix_configured ? "msix" : s_msix_requested ? "msix-unavailable" : "polled",
             __atomic_load_n(&s_irqs, __ATOMIC_RELAXED),
             __atomic_load_n(&s_rx_irqs, __ATOMIC_RELAXED),
             s_last_int, s_last_fh, s_iml_reply, s_boot_stage, s_ipc_status, s_rx_cursor,
             s_alive.status, s_alive.lmac_major, s_alive.lmac_minor,
             s_alive.umac_major, s_alive.umac_minor, s_alive.sku[0], s_alive.sku[1],
             s_alive.sku[2], s_pnvm_version, s_pnvm_bytes, s_pnvm_status,
             s_command_replies, s_expected_group, s_expected_opcode, (unsigned)s_init_complete_seen,
             (unsigned)s_bt_config_ack,
             s_last_rx_group, s_last_rx_opcode, s_last_rx_queue, s_last_rx_index,
             s_nvm.version, s_nvm.sku_flags, s_nvm.channels, s_nvm.valid_channels, s_nvm.lar_enabled,
             s_nvm.tx_chains, s_nvm.rx_chains,
             s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);
}

static void finish(const char *phase, const char *reason)
{
    s_step = AX210_IDLE;
    write_report(s_terminal_report, phase, reason);
    __atomic_store_n(&s_report, s_terminal_report, __ATOMIC_RELEASE);
    printd(DEBUG_BOOT, "ax210: %s, %s, hw_rev=0x%08x rf_id=0x%08x\n",
           phase, reason, s_hw, s_rf);
    const pci_device_t *dev = s_board;
    if (strcmp(phase, "nvm-ready") == 0) {
        printf("ax210: Intel AX210 at %02x:%02x.%u, firmware %u.%08x.%u, "
               "MAC %02x:%02x:%02x:%02x:%02x:%02x, probe complete\n",
               dev->busNo, dev->deviceNo, dev->funcNo,
               s_firmware.api, s_firmware.build, s_firmware.revision,
               s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);
    } else if (strcmp(phase, "stopped") != 0) {
        printf("ax210: Intel AX210 at %02x:%02x.%u, %s. %s\n",
               dev->busNo, dev->deviceNo, dev->funcNo, phase, reason);
    }
}

static void enter_step(ax210_step_t step, unsigned delay_us)
{
    s_step = step;
    s_deadline = micros_now() + delay_us;
}

static bool expired(void)
{
    return micros_now() >= s_deadline;
}

static void release_dma(void)
{
    if (s_pnvm_phys != 0)
        free_memory(s_pnvm_phys);
    if (s_image_phys != 0)
        free_memory(s_image_phys);
    if (s_rx_phys != 0)
        free_memory(s_rx_phys);
    if (s_control_phys != 0)
        free_memory(s_control_phys);
    s_pnvm_phys = s_image_phys = s_rx_phys = s_control_phys = 0;
    s_dma = NULL;
}

static size_t dma_rounded(size_t length)
{
    return (length + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);
}

static bool prepare_dma(void)
{
    size_t image_bytes = dma_rounded(s_firmware.loader.length);
    for (unsigned group = 0; group < AX210_FW_GROUP_COUNT; ++group) {
        const ax210_fw_sections_t *sections = &s_firmware.groups[group];
        for (size_t i = 0; i < sections->count; ++i) {
            size_t length = sections->sections[i].length;
            if (length > AX210_IMAGE_LIMIT || image_bytes > AX210_IMAGE_LIMIT - dma_rounded(length))
                return false;
            image_bytes += dma_rounded(length);
        }
    }

    s_control_phys = allocate_memory_try(sizeof(*s_dma));
    s_rx_phys = allocate_memory_try(AX210_RX_COUNT * PAGE_SIZE);
    s_image_phys = allocate_memory_try(image_bytes);
    if (s_control_phys == 0 || s_rx_phys == 0 || s_image_phys == 0) {
        release_dma();
        return false;
    }
    s_dma = (ax210_boot_dma_t *)PHYS_TO_VIRT(s_control_phys);
    ax210_gen3_set_rings(s_dma, s_control_phys, s_rx_phys, (uint16_t)s_hw);
    uint8_t *images = (uint8_t *)PHYS_TO_VIRT(s_image_phys);
    memcpy(images, s_firmware.loader.data, s_firmware.loader.length);
    size_t offset = dma_rounded(s_firmware.loader.length);
    for (unsigned group = 0; group < AX210_FW_GROUP_COUNT; ++group) {
        const ax210_fw_sections_t *sections = &s_firmware.groups[group];
        for (size_t i = 0; i < sections->count; ++i) {
            ax210_fw_span_t section = sections->sections[i];
            memcpy(images + offset, section.data, section.length);
            uint64_t address = s_image_phys + offset;
            if (group == AX210_FW_LMAC)
                s_dma->scratch.lmac[i] = address;
            else if (group == AX210_FW_UMAC)
                s_dma->scratch.umac[i] = address;
            else
                s_dma->scratch.paging[i] = address;
            offset += dma_rounded(section.length);
        }
    }
    return true;
}

static void csr_write64(unsigned offset, uint64_t value)
{
    csr_write(offset, (uint32_t)value);
    csr_write(offset + 4, (uint32_t)(value >> 32));
}

static void umac_write(uint32_t offset, uint32_t value)
{
    csr_write(AX210_HBUS_PRPH_WADDR, (offset + AX210_UMAC_OFFSET) | (3u << 24));
    csr_write(AX210_HBUS_PRPH_WDATA, value);
}

static bool configure_msix(void)
{
    uint8_t cap = pci_cap(PCI_CAP_MSIX);
    if (cap == 0)
        return false;
    uint32_t ctrl = pci_read(cap), location = pci_read(cap + 4);
    uint32_t offset = location & ~7u;
    unsigned entries = ((ctrl >> 16) & 0x7ffu) + 1;
    if (ctrl == UINT32_MAX || (location & 7u) != 0 ||
        offset < 0x2000 || offset > AX210_BAR_BYTES ||
        entries * 16u > AX210_BAR_BYTES - offset)
        return false;

    csr_write(AX210_MSIX_FH_MASK, UINT32_MAX);
    csr_write(AX210_MSIX_HW_MASK, UINT32_MAX);
    umac_write(AX210_UREG_CHICK, AX210_CHICK_MSIX);
    csr_write(AX210_MSIX_FH_MASK, UINT32_MAX);
    csr_write(AX210_MSIX_HW_MASK, UINT32_MAX);
    csr_write(AX210_MSIX_FH_CAUSE, UINT32_MAX);
    csr_write(AX210_MSIX_HW_CAUSE, UINT32_MAX);
    csr_write_byte(AX210_MSIX_RX_IVAR, AX210_MSIX_EXPLICIT_ACK);
    csr_write_byte(AX210_MSIX_RX_IVAR + 1, AX210_MSIX_EXPLICIT_ACK);
    // FH DMA channels 0 and 1, S2D and FH error, then ALIVE and both HW-bank errors.
    static const uint8_t causes[] = { 0x00, 0x01, 0x03, 0x05, 0x10, 0x29, 0x2d };
    for (size_t i = 0; i < sizeof(causes); ++i)
        csr_write_byte(AX210_MSIX_IVAR + causes[i], AX210_MSIX_EXPLICIT_ACK);

    const pci_device_t *dev = s_board;
    uint8_t bsp = (uint8_t)kCPUs[BSP_CPU].apic.id;
    pci_msix_result_t result = pci_msix_enable_one(dev->busNo, dev->deviceNo, dev->funcNo,
                                                  dev->baseAdd, AX210_MSIX_VECTOR, bsp);
    printd(DEBUG_BOOT, "ax210: MSI-X table BAR %u offset 0x%x, %u entries, entry 0 data 0x%x control 0x%04x\n",
           result.bar, result.offset, result.entries, result.data, result.control_after);
    return result.took;
}

void ax210_isr(void)
{
    if (!__atomic_load_n(&s_msix_armed, __ATOMIC_ACQUIRE))
        return;

    uint32_t fh = csr_read(AX210_MSIX_FH_CAUSE);
    uint32_t hw = csr_read(AX210_MSIX_HW_CAUSE);
    if (fh != 0)
        csr_write(AX210_MSIX_FH_CAUSE, fh);
    if (hw != 0)
        csr_write(AX210_MSIX_HW_CAUSE, hw);
    if ((fh | hw) != 0) {
        __atomic_fetch_or(&s_irq_fh, fh, __ATOMIC_RELAXED);
        __atomic_fetch_or(&s_irq_hw, hw, __ATOMIC_RELAXED);
        __atomic_add_fetch(&s_irqs, 1, __ATOMIC_RELAXED);
        if (fh != UINT32_MAX && (fh & AX210_MSIX_RX) != 0)
            __atomic_add_fetch(&s_rx_irqs, 1, __ATOMIC_RELAXED);
        doorbell_ring(&kNetDoorbell);
    }
    csr_write(AX210_MSIX_AUTOMASK, 1);
}

static void release_mac_access(void)
{
    uint32_t value = csr_read(AX210_CSR_GP_CTRL);
    if (value != UINT32_MAX)
        csr_write(AX210_CSR_GP_CTRL, value & ~AX210_MAC_ACCESS);
}

static bool clear_bus_master(void)
{
    uint32_t command = pci_read(4);
    if (command == UINT32_MAX)
        return false;
    pci_write(4, (command & 0xffffu) & ~PCI_COMMAND_MASTER);
    return (pci_read(4) & PCI_COMMAND_MASTER) == 0;
}

static void stop_failed(const char *reason)
{
    (void)clear_bus_master();
    // Keep published pages when neither reset nor DMA drain was proved.
    finish("unsafe-stop", reason);
    __atomic_store_n(&s_dma_gate, AX210_DMA_UNSAFE, __ATOMIC_RELEASE);
}

static void begin_stop(const char *phase, const char *reason)
{
    s_stop_phase = phase;
    s_stop_reason = reason;
    __atomic_store_n(&s_dma_gate, AX210_DMA_STOP_REQUESTED, __ATOMIC_RELEASE);
    s_iml_reply = csr_read(AX210_CSR_IML_REPLY);
    s_boot_stage = __atomic_load_n(&s_dma->peripheral_info[0], __ATOMIC_RELAXED);
    s_ipc_status = __atomic_load_n(&s_dma->peripheral_info[1], __ATOMIC_RELAXED);
    csr_write(AX210_CSR_INT_MASK, 0);
    if (s_msix_requested) {
        // Both this worker and the handler use the BSP. Close the handler
        // before masking, so an interrupt already in the LAPIC cannot rearm it.
        __atomic_store_n(&s_msix_armed, false, __ATOMIC_RELEASE);
        csr_write(AX210_MSIX_FH_MASK, UINT32_MAX);
        csr_write(AX210_MSIX_HW_MASK, UINT32_MAX);
        if (!disable_message_irq(PCI_CAP_MSIX, PCI_MSIX_ENABLE)) {
            stop_failed("could not disable PCI MSI-X before reset");
            return;
        }
    }
    release_mac_access();
    if (csr_set(AX210_CSR_RESET, AX210_STOP_MASTER))
        enter_step(AX210_WAIT_STOP, 150000);
    else
        stop_failed("unable to request device DMA stop");
}

static void begin_firmware(void)
{
    if (!prepare_dma()) {
        finish("failed", "could not allocate Gen3 firmware and RX buffers");
        return;
    }
    if (!csr_set(AX210_CSR_SHADOW, 0x800fffffu) ||
        !csr_set(AX210_CSR_GP_CTRL, AX210_MAC_ACCESS)) {
        release_dma();
        finish("failed", "could not request firmware register access");
        return;
    }
    s_access_earliest = micros_now() + 2;
    enter_step(AX210_WAIT_ACCESS, 150000);
}

static void start_firmware(void)
{
    ax210_dma_gate_t expected = AX210_DMA_OPEN;
    if (!__atomic_compare_exchange_n(&s_dma_gate, &expected, AX210_DMA_ACTIVE,
                                      false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        release_mac_access();
        release_dma();
        finish("stopped", "shutdown closed firmware startup");
        return;
    }

    if (s_msix_requested) {
        s_msix_configured = configure_msix();
        if (!s_msix_configured) {
            begin_stop("failed", "could not configure AX210 MSI-X entry zero");
            return;
        }
    } else {
        // Select the legacy cause registers with PCI IRQ delivery disabled.
        umac_write(AX210_UREG_CHICK, AX210_CHICK_MSI);
        csr_write(AX210_CSR_INT_MASK, 0);
        csr_write(AX210_CSR_INT, UINT32_MAX);
        csr_write(AX210_CSR_FH_INT, UINT32_MAX);
    }
    // Clear the uCode/host handshake flags. Hardware RF-kill is independent.
    csr_write(AX210_CSR_UCODE_CLR, (1u << 1) | (1u << 2));
    csr_write_byte(4, 0x40); // Interrupt coalescing byte.
    csr_write(AX210_CSR_LTR, AX210_BOOT_LTR);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    csr_write64(AX210_CSR_CONTEXT, s_control_phys);
    csr_write64(AX210_CSR_IML, s_image_phys);
    csr_write(AX210_CSR_IML_SIZE, (uint32_t)s_firmware.loader.length);
    if (!csr_set(AX210_CSR_HW_IF, AX210_CONTEXT_AUTO_BOOT)) {
        begin_stop("failed", "could not program automatic firmware boot");
        return;
    }

    uint32_t command = pci_read(4);
    if (command == UINT32_MAX || (command & (PCI_COMMAND_MEM | PCI_COMMAND_MASTER)) != PCI_COMMAND_MEM) {
        begin_stop("failed", "PCI command changed before firmware start");
        return;
    }
    pci_write(4, (command & 0xffffu) | PCI_COMMAND_MASTER);
    command = pci_read(4);
    if (command == UINT32_MAX || (command & PCI_COMMAND_MASTER) == 0) {
        begin_stop("failed", "PCI bus mastering did not enable for firmware download");
        return;
    }
    if (s_msix_configured) {
        __atomic_store_n(&s_msix_armed, true, __ATOMIC_RELEASE);
        csr_write(AX210_MSIX_AUTOMASK, 1);
        csr_write(AX210_MSIX_FH_MASK, ~AX210_MSIX_FH_EVENTS);
        csr_write(AX210_MSIX_HW_MASK, ~(AX210_INT_ALIVE | AX210_INT_FW_ERROR | AX210_INT_HW_ERROR));
    } else {
        csr_write(AX210_CSR_INT_MASK, AX210_INT_ALIVE | (1u << 31)); // ALIVE and FH RX.
    }
    umac_write(AX210_UREG_INIT_RUN, 1);
    release_mac_access();
    write_report(s_dma_report, "waiting-alive", "firmware loader started");
    __atomic_store_n(&s_report, s_dma_report, __ATOMIC_RELEASE);
    enter_step(AX210_WAIT_ALIVE, 2000000);
}

static void begin_pnvm(void)
{
    size_t length = (uintptr_t)ax210_pnvm_end - (uintptr_t)ax210_pnvm_start;
    if (s_firmware.fragmented_pnvm ||
        ax210_pnvm_select(ax210_pnvm_start, length, s_alive.sku, AX210_MAC_TYPE_TY,
                          AX210_RF_TYPE_GF, &s_selected_pnvm) != AX210_FW_OK) {
        begin_stop("failed", "no supported PNVM layout for the ALIVE SKU");
        return;
    }
    s_pnvm_version = s_selected_pnvm.version;

    const ax210_fw_sections_t *image = &s_selected_pnvm.image;
    size_t bytes = 0;
    for (size_t i = 0; i < image->count; ++i) {
        if (image->sections[i].length > AX210_PNVM_LIMIT - bytes) {
            begin_stop("failed", "PNVM image exceeds the boot limit");
            return;
        }
        bytes += image->sections[i].length;
    }
    s_pnvm_phys = allocate_memory_try(dma_rounded(bytes));
    if (s_pnvm_phys == 0) {
        begin_stop("failed", "could not allocate PNVM DMA memory");
        return;
    }
    uint8_t *dst = (uint8_t *)PHYS_TO_VIRT(s_pnvm_phys);
    for (size_t i = 0; i < image->count; ++i) {
        memcpy(dst, image->sections[i].data, image->sections[i].length);
        dst += image->sections[i].length;
    }
    s_pnvm_bytes = bytes;
    if (!csr_set(AX210_CSR_GP_CTRL, AX210_MAC_ACCESS)) {
        begin_stop("failed", "could not request MAC access for PNVM");
        return;
    }
    // Keep the MAC awake through PNVM and the bounded init command sequence.
    s_access_earliest = micros_now() + 2;
    enter_step(AX210_WAIT_PNVM_ACCESS, 150000);
}

static void publish_pnvm(void)
{
    s_dma->scratch.pnvm_address = s_pnvm_phys;
    s_dma->scratch.pnvm_bytes = s_pnvm_bytes;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    umac_write(AX210_UREG_ISR6, AX210_ISR6_PNVM);
    enter_step(AX210_WAIT_PNVM, 2000000);
}

static void send_command(ax210_step_t wait, uint8_t group, uint8_t opcode,
                         const void *payload, size_t length)
{
    // This sequence owns each TFD and payload until the probe stops.
    if (!ax210_gen3_command(s_dma, s_control_phys, s_next_command, group, opcode, payload, length)) {
        begin_stop("failed", "firmware command exceeds its DMA buffer");
        return;
    }
    s_expected_group = group;
    s_expected_opcode = opcode;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    csr_write(AX210_CMD_DOORBELL, ++s_next_command); // Queue zero.
    enter_step(wait, 1000000);
}

static void send_command_word(ax210_step_t wait, uint8_t group, uint8_t opcode, uint32_t word)
{
    send_command(wait, group, opcode, &word, sizeof(word));
}

static bool read_mac(unsigned offset)
{
    uint32_t first = csr_read(offset), last = csr_read(offset + 4);
    if (first == UINT32_MAX || last == UINT32_MAX)
        return false;
    uint8_t mac[6] = { first >> 24, first >> 16, first >> 8, first, last >> 8, last };
    static const uint8_t zero[6], reserved[6] = { 0x02, 0xcc, 0xaa, 0xff, 0xee, 0 };
    if ((mac[0] & 1u) != 0 || memcmp(mac, zero, sizeof(mac)) == 0 ||
        memcmp(mac, reserved, sizeof(mac)) == 0)
        return false;
    memcpy(s_mac, mac, sizeof(mac));
    return true;
}

static void configure_coexistence(const ax210_notification_t *packet)
{
    if (!ax210_decode_nvm_info(packet, &s_nvm) || (s_nvm.flags & 1u) != 0) {
        begin_stop("failed", "invalid NVM_GET_INFO response");
        return;
    }
    if (!read_mac(AX210_CSR_MAC_STRAP) && !read_mac(AX210_CSR_MAC_OTP)) {
        begin_stop("failed", "NVM reply received but no valid MAC address");
        return;
    }
    printd(DEBUG_BOOT, "ax210: NVM_GET_INFO reply, version 0x%04x, %u/%u valid SKU channels\n",
           s_nvm.version, s_nvm.valid_channels, s_nvm.channels);
    // BT_CONFIG v6 carries the coexistence mode and optional module flags.
    const uint32_t config[] = { AX210_BT_COEX_WIFI, 0 };
    send_command(AX210_WAIT_BT_CONFIG, AX210_LONG_GROUP, AX210_BT_CONFIG, config, sizeof(config));
}

static void request_nvm_info(void)
{
    send_command_word(AX210_WAIT_NVM_INFO, AX210_REGULATORY_GROUP, AX210_NVM_GET_INFO, 0);
}

static void handle_command_reply(const ax210_notification_t *packet)
{
    if (s_step != AX210_WAIT_INIT_CONFIG && s_step != AX210_WAIT_NVM_ACK &&
        s_step != AX210_WAIT_NVM_INFO && s_step != AX210_WAIT_BT_CONFIG)
        return;
    if (packet->queue != 0 || packet->index != s_next_command - 1)
        return;
    // BT_CONFIG is a legacy opcode sent with a wide header in the long group.
    bool group_matches = packet->group == s_expected_group ||
                         (s_step == AX210_WAIT_BT_CONFIG && packet->group == AX210_LEGACY_GROUP);
    if (packet->failed || !group_matches || packet->opcode != s_expected_opcode) {
        begin_stop("failed", "firmware rejected the current command or returned the wrong ID");
        return;
    }
    ++s_command_replies;
    if (s_step == AX210_WAIT_INIT_CONFIG) {
        send_command_word(AX210_WAIT_NVM_ACK, AX210_REGULATORY_GROUP, AX210_NVM_ACCESS_COMPLETE, 0);
    } else if (s_step == AX210_WAIT_NVM_ACK) {
        if (s_init_complete_seen)
            request_nvm_info();
        else
            enter_step(AX210_WAIT_INIT_COMPLETE, 2000000);
    } else if (s_step == AX210_WAIT_NVM_INFO) {
        configure_coexistence(packet);
    } else {
        s_bt_config_ack = true;
        printd(DEBUG_BOOT, "ax210: BT_CONFIG acknowledged\n");
        begin_stop("nvm-ready", "NVM_GET_INFO and BT_CONFIG replies received. PCI bus mastering disabled");
    }
}

static void handle_packet(const ax210_notification_t *packet)
{
    s_last_rx_group = packet->group;
    s_last_rx_opcode = packet->opcode;
    s_last_rx_queue = packet->queue;
    s_last_rx_index = packet->index;
    if (packet->group == AX210_LEGACY_GROUP && packet->opcode == AX210_NOTIF_ERROR) {
        begin_stop("failed", "firmware reported an error during initialization");
        return;
    }
    if (s_step == AX210_WAIT_ALIVE && packet->group == AX210_LEGACY_GROUP &&
        packet->opcode == AX210_NOTIF_ALIVE) {
        if (!ax210_decode_alive(packet, &s_alive) || s_alive.status != 0xcafe || packet->failed) {
            begin_stop("failed", "firmware returned an invalid ALIVE notification");
            return;
        }
        printd(DEBUG_BOOT, "ax210: firmware ALIVE, status 0x%04x sku %08x/%08x/%08x\n",
               s_alive.status, s_alive.sku[0], s_alive.sku[1], s_alive.sku[2]);
        begin_pnvm();
        return;
    }
    if ((packet->queue & AX210_RX_UNSOLICITED) == 0) {
        handle_command_reply(packet);
        return;
    }
    if (s_step == AX210_WAIT_PNVM && packet->group == AX210_REGULATORY_GROUP &&
        packet->opcode == AX210_NOTIF_PNVM_COMPLETE) {
        if (packet->failed || !ax210_decode_pnvm_status(packet, &s_pnvm_status)) {
            begin_stop("failed", "invalid PNVM_INIT_COMPLETE notification");
            return;
        }
        // iwx and iwlwifi advance on the completion itself. Keep its status
        // word for diagnostics and verify initialization with the NVM reply.
        printd(DEBUG_BOOT, "ax210: PNVM %08x completion status 0x%08x (%u bytes)\n",
               s_pnvm_version, s_pnvm_status, s_pnvm_bytes);
        send_command_word(AX210_WAIT_INIT_CONFIG, AX210_SYSTEM_GROUP, AX210_INIT_EXTENDED_CFG, AX210_INIT_NVM);
    } else if ((s_step == AX210_WAIT_NVM_ACK || s_step == AX210_WAIT_INIT_COMPLETE) &&
               packet->group == AX210_LEGACY_GROUP && packet->opcode == AX210_NOTIF_INIT_COMPLETE) {
        s_init_complete_seen = true;
        if (s_step == AX210_WAIT_INIT_COMPLETE)
            request_nvm_info();
    }
}

static void offer_rx(uint16_t count)
{
    // The RFH pointer advances in groups of eight. This probe offers
    // previously unused pages and stops before the ring can wrap.
    __atomic_thread_fence(__ATOMIC_RELEASE);
    csr_write(AX210_RX_DOORBELL, count);
    s_rx_offered = count;
}

static const char *timeout_reason(void)
{
    switch (s_step) {
    case AX210_WAIT_ALIVE:
        return s_rx_offered ? "timed out waiting for DMA ALIVE notification"
                            : "timed out waiting for RFH-ready cause";
    case AX210_WAIT_PNVM:
        return "timed out waiting for PNVM_INIT_COMPLETE";
    case AX210_WAIT_INIT_CONFIG:
        return "timed out waiting for INIT_EXTENDED_CFG reply";
    case AX210_WAIT_NVM_ACK:
        return "timed out waiting for NVM_ACCESS_COMPLETE reply";
    case AX210_WAIT_INIT_COMPLETE:
        return "timed out waiting for INIT_COMPLETE notification";
    case AX210_WAIT_NVM_INFO:
        return "timed out waiting for NVM_GET_INFO reply";
    case AX210_WAIT_BT_CONFIG:
        return "timed out waiting for BT_CONFIG reply";
    default:
        return "firmware initialization timed out";
    }
}

static void service_firmware(void)
{
    uint32_t cause, fh;
    uint32_t fh_error = AX210_FH_ERROR;
    if (s_msix_configured) {
        cause = __atomic_exchange_n(&s_irq_hw, 0, __ATOMIC_ACQUIRE);
        fh = __atomic_exchange_n(&s_irq_fh, 0, __ATOMIC_ACQUIRE);
        fh_error = AX210_MSIX_FH_ERROR;
    } else {
        cause = csr_read(AX210_CSR_INT);
        fh = csr_read(AX210_CSR_FH_INT);
        if (cause != 0)
            csr_write(AX210_CSR_INT, cause);
        if (fh != 0)
            csr_write(AX210_CSR_FH_INT, fh);
    }
    s_last_int |= cause;
    s_last_fh |= fh;
    if ((cause & (AX210_INT_FW_ERROR | AX210_INT_HW_ERROR)) != 0 || (fh & fh_error) != 0) {
        begin_stop("failed", "device fault during firmware initialization");
        return;
    }
    if (s_rx_offered == 0 && (cause & AX210_INT_ALIVE) != 0) {
        // The CSR cause signals that the firmware programmed RFH. Offer the
        // first buffers now, then wait for a separate valid DMA notification.
        offer_rx(AX210_FIRST_RX);
    }

    uint16_t head = __atomic_load_n(&s_dma->completion_head, __ATOMIC_ACQUIRE) & 0xfffu;
    if (head > s_rx_offered || head < s_rx_cursor) {
        begin_stop("failed", "RX completion head is outside the bootstrap window");
        return;
    }
    for (unsigned budget = AX210_RX_BUDGET; s_rx_cursor < head && budget > 0; --budget) {
        const ax210_rx_completion_t *entry = &s_dma->rx_used[s_rx_cursor++];
        uint16_t token = entry->token;
        if (token >= s_rx_offered || (entry->flags & 1u) != 0 ||
            (s_rx_seen[token / 64] & (1ull << (token % 64))) != 0) {
            begin_stop("failed", "invalid RX completion token during bootstrap");
            return;
        }
        s_rx_seen[token / 64] |= 1ull << (token % 64);
        const uint8_t *buffer = (const uint8_t *)PHYS_TO_VIRT(s_rx_phys + token * PAGE_SIZE);
        ax210_notification_t packet;
        ax210_rx_result_t result = ax210_decode_rx(buffer, PAGE_SIZE, &packet);
        if (result == AX210_RX_MALFORMED) {
            begin_stop("failed", "malformed firmware notification");
            return;
        }
        if (result == AX210_RX_PACKET)
            handle_packet(&packet);
        if (s_step == AX210_IDLE || s_step == AX210_WAIT_STOP || s_step == AX210_WAIT_PNVM_ACCESS)
            return;
    }
    if (s_rx_offered != 0 && s_rx_cursor == s_rx_offered) {
        if (s_rx_offered >= AX210_RX_COUNT - AX210_FIRST_RX) {
            begin_stop("failed", "firmware probe exhausted its RX window");
            return;
        }
        offer_rx(s_rx_offered + AX210_FIRST_RX);
    }
    if (expired())
        begin_stop("failed", timeout_reason());
}

static void enable_registers(void)
{
    uint64_t live_bar = bar_address(pci_read(0x10), pci_read(0x14));
    if (live_bar == 0 || kHHDMOffset + live_bar != (uintptr_t)s_mmio) {
        finish("failed", "BAR0 changed after PCI power transition");
        return;
    }
    uint32_t command = pci_read(4);
    if (command == UINT32_MAX) {
        finish("failed", "PCI configuration is unavailable");
        return;
    }
    // Command and Status share this dword. Write zero to the upper W1C bits.
    pci_write(4, ((command & 0xffffu) | PCI_COMMAND_MEM | PCI_COMMAND_INTX_DISABLE)
                 & ~PCI_COMMAND_MASTER);
    uint32_t required = PCI_COMMAND_MEM | PCI_COMMAND_INTX_DISABLE;
    if ((pci_read(4) & (required | PCI_COMMAND_MASTER)) != required ||
        !disable_message_irq(PCI_CAP_MSI, PCI_MSI_ENABLE) ||
        !disable_message_irq(PCI_CAP_MSIX, PCI_MSIX_ENABLE)) {
        finish("failed", "could not enable MMIO with DMA and interrupts disabled");
        return;
    }
    enter_step(AX210_WAIT_PCI_IDLE, 100000);
}

static void setup_pci(void)
{
    if ((pci_read(4) & PCI_COMMAND_MASTER) != 0) {
        finish("failed", "PCI bus mastering is enabled before reset");
        return;
    }

    s_pm_cap = pci_cap(AX210_PCI_CAP_PM);
    s_exp_cap = pci_cap(AX210_PCI_CAP_EXP);
    if (s_exp_cap == 0) {
        finish("failed", "PCI Express capability unavailable");
        return;
    }
    if (s_pm_cap != 0) {
        uint32_t pmcsr = pci_read(s_pm_cap + PCI_PMCSR_OFFSET);
        if (pmcsr == UINT32_MAX) {
            finish("failed", "PCI power management state is unavailable");
            return;
        }
        s_entry_pm = pmcsr & PCI_PMCSR_STATE;
    }
    if (s_pm_cap != 0 && s_entry_pm != 0) {
        // Zero requests D0 and leaves the W1C PME status untouched.
        pci_write(s_pm_cap + PCI_PMCSR_OFFSET, 0);
        enter_step(AX210_D0_WAIT, 10000);
        return;
    }
    enable_registers();
}

static void claim_ownership(void)
{
    s_hw = csr_read(AX210_CSR_HW_REV);
    s_rf = csr_read(AX210_CSR_HW_RF_ID);
    if (((s_hw >> 4) & 0xfffu) != AX210_MAC_TYPE_TY ||
        ((s_rf >> 12) & 0xfffu) != AX210_RF_TYPE_GF) {
        finish("failed", "BAR0 does not report the supported Typhoon Peak MAC and RF");
        return;
    }
    csr_write(AX210_CSR_INT_MASK, 0);
    if (!csr_set(AX210_CSR_HW_IF, AX210_HW_IF_READY)) {
        finish("failed", "could not request NIC ownership");
        return;
    }
    enter_step(AX210_WAIT_OWNERSHIP, 50);
}

static void reset_and_wait(void)
{
    if (!csr_set(AX210_CSR_MBOX_SET, AX210_OS_ALIVE) ||
        !csr_set(AX210_CSR_RESET, AX210_SW_RESET)) {
        finish("failed", "could not reset the controller");
        return;
    }
    enter_step(AX210_RESET_WAIT, 5000);
}

static void enable_clock(void)
{
    uint32_t command = pci_read(4);
    if ((command & (PCI_COMMAND_MEM | PCI_COMMAND_MASTER)) != PCI_COMMAND_MEM) {
        finish("failed", "reset did not leave MMIO enabled with DMA disabled");
        return;
    }
    // The reset interval elapsed. Keep interrupts masked while using the
    // pinned discrete-Typhoon-Peak APM sequence to request the MAC clock.
    csr_write(AX210_CSR_INT_MASK, 0);
    if (!csr_set(AX210_CSR_GIO_CHICKEN, AX210_L1A_NO_L0S_RX) ||
        !csr_set(AX210_CSR_HPET, AX210_HPET_THRESHOLD) ||
        !csr_set(AX210_CSR_HW_IF, AX210_HW_IF_WAKE_L1A) ||
        !csr_set(AX210_CSR_GIO, AX210_L0S_DISABLED) ||
        !csr_set(AX210_CSR_GP_CTRL, AX210_INIT_DONE)) {
        finish("failed", "controller registers unavailable after reset");
        return;
    }
    enter_step(AX210_WAIT_CLOCK, 25000);
}

void ax210_service(void)
{
    if (s_step != AX210_IDLE && s_step != AX210_WAIT_STOP && s_step != AX210_WAIT_STOP_RESET &&
        __atomic_load_n(&s_dma_gate, __ATOMIC_ACQUIRE) == AX210_DMA_STOP_REQUESTED) {
        begin_stop("stopped", "shutdown interrupted firmware startup; PCI bus mastering disabled");
        return;
    }

    uint32_t value;
    switch (s_step) {
    case AX210_IDLE:
        break;
    case AX210_PCI_SETUP:
        setup_pci();
        break;
    case AX210_D0_WAIT:
        if (!expired())
            break;
        if ((pci_read(s_pm_cap + PCI_PMCSR_OFFSET) & PCI_PMCSR_STATE) == 0)
            enable_registers();
        else
            finish("failed", "PCI function did not reach D0");
        break;
    case AX210_WAIT_PCI_IDLE:
        value = pci_read(s_exp_cap + PCI_EXP_DEVCTL);
        if ((value & PCI_EXP_PENDING) == 0)
            claim_ownership();
        else if (expired())
            finish("failed", "PCI transactions did not drain");
        break;
    case AX210_WAIT_OWNERSHIP:
        value = csr_read(AX210_CSR_HW_IF);
        if (value != UINT32_MAX && (value & AX210_HW_IF_READY) != 0)
            reset_and_wait();
        else if (expired()) {
            if (csr_set(AX210_CSR_LINK_POWER, AX210_LINK_PWR_OFF))
                enter_step(AX210_WAKE_LINK_WAIT, 1000);
            else
                finish("failed", "controller stopped responding during ownership request");
        }
        break;
    case AX210_WAKE_LINK_WAIT:
        if (!expired())
            break;
        if (csr_set(AX210_CSR_HW_IF, AX210_HW_IF_PREPARE | AX210_HW_IF_READY))
            enter_step(AX210_RETRY_OWNERSHIP, 150000);
        else
            finish("failed", "could not prepare the PCIe link");
        break;
    case AX210_RETRY_OWNERSHIP:
        value = csr_read(AX210_CSR_HW_IF);
        if (value != UINT32_MAX && (value & AX210_HW_IF_READY) != 0)
            reset_and_wait();
        else if (expired() || !csr_set(AX210_CSR_HW_IF, AX210_HW_IF_READY))
            finish("failed", "NIC ownership timed out");
        break;
    case AX210_RESET_WAIT:
        if (expired())
            enable_clock();
        break;
    case AX210_WAIT_CLOCK:
        value = csr_read(AX210_CSR_GP_CTRL);
        if ((pci_read(4) & PCI_COMMAND_MASTER) != 0)
            finish("failed", "PCI bus mastering enabled while waiting for MAC clock");
        else if (value != UINT32_MAX && (value & AX210_MAC_CLOCK_READY) != 0)
            begin_firmware();
        else if (expired())
            finish("failed", "MAC clock did not become ready after reset");
        break;
    case AX210_WAIT_ACCESS:
        if (micros_now() < s_access_earliest)
            break;
        value = csr_read(AX210_CSR_GP_CTRL);
        if ((value & (AX210_MAC_CLOCK_READY | AX210_MAC_SLEEPING)) == AX210_MAC_CLOCK_READY) {
            start_firmware();
        } else if (expired()) {
            release_mac_access();
            release_dma();
            finish("failed", "MAC access timed out before firmware download");
        }
        break;
    case AX210_WAIT_ALIVE:
    case AX210_WAIT_PNVM:
    case AX210_WAIT_INIT_CONFIG:
    case AX210_WAIT_NVM_ACK:
    case AX210_WAIT_INIT_COMPLETE:
    case AX210_WAIT_NVM_INFO:
    case AX210_WAIT_BT_CONFIG:
        service_firmware();
        break;
    case AX210_WAIT_PNVM_ACCESS:
        if (micros_now() < s_access_earliest)
            break;
        value = csr_read(AX210_CSR_GP_CTRL);
        if ((value & (AX210_MAC_CLOCK_READY | AX210_MAC_SLEEPING)) == AX210_MAC_CLOCK_READY)
            publish_pnvm();
        else if (expired())
            begin_stop("failed", "MAC access timed out before PNVM");
        break;
    case AX210_WAIT_STOP:
        value = csr_read(AX210_CSR_RESET);
        if (value != UINT32_MAX && (value & AX210_MASTER_DISABLED) != 0 &&
            (pci_read(s_exp_cap + PCI_EXP_DEVCTL) & PCI_EXP_PENDING) == 0) {
            if (!clear_bus_master() || !csr_set(AX210_CSR_RESET, AX210_SW_RESET))
                stop_failed("could not clear PCI bus mastering and reset after DMA drain");
            else
                enter_step(AX210_WAIT_STOP_RESET, 5000);
        } else if (expired()) {
            stop_failed("device DMA drain timed out");
        }
        break;
    case AX210_WAIT_STOP_RESET:
        if (!expired())
            break;
        if ((pci_read(4) & PCI_COMMAND_MASTER) != 0 ||
            (pci_read(s_exp_cap + PCI_EXP_DEVCTL) & PCI_EXP_PENDING) != 0) {
            stop_failed("PCI transactions remained active after reset");
            break;
        }
        release_dma();
        finish(s_stop_phase, s_stop_reason);
        __atomic_store_n(&s_dma_gate, AX210_DMA_SAFE, __ATOMIC_RELEASE);
        break;
    }
}

void ax210_probe(void)
{
    if (kWifiMode[0] == '\0')
        return;
    s_msix_requested = strcmp(kWifiMode, "probe-msix") == 0;
    if (!s_msix_requested && strcmp(kWifiMode, "probe") != 0) {
        snprintf(s_boot_report, sizeof(s_boot_report),
                 "phase: disabled\nreason: use WIFI=probe or WIFI=probe-msix\n");
        printd(DEBUG_BOOT, "ax210: unsupported WIFI mode (use WIFI=probe or WIFI=probe-msix)\n");
        return;
    }

    const pci_device_t *dev = find_board(kPCIDeviceHeaders, kPCIDeviceCount);
    if (dev == NULL)
        dev = find_board(kPCIDeviceFunctions, kPCIFunctionCount);
    if (dev == NULL) {
        snprintf(s_boot_report, sizeof(s_boot_report), "phase: absent\nreason: PCI 8086:2725 subsystem 8086:0024 not found\n");
        printd(DEBUG_BOOT, "ax210: probe found no supported PCI function\n");
        return;
    }

    ax210_fw_result_t result = ax210_fw_parse(ax210_ucode_start,
        (uintptr_t)ax210_ucode_end - (uintptr_t)ax210_ucode_start, &s_firmware);
    if (result != AX210_FW_OK) {
        snprintf(s_boot_report, sizeof(s_boot_report), "phase: failed\nreason: firmware container (%u)\n",
                 (unsigned)result);
        printd(DEBUG_BOOT, "ax210: firmware container refused (%u)\n", (unsigned)result);
        printf("ax210: Intel AX210 firmware container refused (%u)\n", (unsigned)result);
        return;
    }

    uint64_t bar = bar_address(dev->baseAdd[0], dev->baseAdd[1]);
    s_board = dev;
    if (bar == 0) {
        finish("failed", "BAR0 is not an assigned page-aligned 64-bit memory BAR");
        return;
    }
    // Isolate old DMA as soon as we find the function, before the wait
    // for knet. The upper half of the dword is PCI Status (W1C).
    uint32_t command = pci_read(4);
    if (command == UINT32_MAX) {
        finish("failed", "PCI configuration is unavailable");
        return;
    }
    s_entry_command = (uint16_t)command;
    pci_write(4, ((command & 0xffffu) | PCI_COMMAND_INTX_DISABLE)
                 & ~(PCI_COMMAND_MEM | PCI_COMMAND_MASTER));
    if ((pci_read(4) & (PCI_COMMAND_MEM | PCI_COMMAND_MASTER)) != 0) {
        finish("failed", "PCI memory decoding or bus mastering did not clear");
        return;
    }
    if (!disable_message_irq(PCI_CAP_MSI, PCI_MSI_ENABLE) ||
        !disable_message_irq(PCI_CAP_MSIX, PCI_MSIX_ENABLE)) {
        finish("failed", "previous PCI interrupt mode could not be disabled");
        return;
    }
    uintptr_t mapped = kHHDMOffset + bar;
    unsigned pages = s_msix_requested ? AX210_BAR_MSIX_PAGES : AX210_BAR_PAGES;
    paging_map_pages((pt_entry_t *)kKernelPML4v, mapped, bar, pages,
                     PAGE_PRESENT | PAGE_WRITE | PAGE_PCD | PAGE_NO_EXECUTE);
    s_mmio = (volatile uint32_t *)mapped;
    write_report(s_boot_report, "preparing", "waiting for knet to reset the controller");
    s_step = AX210_PCI_SETUP;
    printd(DEBUG_BOOT, "ax210: %02x:%02x.%u probe queued, firmware %u.%08x.%u\n",
           dev->busNo, dev->deviceNo, dev->funcNo,
           s_firmware.api, s_firmware.build, s_firmware.revision);
}

const char *ax210_probe_report(void)
{
    return __atomic_load_n(&s_report, __ATOMIC_ACQUIRE);
}

void ax210_shutdown_begin(void)
{
    ax210_dma_gate_t seen = __atomic_load_n(&s_dma_gate, __ATOMIC_ACQUIRE);
    while (seen == AX210_DMA_OPEN || seen == AX210_DMA_ACTIVE) {
        ax210_dma_gate_t next = seen == AX210_DMA_OPEN ? AX210_DMA_SAFE : AX210_DMA_STOP_REQUESTED;
        if (__atomic_compare_exchange_n(&s_dma_gate, &seen, next, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            break;
    }
}

bool ax210_shutdown_waiting(void)
{
    return __atomic_load_n(&s_dma_gate, __ATOMIC_ACQUIRE) == AX210_DMA_STOP_REQUESTED;
}

bool ax210_shutdown_safe(void)
{
    return __atomic_load_n(&s_dma_gate, __ATOMIC_ACQUIRE) == AX210_DMA_SAFE;
}
