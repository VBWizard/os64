import struct

def replace_non_ascii(s):
    """Replace non-ASCII characters in a string with a space."""
    return ''.join(c if ord(c) < 128 else ' ' for c in s)

# Input and output files
PCI_IDS = "pci.ids"
OUTPUT_BIN = "external/pci_devices.bin"

def pad_or_truncate(s, length=64):
    """Pad or truncate a string to a fixed length with null bytes."""
    s = s.strip()  # Trim whitespace
    return s[:length].ljust(length, '\0')  # Truncate and pad

def parse_pci_ids(input_file, output_file):
    with open(input_file, "r", encoding="utf-8") as infile, open(output_file, "wb") as outfile:
        vendor_id = None
        vendor_name = None

        for line in infile:
            line = line.rstrip()

            # Skip blank lines and comments
            if not line or line.startswith("#"):
                continue

            # Vendor line: Ensure it starts with a 4-character hexadecimal ID
            if not line.startswith("\t") and len(line) > 4 and line[:4].isalnum():
                try:
                    vendor_id = int(line[:4], 16)  # First 4 characters as hex
                    vendor_name = pad_or_truncate(line[6:])  # Rest is vendor name
                except ValueError:
                    print(f"Skipping invalid vendor line: {line}")
                continue

            # Device line: Ensure it starts with a tab and a valid 4-character ID
            if line.startswith("\t") and len(line) > 5 and line[1:5].isalnum():
                try:
                    device_id = int(line[1:5], 16)  # First 4 characters after tab as hex
                    device_name = pad_or_truncate(line[7:])  # Rest is device name

                    # Default devclass and devsubclass (placeholder values)
                    devclass = 0x00  # Default class code
                    devsubclass = 0x00  # Default subclass code

                    device_name = replace_non_ascii(device_name)
                    vendor_name = replace_non_ascii(vendor_name)
                    # Write binary output in the expected struct format
                    outfile.write(struct.pack("<H", vendor_id))  # Vendor ID (2 bytes)
                    outfile.write(vendor_name.encode("utf-8"))  # Vendor Name (64 bytes)
                    outfile.write(struct.pack("<H", device_id))  # Device ID (2 bytes)
                    outfile.write(device_name.encode("utf-8"))  # Device Name (64 bytes)
                    outfile.write(struct.pack("<B", devclass))  # Device Class (1 byte)
                    outfile.write(struct.pack("<B", devsubclass))  # Device Subclass (1 byte)
                except ValueError:
                    print(f"Skipping invalid device line: {line}")

if __name__ == "__main__":
    parse_pci_ids(PCI_IDS, OUTPUT_BIN)
    print(f"Binary file written to {OUTPUT_BIN}")
