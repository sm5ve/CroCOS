NUM_VECTORS = 256

with open("isr.inc", "w") as decl_file, open("isr_set.inc", "w") as set_file:
    for i in range(NUM_VECTORS):
        decl_file.write(f'ISR({i})\n')
        set_file.write(f'SET_ISR({i})\n')