/*
 * memtest - Android 设备端内存快测工具
 *
 * 分配一块可用内存（默认自动：MemAvailable 的 50%，上限 512MB），
 * 用多组位型写入后回读校验，检测当前坏区/位翻转。
 * 输出进度到 stdout，最终结果为单行 JSON（--json）。
 *
 * 编译：zig cc -target aarch64-linux-musl -O2 -static memtest.c -o memtest
 * 宿主单测：-DMEMTEST_HOST_TEST（仅编译纯逻辑）
 */
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef MEMTEST_HOST_TEST
#include <sys/mman.h>
#include <unistd.h>
#endif

#define MEMTEST_VERSION "1.0.0"

/* ---------------- 纯逻辑（宿主单测覆盖） ---------------- */

/* 自动测试量：MemAvailable 的 50%，夹在 [32MB, 512MB] */
long long memtest_auto_budget(long long mem_available_bytes, long long requested_bytes) {
    long long floor_bytes = 32LL * 1024 * 1024;
    long long cap_bytes = 512LL * 1024 * 1024;
    if (mem_available_bytes <= 0) return cap_bytes;
    long long budget = mem_available_bytes / 2;
    if (budget > cap_bytes) budget = cap_bytes;
    if (requested_bytes > 0 && requested_bytes < budget) budget = requested_bytes;
    if (budget < floor_bytes) budget = floor_bytes;
    return budget;
}

/* 位型表 */
typedef struct {
    const char* name;
    uint32_t value;
    int is_random;
} memtest_pattern_t;

static const memtest_pattern_t kPatterns[] = {
    {"zero", 0x00000000u, 0},
    {"ff", 0xFFFFFFFFu, 0},
    {"aa", 0xAAAAAAAAu, 0},
    {"55", 0x55555555u, 0},
    {"random", 0, 1},
    {"bit0", 0x00000001u, 0},
    {"bit7", 0x00000080u, 0},
    {"bit16", 0x00008000u, 0},
    {"bit31", 0x80000000u, 0},
};
#define PATTERN_COUNT ((int)(sizeof(kPatterns) / sizeof(kPatterns[0])))

static uint64_t rng_state = 0x123456789ABCDEF0ULL;

static uint32_t memtest_rng(void) {
    rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)((rng_state >> 32) & 0xFFFFFFFFu);
}

/* ---------------- 设备侧 ---------------- */

#ifndef MEMTEST_HOST_TEST

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static long long read_mem_available(void) {
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char line[256];
    long long kb = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "MemAvailable:", 13) == 0) {
            kb = atoll(line + 13);
            break;
        }
    }
    fclose(f);
    return kb < 0 ? -1 : kb * 1024;
}

