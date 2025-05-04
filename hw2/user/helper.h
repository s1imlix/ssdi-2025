#ifndef HELPER_H
#define HELPER_H

#include <linux/expose_pgtbl.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef __NR_remap_page_table
#define __NR_remap_page_table 451
#endif

#ifndef __NR_walk_page_table
#define __NR_walk_page_table 452
#endif

#define PGD_SHIFT 39
#define PUD_SHIFT 30
#define PMD_SHIFT 21
#define PAGE_SHIFT 12
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define NUM_ENTRIES (1UL << (PAGE_SHIFT - 3))
#define BIT_MASK ((1UL << 9) - 1) // 9 bits for indexing (512 entries)
#define PHYS_ADDR_MASK (0x0000FFFFFFFFF000UL)

#define IS_VALID(x) ((x) & 0x1)
#define IS_PRESENT(x) ((x) & 0x2)

// Page table levels
#define LEVEL_PGD 0
#define LEVEL_PUD 1
#define LEVEL_PMD 2
#define LEVEL_PTE 3

typedef enum {
  PT_MISSING_PGD,
  PT_MISSING_PUD,
  PT_MISSING_PMD,
  PT_MISSING_PTE,
  PT_RESOLVE_OK,
  PT_RESOLVE_ERROR
} ResolveStatus;

typedef struct {
  ResolveStatus status;
  void *entry;     // Address of the missing or leaf page table entry
} VaToPaResult;

struct mapped_page {
  unsigned long pfn;
  unsigned long vaddr;
  struct mapped_page *next;
};

struct mapped_page_list {
  // maintain a list for each level of page table that has been mapped
  struct mapped_page *pgdpage;
  struct mapped_page *pudpage;
  struct mapped_page *pmdpage;
  struct mapped_page *ptepage;
};

static inline long remap_page_table(struct expose_pgtbl_args *args) {
  long ret = syscall(__NR_remap_page_table, args);
  return ret;
}

void print_pagetable_page(void *addr);

void cleanup_mapped_page_list(struct mapped_page *head);

int call_remap_page_table(struct expose_pgtbl_args *args,
                          struct mapped_page_list *mapped_list);

#endif
