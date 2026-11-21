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
#include <assert.h>

// Arguments for the fault handler thread
struct handler_args {
  int uffd;
};

struct mapped_page_list* mapped_list;
VaToPaResult* result;
unsigned long pgd_pfn = -1;

#define NUM_TEST_PAGES 20 // set more so i don't have to handle stupid issues

int pte_cleanup_cnt = 0;
int va_test_cnt = 0;
unsigned long va_test_list[NUM_TEST_PAGES] = {0}; // va to be cleaned up
unsigned long pte_cleanup_list[NUM_TEST_PAGES] = {0}; // cleanup before exit_mmap


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
      if (pgd_pfn == -1) {
        pgd_pfn = args->pfn;
      }
    }

    unsigned long index_shift = PAGE_SHIFT + (3 - level) * 9;
    unsigned long index = (va >> index_shift) & BIT_MASK;

    if (!IS_VALID(pgtbl[index]) || (IS_VALID(pgtbl[index]) && !IS_PRESENT(pgtbl[index]))) {
      // invalid or valid but not present
      free(args);
      result->status = PT_MISSING_PGD + level;
      result->entry = (void *)&pgtbl[index];
      return -EINVAL;
    } 

    unsigned long next_pfn = (pgtbl[index] & PHYS_ADDR_MASK) >> PAGE_SHIFT;

    if (level == LEVEL_PTE) {
      result->entry = (void *)&pgtbl[index];
      pa = (next_pfn << PAGE_SHIFT) | (va & (PAGE_SIZE - 1));
    } else {
      args->pfn = next_pfn;
    }
  }

  free(args);
  result->status = PT_RESOLVE_OK;
  return pa;
}

// clean the pte entries at va_cleanup_list
void clean_va(int is_mmap, unsigned long* va_list, int va_cnt) {
  for (int i = 0; i < va_cnt; i++) {
    unsigned long va = va_list[i];
    if (is_mmap) {
      munmap((void*)va, PAGE_SIZE);
    } else {
      va_to_pa(va); // try walk the page
      if (result->status != PT_RESOLVE_OK) {
        return;
      }
      unsigned long* pte = (unsigned long*)result->entry;
      *pte = 0;
    } 
  }
}

void map_fault_va_to_pa(unsigned long fault_va, unsigned long pte) { 
  unsigned long* inval_pgtbl_entry = NULL;
  unsigned long entry_value = 0;
  void* new_pgtbl_page_va = 0;
  unsigned long pa = 0;

  while (1) {
    va_to_pa(fault_va); 

    inval_pgtbl_entry = (unsigned long*)result->entry;

    if (result->status != PT_MISSING_PTE) {
      // unlikely, but handles the case where the fault is not in the PTE level
      new_pgtbl_page_va = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                               MAP_SHARED | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
      memset(new_pgtbl_page_va, 0, PAGE_SIZE); // touch the page   
      pa = va_to_pa((unsigned long)new_pgtbl_page_va);
      *inval_pgtbl_entry = 0;
      if (result->status != PT_RESOLVE_OK) {
        munmap(new_pgtbl_page_va, PAGE_SIZE);
        return;
      }   
      entry_value = (pa & PHYS_ADDR_MASK) | 0x1UL; 
      *inval_pgtbl_entry = entry_value;           
    } else {
      entry_value = (pte & PHYS_ADDR_MASK) | 0x1UL; 
      *inval_pgtbl_entry = entry_value;
      break;
    }
  }
  *(volatile char*)fault_va = 0; // access to wake up main
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
                            MAP_SHARED | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    memset(new_pte_va, 0, PAGE_SIZE); // touch the page   
    unsigned long new_pte = va_to_pa((unsigned long)new_pte_va);
    *(unsigned long*)result->entry = 0;
    va_test_list[va_test_cnt++] = (unsigned long)new_pte_va;
    if (result->status != PT_RESOLVE_OK) {
      munmap(new_pte_va, PAGE_SIZE);
    } else {
      map_fault_va_to_pa(fault_addr, new_pte);
      pte_cleanup_list[pte_cleanup_cnt++] = fault_addr;
    }

    printf("handler: Mapped fault address 0x%lx to new page table page 0x%lx, waking up\n", fault_addr, new_pte);
    
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


  if (munmap(region, 10 * PAGE_SIZE) == -1) {
    perror("munmap failed");
  }

  cleanup_mapped_page_list(mapped_list->pgdpage);
  cleanup_mapped_page_list(mapped_list->pudpage);
  cleanup_mapped_page_list(mapped_list->pmdpage);
  cleanup_mapped_page_list(mapped_list->ptepage);
  free(mapped_list);
  free(result);

  printf("Program completed successfully\n");
  
  return 0;
}
