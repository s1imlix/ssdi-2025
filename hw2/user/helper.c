#include "helper.h"
#include <stdio.h>

void print_pagetable_page(char *addr) {
  /*
   * This function prints the page table page at the given address.
   * 48-bit OA on arm64
   */

  // iterate through all pages
  int NUM_PAGES = PAGE_SIZE / (1 << 6); // 64 bytes per page

  unsigned long long int *entry = (unsigned long long int *)addr;

  for (int i = 0; i < NUM_PAGES; i++) {
    for (int bit = 0; bit < 64; bit++) {
      // print the bits in groups of 4
      if (bit % 4 == 0 && bit != 0) {
        printf(" ");
      }
      // print the bit
      printf("%lld", (entry[i] >> (63 - bit)) & 1);
    }
    entry += PAGE_SIZE;
  }
}
