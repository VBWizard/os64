# os64

A 64-bit x86 operating system built for educational purposes.

## Getting Started

### Prerequisites

Before you begin, ensure you have the following software installed on your system:

*   **make:** To build the project.
*   **QEMU:** To run the operating system in a virtual machine.
*   **xorriso:** To create the bootable ISO image.
*   **git:** To clone the required dependencies.
*   **A C compiler:** Such as GCC or Clang.

### Building and Running

1.  **Clone the repository:**

    ```bash
    git clone git@github.com:VBWizard/os64.git
    cd os64
    ```

2.  **Build the operating system:**

    The following command will build the kernel and create a bootable ISO image named `os64_kernel.iso`:

    ```bash
    make
    ```

3.  **Run the operating system:**

    To run the operating system in QEMU, use the following command:

    ```bash
    make run
    ```

    This will start a QEMU virtual machine and boot from the `os64_kernel.iso` image.

### Debugging

To run the operating system in debugging mode, use the following command:

```bash
make debug
```

This will start QEMU with the GDB server enabled, allowing you to connect a debugger to inspect the operating system.

### Other Commands

The `GNUmakefile` provides several other useful commands:

*   `make clean`: Removes the build artifacts.
*   `make distclean`: Removes the build artifacts and all downloaded dependencies.
*   `make run-uefi`: Runs the operating system in UEFI mode.
*   `make run-hdd`: Runs the operating system from a virtual hard disk image.

For more details, refer to the `GNUmakefile` in the root of the project.