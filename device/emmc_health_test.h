/* emmc_health 宿主单测（-DEMHC_HOST_TEST 时替代 main）
 * 仅覆盖纯逻辑：厂商表、等级映射、结论、Ext CSD 文本解析、JSON 转义。
 * 设备 IO（sysfs/ioctl/debugfs）不在宿主编译范围内。 */
#include <stdio.h>
#include <string.h>

static int g_fail = 0;

#define CHECK(cond, msg)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            printf("FAIL: %s\n", msg);                    \
            g_fail++;                                     \
        } else {                                          \
            printf("ok:   %s\n", msg);                    \
        }                                                 \
    } while (0)

static void test_vendors(void) {
    CHECK(strcmp(emmc_vendor_name(0x11), "Toshiba/Kioxia") == 0, "manfid 0x11 -> Toshiba/Kioxia");
    CHECK(strcmp(emmc_vendor_name(0x15), "Samsung") == 0, "manfid 0x15 -> Samsung");
    CHECK(strcmp(emmc_vendor_name(0x13), "Micron") == 0, "manfid 0x13 -> Micron");
    CHECK(strcmp(emmc_vendor_name(0x88), "FORESEE/Longsys") == 0, "manfid 0x88 -> FORESEE/Longsys");
    CHECK(emmc_vendor_name(0x00) == NULL, "manfid 0x00 -> NULL(unknown)");
}

static void test_levels(void) {
    CHECK(strcmp(emmc_pre_eol_text(0x01), "normal") == 0, "pre_eol 0x01 normal");
    CHECK(strcmp(emmc_pre_eol_text(0x02), "warning") == 0, "pre_eol 0x02 warning");
    CHECK(strcmp(emmc_pre_eol_text(0x03), "urgent") == 0, "pre_eol 0x03 urgent");
    CHECK(strcmp(emmc_pre_eol_text(0x00), "unknown") == 0, "pre_eol 0x00 unknown");
    CHECK(strcmp(emmc_life_text(0x01), "0-10%") == 0, "life 0x01 -> 0-10%");
    CHECK(strcmp(emmc_life_text(0x05), "40-50%") == 0, "life 0x05 -> 40-50%");
    CHECK(strcmp(emmc_life_text(0x0A), "90-100%") == 0, "life 0x0A -> 90-100%");
    CHECK(strcmp(emmc_life_text(0x0B), "over-life") == 0, "life 0x0B -> over-life");
    CHECK(strcmp(emmc_life_text(0x00), "unknown") == 0, "life 0x00 -> unknown");
    CHECK(strcmp(emmc_spec_from_rev(7), "5.1") == 0, "ext_csd rev 7 -> 5.1");
    CHECK(strcmp(emmc_spec_from_rev(6), "5.0") == 0, "ext_csd rev 6 -> 5.0");
    CHECK(strcmp(emmc_spec_from_rev(8), "unknown") == 0, "ext_csd rev 8 -> unknown");
}

static void test_verdict(void) {
    CHECK(emmc_verdict(0x01, 0x01, 0x01, 1) == VERDICT_OK, "all fresh -> ok");
    CHECK(emmc_verdict(0x02, 0x01, 0x01, 1) == VERDICT_WARN, "pre_eol 02 -> warn");
    CHECK(emmc_verdict(0x03, 0x01, 0x01, 1) == VERDICT_URGENT, "pre_eol 03 -> urgent");
    CHECK(emmc_verdict(0x01, 0x0B, 0x01, 1) == VERDICT_URGENT, "life A over-life -> urgent");
    CHECK(emmc_verdict(0x01, 0x09, 0x01, 1) == VERDICT_WARN, "life A >=80% -> warn");
    CHECK(emmc_verdict(0x02, 0x0B, 0x01, 1) == VERDICT_URGENT, "pre_eol 02 + over-life -> urgent");
    CHECK(emmc_verdict(0x01, 0x01, 0x01, 0) == VERDICT_UNKNOWN, "no data -> unknown");
    CHECK(emmc_verdict(0x00, 0x00, 0x00, 1) == VERDICT_OK, "reserved-zero fields -> ok (不误报)");
    CHECK(emmc_verdict(0x00, 0x30, 0x30, 1) == VERDICT_OK, "life 脏值 0x30(文本误读) 不参与判定");
}

static void test_extcsd_line_parse(void) {
    uint8_t v = 0;
    CHECK(emmc_parse_extcsd_line("268: 0x01", EXT_CSD_IDX_PRE_EOL, &v) == 0 && v == 0x01,
          "parse '268: 0x01'");
    CHECK(emmc_parse_extcsd_line("268: 0x01", 269, &v) != 0, "wrong index -> no match");
    CHECK(emmc_parse_extcsd_line("268 01", EXT_CSD_IDX_PRE_EOL, &v) == 0 && v == 0x01,
          "parse '268 01' (dec idx, hex-ish val)");
    CHECK(emmc_parse_extcsd_line("268 08", EXT_CSD_IDX_PRE_EOL, &v) == 0 && v == 0x08,
          "parse '268 08' (十进制值，旧八进制逻辑会错)");
    CHECK(emmc_parse_extcsd_line("010: 02", 10, &v) == 0 && v == 0x02,
          "parse '010: 02' (idx 十进制 10，旧八进制逻辑会错)");
    CHECK(emmc_parse_extcsd_line("0x10c: 0x02", EXT_CSD_IDX_PRE_EOL, &v) == 0 && v == 0x02,
          "parse '0x10c: 0x02' (hex idx)");
    CHECK(emmc_parse_extcsd_line("0x10d\t0x09", EXT_CSD_IDX_LIFE_A, &v) == 0 && v == 0x09,
          "parse '0x10d\\t0x09' (tab sep)");
    CHECK(emmc_parse_extcsd_line("# comment", 268, &v) != 0, "comment line skipped");
    CHECK(emmc_parse_extcsd_line("", 268, &v) != 0, "empty line skipped");
}

