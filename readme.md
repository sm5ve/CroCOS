# Cross-Component Operating System (CroCOS)

## Goals
* This project aspires to implement a reasonably small microkernel, with services like hardware drivers and filesystems all being implemented in userspace. 
* **Well commented**, well organized codebase
* Support for multiple architectures (amd64 and aarch64) and SMP implemented early. The initial focus will be on amd64, since this is the architecture I know best.

## Building and running

CroCOS is built with its own `x86_64-crocos` cross toolchain, which you build once from source:

```sh
./tools/toolchain/build-toolchain.sh
export PATH="$HOME/opt/crocos-toolchain/bin:$PATH"
```

Expect 30–60 minutes and roughly 8 GiB of transient disk; the installed toolchain is about 500 MiB.
You will also need QEMU to run the kernel, and Python 3. On macOS the toolchain build needs
Homebrew's `gmp`, `mpfr` and `libmpc`.

CroCOS is C++26, so CMake must be new enough to enable that dialect for GCC. CMake 4.2.2 works;
CMake 3.28 does **not** and fails at generate time with *"Target ... requires the language dialect
CXX26 ... but the current compiler GNU does not support this"*. Note that CLion bundles its own
CMake, which is typically much newer than the one on your `PATH` — if the command line fails but the
IDE works, this is why.

A stock `x86_64-elf-gcc` (Homebrew's, for instance) **will not** build a correct kernel. Such a compiler
emits global constructors into legacy `.ctors` sections rather than `.init_array`, which the kernel's
linker script does not collect — so every global constructor is silently discarded and the objects are
left zero-initialised, with no diagnostic. See [`tools/toolchain/README.md`](tools/toolchain/README.md)
for the full explanation, and for what else the custom target buys us.

With the toolchain on your `PATH`, build and test the kernel by building the `run` target with CMake.