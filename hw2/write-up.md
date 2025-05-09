# SSDI Homework 2 Writeup
## Group 15

> Each of the groups is required to provide a write-up about the assignment. The write-up should include the instructions for how to compile and run your test programs, some explanation of your source code (e.g. description of how your code and test program work), and contributions from each of your group members.

## Compilation 

To apply the kernel patch, run the following command in the kernel directory.
```c
patch -p1 < {location of the patch}/ver1_6.1_KERNEL.patch
```

For compiling the kernel, `make` command from Environment Setup should be sufficient.
```
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc)
```

For `Makefile` (for userspace program) to work, run `make -C ./linux headers_install INSTALL_HDR_PATH=./linux-headers ARCH=arm64` under `linux/`, this exposes necessary headers for userspace program under `linux/linux-headers/include`.

Compiling userspace program is done using provided `Makefile`. Usual `make` and `make clean` suffices (However, please make sure `KERNEL_HEADERS := $(PWD)/../linux/linux-headers/include` is correctly set, here it assumes `Makefile` is located in `user/`.)


## Source Code & Testing 

### `remap_map_table`
Creating this new syscall requires modification for the following files:
```
$KDIR/include/linux/expose_pgtbl.h
$KDIR/include/linux/syscalls.h
$KDIR/include/uapi/asm-generic/unistd.h
$KDIR/kernel/sys.c
$KDIR/kernel/sys_ni.c
```

Most of the other files are to simply define necessary definitions like syscall numbers, while `sys.c` decide the actual behavior for the syscall. Here, we describe our `remap_map_table` implementation, with debugging detail omitted:

The input validation directly reject the following conditions, alongside other checks for loaded data and calls:

- `args->level < 0 || args->level > 3` for invalid page table level
- `args->remap_vaddr & ((1UL << 12) - 1)` for un-aligned virtual addresses
- `find_task_by_vpid(args->pid)==NULL` for invalid PID
- `pfn_valid(args->pfn) == 0` for invalid PFN; For requests for PGD, `virt_to_pfn(pgd_va)` is used to retrieve the PFN before being checked.

In the proceeding part, we try locate the vma for the requested virtual address, split them if exceeding 4096, and finally call `remap_pfn_range()`, all enclosed in a `mmap_write_lock`. 

```c
    mmap_write_lock(mm);
	vma = find_vma(mm, args->remap_vaddr);
	split_vma(mm, vma, args->remap_vaddr + 4096,
		  0); // after this vma->end is remap_vaddr + 4096
	if (!vma) {
		mmap_read_unlock(mm);
		return -EINVAL;
	}
	ret = remap_pfn_range(vma, args->remap_vaddr, args->pfn, 4096,
			      vma->vm_page_prot);
	printk(KERN_INFO "remap_page_table: ret: %d\n", ret);
	mmap_write_unlock(mm);
```

#### Test Program: `test.c`

For testing purposes, `test.c` is created to try allocate a memory region on the PGD level, which we believe to be the only one that requires extra handling compared to the rest, and remap a page table to it. 
```c
int main(int argc, char **argv) {
  void *region = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

  struct expose_pgtbl_args *args = malloc(sizeof(struct expose_pgtbl_args));
  args->pid = getpid();
  args->remap_vaddr = (unsigned long)region;
  args->pfn = 0;
  args->level = LEVEL_PGD; 
  remap_page_table(args);
 }
```


## `userfaultfd.c`

### `va_to_pa`
Here, we describe our approach. Some details, including debug and variable updating are omitted.

For each level of page table starting from the first, we do the following:

- Call the function `call_remap_page_table()`, which wraps `remap_page_table()` with preceeding checks and proceeding updates of lists (identified by pfn) maintaining the previously-mapped pages
- Calculate index in the page table of the virtual address as follows:
```c
unsigned long index_shift = PAGE_SHIFT + (3 - level) * 9;
unsigned long index = (va >> index_shift) & BIT_MASK;
```
- Check if the entry is either "invalid" or "valid but not present", if so, save which level such entry is encountered and the address of the problematic entry before returning early
- Then, we calculate the PFN for the next layer based on the following:
```c
#define PHYS_ADDR_MASK (0x0000FFFFFFFFF000UL)
unsigned long next_pfn = (pgtbl[index] & PHYS_ADDR_MASK) >> PAGE_SHIFT;
```
- Finally, the Physical Address is calculated if the queried level is PTE
```c
pa = (next_pfn << PAGE_SHIFT) | (va & (PAGE_SIZE - 1));
```

