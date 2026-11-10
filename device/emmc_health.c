/*
 * emmc_health - eMMC 健康度检测工具（Android 设备端）
 *
 * 读取 JEDEC Ext CSD 健康字段（pre_eol_info / life_time_estimation_a/b）
 * 与 /sys/class/mmc_host 身份信息，输出中文文本或 --json 机读结果。
 *
 * 读取途径（按优先级）：
 *   1. MMC_IOC_CMD ioctl 发送 CMD8(SEND_EXT_CSD) 读 512 字节 Ext CSD（需 root）
 *   2. debugfs ext_csd 文件（512 字节裸数据或文本转储，需 root+debugfs）
 *   3. debugfs health 文件（部分 vendor 内核提供，文本格式）
 *
 * 身份信息（通常无需 root）来自 /sys/class/block/mmcblkX/device 与
 * /sys/class/mmc_host/mmcN/mmcN:0001（两者是同一目录）。
 *
 * 宿主单测：-DEMHC_HOST_TEST 仅编译纯逻辑（无设备 IO）。
 * 编译：zig cc -target aarch64-linux-musl -O2 -static emmc_health.c -o emmc_health
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef EMHC_HOST_TEST

#include <sys/ioctl.h>
#include <dirent.h>
#include <sys/sysmacros.h>

#endif

#define EMHC_VERSION "1.0.0"

/* ------------------------------------------------------------------ */
/* 常量与结构                                                          */
/* ------------------------------------------------------------------ */

#define EXT_CSD_SIZE 512
#define EXT_CSD_IDX_REV 192        /* EXT_CSD_REV */
#define EXT_CSD_IDX_SEC_COUNT 212  /* 用户区容量，LE 32bit，单位扇区 */
#define EXT_CSD_IDX_PRE_EOL 268    /* PRE_EOL_INFO */
#define EXT_CSD_IDX_LIFE_A 269     /* DEVICE_LIFE_TIME_EST_TYP_A */
#define EXT_CSD_IDX_LIFE_B 270     /* DEVICE_LIFE_TIME_EST_TYP_B */

/* MMC 块设备主设备号与 ioctl 定义（内核 linux/mmc/ioctl.h 同构） */
#define MMC_BLOCK_MAJOR 179

struct emmc_ioc_cmd {
    union {
        uint64_t data_ptr;
        uint32_t data_ptr32[2];
    };
    uint32_t blocks;
    uint32_t blksz;
    uint32_t opcode;
    uint32_t arg;
    uint32_t flags;
    uint32_t write_flag;
    uint32_t postdelay;
    uint32_t response[4];
    uint32_t busy_timeout;
    uint32_t padding[5];
};

#ifndef EMHC_HOST_TEST
/* 与内核 _IOWR(MMC_BLOCK_MAJOR, 0, struct mmc_ioc_cmd) 完全一致 */
#define EMHC_IOC_CMD _IOWR(MMC_BLOCK_MAJOR, 0, struct emmc_ioc_cmd)

/* 内核 include/linux/mmc/core.h 的 rsp/cmd 标志 */
#define EMHC_RSP_PRESENT (1u << 0)
#define EMHC_RSP_CRC (1u << 2)
#define EMHC_RSP_OPCODE (1u << 4)
#define EMHC_CMD_ADTC (1u << 5)
#define EMHC_RSP_SPI_S1 (1u << 7)
#define EMHC_RSP_R1 (EMHC_RSP_PRESENT | EMHC_RSP_CRC | EMHC_RSP_OPCODE)
#define EMHC_RSP_SPI_R1 (EMHC_RSP_SPI_S1)

#define MMC_SEND_EXT_CSD 8 /* CMD8 */
#endif

/* 结果来源 */
typedef enum {
    SRC_NONE = 0,
    SRC_IOCTL,
    SRC_DEBUGFS_EXT_CSD_BIN,
    SRC_DEBUGFS_EXT_CSD_TEXT,
    SRC_DEBUGFS_HEALTH
} emmc_source_t;

static const char* source_name(emmc_source_t s) {
    switch (s) {
        case SRC_IOCTL: return "ioctl";
        case SRC_DEBUGFS_EXT_CSD_BIN: return "debugfs-extcsd-bin";
        case SRC_DEBUGFS_EXT_CSD_TEXT: return "debugfs-extcsd-text";
        case SRC_DEBUGFS_HEALTH: return "debugfs-health";
        default: return "none";
    }
}

/* ------------------------------------------------------------------ */
/* 纯逻辑：厂商表 / 等级映射 / 结论（宿主单测覆盖这些函数）              */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t id;
    const char* name;
} emmc_vendor_t;

