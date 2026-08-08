// lspci.c — render the PCI inventory exported by /sys/bus/pci.

#include "os64/os64.h"

#define PCI_SYS_DIR   "/sys/bus/pci"
#define PCI_PATH_MAX  96
#define PCI_VALUE_MAX 160

typedef struct {
    char address[16];
    char type[16];
    char vendor[16];
    char device[16];
    char class_id[16];
    char subclass[16];
    char progif[16];
    char subsystem_vendor[16];
    char subsystem_device[16];
    char primary_bus[16];
    char secondary_bus[16];
    char subordinate_bus[16];
    char irq_line[16];
    char irq_pin[16];
    char vendor_name[PCI_VALUE_MAX];
    char device_name[PCI_VALUE_MAX];
} pci_record_t;

static void copy_value(char *out, size_t capacity, const char *value)
{
    if (capacity == 0)
        return;
    os64_snprintf(out, capacity, "%s", value);
}

static void accept_field(pci_record_t *record, const char *key,
                         const char *value)
{
#define PCI_FIELD(name, member) \
    if (os64_streq(key, name)) { copy_value(record->member, sizeof(record->member), value); return; }
    PCI_FIELD("address", address)
    PCI_FIELD("type", type)
    PCI_FIELD("vendor", vendor)
    PCI_FIELD("device", device)
    PCI_FIELD("class", class_id)
    PCI_FIELD("subclass", subclass)
    PCI_FIELD("progif", progif)
    PCI_FIELD("subsystem_vendor", subsystem_vendor)
    PCI_FIELD("subsystem_device", subsystem_device)
    PCI_FIELD("primary_bus", primary_bus)
    PCI_FIELD("secondary_bus", secondary_bus)
    PCI_FIELD("subordinate_bus", subordinate_bus)
    PCI_FIELD("irq_line", irq_line)
    PCI_FIELD("irq_pin", irq_pin)
    PCI_FIELD("vendor_name", vendor_name)
    PCI_FIELD("device_name", device_name)
#undef PCI_FIELD
}

static int read_record(const char *path, pci_record_t *record)
{
    int32_t handle = (int32_t)os64_open(path, "r");
    if (handle < 0)
        return -1;

    char line[256];
    int64_t result;
    while ((result = os64_readline(handle, line, sizeof(line))) == 1)
    {
        char *separator = line;
        while (*separator != '\0' && *separator != ':')
            separator++;
        if (*separator != ':')
            continue;
        *separator++ = '\0';
        while (*separator == ' ' || *separator == '\t')
            separator++;
        accept_field(record, line, separator);
    }

    int close_result = (int)os64_close(handle);
    return result < 0 || close_result < 0 ? -1 : 0;
}

static const char *class_name(const char *class_id, const char *subclass)
{
    if (os64_streq(class_id, "0x01")) {
        if (os64_streq(subclass, "0x06")) return "SATA controller";
        if (os64_streq(subclass, "0x08")) return "Non-Volatile memory controller";
        return "Mass storage controller";
    }
    if (os64_streq(class_id, "0x02")) return "Network controller";
    if (os64_streq(class_id, "0x03")) return "Display controller";
    if (os64_streq(class_id, "0x04")) return "Multimedia controller";
    if (os64_streq(class_id, "0x05")) return "Memory controller";
    if (os64_streq(class_id, "0x06")) {
        if (os64_streq(subclass, "0x00")) return "Host bridge";
        if (os64_streq(subclass, "0x01")) return "ISA bridge";
        if (os64_streq(subclass, "0x04")) return "PCI bridge";
        return "Bridge";
    }
    if (os64_streq(class_id, "0x07")) return "Communication controller";
    if (os64_streq(class_id, "0x08")) return "System peripheral";
    if (os64_streq(class_id, "0x09")) return "Input device controller";
    if (os64_streq(class_id, "0x0c")) {
        if (os64_streq(subclass, "0x03")) return "USB controller";
        return "Serial bus controller";
    }
    return "Unclassified device";
}

static const char *known_name(const char *name, const char *fallback)
{
    return name[0] != '\0' ? name : fallback;
}

static void print_record(const pci_record_t *r, bool numeric, bool verbose)
{
    const char *kind = class_name(r->class_id, r->subclass);
    if (numeric)
        os64_printf("%s %s [%s:%s]\n", r->address, kind, r->vendor, r->device);
    else
        os64_printf("%s %s: %s %s [%s:%s]\n", r->address, kind,
                    known_name(r->vendor_name, "Unknown vendor"),
                    known_name(r->device_name, "Unknown device"),
                    r->vendor, r->device);

    if (!verbose)
        return;

    os64_printf("\tClass: %s, subclass %s, programming interface %s\n",
                r->class_id, r->subclass, r->progif);
    if (os64_streq(r->type, "bridge"))
        os64_printf("\tBuses: primary %s, secondary %s, subordinate %s\n",
                    r->primary_bus, r->secondary_bus, r->subordinate_bus);
    else
        os64_printf("\tSubsystem: %s:%s\n", r->subsystem_vendor,
                    r->subsystem_device);
    os64_printf("\tIRQ: line %s, pin %s\n", r->irq_line, r->irq_pin);
}

int main(int argc, char **argv)
{
    bool numeric = false;
    bool verbose = false;
    const char *slot = NULL;
    const os64_optspec_t specs[] = {
        {'n', "numeric", false, "show numeric vendor and device IDs only",
         .flag = &numeric},
        {'v', "verbose", false, "show class, subsystem, IRQ, and bridge details",
         .flag = &verbose},
        {'s', "slot", true, "show only the specified bus:device.function",
         .value_out = &slot}
    };
    os64_args_t args = {0};
    os64_args_init(&args, argc, argv, specs, 3);
    args.about = "List PCI devices discovered by the kernel.";
    args.details = "The default one-device-per-line output is intended to be grep-friendly.";

    int32_t parsed = os64_args_parse(&args, "lspci [-nv] [-s SLOT]", NULL, 0);
    if (parsed == OS64_ARG_HELP)
        return 0;
    if (parsed < 0)
        return 2;

    int32_t directory = (int32_t)os64_opendir(PCI_SYS_DIR);
    if (directory < 0) {
        os64_hprintf(OS64_STDERR, "lspci: cannot open %s\n", PCI_SYS_DIR);
        return 1;
    }

    int status = 0;
    int matches = 0;
    int64_t result;
    os64_dirent_t entry = {0};
    while ((result = os64_readdir(directory, &entry)) == 1)
    {
        if ((entry.flags & OS64_DE_DIR) != 0)
            continue;
        if (slot != NULL && !os64_streq(slot, entry.name))
            continue;

        char path[PCI_PATH_MAX];
        os64_snprintf(path, sizeof(path), "%s/%s", PCI_SYS_DIR, entry.name);
        pci_record_t record = {0};
        if (read_record(path, &record) < 0) {
            os64_hprintf(OS64_STDERR, "lspci: cannot read %s\n", path);
            status = 1;
            continue;
        }
        print_record(&record, numeric, verbose);
        matches++;
    }
    if (result < 0) {
        os64_hprintf(OS64_STDERR, "lspci: cannot read %s\n", PCI_SYS_DIR);
        status = 1;
    }
    if (os64_close(directory) < 0)
        status = 1;
    if (slot != NULL && matches == 0) {
        os64_hprintf(OS64_STDERR, "lspci: no device at %s\n", slot);
        status = 1;
    }
    return status;
}
