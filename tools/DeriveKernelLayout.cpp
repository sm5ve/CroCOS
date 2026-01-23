//
// Created by Spencer Martin on 1/22/26.
//

#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <iomanip>
#include <kmemlayout.h>

std::string format_hex(uint64_t value) {
    std::ostringstream oss;
    oss << std::hex << value;
    return oss.str();
}

void replace_all(std::string& str, const std::string& from, const std::string& to) {
    size_t pos = 0;
    while ((pos = str.find(from, pos)) != std::string::npos) {
        str.replace(pos, from.length(), to);
        pos += to.length();
    }
}

int main(int argc, char** argv) {
    // Compute the kernel memory layout
    constexpr auto kernelBase = kernel::getKernelMemRegionStart(0);
    std::cout << "Kernel base: " << std::hex << kernelBase.value << '\n';

    if (argc != 4) {
        std::cerr << "Usage: " << argv[0] << " <template_file> <output_linker_file> <output_header_file>\n";
        return 1;
    }

    const char* template_path = argv[1];
    const char* output_path = argv[2];
    const char* output_header_path = argv[3];

    // Open input and output files
    std::ifstream in(template_path);
    if (!in) {
        std::cerr << "Error: Could not open template file: " << template_path << "\n";
        return 1;
    }

    std::ofstream out(output_path);
    if (!out) {
        std::cerr << "Error: Could not open output file: " << output_path << "\n";
        return 1;
    }

    // Process each line
    std::string line;
    while (std::getline(in, line)) {
        replace_all(line, "@KERNEL_BASE@", "0x" + format_hex(kernelBase.value));
        //replace_all(line, "@PAGE_ALLOC_META_BASE@", "0x" + format_hex(layout.page_alloc_meta_base));
        // Add more replacements as needed

        out << line << '\n';
    }

    std::ofstream header_out(output_header_path);
    header_out << "#define VMEM_OFFSET 0x" << std::hex << kernelBase.value << '\n';

    return 0;
}