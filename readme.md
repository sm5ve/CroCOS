# Cross-Component Operating System (CroCOS)

## Goals
* This project aspires to implement a reasonably small microkernel, with services like hardware drivers and filesystems all being implemented in userspace. 
* **Well commented**, well organized codebase
* Support for multiple architectures (amd64 and aarch64) and SMP implemented early. The initial focus will be on amd64, since this is the architecture I know best.

## Building and running

CroCOS makes use of the `__builtin_is_virtual_base_of` type intrinsic in GCC, present in version 15 and above. 
You will need an x86_64-elf cross compiler. On macOS, this can be installed with homebrew via 
`brew install x86_64-elf-gcc`. If you have QEMU installed, you may build and test the kernel by building the `run` 
target with CMake.