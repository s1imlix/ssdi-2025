#define _GNU_SOURCE
#include "helper.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/expose_pgtbl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

// Arguments for the fault handler thread
struct handler_args {
  int uffd;
};

struct mapped_page_list* mapped_list;
VaToPaResult* result;
unsigned long pgd_pfn = -1;


/*
  va_to_pa: Converts a virtual address to a physical address using the cached page table
  updates caches if the path is not cached already. 
  @va : virtual address to convert
  @mapped_list : structure containing cached page table pages 
  @result : structure to store the result of the conversion
  @return : physical address if successful, errno if failed
*/
unsigned long va_to_pa(unsigned long va) {
  struct expose_pgtbl_args *args = malloc(sizeof(struct expose_pgtbl_args));
  void *region = NULL;
  unsigned long *pgtbl = NULL;
  long ret;
  unsigned long pa = 0;

  args->pid = getpid();
  args->pfn = pgd_pfn;

  for (int level = LEVEL_PGD; level <= LEVEL_PTE; level++) {
    args->level = level;
    args->remap_vaddr = (unsigned long)region;

    printf("va_to_pa: level %d, va: 0x%lx\n", level, va);
    ret = call_remap_page_table(args, mapped_list);

    if (ret < 0) {
      perror("remap_page_table failed");
      result->status = PT_RESOLVE_ERROR;
      result->entry = NULL;
      free(args);
      return ret;
    }

    pgtbl = (unsigned long *)args->remap_vaddr;
    region = (void *)args->remap_vaddr;

    if (level == LEVEL_PGD) {
      // printf("va_to_pa: Got PGD PFN: %lx\n", args->pfn);
      if (pgd_pfn == -1) {
        pgd_pfn = args->pfn;
      }
    }

    unsigned long index_shift = PAGE_SHIFT + (3 - level) * 9;
    unsigned long index = (va >> index_shift) & BIT_MASK;
    printf("pgtbl: 0x%lx\n", (unsigned long)pgtbl);
    printf("va_to_pa: Calculating index: (va=0x%lx >> %lu) & 0x%lx = %lu "
           "(0x%lx)\n",
          va, index_shift, BIT_MASK, index, index);
    printf("va_to_pa: pgtbl[%lu] = 0x%lx\n", index, pgtbl[index]);
    if (!IS_VALID(pgtbl[index]) || (IS_VALID(pgtbl[index]) && !IS_PRESENT(pgtbl[index]))) {
      // invalid or valid but not present
      printf("va_to_pa: Invalid entry at level %d, index %lu\n", level, index);
      printf("va_to_pa: Invalid entry value: 0x%lx\n", pgtbl[index]);
      munmap(region, PAGE_SIZE);
      free(args);
      result->status = PT_MISSING_PGD + level;
      result->entry = (void *)&pgtbl[index];
      return -EINVAL;
    } 

    unsigned long next_pfn = (pgtbl[index] & PHYS_ADDR_MASK) >> PAGE_SHIFT;
    printf("va_to_pa: Extracted next PFN from entry: %lx\n", next_pfn);

    if (level == LEVEL_PTE) {
      result->entry = (void *)&pgtbl[index];
      pa = (next_pfn << PAGE_SHIFT) | (va & (PAGE_SIZE - 1));
      printf("va_to_pa: Final Physical address: 0x%lx\n", pa);
    } else {
      args->pfn = next_pfn;
    }
  }

  munmap(region, PAGE_SIZE);
  free(args);
  result->status = PT_RESOLVE_OK;
  return pa;
}

void map_fault_va_to_pa(unsigned long fault_va, unsigned long pte) { 
  unsigned long* inval_pgtbl_entry = NULL;
  void* new_pgtbl_page_va = 0;
  while (1) {
    va_to_pa(fault_va); // try walk the page
    if (result->status == PT_RESOLVE_OK) { 
      // reached pte and valid
      break;
    }

    printf("map_fault_va_to_pa: Faulting at level %d, va: 0x%lx\n", result->status, fault_va);
    inval_pgtbl_entry = (unsigned long*)result->entry;

    if (result->status != PT_MISSING_PTE) {
      new_pgtbl_page_va = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
      memset(new_pgtbl_page_va, 0, PAGE_SIZE); // touch the page                          
    } else {
      new_pgtbl_page_va = (void*)pte;
    }

    printf("map_fault_va_to_pa: New page table page allocated at: 0x%lx\n", (unsigned long)new_pgtbl_page_va);
    unsigned long pfn = va_to_pa((unsigned long)new_pgtbl_page_va) >> PAGE_SHIFT;
    if (result->status != PT_RESOLVE_OK) {
      munmap(new_pgtbl_page_va, PAGE_SIZE);
      return;
    }
    *inval_pgtbl_entry = (pfn << PAGE_SHIFT) | 0x3; // set present and writable bits
  }
  return; 
}

