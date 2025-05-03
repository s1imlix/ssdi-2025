#ifndef _UAPI_EXPOSE_PGTBL_H
#define _UAPI_EXPOSE_PGTBL_H

#include <linux/types.h>

struct expose_pgtbl_args {
	// PID of the target task
	pid_t pid;
	// User-space virtual address to map the page table page
	unsigned long remap_vaddr;
	// Physical frame number from the previous level page table entry
	unsigned long pfn;
	// page table level (0: PGD, 1: PUD, 2: PMD, 3: PTE)
	unsigned int level;
};

struct walk_page_args {
	pid_t pid;           
	unsigned long vaddr; 
	unsigned long paddr; 
};

#endif /* _UAPI_EXPOSE_PGTBL_H */
