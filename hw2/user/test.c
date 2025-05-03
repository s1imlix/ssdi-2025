#define _GNU_SOURCE
#include "helper.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/expose_pgtbl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

int main(int argc, char **argv) {
  // Allocate memory region to be handled by userfaultfd
  char *region = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (region == MAP_FAILED) {
    perror("mmap failed for region");
    return 1;
  }

  struct expose_pgtbl_args *args = malloc(sizeof(struct expose_pgtbl_args));
  args->pid = getpid();
  args->remap_vaddr = (unsigned long)region;
  args->pfn = 0;
  args->level = LEVEL_PGD; // Start with PGD level

  printf("remap_page_table(pid=%d, vaddr=%lx, pfn=%lx, level=%d)\n", args->pid,
         args->remap_vaddr, args->pfn, args->level);
  remap_page_table(args);
  printf("remap_page_table returned\n");
  // Print the page table entry
  print_pagetable_page(region);
  return 0;
}