// Page fault handler thread
static void *fault_handler(void *arg) {
  struct handler_args *h = (struct handler_args *)arg;
  struct pollfd pfd = {.fd = h->uffd, .events = POLLIN};
  struct uffd_msg msg;

  for (;;) {
    if (poll(&pfd, 1, -1) <= 0) {
      continue;
    }

    if (read(h->uffd, &msg, sizeof(msg)) != sizeof(msg)) {
      continue;
    }

    if (msg.event != UFFD_EVENT_PAGEFAULT) {
      continue;
    }

    // Get fault address and page-aligned address
    unsigned long page_fault_addr = msg.arg.pagefault.address;
    unsigned long fault_addr = page_fault_addr & ~(PAGE_SIZE - 1); // lower 3 bytes to zeros

    printf("======= Page fault at address: 0x%lx =======\n", fault_addr);
    void* new_pte_va = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    memset(new_pte_va, 0, PAGE_SIZE); // touch the page
    printf("handler: New page table page allocated at va=0x%lx -> pa: ?\n", (unsigned long)new_pte_va);
    unsigned long new_pte = va_to_pa((unsigned long)new_pte_va) >> PAGE_SHIFT;
    if (result->status != PT_RESOLVE_OK) {
      printf("handler: va_to_pa failed to find new page at va=0x%lx\n", (unsigned long)new_pte_va);
      munmap(new_pte_va, PAGE_SIZE);
    } else {
      printf("handler: va=0x%lx -> pa=0x%lx, start handling 0x%lx\n", (unsigned long)new_pte_va, new_pte, fault_addr);
      map_fault_va_to_pa(fault_addr, new_pte);
    }
    

    // Wake up the faulting thread
    struct uffdio_range ur = {
        .start = fault_addr,
        .len = PAGE_SIZE,
    };

    if (ioctl(h->uffd, UFFDIO_WAKE, &ur) == -1) {
      perror("ioctl(UFFDIO_WAKE) failed");
    }
  }

  return NULL;
}

int main(int argc, char **argv) {
  mapped_list = malloc(sizeof(struct mapped_page_list));
  result = malloc(sizeof(VaToPaResult));

  int uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
  if (uffd < 0) {
    perror("userfaultfd failed");
    return 1;
  }

  struct uffdio_api ua = {.api = UFFD_API};
  if (ioctl(uffd, UFFDIO_API, &ua) == -1) {
    perror("ioctl(UFFDIO_API) failed");
    return 1;
  }

  // Allocate memory region to be handled by userfaultfd
  char *region = mmap(NULL, 10 * PAGE_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (region == MAP_FAILED) {
    perror("mmap failed for region");
    return 1;
  }

  // Register the region with userfaultfd
  struct uffdio_register ur = {
      .range.start = (unsigned long)region,
      .range.len = 10 * PAGE_SIZE,
      .mode = UFFDIO_REGISTER_MODE_MISSING,
  };

  if (ioctl(uffd, UFFDIO_REGISTER, &ur) == -1) {
    perror("ioctl(UFFDIO_REGISTER) failed");
    return 1;
  }

  struct handler_args h = {
      .uffd = uffd,
  };

  pthread_t tid;
  if (pthread_create(&tid, NULL, fault_handler, &h) != 0) {
    perror("Failed to create fault handler thread");
    return 1;
  }

  printf("Fault handler thread created\n");

  // Access pages to trigger faults
  for (size_t i = 0; i < 10 * PAGE_SIZE; i += PAGE_SIZE) {
    volatile char c = region[i]; // Read to trigger fault
    printf("Page %zu: First byte was %c\n", i / PAGE_SIZE, c);

    region[i] = 'A' + (i / PAGE_SIZE); // Write to page
    printf("Page %zu: First byte now %c\n", i / PAGE_SIZE, region[i]);
  }

  printf("All pages accessed successfully\n");

  printf("Cleaning up resources...\n");

  pthread_cancel(tid);
  pthread_join(tid, NULL);

  if (ioctl(uffd, UFFDIO_UNREGISTER, &ur) == -1) {
    perror("ioctl(UFFDIO_UNREGISTER) failed");
  }

  close(uffd);
  munmap(region, 10 * PAGE_SIZE);

  cleanup_mapped_page_list(mapped_list->pgdpage);
  cleanup_mapped_page_list(mapped_list->pudpage);
  cleanup_mapped_page_list(mapped_list->pmdpage);
  cleanup_mapped_page_list(mapped_list->ptepage);
  free(mapped_list);
  free(result);

  printf("Program completed successfully\n");
  return 0;
}
