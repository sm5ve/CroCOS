/* The bootloader will look at this image and start execution at the symbol
   designated as the entry point. */
ENTRY(_start)

/* Tell where the various sections of the object files will be put in the final
   kernel image. */
SECTIONS
{

   /* First put the multiboot header, as it is required to be put very early
       early in the image or the bootloader won't recognize the file format.
       Next we'll put the .text section. */
    . = 1M;
	phys_start = .;
	.multiboot  :
        {
            KEEP(*(.multiboot))
            KEEP(*(.text.bootstrap))
        }
    /* Begin putting sections at 1 MiB, a conventional place for kernels to be
    	   loaded at by the bootloader. */

	. += 0xffffffff80000000;

	.text ALIGN(4K) : AT(ADDR(.text) - 0xffffffff80000000)
	{
		*(.text)
	}

	/* Read-only data. */
	.rodata ALIGN(4K) : AT(ADDR(.rodata) - 0xffffffff80000000)
	{
		*(.rodata)
	}

	/* Read-write data (initialized) */
	.data ALIGN(4K) : AT(ADDR(.data) - 0xffffffff80000000)
	{
		*(.data)
	}

   .init_array ALIGN(4K) : AT(ADDR(.init_array) - 0xffffffff80000000){
           PROVIDE_HIDDEN(__init_array_start = .);
           KEEP (*(.init_array))
           KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*)))
           PROVIDE_HIDDEN (__init_array_end = .);
       }

       .crocos_presort_array ALIGN(4K) : AT(ADDR(.crocos_presort_array) - 0xffffffff80000000){
           . = ALIGN(8);
           __crocos_presort_array_start = .;
           KEEP(*(.crocos_presort_array.*))
           KEEP(*(.crocos_presort_array))
           __crocos_presort_array_end = .;
       }

    .trampoline : AT(ADDR(.trampoline) - 0xffffffff80000000){
        *(.trampoline)
   }

	/* Read-write data (uninitialized) and stack */
	.bss ALIGN(4K) : AT(ADDR(.bss) - 0xffffffff80000000)
	{
	    PROVIDE(bss_start = . - 0xffffffff80000000);
		*(COMMON)
		*(.bss)
	}

	phys_end = (. - 0xffffffff80000000);
	/* The compiler may produce other sections, by default it will put them in
	   a segment with the same name. Simply add stuff here as needed. */
}