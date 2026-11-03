#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *bin_path = (argc > 1) ? argv[1] : "/opt/xdjaz/pdj/EP147";
    FILE *f = fopen(bin_path, "rb");
    if (!f) { perror("fopen"); return 1; }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *data = malloc(sz);
    if (!data) return 1;
    fread(data, 1, sz, f);
    fclose(f);

    uint64_t target_table = 0x29aff60;
    uint64_t page_base = target_table & ~0xfffULL;
    uint32_t page_off  = target_table & 0xfff;

    printf("Searching for references to Caution Table %lx (page %lx, off %x)...\n",
           target_table, page_base, page_off);

    /* 1. Pointer search */
    for (long i = 0; i < sz - 8; i += 4) {
        uint64_t val = *(uint64_t*)(data + i);
        if (val == target_table) {
            printf("Found pointer to table at VA %lx (file offset %lx)\n", i + 0x400000, i);
        }
    }

    /* 2. ADRP search */
    for (long pc = 0x400000; pc < sz + 0x400000 - 8; pc += 4) {
        uint32_t inst = *(uint32_t*)(data + (pc - 0x400000));
        if ((inst & 0x9f000000) == 0x90000000) { /* ADRP */
            uint32_t immlo = (inst >> 29) & 3;
            uint32_t immhi = (inst >> 5) & 0x7ffff;
            int64_t imm = (immhi << 2) | immlo;
            if (imm & 0x100000) imm -= 0x200000;
            uint64_t target = ((pc >> 12) + imm) << 12;
            if (target == page_base) {
                uint32_t rd = inst & 0x1f;
                uint32_t next = *(uint32_t*)(data + (pc - 0x400000 + 4));
                /* ADD 64-bit immediate */
                if ((next & 0xff800000) == 0x91000000) {
                    uint32_t add_imm = (next >> 10) & 0xfff;
                    uint32_t rn = (next >> 5) & 0x1f;
                    if (rn == rd && add_imm == page_off) {
                        printf("Found ADRP+ADD at VA %lx -> sets reg x%d to table base\n", pc, next & 0x1f);
                    }
                }
                /* Or LDR */
                if ((next & 0xffc00000) == 0xf9400000) {
                    uint32_t ldr_imm = ((next >> 10) & 0xfff) << 3;
                    uint32_t rn = (next >> 5) & 0x1f;
                    if (rn == rd && ldr_imm == page_off) {
                        printf("Found ADRP+LDR at VA %lx\n", pc);
                    }
                }
            }
        }
    }

    printf("\nScanning for all 'mov wX, #0x2b' (Caution 43) sites in .text...\n");
    for (long pc = 0x400000; pc < sz + 0x400000; pc += 4) {
        uint32_t inst = *(uint32_t*)(data + (pc - 0x400000));
        if ((inst & 0xffffffe0) == 0x52800560) {
            uint32_t rd = inst & 0x1f;
            printf("Found 'mov w%d, #0x2b' at VA %lx (file offset %lx)\n", rd, pc, pc - 0x400000);
        }
    }

    return 0;
}
