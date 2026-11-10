#!/bin/sh
# 宿主端到端测试：假 adb + 回环 TCP 监听器，跑通 扫描→连接→解析→报告 全链路。
# 用法: sh test/run-e2e.sh [exe路径]  (默认 build/eMMC健康扫描.exe)
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EXE="${1:-$ROOT/build/eMMC健康扫描.exe}"
PORT=15555   # 避开真实 adb 端口，防止误连
REPORT_DIR="$ROOT/build/e2e-reports"
rm -rf "$REPORT_DIR"

# 1) 回环监听器（模拟设备 5555 端口开放）
python "$ROOT/test/fake-adb/listener.py" "$PORT" >/dev/null 2>&1 &
LISTENER_PID=$!
trap 'kill $LISTENER_PID 2>/dev/null || true' EXIT

# 等监听器就绪
for i in 1 2 3 4 5 6 7 8 9 10; do
    if python -c "import socket,sys; s=socket.socket(); s.settimeout(0.5); sys.exit(0 if s.connect_ex(('127.0.0.1',$PORT))==0 else 1)" 2>/dev/null; then
        break
    fi
    sleep 0.3
done

# 2) 跑被测 exe：网段扫描模式（127.0.0.1/32 展开→探测→假 adb 全链路）
OUT_FILE="$ROOT/build/e2e-out.txt"
MSYS_NO_PATHCONV=1 cmd.exe /c "$(cygpath -w "$EXE")" \
    --subnet 127.0.0.1/32 --port "$PORT" \
    --adb "$(cygpath -w "$ROOT/test/fake-adb/adb.bat")" \
    --out "$(cygpath -w "$REPORT_DIR")" --no-color > "$OUT_FILE" 2>&1 || true

cat "$OUT_FILE"

# 3) 断言
PASS=0
FAIL=0
check() {  # check <描述> <grep模式>
    if grep -q -- "$2" "$OUT_FILE"; then
        echo "ok:   $1"
        PASS=$((PASS+1))
    else
        echo "FAIL: $1 (未找到: $2)"
        FAIL=$((FAIL+1))
    fi
}

check "扫描到目标设备"        "127.0.0.1:$PORT"
check "设备扫描成功"          "扫描成功"
check "识别 eMMC 型号"        "8GTF4R"
check "识别厂商 FORESEE"      "FORESEE/Longsys"
check "结论 正常"             "结论=正常"
check "设备型号"              "FakeBox-Test"
check "root 通道 su 0"        "su 0"
[ -d "$REPORT_DIR" ] && { echo "ok:   报告目录已生成"; PASS=$((PASS+1)); } || { echo "FAIL: 报告目录未生成"; FAIL=$((FAIL+1)); }
CSV=$(find "$REPORT_DIR" -name "eMMC健康汇总_*.csv" 2>/dev/null | head -1)
if [ -n "$CSV" ] && grep -q "FORESEE/Longsys" "$CSV"; then
    echo "ok:   汇总 CSV 内容正确"; PASS=$((PASS+1))
else
    echo "FAIL: 汇总 CSV 缺失或内容错误"; FAIL=$((FAIL+1))
fi
TXT=$(find "$REPORT_DIR" -name "emmc_127_0_0_1_*.txt" 2>/dev/null | head -1)
if [ -n "$TXT" ] && grep -q "综合结论: 正常" "$TXT"; then
    echo "ok:   明细 TXT 内容正确"; PASS=$((PASS+1))
else
    echo "FAIL: 明细 TXT 缺失或内容错误"; FAIL=$((FAIL+1))
fi

echo "== E2E: $PASS 通过, $FAIL 失败 =="
[ "$FAIL" -eq 0 ]
