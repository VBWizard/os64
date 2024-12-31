import sys

def extract_indices(virt_addr_hex):
    # Convert hex string to integer
    virt_addr = int(virt_addr_hex, 16)

    # Each index is 9 bits wide. The bit positions for a 64-bit virtual address:
    # PML4: bits [47:39]
    # PDPT: bits [38:30]
    # PD:   bits [29:21]
    # PT:   bits [20:12]

    pml4 = (virt_addr >> 39) & 0x1FF
    pdpt = (virt_addr >> 30) & 0x1FF
    pd   = (virt_addr >> 21) & 0x1FF
    pt   = (virt_addr >> 12) & 0x1FF

    return pml4, pdpt, pd, pt

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("Usage: python script.py <hex_address>")
        sys.exit(1)

    address_hex = sys.argv[1]
    pml4, pdpt, pd, pt = extract_indices(address_hex)
    print("PML4 Index:", pml4)
    print("PDPT Index:", pdpt)
    print("PD Index:", pd)
    print("PT Index:", pt)