/* 内核 drivers/mmc/core/card.h + mmc-utils lsmmc 常见表（仅保留可靠项） */
static const emmc_vendor_t kVendors[] = {
    {0x02, "SanDisk"},
    {0x03, "SanDisk"},
    {0x05, "Nanya"},
    {0x06, "Toshiba"},
    {0x09, "ATP"},
    {0x11, "Toshiba/Kioxia"},
    {0x13, "Micron"},
    {0x15, "Samsung"},
    {0x27, "Apacer"},
    {0x45, "SanDisk/Western Digital"},
    {0x5D, "Swissbit"},
    {0x88, "FORESEE/Longsys"},
    {0x90, "SK Hynix"},
    {0x9B, "YMTC"},
    {0xFE, "Numonyx/Micron"},
};

const char* emmc_vendor_name(uint8_t mid) {
    size_t i;
    for (i = 0; i < sizeof(kVendors) / sizeof(kVendors[0]); i++) {
        if (kVendors[i].id == mid) return kVendors[i].name;
    }
    return NULL;
}

/* pre_eol_info：0x01 正常 / 0x02 警告（预留块已耗 80%-90%）/ 0x03 紧急（>90%） */
const char* emmc_pre_eol_text(uint8_t v) {
    if (v == 0x01) return "normal";
    if (v == 0x02) return "warning";
    if (v == 0x03) return "urgent";
    return "unknown";
}

/* life_time_estimation：0x01=0-10% ... 0x0A=90-100%，0x0B=超过设计寿命 */
const char* emmc_life_text(uint8_t v) {
    static const char* bands[] = {"0-10%", "10-20%", "20-30%", "30-40%",
                                  "40-50%", "50-60%", "60-70%", "70-80%",
                                  "80-90%", "90-100%", "over-life"};
    if (v >= 0x01 && v <= 0x0B) return bands[v - 0x01];
    return "unknown";
}

typedef enum { VERDICT_OK = 0, VERDICT_WARN, VERDICT_URGENT, VERDICT_UNKNOWN } emmc_verdict_t;

const char* emmc_verdict_text(emmc_verdict_t v) {
    switch (v) {
        case VERDICT_OK: return "ok";
        case VERDICT_WARN: return "warning";
        case VERDICT_URGENT: return "urgent";
        default: return "unknown";
    }
}

/* 综合结论：pre_eol 主导；寿命 >=80%（0x09+）贡献警告，超过设计寿命贡献紧急。
 * 非法值（>0x0B，常见于文本误读）不参与判定，避免放大成误报。 */
emmc_verdict_t emmc_verdict(uint8_t pre_eol, uint8_t life_a, uint8_t life_b, int has_data) {
    emmc_verdict_t v = VERDICT_OK;
    if (!has_data) return VERDICT_UNKNOWN;
    if (pre_eol == 0x02) v = VERDICT_WARN;
    if (pre_eol == 0x03) return VERDICT_URGENT;
    int la = (life_a >= 0x01 && life_a <= 0x0B) ? life_a : 0;
    int lb = (life_b >= 0x01 && life_b <= 0x0B) ? life_b : 0;
    if (la == 0x0B || lb == 0x0B) return VERDICT_URGENT;
    if (la >= 0x09 || lb >= 0x09) {
        if (v < VERDICT_WARN) v = VERDICT_WARN;
    }
    return v;
}

/* ext_csd rev(字节192) -> MMC 规范版本 */
const char* emmc_spec_from_rev(uint8_t rev) {
    static const char* specs[] = {"4.0", "4.1", "4.2", "4.3", "4.4", "4.5", "5.0", "5.1"};
    if (rev <= 7) return specs[rev];
    return "unknown";
}

/* debugfs ext_csd 文本行解析："268: 0x01" / "268 01" / "268 08" / "0x10c: 0x02"
 * token 规则：带 0x 前缀按 16 进制，否则一律 10 进制（避免 0 前缀八进制歧义）。
 * 返回 0 命中并写 *value；非 0 未命中。*/
static unsigned long parse_idx_token(const char* s, size_t len) {
    if (len > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        char tmp[64];
        size_t n = len < sizeof(tmp) - 1 ? len : sizeof(tmp) - 1;
        memcpy(tmp, s, n);
        tmp[n] = '\0';
        return strtoul(tmp + 2, NULL, 16);
    }
    char tmp[64];
    size_t n = len < sizeof(tmp) - 1 ? len : sizeof(tmp) - 1;
    memcpy(tmp, s, n);
    tmp[n] = '\0';
    return strtoul(tmp, NULL, 10);
}

