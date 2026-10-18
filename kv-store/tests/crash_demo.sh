#!/bin/bash
# ============================================================================
# [P4] kill -9 崩溃恢复演示
# ----------------------------------------------------------------------------
# 流程：
#   1. 启动写入进程，持续写入并 ACK
#   2. 随机时刻 kill -9（不可捕获、不可清理 —— 最粗暴的崩溃）
#   3. 启动恢复进程，验证「所有 ACK 过的写入」一条不少
#   4. 重复多轮，每轮崩溃后重启继续写（模拟真实服务反复重启）
#
# 用法：cd fixed && bash tests/crash_demo.sh
# ============================================================================
set -u
export LC_ALL=C.UTF-8
cd "$(dirname "$0")/.." || exit 1

BIN=./bin
DIR=/tmp/p4crash
ROUNDS=${1:-3}

mkdir -p "$BIN"

echo "================================================================"
echo " P4 崩溃恢复演示：kill -9 后数据是否丢失"
echo "================================================================"
echo
echo "编译..."
g++ tests/crash_writer.cpp  -o "$BIN/crash_writer"  -std=c++17 -pthread -O2 2>&1 | head -5
g++ tests/crash_recover.cpp -o "$BIN/crash_recover" -std=c++17 -pthread -O2 2>&1 | head -5
echo "编译完成"
echo

rm -rf "$DIR"; mkdir -p "$DIR"

LAST_ACK=-1

for round in $(seq 1 "$ROUNDS"); do
    echo "---------------- 第 $round 轮 ----------------"

    # 启动写入进程，stderr 重定向到文件（记录所有 ACK）
    "$BIN/crash_writer" "$DIR" 200000 > /dev/null 2> "$DIR/round$round.log" &
    WPID=$!

    # 随机等 0.3~1.2 秒，然后 kill -9
    SLEEP=$(awk -v s="$round" 'BEGIN{srand(s*7+3); printf "%.2f", 0.3+rand()*0.9}')
    sleep "$SLEEP"

    if kill -0 "$WPID" 2>/dev/null; then
        kill -9 "$WPID" 2>/dev/null
        echo "  在 ${SLEEP}s 处 kill -9 (pid=$WPID)"
    else
        echo "  进程已自行结束（写入太快）"
    fi
    wait "$WPID" 2>/dev/null

    # 统计 ACK 到哪一条
    ACKED=$(grep -c '^ACK ' "$DIR/round$round.log" 2>/dev/null || echo 0)
    LAST_ACK=$(grep '^ACK ' "$DIR/round$round.log" 2>/dev/null | tail -1 | awk '{print $2}')
    LAST_ACK=${LAST_ACK:--1}
    echo "  本轮 ACK 了 $ACKED 条，最后一条 key=$LAST_ACK"

    # WAL 文件状态
    if [ -f "$DIR/wal.log" ]; then
        echo "  WAL 文件大小: $(stat -c%s "$DIR/wal.log") 字节"
    fi

    # 恢复并校验
    echo "  开始恢复..."
    "$BIN/crash_recover" "$DIR" "$LAST_ACK" 2>&1 | sed 's/^/  /'
    RC=${PIPESTATUS[0]}

    if [ "$RC" -eq 0 ]; then
        echo "  ✅ 第 $round 轮通过：崩溃前确认的写入全部恢复"
    else
        echo "  ❌ 第 $round 轮失败"
        exit 1
    fi
    echo
done

echo "================================================================"
echo " 结论"
echo "================================================================"
echo "  经过 $ROUNDS 轮 kill -9，每一轮「已 ACK 的写入」都完整恢复。"
echo
echo "  这证明了 WAL 的核心保证："
echo "    先写日志（含 CRC）并落盘 → 再改内存 → 才向调用方确认"
echo "  所以任何被确认过的写入，即使进程被强杀，重启后都能重建。"
echo
echo "  对比原版：只有 dump_file() 全量覆盖，kill -9 后"
echo "  上次 dump 之后的所有写入全部丢失。"
echo "================================================================"
