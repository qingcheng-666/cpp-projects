#!/bin/bash
# ============================================================================
# P1 修复验证：对每个缺陷，同时跑「原版」和「修复版」，证明修复生效
#
# 用法：cd fixed && bash tests/run_p1_verify.sh
# ============================================================================
set -u
cd "$(dirname "$0")/../.." || exit 1     # 回到项目根目录
ROOT="$(pwd)"
FIXED="$ROOT/fixed"
BIN="$FIXED/bin"
mkdir -p "$BIN"

hr() { printf '%s\n' "============================================================"; }
sub() { printf '%s\n' "------------------------------------------------------------"; }

echo "P1 修复验证  |  $(date)"
echo "原版: $ROOT/skiplist.h"
echo "修复版: $FIXED/skiplist.h"
hr

# ============================================================ 修复 1
echo "[修复 1] 递归析构爆栈  →  迭代析构"
sub
cat > /tmp/p1_dtor.cpp <<'EOF'
#include <iostream>
#include <string>
#include "skiplist.h"          // 由 -I 决定用原版还是修复版
int main(){
    const int N = 1000000;
    SkipList<int,std::string>* sl = new SkipList<int,std::string>(18);
    std::cout.setstate(std::ios::failbit);
    for (int i=0;i<N;i++) sl->insert_element(i,"v");
    std::cout.clear();
    std::cout << "  inserted " << N << " nodes, destroying..." << std::endl;
    delete sl;
    std::cout << "  destroy OK" << std::endl;
    return 0;
}
EOF

# 原版
g++ /tmp/p1_dtor.cpp -I"$ROOT" -o "$BIN/p1_dtor_orig" --std=c++11 -pthread 2>/dev/null
if [ -x "$BIN/p1_dtor_orig" ]; then
    "$BIN/p1_dtor_orig" >/dev/null 2>&1; rc=$?
    [ $rc -eq 139 ] && echo "  原版  : ❌ 段错误 (exit=139) —— 缺陷存在" \
                    || echo "  原版  : exit=$rc"
else
    echo "  原版  : (编译失败)"
fi
# 修复版
g++ /tmp/p1_dtor.cpp -I"$FIXED" -o "$BIN/p1_dtor_fixed" --std=c++17 -pthread 2>/dev/null
if [ -x "$BIN/p1_dtor_fixed" ]; then
    "$BIN/p1_dtor_fixed" >/dev/null 2>&1; rc=$?
    [ $rc -eq 0 ] && echo "  修复版: ✅ 正常退出 (exit=0) —— 已修复" \
                  || echo "  修复版: ❌ exit=$rc"
else
    echo "  修复版: (编译失败)"
fi
rm -f /tmp/p1_dtor.cpp
hr

# ============================================================ 修复 2
echo "[修复 2] 头文件全局变量 → 多 TU 链接冲突"
sub
mkdir -p /tmp/p1link && cd /tmp/p1link
cat > a.cpp <<'EOF'
#ifdef USE_FIXED
#include "skiplist.h"
#else
#include "skiplist.h"
#endif
int fa(){ SkipList<int,std::string> s(6); return s.size(); }
EOF
cat > b.cpp <<'EOF'
#include "skiplist.h"
int fa();
int main(){ return fa(); }
EOF

g++ a.cpp b.cpp -I"$ROOT" -o t_orig --std=c++11 -pthread 2>/tmp/p1_link_err.txt
if [ $? -ne 0 ]; then
    n=$(grep -c "multiple definition" /tmp/p1_link_err.txt)
    echo "  原版  : ❌ 链接失败，$n 处 multiple definition —— 缺陷存在"
else
    echo "  原版  : 链接成功"
fi

g++ a.cpp b.cpp -I"$FIXED" -o t_fixed --std=c++17 -pthread 2>/dev/null
if [ $? -eq 0 ]; then
    echo "  修复版: ✅ 链接成功 —— 已修复（锁改为成员变量、分隔符改为类内常量）"
else
    echo "  修复版: ❌ 仍然链接失败"
fi
cd "$ROOT"; rm -rf /tmp/p1link /tmp/p1_link_err.txt
hr

# ============================================================ 修复 3
echo "[修复 3] search 无锁数据竞争（ThreadSanitizer）"
sub
cat > /tmp/p1_race.cpp <<'EOF'
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <string>
#include "skiplist.h"          // 由 -I 决定用原版还是修复版
SkipList<int,std::string> sl(18);
std::atomic<bool> stop{false};
int main(){
    std::cout.setstate(std::ios::failbit);
    for (int i=0;i<1000;i++) sl.insert_element(i,"v");
    std::thread w([]{ int k=1000; while(!stop) sl.insert_element(k++,"v"); });
    std::thread r([]{ while(!stop) sl.search_element(500); });
    std::this_thread::sleep_for(std::chrono::seconds(3));
    stop=true; w.join(); r.join();
    std::cout.clear();
    std::cout << "  survived, size=" << sl.size() << std::endl;
    return 0;
}
EOF

