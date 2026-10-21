/*
 * ModKitTweaks · arm64 内联 hook（Android 15 / W^X 适配版）
 * ------------------------------------------------------
 * 本机 W^X 实测结论：
 *   RX → RWX        允许      （只加 W，X 本来就在）
 *   RX → RW → RX    被拒 EACCES（加 X 到可写页，SELinux execmod）
 *   anon RW → RX    允许
 * 所以：目标代码页只做一次性 mprotect(RWX)，之后永不改回；跳板放匿名内存。
 *
 * 覆盖目标函数开头 16 字节：
 *     58000050   ldr  x16, #8
 *     D61F0200   br   x16
 *     <8 字节>   替换函数地址
 * 被覆盖的 4 条指令复制进跳板；**所有跳转一律改成「ldr x16,#12 + br/blr + 字面量」
 * 的绝对跳转**，因此对跳板落点没有距离要求（实测匿名内存总是在 4 GB 之外）。
 */
#include "hook.h"

#include <android/log.h>
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define TAG "modkit"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define PATCH_LEN 16          /* 覆盖 4 条指令 */
#define TRAMP_SIZE 4096
#define MAX_INSN 512

static long g_page = 0;
static long page_size() {
    if (!g_page) g_page = sysconf(_SC_PAGESIZE);
    return g_page;
}

