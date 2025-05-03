// For testing walk_page_table


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

#ifndef __NR_walk_page_table
#define __NR_walk_page_table 452
#endif

struct walk_page_args {
    pid_t pid;
    unsigned long vaddr;
    unsigned long paddr;
};

int main() {
    struct walk_page_args args;
    
    int ret;
    //use mmap to allocate memory

    unsigned long *ptr = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        perror("mmap failed");
        return 1;
    }

   
    *(char*)ptr = 1;

    args.pid = getpid();
    args.vaddr = (unsigned long)ptr;
    args.paddr = 0;

    printf("Testing page table walk for address: %p\n", ptr);
    
    ret = syscall(__NR_walk_page_table, &args);
    if (ret < 0) {
        printf("Syscall failed: %s\n", strerror(errno));
        free(ptr);
        return 1;
    }

    printf("Virtual address:  0x%lx\n", args.vaddr);
    printf("Physical address: 0x%lx\n", args.paddr);

    // Free the allocated memory
    if (munmap(ptr, 4096) == -1) {
        perror("munmap failed");
        return 1;
    }
    return 0;
}