int main(int argc, char** argv) {
    long long requested = 0; /* 0 = auto */
    int seconds = 90;
    int json = 0;
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) {
            json = 1;
        } else if (strcmp(argv[i], "--mb") == 0 && i + 1 < argc) {
            if (strcmp(argv[i + 1], "auto") != 0) requested = atoll(argv[i + 1]) * 1024LL * 1024LL;
            i++;
        } else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            seconds = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("memtest %s\n", MEMTEST_VERSION);
            return 0;
        }
    }

    long long avail = read_mem_available();
    long long budget = memtest_auto_budget(avail, requested);

    /* 分配（失败则逐步缩减 10% 重试） */
    uint32_t* buf = NULL;
    long long total = budget;
    while (total >= 8LL * 1024 * 1024) {
        buf = (uint32_t*)malloc((size_t)total);
        if (buf) break;
        total -= total / 10;
    }
    if (!buf) {
        printf("%s", json
            ? "{\"status\":\"error\",\"error\":\"alloc failed\",\"tested_bytes\":0,\"errors\":0}\n"
            : "内存分配失败（可用内存不足）\n");
        return 1;
    }
    long long words = total / 4;

    if (mlock(buf, (size_t)total) != 0) {
        /* 锁不住不致命（无 swap 时意义也小），继续 */
    }

    long long start = now_ms();
    long long deadline = start + (long long)seconds * 1000;
    int patterns_run = 0;
    long long errors = 0;
    long long err_offset = -1;
    uint32_t err_expected = 0, err_got = 0;
    const char* err_pattern = "";
    int stopped_by_deadline = 0;
    rng_state = (uint32_t)(start & 0xFFFFFFFFu) ^ 0x5DEECE66u;

    for (i = 0; i < PATTERN_COUNT; i++) {
        const memtest_pattern_t* p = &kPatterns[i];
        long long t = now_ms();
        if (i > 0 && t > deadline) {
            stopped_by_deadline = 1;
            break;
        }
        if (!json) printf("[%d/%d] 位型 %s ...\n", i + 1, PATTERN_COUNT, p->name);
        uint32_t saved_rng = rng_state;
        long long w;
        if (p->is_random) {
            for (w = 0; w < words; w++) buf[w] = memtest_rng();
            rng_state = saved_rng; /* 校验时重放同一随机序列 */
        } else {
            for (w = 0; w < words; w++) buf[w] = p->value;
        }
        long long local_err = 0;
        for (w = 0; w < words; w++) {
            uint32_t expected = p->is_random ? memtest_rng() : p->value;
            if (buf[w] != expected) {
                errors++;
                local_err++;
                if (err_offset < 0) {
                    err_offset = w * 4;
                    err_expected = p->is_random ? expected : p->value;
                    err_got = buf[w];
                    err_pattern = p->name;
                }
                if (local_err > 100) break;
            }
        }
        patterns_run++;
    }

    long long elapsed = now_ms() - start;
    int status_fail = errors > 0;
    munlock(buf, (size_t)total);
    free(buf);

    const char* status = status_fail ? "fail" : (stopped_by_deadline ? "partial-pass" : "pass");
    if (json) {
        printf("{\"status\":\"%s\",\"tested_bytes\":%lld,\"patterns_run\":%d,"
               "\"patterns_total\":%d,\"errors\":%lld,"
               "\"first_error_offset\":%lld,\"first_error_pattern\":\"%s\","
               "\"expected\":\"0x%08x\",\"got\":\"0x%08x\",\"elapsed_ms\":%lld,"
               "\"mem_available_bytes\":%lld,\"stopped_by_deadline\":%d}\n",
               status, total, patterns_run, PATTERN_COUNT, errors, err_offset,
               err_pattern, err_expected, err_got, elapsed, avail,
               stopped_by_deadline ? 1 : 0);
    } else {
        printf("测试内存: %lld MB（可用 %lld MB）\n", total / 1024 / 1024, avail / 1024 / 1024);
        printf("位型: %d/%d  错误: %lld  耗时: %.1fs\n", patterns_run, PATTERN_COUNT, errors,
               elapsed / 1000.0);
        if (errors > 0) {
            printf("首个错误: 偏移 0x%llx 位型 %s 期望 0x%08x 实际 0x%08x\n", err_offset,
                   err_pattern, err_expected, err_got);
        }
        printf("结论: %s\n", status_fail ? "失败（内存存在坏区）" : "通过");
    }
    return status_fail ? 1 : 0;
}

#else /* MEMTEST_HOST_TEST */

#include <assert.h>

static void memtest_host_tests(void) {
    /* 自动预算：MemAvailable 2GB → 1GB，被 512MB 上限截住 */
    assert(memtest_auto_budget(2LL * 1024 * 1024 * 1024, 0) == 512LL * 1024 * 1024);
    /* MemAvailable 400MB → 200MB */
    assert(memtest_auto_budget(400LL * 1024 * 1024, 0) == 200LL * 1024 * 1024);
    /* 指定量小于自动预算 → 用指定量（64MB） */
    assert(memtest_auto_budget(400LL * 1024 * 1024, 64LL * 1024 * 1024) == 64LL * 1024 * 1024);
    /* 指定量大于自动预算 → 用自动预算 */
    assert(memtest_auto_budget(400LL * 1024 * 1024, 300LL * 1024 * 1024) == 200LL * 1024 * 1024);
    /* MemAvailable 极小 → 下限 32MB */
    assert(memtest_auto_budget(10LL * 1024 * 1024, 0) == 32LL * 1024 * 1024);
    /* MemAvailable 未知 → 上限 512MB */
    assert(memtest_auto_budget(-1, 0) == 512LL * 1024 * 1024);
    /* 位型数量合理 */
    assert(PATTERN_COUNT == 9);
    printf("memtest host tests passed\n");
}

int main(void) {
    memtest_host_tests();
    return 0;
}

#endif
