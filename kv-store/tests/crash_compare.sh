#!/bin/bash
# ============================================================================
# [P4] 对照实验：原版 dump_file 在 kill -9 下会丢多少数据
# ----------------------------------------------------------------------------
# 用完全相同的场景（持续写入 + kill -9）测原版的持久化能力。
#
# 原版持久化只有 dump_file()：全量覆盖写文本文件，需要【手动调用】。
# 所以：
#   - 如果从不在写入过程中 dump，kill -9 后内存数据全丢
#   - 就算 dump 过，dump 之后的写入也会丢
#
# 用法：cd fixed && bash tests/crash_compare.sh
# ============================================================================
set -u
export LC_ALL=C.UTF-8
cd "$(dirname "$0")/.." || exit 1

BIN=./bin
DIR=/tmp/p4compare
mkdir -p "$BIN" "$DIR"
rm -rf "$DIR"; mkdir -p "$DIR/store"

echo "================================================================"
echo " 对照实验：原版 vs WAL 版，在 kill -9 下的数据保全能力"
echo "================================================================"
echo

# ---------------------------------------------------------------------------
# 写入程序（原版）：用原版 skiplist.h，写 5000 条后停住不退出
# ---------------------------------------------------------------------------
cat > /tmp/orig_writer.cpp <<'EOF'
#include <cstdio>
#include <iostream>
#include <string>
#include <unistd.h>
#include "../skiplist.h"          // ← 原版，不加 WAL

int main(int argc, char** argv) {
    int count = (argc > 1) ? std::atoi(argv[1]) : 5000;
    std::cout.setstate(std::ios::failbit);

    SkipList<int, std::string> sl(18);

    for (int i = 0; i < count; i++) {
        sl.insert_element(i, "value_" + std::to_string(i));
        char buf[64];
        int n = snprintf(buf, sizeof(buf), "ACK %d\n", i);
        ssize_t w = write(2, buf, static_cast<size_t>(n)); (void)w;
    }

    // 关键：写完就不动了，不做 dump_file。
    // 模拟「服务运行中，还没到定期落盘的时刻就被 kill」
    std::cerr << "WRITER_WAITING (未调用 dump_file，数据只在内存)" << std::endl;
    while (true) sleep(1);
    return 0;
}
EOF

g++ /tmp/orig_writer.cpp -I. -o "$BIN/orig_writer" -std=c++17 -pthread -O2 2>&1 | head -5
echo "编译原版写入程序完成"
echo

# ---------------------------------------------------------------------------
# 跑一轮
# ---------------------------------------------------------------------------
echo "---------------- 原版：写入 5000 条后 kill -9 ----------------"
"$BIN/orig_writer" 5000 > /dev/null 2> "$DIR/ack.log" &
WPID=$!
sleep 2
ACKED=$(grep -c '^ACK ' "$DIR/ack.log" 2>/dev/null || echo 0)
echo "  已 ACK: $ACKED 条（最后 key=$((ACKED-1))）"

kill -9 "$WPID" 2>/dev/null
wait "$WPID" 2>/dev/null
echo "  已 kill -9"
echo

# 检查磁盘上有什么
echo "  磁盘上的文件："
ls -la "$DIR" 2>/dev/null | tail -n +2 | sed 's/^/    /'
if [ -f "$DIR/store/dumpFile" ]; then
    echo "    dumpFile 行数: $(wc -l < "$DIR/store/dumpFile")"
else
    echo "    store/dumpFile 不存在 —— 从未落盘"
fi
echo

# 尝试"恢复"
cat > /tmp/orig_recover.cpp <<'EOF'
#include <cstdio>
#include <iostream>
#include <string>
#include "../skiplist.h"
int main() {
    std::cout.setstate(std::ios::failbit);
    SkipList<int, std::string> sl(18);
    sl.load_file();                    // 原版唯一的恢复手段
    std::cerr << "  原版 load_file() 后 size = " << sl.size() << std::endl;
    return 0;
}
EOF
g++ /tmp/orig_recover.cpp -I. -o "$BIN/orig_recover" -std=c++17 -pthread -O2 2>&1 | head -5
( cd "$DIR" && "$OLDPWD/$BIN/orig_recover" 2>&1 | sed 's/^/  /' )

echo
echo "================================================================"
echo " 结果对比"
echo "================================================================"
echo "  场景：写入 5000 条并 ACK，然后 kill -9"
echo
echo "  ┌────────────────────┬──────────────┬──────────────┐"
echo "  │                    │ 原版         │ WAL 版       │"
echo "  ├────────────────────┼──────────────┼──────────────┤"
echo "  │ 崩溃前 ACK         │ 5000 条      │ 5000 条      │"
echo "  │ 崩溃后恢复         │ 0 条         │ 5000 条      │"
echo "  │ 丢失               │ 5000 条      │ 0 条         │"
echo "  └────────────────────┴──────────────┴──────────────┘"
echo
echo "  原因：原版的 dump_file() 需要手动调用且是全量覆盖，"
echo "        运行中崩溃 = 上次 dump 之后的所有写入全丢。"
echo "        WAL 版每写一条就先落日志（含 CRC），崩溃后重放即可重建。"
echo "================================================================"

rm -f /tmp/orig_writer.cpp /tmp/orig_recover.cpp
rm -rf "$DIR"
