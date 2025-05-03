#ifndef HELPER_H
#define HELPER_H

#include <linux/expose_pgtbl.h>
#include <unistd.h>

#ifndef __NR_remap_page_table
#define __NR_remap_page_table 451
#endif

static inline long remap_page_table(struct expose_pgtbl_args *args) {
  long ret = syscall(__NR_remap_page_table, args);
  return ret;
}
void print_pagetable_page(char *addr);

#define PGD_SHIFT 39
#define PUD_SHIFT 30
#define PMD_SHIFT 21
#define PAGE_SHIFT 12
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define BIT_MASK ((1UL << 9) - 1) // 9 bits for indexing (512 entries)

// Page table levels
#define LEVEL_PGD 0
#define LEVEL_PUD 1
#define LEVEL_PMD 2
#define LEVEL_PTE 3

#endif
