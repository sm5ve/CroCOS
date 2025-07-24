/*
 * Linker script for CroCOS unit tests
 * Defines custom sections for test registration (similar to Object.h pattern)
 */

SECTIONS
{
    .crocos_unit_tests : {
        PROVIDE(__crocos_unit_tests_start = .);
        KEEP(*(.crocos_unit_tests))
        PROVIDE(__crocos_unit_tests_end = .);
    }
}

/* Insert the test sections into the default linker script */
INSERT AFTER .rodata;