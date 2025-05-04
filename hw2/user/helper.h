#ifndef HELPER_H
#define HELPER_H

#include <sys/types.h> 
#include <linux/expose_pgtbl.h>
#include <unistd.h>

#ifndef __NR_remap_page_table
#define __NR_remap_page_table 451
#endif

#ifndef __NR_walk_page_table
#define __NR_walk_page_table 452
#endif

static inline long remap_page_table(struct expose_pgtbl_args *args) {
  long ret = syscall(__NR_remap_page_table, args);
  return ret;
}
void print_pagetable_page(void *addr);

#define PGD_SHIFT 39
#define PUD_SHIFT 30
#define PMD_SHIFT 21
#define PAGE_SHIFT 12
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define NUM_ENTRIES (1UL << (PAGE_SHIFT - 3))
#define BIT_MASK ((1UL << 9) - 1) // 9 bits for indexing (512 entries)

// Page table levels
#define LEVEL_PGD 0
#define LEVEL_PUD 1
#define LEVEL_PMD 2
#define LEVEL_PTE 3

typedef struct pte {
  unsigned long pte;
} PTE;

typedef struct pmd {
  PTE *entries[NUM_ENTRIES];
} PMD;

typedef struct pud {
  PMD *entries[NUM_ENTRIES];
} PUD;

typedef struct pgd {
  PUD *entries[NUM_ENTRIES];
} PGD;

typedef enum {
  PT_RESOLVE_OK,
  PT_MISSING_PGD,
  PT_MISSING_PUD,
  PT_MISSING_PMD,
  PT_MISSING_PTE,
  PT_RESOLVE_ERROR
} ResolveStatus;

typedef struct {
  ResolveStatus status;
  void **entry;         // Address of the missing or leaf page table entry
  unsigned long pa;     // Valid if status == PT_RESOLVE_OK
} VaToPaResult;

#endif
