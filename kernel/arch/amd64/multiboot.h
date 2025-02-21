#ifndef MULTIBOOT
#define MULTIBOOT

#define MULTIBOOT_HEADER_MAGIC 0x1BADB002
#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

#define MULTIBOOT_PAGE_ALIGN 1
#define MULTIBOOT_MEMORY_INFO 2
#define MULTIBOOT_VIDEO_MODE 4
#define MULTIBOOT_AOUT_KLUDGE 0x10000

#define MULTIBOOT_HAS_MEMORY
#define MULTIBOOT_HAS_BOOTDEV
#define MULTIBOOT_HAS_CMDLINE
#define MULTIBOOT_HAS_MODS

#define MULTIBOOT_HAS_ELF_SYMBS 0x20
#define MULTIBOOT_HAS_FULL_MMAP 0x40
#define MULTIBOOT_HAS_DRIVE_INFO 0x80
#define MULTIBOOT_HAS_NAME 0x200
#define MULTIBOOT_HAS_VBE 0x800
#define MULTIBOOT_HAS_FRAMEBUFFER 0x1000

#ifndef ASM_FILE

#include <stdint.h>

struct __attribute__((packed)) mboot_info{
    uint32_t flags;

    uint32_t mem_lower;
    uint32_t mem_upper;
	
    uint32_t boot_dev;
	
    uint32_t cmdline_ptr; // char* of kernel arguments
	
    uint32_t mods_count;
    uint32_t mods_ptr;

    struct __attribute__((packed)){
        uint32_t num;
        uint32_t size;
        uint32_t ptr;
        uint32_t shndx;
    } elf_section_header;

    uint32_t mmap_len;
    uint32_t mmap_ptr;
	
    uint32_t drives_len;
    uint32_t drives_ptr;
	
    uint32_t config_ptr;

    uint32_t bootloader_name; // char* with bootloader name
	
    uint32_t apm_table;

    uint32_t vbe_ctrl_into;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_segment;
    uint16_t vbe_interface_offset;
    uint16_t vbe_interface_len;

    uint64_t framebuffer_ptr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t framebuffer_bpp;
    union{
        struct __attribute__((packed)){
            uint32_t addr;
            uint16_t num_colors;
        } pallet;
        struct __attribute__((packed)){
            uint8_t red_field_pos;
            uint8_t red_mask_size;
            uint8_t green_field_pos;
            uint8_t green_mask_size;
            uint8_t blue_field_pos;
            uint8_t blue_mask_size;
        } colors;
    };
};

struct __attribute__((packed)) mboot_color{
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

#define MEM_AVAILABLE 1
#define MEM_RESERVED 2
#define MEM_ACPI_RECLAIMABLE 3
#define MEM_NVS 4
#define MEM_BAD 5


struct __attribute__((packed)) mboot_mmap_entry{
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type;
};

struct __attribute__((packed)) mboot_module{
    uint32_t start_addr;
    uint32_t end_addr;
    uint32_t name_ptr;
    uint32_t reserved;
};

#endif

#endif