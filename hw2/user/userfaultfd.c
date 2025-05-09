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

#define NUM_TEST_PAGES 20 // set more so i don't have to handle stupid issues
int va_cleanup_cnt = 0;
int pte_cleanup_cnt = 0;
unsigned long va_cleanup_list[NUM_TEST_PAGES] = {0}; // cleanup before exit_mmap
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

    // printf("va_to_pa: level %d, va: 0x%lx\n", level, va);
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
    //printf("va_to_pa: Calculating index: (va=0x%lx >> %lu) & 0x%lx = %lu "
    //       "(0x%lx)\n",
    //      va, index_shift, BIT_MASK, index, index);
    // printf("va_to_pa: pgtbl[%lu] = 0x%lx\n", index, pgtbl[index]);
    if (!IS_VALID(pgtbl[index]) || (IS_VALID(pgtbl[index]) && !IS_PRESENT(pgtbl[index]))) {
      // invalid or valid but not present
      // printf("va_to_pa: Invalid entry at level %d, index %lu\n", level, index);
      // printf("va_to_pa: Invalid entry value: 0x%lx\n", pgtbl[index]);
      free(args);
      result->status = PT_MISSING_PGD + level;
      result->entry = (void *)&pgtbl[index];
      // printf("va_to_pa: Invalid entry location: 0x%lx\n", (void *)&pgtbl[index]);
      return -EINVAL;
    } 

    unsigned long next_pfn = (pgtbl[index] & PHYS_ADDR_MASK) >> PAGE_SHIFT;
    // printf("va_to_pa: Extracted next PFN from entry: %lx\n", next_pfn);

    if (level == LEVEL_PTE) {
      result->entry = (void *)&pgtbl[index];
      pa = (next_pfn << PAGE_SHIFT) | (va & (PAGE_SIZE - 1));
      // printf("va_to_pa: Final Physical address: 0x%lx\n", pa);
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
  printf("Performing all overwritten %d pte cleanup\n", va_cnt);
  for (int i = 0; i < va_cnt; i++) {
    // printf("clean_pte: mapped_pte_entries[%d] = 0x%lx\n", i, mapped_pte_entries[i]);
    unsigned long va = va_list[i];
    if (is_mmap) {
      // printf("clean_pte: va_cleanup_list[%d] = 0x%lx\n", i, va);
      munmap((void*)va, PAGE_SIZE);
    } else {
      va_to_pa(va); // try walk the page
      if (result->status != PT_RESOLVE_OK) {
        printf("clean_pte: va_to_pa failed to find page at va=0x%lx\n", va);
        return;
      }
      // printf("cleaning up pte at va=0x%lx\n", va);
      unsigned long* pte = (unsigned long*)result->entry;
      *pte = 0;
    } 
  }
}

void map_fault_va_to_pa(unsigned long fault_va, unsigned long pte) { 
  unsigned long* inval_pgtbl_entry = NULL;
  unsigned long entry_value = 0;
  void* new_pgtbl_page_va = 0;
  unsigned long pfn = 0;
  int total_walk = 0;

  while (total_walk < LEVEL_PTE) {
    va_to_pa(fault_va); // try walk the page
    if (result->status == PT_RESOLVE_OK) { 
      // reached pte and valid
      // printf("map_fault_va_to_pa: Reached PTE, va: 0x%lx\n", fault_va);
      break;
    }

    // printf("map_fault_va_to_pa: Faulting at level %d, va: 0x%lx\n", result->status, fault_va);
    inval_pgtbl_entry = (unsigned long*)result->entry;

    if (result->status != PT_MISSING_PTE) {
      new_pgtbl_page_va = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                               MAP_SHARED | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
      // memset(new_pgtbl_page_va, 0, PAGE_SIZE); // touch the page   
      // printf("map_fault_va_to_pa: New page table page allocated at: 0x%lx\n", (unsigned long)new_pgtbl_page_va);
      pfn = va_to_pa((unsigned long)new_pgtbl_page_va) >> PAGE_SHIFT;
      if (result->status != PT_RESOLVE_OK) {
        // printf("map_fault_va_to_pa: va_to_pa failed to find new page at va=0x%lx\n", (unsigned long)new_pgtbl_page_va);  
        munmap(new_pgtbl_page_va, PAGE_SIZE);
        return;
      }   
      entry_value = (pfn << PAGE_SHIFT) | 0x3UL;                   
    } else {
      printf("map_fault_va_to_pa: Reached PTE, overwriting &pte=0x%lx with provided pte=0x%lx\n", inval_pgtbl_entry, pte);
      entry_value = (pte << PAGE_SHIFT) | 0x3UL; 
      pte_cleanup_list[pte_cleanup_cnt++] = fault_va;
    }

    *inval_pgtbl_entry = entry_value;
    total_walk++;
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
      printf("no poll notification\n");
      continue;
    } 

    if (read(h->uffd, &msg, sizeof(msg)) != sizeof(msg)) {
      printf("read failed\n");
      continue;
    }

    if (msg.event != UFFD_EVENT_PAGEFAULT) {
      printf("Unexpected event: %d\n", msg.event);
      continue;
    }

    // Get fault address and page-aligned address
    unsigned long page_fault_addr = msg.arg.pagefault.address;
    unsigned long fault_addr = page_fault_addr & ~(PAGE_SIZE - 1); // lower 3 bytes to zeros

    printf("======= Page fault at address: 0x%lx =======\n", fault_addr);
    void* new_pte_va = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    va_cleanup_list[va_cleanup_cnt++] = new_pte_va; // we need to clean up these mapping later
    unsigned long new_pte = va_to_pa((unsigned long)new_pte_va) >> PAGE_SHIFT;
    if (result->status != PT_RESOLVE_OK) {
      printf("handler: va_to_pa failed to find new page at va=0x%lx\n", (unsigned long)new_pte_va);
      munmap(new_pte_va, PAGE_SIZE);
    } else {
      printf("handler: va=0x%lx -> pa=0x%lx, start handling 0x%lx\n", (unsigned long)new_pte_va, new_pte, fault_addr);
      map_fault_va_to_pa(fault_addr, new_pte);
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
  munmap(region, 10 * PAGE_SIZE);
  clean_va(0, va_cleanup_list, va_cleanup_cnt); // clean up self-allocated mmap, which already handles region cleanup.
  // clean_va(0, pte_cleanup_list, pte_cleanup_cnt); // clean up modified pte


  cleanup_mapped_page_list(mapped_list->pgdpage);
  cleanup_mapped_page_list(mapped_list->pudpage);
  cleanup_mapped_page_list(mapped_list->pmdpage);
  cleanup_mapped_page_list(mapped_list->ptepage);
  free(mapped_list);
  free(result);

  printf("Program completed successfully\n");
  return 0;
}
