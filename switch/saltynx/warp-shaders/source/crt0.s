// Warp Shaders capture plugin: entry point, MOD0 header and the few syscalls it uses.
//
// SaltyNX Core loads a plugin into the game's heap, links its imports (GLOB_DAT relocations
// against SaltySD* and the game's own symbols), then calls the image base like a function.
// We apply our own R_AARCH64_RELATIVE relocations, clear .bss, run wp_plugin_main and return.
// No libnx, no newlib: nothing here touches the game's TLS or the SDK.

    .section .crt0, "ax", %progbits
    .global _start
    .align 2
_start:
    b       1f
    .word   __nx_mod0 - _start          // SaltyNX reads the MOD0 offset at base+4
    .ascii  "WARPSHDR"

1:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    adr     x0, _start
    adr     x1, __nx_mod0
    bl      wp_relocate
    bl      wp_plugin_main
    ldp     x29, x30, [sp], #16
    ret

    .align 2
    .global __nx_mod0
__nx_mod0:
    .ascii  "MOD0"
    .word   _DYNAMIC        - __nx_mod0
    .word   __bss_start__   - __nx_mod0
    .word   __bss_end__     - __nx_mod0
    .word   0
    .word   0
    .word   0

    .text

// Result svcSleepThread(s64 ns)
    .global wp_svcSleepThread
    .type wp_svcSleepThread, %function
wp_svcSleepThread:
    svc     0x0B
    ret

// Result svcCreateThread(Handle* out, void* entry, void* arg, void* stack_top, int prio, int cpuid)
    .global wp_svcCreateThread
    .type wp_svcCreateThread, %function
wp_svcCreateThread:
    str     x0, [sp, #-16]!
    svc     0x08
    ldr     x2, [sp], #16
    str     w1, [x2]
    ret

// Result svcStartThread(Handle h)
    .global wp_svcStartThread
    .type wp_svcStartThread, %function
wp_svcStartThread:
    svc     0x09
    ret

// Result svcGetInfo(u64* out, u32 id, Handle h, u64 sub)
    .global wp_svcGetInfo
    .type wp_svcGetInfo, %function
wp_svcGetInfo:
    str     x0, [sp, #-16]!
    svc     0x29
    ldr     x2, [sp], #16
    str     x1, [x2]
    ret

// Result svcQueryMemory(MemoryInfo* out, u32* pageinfo, u64 addr)
    .global wp_svcQueryMemory
    .type wp_svcQueryMemory, %function
wp_svcQueryMemory:
    str     x1, [sp, #-16]!
    svc     0x06
    ldr     x2, [sp], #16
    str     w1, [x2]
    ret

// u64 wp_ticks(void): the 19.2 MHz system counter
    .global wp_ticks
    .type wp_ticks, %function
wp_ticks:
    mrs     x0, cntpct_el0
    ret
