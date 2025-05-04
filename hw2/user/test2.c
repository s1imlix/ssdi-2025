// For testing va_to_pa

#include "helper.h"
#include <stdio.h>
#include <stdlib.h>
#include <asm-generic/unistd.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/types.h>

#define PHYS_ADDR_MASK (0x0000FFFFFFFFF000UL)



struct mapped_page {
    unsigned long pfn;
    unsigned long vaddr;
    struct mapped_page *next;
};

struct mapped_page_list {
    //maintain a list for each level of page table that has been mapped
    struct mapped_page *pgdpage;
    struct mapped_page *pudpage;
    struct mapped_page *pmdpage;
    struct mapped_page *ptepage;
};


int call_remap_page_table(struct expose_pgtbl_args *args, struct mapped_page_list *mapped_list) {
    struct mapped_page **level_head_ptr;
    struct mapped_page *current_node;
    long ret;


    // check cache
    switch (args->level) {
        case LEVEL_PGD: level_head_ptr = &mapped_list->pgdpage; break;
        case LEVEL_PUD: level_head_ptr = &mapped_list->pudpage; break;
        case LEVEL_PMD: level_head_ptr = &mapped_list->pmdpage; break;
        case LEVEL_PTE: level_head_ptr = &mapped_list->ptepage; break;
        default:
            fprintf(stderr, "Invalid level: %d\n", args->level);
            return -EINVAL;
    }

    current_node = *level_head_ptr;
    while (current_node != NULL) {
        if (current_node->pfn == args->pfn) {
            args->remap_vaddr = current_node->vaddr;
            args->pfn = current_node->pfn;
            printf("Page already mapped at level %d, PFN: %lx\n", args->level, args->pfn);
            return 0;
        }
        current_node = current_node->next;
    }

    printf("call_remap_page_table: No existing mapping for level %d PFN %lx. Creating new one.\n",
        args->level, args->pfn);


    // Call the remap_page_table syscall
    unsigned long *ptr = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        perror("mmap failed");
        return -ENOMEM;
    }
    args->remap_vaddr = (unsigned long)ptr;

    ret = remap_page_table(args);
    if (ret < 0) {
        perror("remap_page_table syscall failed");
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


void cleanup_mapped_page_list(struct mapped_page *head) {
    struct mapped_page *current = head;
    struct mapped_page *next;
    while (current != NULL) {
        next = current->next;
        printf("Cleaning up mapping: VA 0x%lx (PFN %lx)\n", current->vaddr, current->pfn);
        free(current);
        current = next;
    }
}


unsigned long va_to_pa(unsigned long va, struct mapped_page_list *mapped_list, VaToPaResult *result) {
    struct expose_pgtbl_args *args = malloc(sizeof(struct expose_pgtbl_args));
    void *region = NULL;
    uint64_t *pgtbl = NULL;
    long ret;
    unsigned long pa = 0;

    args->pid = getpid();
    args->remap_vaddr = (unsigned long)region;
    args->pfn = -1; 
    
    for (int level = LEVEL_PGD; level <= LEVEL_PTE; level++) {
        args->level = level;
        args->remap_vaddr = (unsigned long)region;
        
        printf("va_to_pa: level %d, va: 0x%lx\n", level, va);
        ret = call_remap_page_table(args, mapped_list);

        if (ret < 0) {
            perror("remap_page_table failed");
            munmap(region, PAGE_SIZE);
            free(args);
            result->status = PT_RESOLVE_ERROR;
            result->entry = NULL;
            result->pa = -1;
            return -EINVAL;
        }

        pgtbl = (uint64_t *)args->remap_vaddr;
        region = (void *)args->remap_vaddr;

        if (level == LEVEL_PGD) {
            printf("va_to_pa: Got PGD PFN: %lx\n", args->pfn);
        }

        unsigned long index_shift = PAGE_SHIFT + (3 - level) * 9;
        unsigned long index = (va >> index_shift) & BIT_MASK;
        printf("va_to_pa: Calculating index: (va=0x%lx >> %lu) & 0x%lx = %lu (0x%lx)\n",
              va, index_shift, BIT_MASK, index, index);


        if (!(pgtbl[index] & 0x1)) {
            printf("va_to_pa: Invalid entry at level %d, index %lu\n", level, index);
            printf("va_to_pa: Invalid entry value: 0x%lx\n", pgtbl[index]);
            munmap(region, PAGE_SIZE);
            free(args);
            result->status = PT_MISSING_PGD + level;
            result->entry = (void **)&pgtbl[index];
            result->pa = -1;
            return 0;
        }

        unsigned long next_pfn = (pgtbl[index] & PHYS_ADDR_MASK) >> PAGE_SHIFT;
        printf("va_to_pa: Extracted next PFN from entry: %lx\n", next_pfn);
        
        if (level == LEVEL_PTE) {
            result->entry = (void **)&pgtbl[index];
            pa = (next_pfn << PAGE_SHIFT) | (va & (PAGE_SIZE - 1));
            printf("va_to_pa: Final Physical address: 0x%lx\n", pa);
            break; 
        } else {
            args->pfn = next_pfn;
        }
    }

    munmap(region, PAGE_SIZE);
    free(args);
    result->status = PT_RESOLVE_OK;
    result->pa = pa;
    return pa;
}





int main() {
    struct walk_page_args args;
    int ret;
    VaToPaResult *result = malloc(sizeof(VaToPaResult));

    // initialize the mapping page list
    struct mapped_page_list *mapped_list = malloc(sizeof(struct mapped_page_list));
    mapped_list->pgdpage = NULL;
    mapped_list->pudpage = NULL;
    mapped_list->pmdpage = NULL;
    mapped_list->ptepage = NULL;



    unsigned long *ptr = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        perror("mmap failed");
        free(mapped_list);
        return -ENOMEM;
    }

   
    *(char*)ptr = 1;

    args.pid = getpid();
    args.vaddr = (unsigned long)ptr;
    args.paddr = 0;

    
    printf("[KERNELSPACE] Testing page table walk for address: %p\n", ptr);
    
    ret = syscall(__NR_walk_page_table, &args);
    if (ret < 0) {
        printf("Syscall failed: %s\n", strerror(errno));
        free(ptr);
        return ret;
    }

    printf("[KERNELSPACE] Virtual address:  0x%lx, Physical address: 0x%lx\n", args.vaddr, args.paddr);
    
    unsigned long pa = va_to_pa(args.vaddr, mapped_list, result);
    if (result->status != PT_RESOLVE_OK) {
        printf("va_to_pa failed with status: %d\n", result->status);
        return -EINVAL;
    }
    
    printf("[USERSPACE] Virtual address:  0x%lx, Physical address: 0x%lx\n", args.vaddr, pa);



    // Free the allocated memory
    if (munmap(ptr, 4096) == -1) {
        perror("munmap failed");
    }

    cleanup_mapped_page_list(mapped_list->pgdpage);
    cleanup_mapped_page_list(mapped_list->pudpage);
    cleanup_mapped_page_list(mapped_list->pmdpage);
    cleanup_mapped_page_list(mapped_list->ptepage);
    free(mapped_list);
    free(result);

    return 0;
}