int emmc_parse_extcsd_line(const char* line, int want_index, uint8_t* value) {
    long idx = -1;
    unsigned long val = 0;
    int have_idx = 0, have_val = 0;
    const char* p = line;
    while (*p && (isspace((unsigned char)*p))) p++;
    if (*p == '\0' || *p == '#') return -1;
    while (*p) {
        const char* tok_start = p;
        while (*p && !isspace((unsigned char)*p) && *p != ':' && *p != '=') p++;
        size_t len = (size_t)(p - tok_start);
        int is_idx_sep = (*p == ':' || *p == '=');
        if (len > 0) {
            unsigned long n = parse_idx_token(tok_start, len);
            if (!have_idx) {
                idx = (long)n;
                have_idx = 1;
            } else if (!have_val) {
                val = n;
                have_val = 1;
            }
        }
        if (is_idx_sep) {
            p++;
        } else if (*p) {
            p++;
        }
        while (*p && (isspace((unsigned char)*p) || *p == ':' || *p == '=')) p++;
        if (have_idx && have_val) break;
    }
    if (have_idx && have_val && idx == want_index) {
        *value = (uint8_t)(val & 0xFF);
        return 0;
    }
    return -1;
}

/* 连续 hex blob 纯逻辑解析（3.x debugfs ext_csd 默认格式；宿主单测可直接测）：
 * 1024 个 hex 字符（每字节 2 字符，无分隔），容忍结尾一个换行；
 * 出现其他空白/字符即判定不是 blob。 */
int emmc_parse_hexblob_buf(const char* buf, size_t len, int index, uint8_t* value) {
    size_t need = (size_t)(index + 1) * 2;
    size_t hex_count = 0;
    size_t i;
    for (i = 0; i < len; i++) {
        char c = buf[i];
        if (isxdigit((unsigned char)c)) {
            hex_count++;
        } else if (c == '\n' || c == '\r' || c == '\0') {
            break; /* 尾部换行结束 */
        } else {
            return -1; /* 中途空白/分隔符 → 不是 blob 格式 */
        }
    }
    if (hex_count < need) return -1;
    size_t seen = 0;
    for (i = 0; i + 1 < len; i++) {
        if (!isxdigit((unsigned char)buf[i])) continue;
        if (!isxdigit((unsigned char)buf[i + 1])) return -1;
        if (seen == (size_t)index) {
            char pair[3] = {buf[i], buf[i + 1], '\0'};
            *value = (uint8_t)strtoul(pair, NULL, 16);
            return 0;
        }
        seen++;
        i++;
    }
    return -1;
}

/* debugfs health 文本：提取 pre_eol_info / life_time_estimation_a|b 的 16 进制值 */
static int emmc_parse_health_token(const char* line, const char* token, uint8_t* value) {
    const char* hit = strstr(line, token);
    size_t tlen = strlen(token);
    if (!hit) return -1;
    const char* p = hit + tlen;
    while (*p && (isspace((unsigned char)*p) || *p == ':' || *p == '=')) p++;
    if (*p == '\0') return -1;
    unsigned long v = strtoul(p, NULL, 16);
    *value = (uint8_t)(v & 0xFF);
    return 0;
}

int emmc_parse_health_line(const char* line, int want_index, uint8_t* value) {
    if (want_index == EXT_CSD_IDX_PRE_EOL)
        return emmc_parse_health_token(line, "pre_eol_info", value);
    if (want_index == EXT_CSD_IDX_LIFE_A)
        return emmc_parse_health_token(line, "life_time_estimation_a", value);
    if (want_index == EXT_CSD_IDX_LIFE_B)
        return emmc_parse_health_token(line, "life_time_estimation_b", value);
    return -1;
}

/* 老内核 debugfs ext_csd 表格转储：每行 "<行起始hex> b0 b1 ... bN"（hex 字节序列）。
 * 目标 index 落在 [row, row+N) 时取对应字节。0x 前缀可有可无。 */
int emmc_parse_extcsd_table(const char* buf, int index, uint8_t* value) {
    const char* p = buf;
    while (*p) {
        const char* eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[512];
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        int row = -1;
        int vals[64];
        int nvals = 0;
        int bad = 0;
        char* saveptr = NULL;
        char* tok = strtok_r(line, " \t\r", &saveptr);
        while (tok) {
            char* end = NULL;
            long v = strtol(tok, &end, 16);
            if (end == tok || *end != '\0') {
                bad = 1; /* 非纯 hex token，此行不属于表格 */
                break;
            }
            if (row < 0) {
                row = (int)v;
            } else if (nvals < 64) {
                vals[nvals++] = (int)v;
            }
            tok = strtok_r(NULL, " \t\r", &saveptr);
        }
        if (!bad && row >= 0 && nvals > 0 && index >= row && index < row + nvals) {
            *value = (uint8_t)vals[index - row];
            return 0;
        }
        if (!eol) break;
        p = eol + 1;
    }
    return -1;
}

