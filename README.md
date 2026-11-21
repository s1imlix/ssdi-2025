# NTU SSDI (Spring 2025): Kernel Assignments

Group assignments for **CSIE5374 Advanced Topics in Software Systems Design and Implementation** at National Taiwan University, Spring 2025 (Group 15). Both assignments target the Linux v6.1 kernel on AArch64 (emulated via QEMU), written in C.

| Assignment | Topic | My Role (B11902083) | Write-up & Spec |
|---|---|---|---|
| [hw1](hw1) | Loadable kernel module (rootkit & syscall filter) | Core LKM (`kallsyms_lookup_name` lookup via kprobe, `.rodata` write enable, module hiding, process masquerading, `read`/`mkdir` syscall hooking and filtering, general cleanup) and testbench | [write-up.md](hw1/write-up.md), [spec](hw1/assignment-1-spec.pdf) |
| [hw2](hw2) | Userspace page-table remapping with `userfaultfd` | `remap_page_table` syscall, `map_fault_va_to_pa`, and userspace page-fault handler | [write-up.md](hw2/write-up.md), [spec](hw2/assignment-2-spec.pdf) |

See each assignment's `write-up.md` for build instructions, technical details, and full team contributions.

## Notes

- Merged from our two original assignment repositories (`hw1/` and `hw2/`) with commit history preserved; untouched Linux kernel headers were stripped from `hw2`'s history.
- Assignment specifications belong to the course staff.
