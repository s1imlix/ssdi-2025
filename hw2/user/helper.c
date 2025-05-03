#include "helper.h"
#include <stdint.h>
#include <stdio.h>

void print_pagetable_page(void *addr) {
  printf("=========================\n");
  printf("Page Table Page at %p\n", addr);
  printf("=========================\n");

  /*
   * This function prints the page table page at the given address.
   * 48-bit OA on arm64
   */

  // iterate through all pages
  int NUM_PAGES = PAGE_SIZE / (1 << 3); // 8 bytes per page
  uint64_t *page_table = (uint64_t *)addr;
  for (int i = 0; i < NUM_PAGES; i++) {
    printf("PTE[%02d] = 0x%016lx\n", i, page_table[i]);
  }
}
