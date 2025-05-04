#include "helper.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

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

void cleanup_mapped_page_list(struct mapped_page *head) {
  struct mapped_page *current = head;
  struct mapped_page *next;
  while (current != NULL) {
    next = current->next;
    printf("Cleaning up mapping: VA 0x%lx (PFN %lx)\n", current->vaddr,
           current->pfn);
    free(current);
    current = next;
  }
}

/*
  * call_remap_page_table: Wrapper of the remap_page_table syscall to map a page table or return a cached one
  * @args: Arguments for the remap_page_table syscall
  * @mapped_list: List of mapped pages for each level
  * @return: 0 and corresponding page on args on success, negative error code on failure
*/


int call_remap_page_table(struct expose_pgtbl_args *args,
                          struct mapped_page_list *mapped_list) {
  struct mapped_page **level_head_ptr;
  struct mapped_page *current_node;
  long ret;

  // check cache
  switch (args->level) {
  case LEVEL_PGD:
    level_head_ptr = &mapped_list->pgdpage;
    break;
  case LEVEL_PUD:
    level_head_ptr = &mapped_list->pudpage;
    break;
  case LEVEL_PMD:
    level_head_ptr = &mapped_list->pmdpage;
    break;
  case LEVEL_PTE:
    level_head_ptr = &mapped_list->ptepage;
    break;
  default:
    fprintf(stderr, "Invalid level: %d\n", args->level);
    return -EINVAL;
  }

  current_node = *level_head_ptr;

  while (current_node != NULL) {
    if (current_node->pfn == args->pfn) {
      args->remap_vaddr = current_node->vaddr;
      args->pfn = current_node->pfn;
      printf("Page already mapped at level %d, PFN: %lx\n", args->level, current_node->pfn);
      return 0;
    }
    current_node = current_node->next;
  }

  printf("call_remap_page_table: No existing mapping for level %d PFN %lx. "
         "Creating new one.\n",
         args->level, args->pfn);

  // Call the remap_page_table syscall
  unsigned long *ptr = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (ptr == MAP_FAILED) {
    perror("mmap failed");
    return -ENOMEM;
  }
  args->remap_vaddr = (unsigned long)ptr;
  printf("call_remap_page_table: remap_page_table(pfn=%lx, level=%d, vaddr=%lx)\n", args->pfn, args->level, args->remap_vaddr);
  ret = remap_page_table(args);
  if (ret < 0) {
    perror("remap_page_table syscall failed");
    munmap(ptr, PAGE_SIZE);
    return ret;
  }

  printf("call_remap_page_table: Mapped PFN %lx at level %d\n", args->pfn, args->level);

  // update cache
  struct mapped_page *new_node = malloc(sizeof(struct mapped_page));
  if (new_node == NULL) {
    perror("malloc failed");
    return -ENOMEM;
  }
  new_node->pfn = args->pfn;
  new_node->vaddr = args->remap_vaddr;
  new_node->next = NULL;
  if (*level_head_ptr == NULL) {
    *level_head_ptr = new_node;
  } else {
    struct mapped_page *temp = *level_head_ptr;
    while (temp->next != NULL) {
      temp = temp->next;
    }
    temp->next = new_node;
  }
  return 0;
}