static void test_health_line_parse(void) {
    uint8_t v = 0;
    CHECK(emmc_parse_health_line("pre_eol_info: 0x01", EXT_CSD_IDX_PRE_EOL, &v) == 0 && v == 0x01,
          "health 'pre_eol_info: 0x01'");
    CHECK(emmc_parse_health_line("life_time_estimation_a 0x03", EXT_CSD_IDX_LIFE_A, &v) == 0 &&
              v == 0x03,
          "health 'life_time_estimation_a 0x03'");
    CHECK(emmc_parse_health_line("life_time_estimation_b=0x0b", EXT_CSD_IDX_LIFE_B, &v) == 0 &&
              v == 0x0B,
          "health 'life_time_estimation_b=0x0b'");
    CHECK(emmc_parse_health_line("unrelated text", EXT_CSD_IDX_PRE_EOL, &v) != 0,
          "health unrelated line no match");
}

static void test_extcsd_table_parse(void) {
    uint8_t v = 0;
    /* 16 字节/行表格：行首 0x100 覆盖 0x100..0x10f，下一行 0x110 覆盖 0x110..0x11f */
    const char* table =
        "100 00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f\n"
        "110 10 11 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f\n";
    CHECK(emmc_parse_extcsd_table(table, 100, &v) == -1, "table: 越界索引(0x64) -> no match");
    CHECK(emmc_parse_extcsd_table(table, 268, &v) == 0 && v == 0x0c,
          "table: 命中 268(0x10c)=0x0c (pre_eol)");
    CHECK(emmc_parse_extcsd_table(table, 269, &v) == 0 && v == 0x0d,
          "table: 269(0x10d)=0x0d (life_a)");
    CHECK(emmc_parse_extcsd_table(table, 287, &v) == 0 && v == 0x1f,
          "table: 287(0x11f)=0x1f (第二行末字节)");
    /* 0x 前缀 + tab 分隔变体 */
    const char* table2 =
        "0x100\t0\t1\t2\t3\t4\t5\t6\t7\t8\t9\ta\tb\tc\td\te\tf\r\n"
        "0x110\t10\t11\t12\t13\t14\t15\t16\t17\t18\t19\t1a\t1b\t1c\t1d\t1e\t1f\r\n";
    CHECK(emmc_parse_extcsd_table(table2, 269, &v) == 0 && v == 0x0d,
          "table2: 0x 前缀+tab+CRLF, 269=0x0d");
    /* 非 hex 噪声行跳过 */
    const char* noisy = "EXT_CSD dump:\n100 00 01 02\n";
    CHECK(emmc_parse_extcsd_table(noisy, 268, &v) == -1, "table: 噪声行跳过，无命中");
}

static void test_extcsd_blob_parse(void) {
    uint8_t v = 0;
    /* 3.x debugfs 连续 hex blob：字节268=0x0a, 269=0x03, 270=0x02 */
    char blob[2048];
    size_t o = 0;
    int i;
    for (i = 0; i < 512; i++) {
        if (i == 268) { memcpy(blob + o, "0a", 2); o += 2; }
        else if (i == 269) { memcpy(blob + o, "03", 2); o += 2; }
        else if (i == 270) { memcpy(blob + o, "02", 2); o += 2; }
        else { memcpy(blob + o, "00", 2); o += 2; }
    }
    blob[o++] = '\n';
    blob[o] = '\0';
    CHECK(emmc_parse_hexblob_buf(blob, o, 268, &v) == 0 && v == 0x0a,
          "hexblob: 字节268=0x0a (pre_eol)");
    CHECK(emmc_parse_hexblob_buf(blob, o, 269, &v) == 0 && v == 0x03,
          "hexblob: 字节269=0x03 (life_a)");
    CHECK(emmc_parse_hexblob_buf(blob, o, 270, &v) == 0 && v == 0x02,
          "hexblob: 字节270=0x02 (life_b)");
    CHECK(emmc_parse_hexblob_buf(blob, o, 192, &v) == 0 && v == 0x00,
          "hexblob: 字节192=0x00");
    CHECK(emmc_parse_hexblob_buf(blob, o, 512, &v) == -1, "hexblob: 越界索引 -> no match");
    CHECK(emmc_parse_extcsd_table(blob, 268, &v) == -1, "blob 不是表格格式");
    /* 行式文件不是 blob */
    const char* linefmt = "268: 0x01\n";
    CHECK(emmc_parse_hexblob_buf(linefmt, strlen(linefmt), 268, &v) == -1,
          "hexblob: 行式(含冒号/空格) 判定非 blob");
}

static void test_json_escape(void) {
    char out[64];
    emmc_json_escape("plain", out, sizeof(out));
    CHECK(strcmp(out, "plain") == 0, "json escape plain");
    emmc_json_escape("a\"b\\c", out, sizeof(out));
    CHECK(strcmp(out, "a\\\"b\\\\c") == 0, "json escape quote/backslash");
    emmc_json_escape("x\x01y", out, sizeof(out));
    CHECK(strcmp(out, "x\\u0001y") == 0, "json escape control char");
    emmc_json_escape("truncate-test-string-long", out, 8);
    CHECK(strlen(out) < 8, "json escape respects capacity");
}

int emmc_host_test_main(void) {
    printf("== emmc_health host tests ==\n");
    test_vendors();
    test_levels();
    test_verdict();
    test_extcsd_line_parse();
    test_extcsd_table_parse();
    test_health_line_parse();
    test_json_escape();
    printf("== %s (%d failures) ==\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
