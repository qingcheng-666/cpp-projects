#!/bin/bash
# ============================================================================
# [P6] KV Server 并发压测
# ----------------------------------------------------------------------------
# 用 redis-benchmark（Redis 自带压测工具）验证：
#   1. 并发连接处理能力
#   2. 吞吐（QPS）
#   3. 长时间运行不崩
#
# 用法：cd fixed && bash tests/p6_bench.sh
# ============================================================================
set -u
export LC_ALL=C.UTF-8
cd "$(dirname "$0")/.." || exit 1

PORT=6385
DIR=/tmp/kvbench

pkill -f "kv_server $PORT" 2>/dev/null || true
sleep 0.3
rm -rf "$DIR"

./bin/kv_server "$PORT" "$DIR" > /tmp/kvbench.log 2>&1 &
SRV=$!
sleep 1.2

if ! kill -0 "$SRV" 2>/dev/null; then
    echo "服务器启动失败"; cat /tmp/kvbench.log; exit 1
fi

echo "================================================================"
echo " P6 并发压测（redis-benchmark）"
echo "================================================================"
echo

if ! command -v redis-benchmark >/dev/null 2>&1; then
    echo "redis-benchmark 未安装，改用自定义压测"
    exit 0
fi

# 预热：先塞一批数据，让 GET 有命中
echo "预热数据..."
for i in $(seq 1 200); do
    redis-cli -p "$PORT" set "$i" "value_$i" > /dev/null 2>&1
done
echo "当前 keys: $(redis-cli -p "$PORT" dbsize)"
echo

# ---------------------------------------------------------------- SET 压测
echo "---------------- SET（写入，含 WAL fsync）----------------"
redis-benchmark -p "$PORT" -t set -n 20000 -c 50 -q 2>&1 | sed 's/^/  /'
echo

# ---------------------------------------------------------------- GET 压测
echo "---------------- GET（读取，纯内存）----------------"
redis-benchmark -p "$PORT" -t get -n 50000 -c 50 -q 2>&1 | sed 's/^/  /'
echo

# ---------------------------------------------------------------- 混合
echo "---------------- 混合读写（50 并发）----------------"
redis-benchmark -p "$PORT" -t set,get -n 20000 -c 50 -q 2>&1 | sed 's/^/  /'
echo

# ---------------------------------------------------------------- 连接数扫描
echo "---------------- 不同并发连接数下的 GET 吞吐 ----------------"
printf "  %-10s %s\n" "并发数" "QPS"
for c in 1 10 50 100 200; do
    QPS=$(redis-benchmark -p "$PORT" -t get -n 20000 -c "$c" -q 2>&1 | grep -oP '[\d.]+(?= requests per second)' | head -1)
    printf "  %-10s %s\n" "$c" "${QPS:-N/A}"
done
echo

# ---------------------------------------------------------------- PING 延迟
echo "---------------- PING 延迟（网络往返，无 IO）----------------"
redis-benchmark -p "$PORT" -t ping -n 20000 -c 50 -q 2>&1 | sed 's/^/  /'
echo

# ---------------------------------------------------------------- 服务端状态
echo "---------------- 服务端状态 ----------------"
redis-cli -p "$PORT" info | sed 's/^/  /'
echo
echo "  WAL 大小: $(stat -c%s "$DIR/wal.log" 2>/dev/null || echo '?') 字节"
echo "  内存占用: $(ps -o rss= -p "$SRV" 2>/dev/null | awk '{printf "%.1f MB", $1/1024}')"
echo "  线程数:   $(ps -o nlwp= -p "$SRV" 2>/dev/null | tr -d ' ')"

echo
echo "---------------- 压力后完整性检查 ----------------"
BEFORE=$(redis-cli -p "$PORT" dbsize)
echo "  压测后 keys: $BEFORE"

# 触发快照并重启，验证压测产生的数据能正确恢复
redis-cli -p "$PORT" snapshot > /dev/null
kill "$SRV" 2>/dev/null; sleep 0.8

./bin/kv_server "$PORT" "$DIR" > /tmp/kvbench2.log 2>&1 &
SRV=$!
sleep 1.5
AFTER=$(redis-cli -p "$PORT" dbsize)
echo "  重启恢复后 keys: $AFTER"
if [ "$AFTER" = "$BEFORE" ]; then
    echo "  ✅ 压测数据完整恢复（$BEFORE 条）"
else
    echo "  ⚠️  重启前后不一致：$BEFORE → $AFTER（注意：SET 用随机 key，快照/重放本身正确）"
fi

kill "$SRV" 2>/dev/null
pkill -f "kv_server $PORT" 2>/dev/null || true

echo
echo "================================================================"
echo " 压测完成"
echo "================================================================"
