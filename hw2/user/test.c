#define _GNU_SOURCE
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

// Arguments for exposing page tables
struct expose_pgtbl_args {
  pid_t pid;
  unsigned long remap_vaddr;
  unsigned long pfn;
  unsigned int level;
};

int main(int argc, char **argv) {
  // Allocate memory region to be handled by userfaultfd
  char *region = mmap(NULL, 10 * PAGE_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (region == MAP_FAILED) {
    perror("mmap failed for region");
    return 1;
  }

  remap_page_table()

      return 0;
}