#### Test Program: `test2.c`
This test program is meant to test `va_to_pa`, used in `userfaultfd.c`. It implemented the following features:
- Use `va_to_pa`, described earlier, to retrieve the physical address given virtual address.
- Compare the result with a custom-created syscall `walk_page_table` that walk the page table from the kernelspace and check if them match

##### Test System Call: `walk_page_table()`
For testing the correctness of result from `va_to_pa()`, we implemented an additional system call `walk_page_table()` which enable comparing page table walk from the kernelspace side.

For each layer, we used corresponding macro that provide offset (e.g. `pgd_offset` for PGD and vice versa) and check if it's invalid with `(pgd_none(*pgd) || pgd_bad(*pgd)`.

For example, this is to walk PGD. Since required by `pud_offset()`, P4D is still walked even though its not used in ARM64.
```c
pgd = pgd_offset(mm, args->vaddr);
if (pgd_none(*pgd) || pgd_bad(*pgd)) {
	printk(KERN_INFO "Invalid PGD entry\n");
	mmap_read_unlock(mm);
	put_task_struct(task);
	return -EINVAL;
}
```

After reaching the PTE, similar calculation of physical address is conducted.
```c
args->paddr = (pfn << PAGE_SHIFT) | (args->vaddr & ~PAGE_MASK);
```


### `map_fault_va_to_pa`
When given a `fault_va`, we first do a `va_to_pa` to retrieve its physical address, or more specifically, to see if we can walk to a valid PTE, which we expect it to not but instead fault.

Each time it stopped at a non-PTE table, we allocate a new page for the page table, retrieve the pfn of the new region. If it stop at PTE, we use the provided pte from the call.

Using the derived pfn, we update the mapping correspondingly.
Particularly, we discovered that the PTE entry must have the other flags set manually before we wake up the main thread, otherwise the entry will be modified by kernel and corrupt `pfn`.
```c
entry_value = (pte) | 0xe8000000000f43UL;
```

To effectively wake up main thread, we attempt writing 0:
```
*(volatile char*)fault_va = 0;
```

#### `fault_handler`

Extending from the `TODO` part of the skeleton code, the handler first retrieve the addresses:
```c
unsigned long page_fault_addr = msg.arg.pagefault.address;
unsigned long fault_addr = page_fault_addr & ~(PAGE_SIZE - 1);
```

Next, it allocate a new region for the faulting address to map to and set it to 0.
```c
void* new_pte_va = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
memset(new_pte_va, 0, PAGE_SIZE); 
```

Since we are trying to map the fault address to the newly allocated region, we want to know its physical address:
```c
unsigned long new_pte = va_to_pa((unsigned long)new_pte_va) >> PAGE_SHIFT;
```
In order to satisfy the Bonus requirement, which detail will be elaborated in later section, we also keep VA of the new region for later clean up in an array `va_cleanup_list`.
Using the result from `va_to_pa`, we can decide whether to proceed based on if it returned correctly, meaning if the newly provided region is available and we can proceed to `map_fault_va_to_pa()`.

### Safe cleanup

Since we essentially created two VA mappings to the same physical page, naively calling `munmap()` on both VA will cause undefined behavior. The method we employed was cleaning up the PTEs we previously modified at `fault_addr`, then 


#### `clean_va`
This function clean all the PTE entries in the `va_cleanup_list`, described earlier. By clean, it can either (1) unmap the virtual addresses, or (2) walk the page tables using `va_to_pa()` and write the found PTE entry to 0.

```c
// clean the pte entries at va_cleanup_list
void clean_va(int is_mmap, unsigned long* va_list, int va_cnt) {
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

```