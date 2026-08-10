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

    .crocos_test_cleanup : {
        PROVIDE(__crocos_test_cleanup_start = .);
        KEEP(*(.crocos_test_cleanup))
        PROVIDE(__crocos_test_cleanup_end = .);
    }

    .crocos_test_prologue : {
        PROVIDE(__crocos_test_prologue_start = .);
        KEEP(*(.crocos_test_prologue))
        PROVIDE(__crocos_test_prologue_end = .);
    }
}

/* Insert the test sections into the default linker script */
INSERT AFTER .rodata;