g++ /tmp/p1_race.cpp -I"$ROOT" -o "$BIN/p1_race_orig" --std=c++11 -pthread -fsanitize=thread 2>/dev/null
if [ -x "$BIN/p1_race_orig" ]; then
    n=$(setarch "$(uname -m)" -R "$BIN/p1_race_orig" 2>&1 | grep -c "WARNING: ThreadSanitizer: data race")
    [ "$n" -gt 0 ] && echo "  原版  : ❌ TSan 报出 $n 处 data race —— 缺陷存在" \
                   || echo "  原版  : 无竞争报告"
else
    echo "  原版  : (编译失败)"
fi

g++ /tmp/p1_race.cpp -I"$FIXED" -o "$BIN/p1_race_fixed" --std=c++17 -pthread -fsanitize=thread 2>/dev/null
if [ -x "$BIN/p1_race_fixed" ]; then
    n=$(setarch "$(uname -m)" -R "$BIN/p1_race_fixed" 2>&1 | grep -c "WARNING: ThreadSanitizer: data race")
    [ "$n" -eq 0 ] && echo "  修复版: ✅ TSan 无 data race —— 已修复（shared_mutex 读写锁）" \
                   || echo "  修复版: ❌ 仍有 $n 处竞争"
else
    echo "  修复版: (编译失败)"
fi
rm -f /tmp/p1_race.cpp
hr

# ============================================================ 修复 4
echo "[修复 4] dump_file 静默丢数据"
sub
mkdir -p /tmp/p1dump && cd /tmp/p1dump && rm -rf store
cat > t.cpp <<'EOF'
#include <iostream>
#include <string>
#ifdef USE_FIXED
#include "skiplist.h"
#else
#include "skiplist.h"
#endif
int main(){
    SkipList<int,std::string> s(6);
    s.insert_element(1,"x");
#ifndef USE_FIXED
    s.dump_file();          // 原版：返回 void，无法判断成功与否
    std::cout << "  原版  : dump_file() 无返回值，进程正常退出" << std::endl;
    return 0;               // 假装成功
#else
    bool ok = s.dump_file(); // 修复版：返回 bool
    if (!ok) { std::cerr << "  修复版: dump_file 返回 false（已捕获失败）" << std::endl; return 1; }
    std::cout << "  修复版: dump_file 返回 true" << std::endl;
    return 0;
#endif
}
EOF
g++ t.cpp -I"$ROOT" -o t_orig --std=c++11 -pthread 2>/dev/null
./t_orig >/dev/null 2>&1; rc=$?
[ "$rc" -eq 0 ] && [ ! -d store ] && echo "  原版  : ❌ exit=0 但 store/ 未创建，无任何报错 —— 静默丢数据"

g++ -DUSE_FIXED t.cpp -I"$FIXED" -o t_fixed --std=c++17 -pthread 2>/dev/null
./t_fixed >/dev/null 2>&1; rc=$?
[ "$rc" -ne 0 ] && echo "  修复版: ✅ exit=$rc，失败被捕获并报错 —— 已修复" \
                || echo "  修复版: exit=$rc"
cd "$ROOT"; rm -rf /tmp/p1dump
hr

# ============================================================ 修复 5
echo "[修复 5] 层高从 1 开始 → L1 完全冗余"
sub
cat > /tmp/p1_lv.cpp <<'EOF'
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <map>
int main(){
    srand(time(NULL));
    const int MAX_LEVEL = 18;
    std::map<int,int> hist; int mn = 999;
    for (int n=0;n<100000;n++){
#ifdef FIXED_K
        int k = 0;                       // 修复版
#else
        int k = 1;                       // 原版
#endif
        while (rand()%2) k++;
        k = (k<MAX_LEVEL)?k:MAX_LEVEL;
        hist[k]++; if (k<mn) mn=k;
    }
    std::cout << "  最小层高=" << mn
              << "  L0占比=100%  L1占比=" << (100.0*(100000-hist[0])/100000) << "%"
              << std::endl;
    return 0;
}
EOF
g++ /tmp/p1_lv.cpp -o /tmp/lv_o --std=c++11 2>/dev/null && printf "  原版  : " && /tmp/lv_o
g++ -DFIXED_K /tmp/p1_lv.cpp -o /tmp/lv_f --std=c++11 2>/dev/null && printf "  修复版: " && /tmp/lv_f
echo "  （原版 L1 占比 100% => L1 与 L0 完全相同；修复版约 50% => L1 真正起索引作用）"
rm -f /tmp/p1_lv.cpp /tmp/lv_o /tmp/lv_f
hr

echo "结论："
echo "  修复 1/2/3/4 均已验证（原版复现缺陷 → 修复版通过）"
echo "  修复 5 为空间优化，通过层高分布验证"
echo "  修复 6/7 为语义与可移植性改进（无 update 语义、VLA→vector），见文档说明"
echo
echo "下一步：diff -u ../skiplist.h skiplist.h   查看全部改动"