static bool make_rwx(void* addr, size_t len) {
    long pg = page_size();
    uintptr_t start = (uintptr_t)addr & ~(uintptr_t)(pg - 1);
    uintptr_t end = ((uintptr_t)addr + len + pg - 1) & ~(uintptr_t)(pg - 1);
    errno = 0;
    if (mprotect((void*)start, end - start, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        LOGE("mprotect(RWX) failed @%p e%d", addr, errno);
        return false;
    }
    return true;
}

/* 绝对跳转：ldr x16,#12 ; br/blr x16 ; (b/nop) ; .quad 目标 */
static int emit_far(uint32_t* o, int n, uintptr_t target, bool is_call) {
    o[n++] = 0x58000070;                       /* ldr x16, #12 */
    o[n++] = is_call ? 0xD63F0200 : 0xD61F0200;/* blr x16 / br x16 */
    o[n++] = is_call ? 0x14000002 : 0xD503201F;/* b +8 / nop */
    *(uintptr_t*)&o[n] = target;
    return n + 2;
}

static void* build_trampoline(uintptr_t target, int* nout) {
    uint32_t saved[4];
    memcpy(saved, (void*)target, PATCH_LEN);

    void* mem = mmap(nullptr, TRAMP_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) { LOGE("trampoline mmap failed e%d", errno); return nullptr; }

    uint32_t* o = (uint32_t*)mem;
    int n = 0;
    for (int i = 0; i < 4; ++i) {
        uint32_t w = saved[i];
        uintptr_t pc = target + (uintptr_t)i * 4;

        if ((w & 0x1F000000) == 0x10000000) {
            /* adr/adrp：原指令算的是「PC 相对页」，搬到跳板后 PC 变了。
             * 直接算出它本来要的绝对地址，用 4 条 movz/movk 拼进同一个寄存器 —— 语义等价。 */
            bool is_adrp = (w & 0x80000000u) != 0;
            uint32_t rd = w & 0x1F;
            int64_t immlo = (int64_t)((w >> 29) & 0x3);
            int64_t immhi = (int64_t)((w >> 5) & 0x7FFFF);
            int64_t imm = (immhi << 2) | immlo;
            if (imm & (1 << 20)) imm -= (1 << 21);       /* 符号扩展 21 位 */
            uintptr_t base = is_adrp ? (pc & ~(uintptr_t)0xFFF) : pc;
            uintptr_t addr = (uintptr_t)((intptr_t)base + (imm << (is_adrp ? 12 : 0)));
            uint64_t v = (uint64_t)addr;
            o[n++] = 0xD2800000u | (((uint32_t)(v & 0xFFFF)) << 5) | rd;             /* movz */
            o[n++] = 0xF2800000u | (1u << 21) | (((uint32_t)((v >> 16) & 0xFFFF)) << 5) | rd;
            o[n++] = 0xF2800000u | (2u << 21) | (((uint32_t)((v >> 32) & 0xFFFF)) << 5) | rd;
            o[n++] = 0xF2800000u | (3u << 21) | (((uint32_t)((v >> 48) & 0xFFFF)) << 5) | rd;
            continue;
        }
        if ((w & 0x3B000000) == 0x18000000) { LOGE("tramp: ldr-literal @+%d", i * 4); goto fail; }
        if ((w & 0x7E000000) == 0x36000000) { LOGE("tramp: tbz/tbnz @+%d", i * 4); goto fail; }

        /* b.cond */
        if ((w & 0xFF000010) == 0x54000000) {
            int64_t imm = (int64_t)((w >> 5) & 0x7FFFF);
            if (imm & (1 << 18)) imm -= (1 << 19);
            uintptr_t tgt = pc + (uintptr_t)(imm * 4);
            if (tgt >= target && tgt < target + PATCH_LEN) { o[n++] = w; continue; }
            uint32_t cond = w & 0xF;
            int ci = n;
            o[n++] = (w & ~0x00FFFFE0u) | (cond ^ 1u);   /* b.!cond（偏移稍后回填） */
            n = emit_far(o, n, tgt, false);
            o[ci] = (o[ci] & ~0x00FFFFE0u) | ((uint32_t)(n - ci) << 5);
            continue;
        }
        /* cbz / cbnz */
        if ((w & 0x7E000000) == 0x34000000) {
            int64_t imm = (int64_t)((w >> 5) & 0x7FFFF);
            if (imm & (1 << 18)) imm -= (1 << 19);
            uintptr_t tgt = pc + (uintptr_t)(imm * 4);
            if (tgt >= target && tgt < target + PATCH_LEN) { o[n++] = w; continue; }
            int ci = n;
            o[n++] = (w ^ 0x01000000u) & ~0x00FFFFE0u;   /* 取反 opcode，偏移稍后回填 */
            n = emit_far(o, n, tgt, false);
            o[ci] = (o[ci] & ~0x00FFFFE0u) | ((uint32_t)(n - ci) << 5);
            continue;
        }
        /* b / bl */
        if ((w & 0x7C000000) == 0x14000000) {
            int64_t imm = (int64_t)(w & 0x3FFFFFF);
            if (imm & (1 << 25)) imm -= (1 << 26);
            uintptr_t tgt = pc + (uintptr_t)(imm * 4);
            n = emit_far(o, n, tgt, (w & 0x80000000u) != 0);
            continue;
        }
        o[n++] = w;
    }

    n = emit_far(o, n, target + PATCH_LEN, false);   /* 跳回原函数 +16 */
    *nout = n;

    __builtin___clear_cache((char*)mem, (char*)mem + n * 4);
    errno = 0;
    if (mprotect(mem, TRAMP_SIZE, PROT_READ | PROT_EXEC) != 0) {
        LOGE("trampoline mprotect(RX) failed e%d", errno);
        munmap(mem, TRAMP_SIZE);
        return nullptr;
    }
    return mem;

fail:
    munmap(mem, TRAMP_SIZE);
    return nullptr;
}

bool hook_install(const char* name, void* target, void* replace, void** orig_out) {
    if (!target || !replace) return false;

    int ncode = 0;
    void* tramp = build_trampoline((uintptr_t)target, &ncode);
    if (!tramp) { LOGE("hook %s: 跳板构造失败", name); return false; }

    if (!make_rwx(target, PATCH_LEN)) { LOGE("hook %s: 代码页不可写", name); return false; }

    uint32_t* t = (uint32_t*)target;
    t[0] = 0x58000050;
    t[1] = 0xD61F0200;
    *(uintptr_t*)&t[2] = (uintptr_t)replace;
    __builtin___clear_cache((char*)target, (char*)target + PATCH_LEN);

    if (orig_out) *orig_out = tramp;
    LOGI("hook %s: ok  target=%p replace=%p tramp=%p (%d insn)", name, target, replace,
         tramp, ncode);
    return true;
}
