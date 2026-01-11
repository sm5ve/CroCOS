#!/usr/bin/env python3

import argparse
from toollib import *

NUM_VECTORS = 256

parser = argparse.ArgumentParser(description='Generate IDT install macros')


parser.add_argument('-o', type=dir_path,
                    help='')

args = parser.parse_args()

output_dir = args.o

with open(output_dir / "isr.inc", "w") as decl_file, open(output_dir / "isr_set.inc", "w") as set_file:
    for i in range(NUM_VECTORS):
        decl_file.write(f'ISR({i})\n')
        set_file.write(f'SET_ISR({i})\n')