/* JSON 字符串转义（值全部按 ASCII 安全输出） */
size_t emmc_json_escape(const char* in, char* out, size_t out_cap) {
    size_t o = 0;
    for (; *in && o + 6 < out_cap; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            o += (size_t)snprintf(out + o, 7, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
    return o;
}

/* ------------------------------------------------------------------ */
/* 设备侧实现（宿主单测不编译）                                        */
/* ------------------------------------------------------------------ */

/* 目标设备身份（纯数据结构，宿主单测也需要） */
typedef struct {
    char block_name[64];   /* mmcblk0 */
    char device_dir[256];  /* /sys/class/block/mmcblk0/device */
    char node[128];        /* /dev/block/mmcblk0 */
    char manfid_text[64];
    char oemid_text[64];
    char serial_text[64];
    char date_text[64];
    char cid_text[256];
    char csd_text[256];
    char name_text[128];
    char type_text[32];
    uint64_t capacity_bytes;
} emmc_target_t;

#ifndef EMHC_HOST_TEST

static void read_text_file(const char* path, char* out, size_t cap) {
    out[0] = '\0';
    FILE* f = fopen(path, "r");
    if (!f) return;
    size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = '\0';
    /* 去掉结尾换行 */
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = '\0';
}

static int read_u64_file(const char* path, uint64_t* value) {
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    unsigned long long v = 0;
    int ok = (fscanf(f, "%llu", &v) == 1);
    fclose(f);
    if (!ok) return -1;
    *value = (uint64_t)v;
    return 0;
}

/* 读取整段文本文件（debugfs 探测用） */
static int read_file_buffer(const char* path, char* buf, size_t cap, size_t* out_len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    size_t got = 0;
    while (got + 1 < cap) {
        ssize_t n = read(fd, buf + got, cap - 1 - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    buf[got] = '\0';
    if (out_len) *out_len = got;
    return 0;
}

/* 枚举 /sys/class/mmc_host，找第一块 eMMC（type == MMC），填 target。
 * 返回 0 找到；-1 未找到。--device 覆盖时直接采用。*/
static int discover_emmc(const char* override_node, emmc_target_t* t) {
    memset(t, 0, sizeof(*t));

    if (override_node && override_node[0]) {
        const char* base = strrchr(override_node, '/');
        const char* bn = base ? base + 1 : override_node;
        snprintf(t->block_name, sizeof(t->block_name), "%s", bn);
        snprintf(t->node, sizeof(t->node), "%s", override_node);
        snprintf(t->device_dir, sizeof(t->device_dir), "/sys/class/block/%s/device", t->block_name);
        char cap_path[256];
        snprintf(cap_path, sizeof(cap_path), "/sys/class/block/%s/size", t->block_name);
        uint64_t sectors = 0;
        if (read_u64_file(cap_path, &sectors) == 0) t->capacity_bytes = sectors * 512u;
        return 0;
    }

    DIR* hosts = opendir("/sys/class/mmc_host");
    if (!hosts) return -1;
    struct dirent* he;
    int found = -1;
    while ((he = readdir(hosts)) != NULL) {
        if (strncmp(he->d_name, "mmc", 3) != 0) continue;
        char host_dir[256];
        snprintf(host_dir, sizeof(host_dir), "/sys/class/mmc_host/%s", he->d_name);
        DIR* cards = opendir(host_dir);
        if (!cards) continue;
        struct dirent* ce;
        while ((ce = readdir(cards)) != NULL) {
            if (strncmp(ce->d_name, "mmc", 3) != 0 || strchr(ce->d_name, ':') == NULL) continue;
            char card_dir[512];
            snprintf(card_dir, sizeof(card_dir), "%s/%s", host_dir, ce->d_name);
            char type_path[640];
            char type[64] = "";
            snprintf(type_path, sizeof(type_path), "%s/type", card_dir);
            read_text_file(type_path, type, sizeof(type));
            if (strcmp(type, "MMC") != 0) continue; /* 跳过 SD 卡 */

            /* 通过 block/ 链接找到 mmcblkX */
            char block_dir[768];
            snprintf(block_dir, sizeof(block_dir), "%s/block", card_dir);
            DIR* blocks = opendir(block_dir);
            if (!blocks) continue;
            struct dirent* be;
            while ((be = readdir(blocks)) != NULL) {
                if (be->d_name[0] == '.') continue;
                if (strncmp(be->d_name, "mmcblk", 6) != 0) continue;
                char* p = strstr(be->d_name, "boot");
                if (p) continue; /* 跳过 boot 分区节点 */
                snprintf(t->block_name, sizeof(t->block_name), "%s", be->d_name);
                snprintf(t->node, sizeof(t->node), "/dev/block/%s", be->d_name);
                snprintf(t->device_dir, sizeof(t->device_dir), "%s", card_dir);
                snprintf(t->type_text, sizeof(t->type_text), "%s", type);
                char size_path[896];
                snprintf(size_path, sizeof(size_path), "/sys/class/block/%s/size", be->d_name);
                uint64_t sectors = 0;
                if (read_u64_file(size_path, &sectors) == 0) t->capacity_bytes = sectors * 512u;
                found = 0;
                break;
            }
            closedir(blocks);
            if (found == 0) break;
        }
        closedir(cards);
        if (found == 0) break;
    }
    closedir(hosts);
    return found;
}

/* Ext CSD 512 字节裸数据读取（debugfs 二进制文件，少数内核）。
 * 必须先 stat 确认恰好 512 字节：文本转储文件的前 512 字节会被误当成裸数据。 */
static int read_debugfs_extcsd_bin(const char* path, unsigned char out[EXT_CSD_SIZE]) {
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size != EXT_CSD_SIZE) return -1;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    size_t got = 0;
    while (got < EXT_CSD_SIZE) {
        ssize_t n = read(fd, out + got, EXT_CSD_SIZE - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    return got == EXT_CSD_SIZE ? 0 : -1;
}

/* 3.x 内核 debugfs ext_csd 连续 hex 流读取（文件薄封装） */
static int read_debugfs_extcsd_hexblob(const char* path, int index, uint8_t* value) {
    static char buf[2048];
    size_t len = 0;
    if (read_file_buffer(path, buf, sizeof(buf), &len) != 0) return -1;
    return emmc_parse_hexblob_buf(buf, len, index, value);
}

/* debugfs ext_csd 文本转储（老内核 mmc_dbg_ext_csd）：逐行 idx:value → 表格 → 连续 blob */
static int read_debugfs_extcsd_text(const char* path, int index, uint8_t* value) {
    static char buf[8192];
    size_t len = 0;
    if (read_file_buffer(path, buf, sizeof(buf), &len) != 0) return -1;
    if (len == EXT_CSD_SIZE) return -1; /* 裸数据交由 bin 分支处理 */
    char* saveptr = NULL;
    char* line = strtok_r(buf, "\n", &saveptr);
    while (line) {
        if (emmc_parse_extcsd_line(line, index, value) == 0) return 0;
        line = strtok_r(NULL, "\n", &saveptr);
    }
    /* 逐行 idx:value 未命中 → 尝试表格转储格式（strtok_r 已破坏 buf，重新读取） */
    if (read_file_buffer(path, buf, sizeof(buf), &len) != 0) return -1;
    if (emmc_parse_extcsd_table(buf, index, value) == 0) return 0;
    /* 表格仍未命中 → 尝试连续 hex blob（3.x debugfs 默认格式） */
    return read_debugfs_extcsd_hexblob(path, index, value);
}

/* debugfs health 文本（vendor 内核） */
static int read_debugfs_health(const char* path, int index, uint8_t* value) {
    static char buf[4096];
    if (read_file_buffer(path, buf, sizeof(buf), NULL) != 0) return -1;
    char* saveptr = NULL;
    char* line = strtok_r(buf, "\n", &saveptr);
    while (line) {
        if (emmc_parse_health_line(line, index, value) == 0) return 0;
        line = strtok_r(NULL, "\n", &saveptr);
    }
    return -1;
}

/* 在 /sys/kernel/debug/mmc* 下搜索可读的 ext_csd / health 文件。
 * 老内核/vendor 内核的目录布局不一（mmcN/mmcN:0001、mmcN 直挂、Reserve 目录等），
 * 按名字推导不可靠，直接穷举两层目录。 */
static void search_debugfs_files(char (*paths)[1024], int max, int* count) {
    *count = 0;
    DIR* dbg = opendir("/sys/kernel/debug");
    if (!dbg) return;
    struct dirent* he;
    while ((he = readdir(dbg)) != NULL) {
        if (strncmp(he->d_name, "mmc", 3) != 0) continue;
        char host_dir[512];
        snprintf(host_dir, sizeof(host_dir), "/sys/kernel/debug/%s", he->d_name);
        DIR* h = opendir(host_dir);
        if (!h) continue;
        struct dirent* ce;
        while ((ce = readdir(h)) != NULL) {
            if (ce->d_name[0] == '.') continue;
            char sub_dir[768];
            snprintf(sub_dir, sizeof(sub_dir), "%s/%s", host_dir, ce->d_name);
            /* 文件本身是 ext_csd/health（mmcN 直挂） */
            static const char* names[] = {"ext_csd", "health"};
            size_t k;
            for (k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
                if (strcmp(ce->d_name, names[k]) == 0 && *count < max) {
                    snprintf(paths[(*count)++], 1024, "%s", sub_dir);
                }
            }
            /* 卡子目录下的 ext_csd/health */
            DIR* s = opendir(sub_dir);
            if (!s) continue;
            struct dirent* fe;
            while ((fe = readdir(s)) != NULL) {
                size_t j;
                for (j = 0; j < sizeof(names) / sizeof(names[0]); j++) {
                    if (strcmp(fe->d_name, names[j]) == 0 && *count < max) {
                        snprintf(paths[(*count)++], 1024, "%s/%s", sub_dir, fe->d_name);
                    }
                }
            }
            closedir(s);
        }
        closedir(h);
    }
    closedir(dbg);
}

/* 主路径：MMC_IOC_CMD 发 CMD8 读 Ext CSD。个别内核要求 RDWR 权限，EINVAL 时换 O_RDWR 重试 */
static int read_extcsd_ioctl(const char* node, unsigned char out[EXT_CSD_SIZE], int* os_error) {
    int attempt;
    for (attempt = 0; attempt < 2; attempt++) {
        int fd = open(node, attempt == 0 ? O_RDONLY : O_RDWR);
        if (fd < 0) {
            *os_error = errno;
            continue;
        }
        struct emmc_ioc_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.opcode = MMC_SEND_EXT_CSD;
        cmd.arg = 0;
        cmd.flags = EMHC_RSP_R1 | EMHC_RSP_SPI_R1 | EMHC_CMD_ADTC;
        cmd.blksz = EXT_CSD_SIZE;
        cmd.blocks = 1;
        cmd.data_ptr = (uint64_t)(uintptr_t)out;
        int rc = ioctl(fd, EMHC_IOC_CMD, &cmd);
        int saved = errno;
        close(fd);
        if (rc == 0) return 0;
        *os_error = saved;
    }
    return -1;
}

static void fill_identity(emmc_target_t* t) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/manfid", t->device_dir);
    read_text_file(path, t->manfid_text, sizeof(t->manfid_text));
    snprintf(path, sizeof(path), "%s/oemid", t->device_dir);
    read_text_file(path, t->oemid_text, sizeof(t->oemid_text));
    snprintf(path, sizeof(path), "%s/serial", t->device_dir);
    read_text_file(path, t->serial_text, sizeof(t->serial_text));
    snprintf(path, sizeof(path), "%s/date", t->device_dir);
    read_text_file(path, t->date_text, sizeof(t->date_text));
    snprintf(path, sizeof(path), "%s/cid", t->device_dir);
    read_text_file(path, t->cid_text, sizeof(t->cid_text));
    snprintf(path, sizeof(path), "%s/csd", t->device_dir);
    read_text_file(path, t->csd_text, sizeof(t->csd_text));
    snprintf(path, sizeof(path), "%s/name", t->device_dir);
    read_text_file(path, t->name_text, sizeof(t->name_text));
}

#endif /* !EMHC_HOST_TEST */

/* ------------------------------------------------------------------ */
/* 输出                                                                */
/* ------------------------------------------------------------------ */

static const char* zh_verdict(emmc_verdict_t v) {
    switch (v) {
        case VERDICT_OK: return "正常";
        case VERDICT_WARN: return "警告";
        case VERDICT_URGENT: return "紧急";
        default: return "未知";
    }
}

static const char* zh_pre_eol(uint8_t v) {
    if (v == 0x01) return "正常";
    if (v == 0x02) return "警告(预留块已耗80%-90%)";
    if (v == 0x03) return "紧急(预留块耗尽>90%)";
    return "未知";
}

static const char* zh_life(uint8_t v) {
    if (v >= 0x01 && v <= 0x0A) {
        static char buf[4][32];
        static int idx = 0;
        char* b = buf[idx & 3];
        idx++;
        snprintf(b, 32, "%d%%-%d%%", (v - 1) * 10, v * 10);
        return b;
    }
    if (v == 0x0B) return "超过设计寿命";
    return "未定义";
}

typedef struct {
    emmc_target_t t;
    unsigned char extcsd[EXT_CSD_SIZE];
    int have_extcsd;
    int have_rev;
    emmc_source_t source;
    int pre_eol, life_a, life_b; /* -1 = 未读到；0xFF 芯片值原样保留 */
    int ext_csd_rev;
    char error[256];
    char os_error_text[128];
} emmc_result_t;

static void run_scan(const char* override_node, emmc_result_t* r) {
    memset(r, 0, sizeof(*r));
    r->source = SRC_NONE;

#ifndef EMHC_HOST_TEST
    emmc_target_t* t = &r->t;
    r->pre_eol = -1;
    r->life_a = -1;
    r->life_b = -1;
    r->ext_csd_rev = -1;
    if (discover_emmc(override_node, t) != 0) {
        snprintf(r->error, sizeof(r->error), "no emmc found");
        return;
    }
    fill_identity(t);

    /* 1) ioctl 主路径 */
    int os_err = 0;
    if (geteuid() == 0) {
        if (read_extcsd_ioctl(t->node, r->extcsd, &os_err) == 0) {
            r->have_extcsd = 1;
            r->source = SRC_IOCTL;
        } else {
            snprintf(r->os_error_text, sizeof(r->os_error_text), "ioctl errno=%d", os_err);
        }
    } else {
        snprintf(r->os_error_text, sizeof(r->os_error_text), "not root (geteuid=%d)", geteuid());
    }

    /* 2/3) debugfs 兜底：搜索 /sys/kernel/debug/mmc* 下的 ext_csd / health */
    if (!r->have_extcsd) {
        char paths[8][1024];
        int count = 0;
        search_debugfs_files(paths, 8, &count);
        int idx;
        for (idx = 0; idx < count && r->have_extcsd != 1; idx++) {
            unsigned char bin[EXT_CSD_SIZE];
            if (read_debugfs_extcsd_bin(paths[idx], bin) == 0) {
                memcpy(r->extcsd, bin, EXT_CSD_SIZE);
                r->have_extcsd = 1;
                r->source = SRC_DEBUGFS_EXT_CSD_BIN;
                break;
            }
            uint8_t v = 0;
            int got = 0;
            if (read_debugfs_extcsd_text(paths[idx], EXT_CSD_IDX_PRE_EOL, &v) == 0) {
                r->pre_eol = v;
                got = 1;
            }
            v = 0;
            if (read_debugfs_extcsd_text(paths[idx], EXT_CSD_IDX_LIFE_A, &v) == 0) {
                r->life_a = v;
                got = 1;
            }
            v = 0;
            if (read_debugfs_extcsd_text(paths[idx], EXT_CSD_IDX_LIFE_B, &v) == 0) {
                r->life_b = v;
                got = 1;
            }
            if (got) {
                /* 版本号顺带读取（部分字段模式下也有值） */
                uint8_t rev = 0;
                if (read_debugfs_extcsd_text(paths[idx], EXT_CSD_IDX_REV, &rev) == 0) {
                    r->ext_csd_rev = rev;
                    r->have_rev = 1;
                }
                r->source = SRC_DEBUGFS_EXT_CSD_TEXT;
                r->have_extcsd = 2; /* 部分字段 */
                break;
            }
            v = 0;
            if (read_debugfs_health(paths[idx], EXT_CSD_IDX_PRE_EOL, &v) == 0) {
                r->pre_eol = v;
                v = 0;
                if (read_debugfs_health(paths[idx], EXT_CSD_IDX_LIFE_A, &v) == 0) r->life_a = v;
                else r->life_a = -1;
                v = 0;
                if (read_debugfs_health(paths[idx], EXT_CSD_IDX_LIFE_B, &v) == 0) r->life_b = v;
                else r->life_b = -1;
                r->source = SRC_DEBUGFS_HEALTH;
                r->have_extcsd = 2;
                break;
            }
        }
    }

    if (r->have_extcsd == 1) {
        r->ext_csd_rev = r->extcsd[EXT_CSD_IDX_REV];
        r->pre_eol = r->extcsd[EXT_CSD_IDX_PRE_EOL];
        r->life_a = r->extcsd[EXT_CSD_IDX_LIFE_A];
        r->life_b = r->extcsd[EXT_CSD_IDX_LIFE_B];
    }
#endif
}

static void emit_json(const emmc_result_t* r) {
    char esc[512];
    uint8_t mid = (uint8_t)strtoul(r->t.manfid_text, NULL, 16);
    const char* vendor = emmc_vendor_name(mid);
    int has_any = (r->pre_eol >= 0 || r->life_a >= 0 || r->life_b >= 0);
    emmc_verdict_t verdict = emmc_verdict(
        (uint8_t)(r->pre_eol < 0 ? 0 : r->pre_eol),
        (uint8_t)(r->life_a < 0 ? 0 : r->life_a),
        (uint8_t)(r->life_b < 0 ? 0 : r->life_b),
        has_any);

    printf("{");
    printf("\"device\":\"%s\",", r->t.block_name);
    printf("\"node\":\"%s\",", r->t.node);
    printf("\"type\":\"%s\",", r->t.type_text);
    printf("\"manfid\":\"0x%02x\",", mid);
    printf("\"manufacturer\":\"%s\",", vendor ? vendor : "unknown");
    emmc_json_escape(r->t.name_text, esc, sizeof(esc));
    printf("\"name\":\"%s\",", esc);
    emmc_json_escape(r->t.oemid_text, esc, sizeof(esc));
    printf("\"oemid\":\"%s\",", esc);
    emmc_json_escape(r->t.serial_text, esc, sizeof(esc));
    printf("\"serial\":\"%s\",", esc);
    emmc_json_escape(r->t.date_text, esc, sizeof(esc));
    printf("\"date\":\"%s\",", esc);
    emmc_json_escape(r->t.cid_text, esc, sizeof(esc));
    printf("\"cid\":\"%s\",", esc);
    emmc_json_escape(r->t.csd_text, esc, sizeof(esc));
    printf("\"csd\":\"%s\",", esc);
    printf("\"capacity_bytes\":%llu,", (unsigned long long)r->t.capacity_bytes);
    printf("\"ext_csd_rev\":%d,", (r->have_extcsd == 1 || r->have_rev) ? (int)r->ext_csd_rev : -1);
    printf("\"mmc_spec\":\"%s\",",
           (r->have_extcsd == 1 || r->have_rev) ? emmc_spec_from_rev(r->ext_csd_rev) : "unknown");
    printf("\"pre_eol\":%d,", (int)r->pre_eol);
    printf("\"life_a\":%d,", (int)r->life_a);
    printf("\"life_b\":%d,", (int)r->life_b);
    printf("\"source\":\"%s\",", source_name(r->source));
    printf("\"verdict\":\"%s\",", emmc_verdict_text(verdict));
    emmc_json_escape(r->error, esc, sizeof(esc));
    printf("\"error\":\"%s\",", esc);
    emmc_json_escape(r->os_error_text, esc, sizeof(esc));
    printf("\"os_error\":\"%s\"", esc);
    printf("}\n");
}

static void emit_text(const emmc_result_t* r) {
    uint8_t mid = (uint8_t)strtoul(r->t.manfid_text, NULL, 16);
    const char* vendor = emmc_vendor_name(mid);
    int has_any = (r->pre_eol >= 0 || r->life_a >= 0 || r->life_b >= 0);
    emmc_verdict_t verdict = emmc_verdict(
        (uint8_t)(r->pre_eol < 0 ? 0 : r->pre_eol),
        (uint8_t)(r->life_a < 0 ? 0 : r->life_a),
        (uint8_t)(r->life_b < 0 ? 0 : r->life_b),
        has_any);
    double gib = (double)r->t.capacity_bytes / (1024.0 * 1024.0 * 1024.0);

    printf("========== eMMC 健康检测 ==========\n");
    printf("设备: %s (%s)\n", r->t.block_name, r->t.node);
    printf("类型: %s\n", r->t.type_text[0] ? r->t.type_text : "eMMC");
    printf("型号: %s\n", r->t.name_text[0] ? r->t.name_text : "未知");
    printf("厂商: %s (MANFID 0x%02x)\n", vendor ? vendor : "未知", mid);
    printf("OEMID: %s  序列号: %s  生产日期: %s\n", r->t.oemid_text, r->t.serial_text,
           r->t.date_text);
    printf("容量: %.1f GiB (%llu 字节)\n", gib, (unsigned long long)r->t.capacity_bytes);
    printf("CID: %s\n", r->t.cid_text);
    printf("\n[健康度]\n");
    if (r->have_extcsd) {
        if (r->have_rev || r->have_extcsd == 1) {
            printf("Ext CSD 版本: %d (MMC %s)\n", r->ext_csd_rev,
                   emmc_spec_from_rev(r->ext_csd_rev));
        }
        if (r->pre_eol >= 0) {
            printf("预留块状态(pre_eol_info): 0x%02x（%s）\n", r->pre_eol, zh_pre_eol(r->pre_eol));
        } else {
            printf("预留块状态(pre_eol_info): 无数据\n");
        }
        if (r->life_a >= 0) {
            printf("寿命估计A(区域0): 0x%02x（%s）\n", r->life_a, zh_life(r->life_a));
        } else {
            printf("寿命估计A(区域0): 无数据\n");
        }
        if (r->life_b >= 0) {
            printf("寿命估计B(区域1): 0x%02x（%s）\n", r->life_b, zh_life(r->life_b));
        } else {
            printf("寿命估计B(区域1): 无数据\n");
        }
        printf("综合结论: %s\n", zh_verdict(verdict));
        printf("数据来源: %s\n", source_name(r->source));
    } else {
        printf("Ext CSD 读取失败: %s %s\n", r->error, r->os_error_text);
        printf("说明: 健康字段需要 root 权限（su 0）读取，身份信息已展示。\n");
        printf("综合结论: 未知\n");
    }
}

/* ------------------------------------------------------------------ */

#ifdef EMHC_HOST_TEST
/* 宿主单测入口：仅纯逻辑断言 */
#include "emmc_health_test.h"
int main(void) { return emmc_host_test_main(); }
#else

int main(int argc, char** argv) {
    const char* override_node = NULL;
    int json = 0;
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) {
            json = 1;
        } else if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            override_node = argv[++i];
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("emmc_health %s\n", EMHC_VERSION);
            return 0;
        } else if (strcmp(argv[i], "help") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("emmc_health %s — eMMC 健康度检测\n", EMHC_VERSION);
            printf("用法: emmc_health [--json] [--device /dev/block/mmcblkN]\n");
            printf("  --json     输出机读 JSON（字段值全部 ASCII）\n");
            printf("  --device   指定块设备节点（默认自动枚举 /sys/class/mmc_host 中 type=MMC 的 eMMC）\n");
            printf("健康字段（pre_eol_info/life_time）需要 root：su 0 emmc_health\n");
            return 0;
        }
    }
    emmc_result_t r;
    run_scan(override_node, &r);
    if (json) {
        emit_json(&r);
    } else {
        emit_text(&r);
    }
    return r.t.block_name[0] ? 0 : 1;
}
#endif
