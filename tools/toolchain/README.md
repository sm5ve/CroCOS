# The CroCOS cross toolchain

CroCOS builds with `x86_64-crocos-gcc`, a cross compiler built from this
directory. A stock `x86_64-elf-gcc` (Homebrew's, for instance) **cannot** build
a correct CroCOS kernel. This document explains why, and how to build one.

```sh
./build-toolchain.sh
export PATH="$HOME/opt/crocos-toolchain/bin:$PATH"
```

Expect 30–60 minutes and about 8 GiB of transient disk. The build trees are
deleted afterwards; the installed toolchain is roughly 500 MiB.

## Why a custom toolchain

### Global constructors were silently not running

GCC decides at *configure* time whether to emit global constructors into modern
`.init_array` sections or legacy `.ctors` sections. The relevant logic lives in
`gcc/acinclude.m4`:

```
    case "${target}" in
      aarch64*-linux-gnu*)
	gcc_cv_initfini_array=yes
	;;

      *)
	AC_MSG_CHECKING(cross compile... guessing)
	gcc_cv_initfini_array=no
	;;
    esac
```

For any cross compiler other than `aarch64-linux-gnu`, GCC cannot run a test
program on the target, so it **guesses no** and falls back to `.ctors`. The
guess is only overridden if you pass `--enable-initfini-array` explicitly, and
Homebrew's `x86_64-elf-gcc` formula does not:

```
Configured with: ../configure --target=x86_64-elf ... --enable-languages=c,c++
```

The CroCOS linker script only collects `.init_array`:

```
.init_array ALIGN(4K) : AT(ADDR(.init_array) - @KERNEL_BASE@){
        PROVIDE(__init_array_start = .);
        KEEP (*(.init_array))
        ...
```

So under a stock cross compiler, every namespace-scope object with a dynamic
initializer had its constructor emitted into `.ctors`, which nothing collected
and nothing ran. There was no diagnostic — the objects were simply left
zero-initialized, and `runGlobalConstructors()` iterated an empty array.

This is what the old `WITH_GLOBAL_CONSTRUCTOR` macro worked around: it hand-rolled
a function pointer into `.init_array` and used placement `new` to run the
constructor, because the compiler's own mechanism was being discarded. The macro
was correct, but it had to be applied by hand to every affected global, and
forgetting it was silent.

A historical note: the comment on that macro attributed the problem to
`crtbegin.o`/`crtend.o` not being available in a `-mcmodel=kernel` build. That
was a misdiagnosis. `__init_array_start`/`__init_array_end` come from the kernel's
own linker script, not from `crtbegin.o`, and `__dso_handle` is defined in
`kernel/cxxcompat.cpp`. The kernel never needed those objects for constructors
to work.

The patched toolchain fixes this in two places: `--enable-initfini-array` is
passed explicitly by `build-toolchain.sh`, *and* `*-*-crocos*` is added to the
target case above so that a hand-configured build is correct too.

### libgcc did not share the kernel's ABI

The kernel links against `libgcc` for compiler support routines (`__udivti3` and
similar). A stock cross toolchain ships exactly one `libgcc`, built
`-mcmodel=small` with the red zone enabled — neither of which is true of the
kernel, which is compiled `-mcmodel=kernel -mno-red-zone`.

This happened to work, because the routines the kernel actually references are
self-contained integer code with no absolute references to global data. That is
a property of the current call graph, not a guarantee.

The patched toolchain adds a multilib (`gcc/config/i386/t-x86_64-crocos`) so that
a second `libgcc` is built with the kernel's own flags, and the kernel links
against that one. `build-toolchain.sh` verifies the selection at the end of the
build.

### A target of our own

The triple is `x86_64-crocos` rather than `x86_64-elf`. Today this changes very
little — CroCOS is ELF and the kernel links with an explicit `-T` script — but it
gives us the places to put things that a bare `-elf` target has nowhere to hold:

- `gcc/config/crocos.h` defines `__crocos__`, so code can test for the OS the way
  it would test `__linux__`, and holds `STARTFILE_SPEC`/`ENDFILE_SPEC`/`LIB_SPEC`
  for the eventual userspace link.
- `ld/emulparams/elf_x86_64_crocos.sh` is where userspace link defaults and the
  dynamic loader path will go.
- `libgcc/config.host` builds `crti.o`/`crtn.o` for us, since there is no CroCOS
  libc to supply them yet.

None of this is load-bearing for the kernel. It exists so that adding a C library
later is a matter of filling in specs rather than inventing a target.

## What the patches change

Both patches are against pristine upstream tarballs and apply with `patch -p1`.

`patches/binutils-2.44-crocos.patch`

| File | Change |
|---|---|
| `config.sub` | recognise `crocos` as an OS name |
| `bfd/config.bfd` | x86-64 ELF vectors for `x86_64-*-crocos*` |
| `gas/configure.tgt` | assemble as plain ELF |
| `ld/configure.tgt` | default emulation `elf_x86_64_crocos` |
| `ld/emulparams/elf_x86_64_crocos.sh` | new; currently just sources `elf_x86_64.sh` |
| `ld/Makefile.am`, `ld/Makefile.in` | register the emulation source |

`patches/gcc-15.1.0-crocos.patch`

| File | Change |
|---|---|
| `config.sub` | recognise `crocos` as an OS name |
| `gcc/acinclude.m4`, `gcc/configure` | default `.init_array` to on for `*-*-crocos*` |
| `gcc/config.gcc` | OS case and `x86_64-*-crocos*` `tm_file` |
| `gcc/config/crocos.h` | new; OS header |
| `gcc/config/i386/t-x86_64-crocos` | new; `-mcmodel=kernel -mno-red-zone` multilib |
| `libgcc/config.host` | build `libgcc`, `crti.o`, `crtn.o` for the target |
| `fixincludes/mkfixinc.sh` | no system headers to fix |

### libgcc is not built PIC

`x86_64-*-elf*` uses the `t-libgcc-pic` fragment, which appends `$(PICFLAG)` to
`HOST_LIBGCC2_CFLAGS`. We deliberately do not, because that flag applies to
*every* multilib and `-mcmodel=kernel` rejects PIC outright:

```
cc1: error: code model kernel does not support PIC mode
```

There is no per-multilib override for it, so the choice is between a PIC libgcc
and a kernel multilib, and the kernel multilib is the one that matters. Both
variants are therefore non-PIC. That is correct for the kernel and adequate for
a static userspace. If CroCOS ever grows shared libraries, the PIC libgcc they
need must be arranged for the default multilib only, together with
`crtbeginS.o`/`crtendS.o` in `extra_parts`.

`gcc/configure` and `ld/Makefile.in` are generated files. They are patched in
lockstep with `acinclude.m4` and `Makefile.am` rather than regenerated, because
regenerating requires the exact autotools versions upstream used. Neither project
enables maintainer mode by default, so they will not be regenerated during the
build.

## Rebasing onto a newer GCC or binutils

Bump the version and checksum at the top of `build-toolchain.sh`, then re-apply
the patch with `patch -p1 --merge` and fix up any rejects. The hunks are small
and additive. Two are worth re-checking by hand after a rebase:

- the `gcc_cv_initfini_array` case in `acinclude.m4` and `configure` — if
  upstream restructures it, the hunk may apply to the wrong place. The build
  script's final check will catch this: it compiles a test object and fails the
  build if constructors land anywhere other than `.init_array`.
- `ld/Makefile.in`'s dependency-include line, which moves around between
  automake versions.
