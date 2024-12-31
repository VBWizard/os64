import struct

# Input binary file
INPUT_BIN = "external/pci_devices.bin"

def parse_binary_file(input_file):
    struct_format = "<H64sH64sBB"  # Format: uint16_t, char[64], uint16_t, char[64], uint8_t, uint8_t
    struct_size = struct.calcsize(struct_format)

    with open(input_file, "rb") as infile:
        while chunk := infile.read(struct_size):
            if len(chunk) != struct_size:
                print(f"Warning: Partial record found ({len(chunk)} bytes), skipping.")
                continue

            # Unpack binary data
            venid, vendor, device_no, devname, devclass, devsubclass = struct.unpack(struct_format, chunk)

            # Decode strings and strip null bytes, with error handling
            vendor = vendor.decode("utf-8", errors="replace").rstrip("\0")
            devname = devname.decode("utf-8", errors="replace").rstrip("\0")

            # Print the record
            print(f"Vendor ID: 0x{venid:04X}, Vendor Name: {vendor}, Device ID: 0x{device_no:04X}, Device Name: {devname}, Device Class: 0x{devclass:02X}, Device Subclass: 0x{devsubclass:02X}")

if __name__ == "__main__":
    parse_binary_file(INPUT_BIN)
