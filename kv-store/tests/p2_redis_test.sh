#!/bin/bash
# ============================================================================
# [P2] 用 redis-cli 验证 string key
# ----------------------------------------------------------------------------
# 本文件必须以 UTF-8 保存（含中文测试数据）。
#
# 用法：cd fixed && bash tests/p2_redis_test.sh
# ============================================================================
set -u
export LC_ALL=C.UTF-8
cd "$(dirname "$0")/.." || exit 1

PORT=6387
DIR=/tmp/kvdata_p2
PASS=0
FAIL=0

ok()  { PASS=$((PASS+1)); echo "  ✅ $1"; }
bad() { FAIL=$((FAIL+1)); echo "  ❌ $1  (got: $2)"; }
expect() {  # expect <期望> <说明> <命令...>
    local want="$1"; shift
    local desc="$1"; shift
    local got
    got=$(redis-cli -p "$PORT" "$@" 2>&1)
    if [ "$got" = "$want" ]; then ok "$desc"; else bad "$desc → want[$want]" "$got"; fi
}

echo "================================================================"
echo " P2 string key 验证（真实 redis-cli）"
echo "================================================================"
echo

pkill -f "kv_server $PORT" 2>/dev/null || true
sleep 0.3
rm -rf "$DIR"

./bin/kv_server "$PORT" "$DIR" > /tmp/kv_p2.log 2>&1 &
SRV=$!
sleep 1.2

if ! kill -0 "$SRV" 2>/dev/null; then
    echo "服务器启动失败："; cat /tmp/kv_p2.log; exit 1
fi
echo "服务器已启动 (pid=$SRV, port=$PORT)"
echo

# ---------------------------------------------------------------- 核心：string key
echo "--- 核心：现在 key 可以是任意字符串了 ---"
expect "OK"      "SET username alice"        set username alice
expect "alice"   "GET username"              get username
expect "OK"      "SET 中文键 中文值"          set 中文键 中文值
expect "中文值"   "GET 中文键"                get 中文键
expect "OK"      "SET 'key with space' v"    set "key with space" "hello world"
expect "hello world" "GET 'key with space'"  get "key with space"

# ---------------------------------------------------------------- 含冒号（原版会截断）
echo
echo "--- 含冒号的 key（原版 find 会截断，现改为 rfind）---"
expect "OK"      "SET user:1001:name bob"    set user:1001:name bob
expect "bob"     "GET user:1001:name"        get user:1001:name
expect "OK"      "SET user:1002:name carol"  set user:1002:name carol
expect "OK"      "SET user:1001:age 30"      set user:1001:age 30
expect "30"      "GET user:1001:age"         get user:1001:age

# ---------------------------------------------------------------- 覆盖语义
echo
echo "--- 覆盖语义 ---"
redis-cli -p "$PORT" set counter 1 > /dev/null
redis-cli -p "$PORT" set counter 2 > /dev/null
expect "2"       "同一 key 写两次，取最后一次"  get counter

# ---------------------------------------------------------------- 字典序范围查询
echo
echo "--- 字典序范围查询（P3 的能力，现在按字符串排序）---"
# 注意 prefix 区间的写法：用 [prefix, prefix+\xff)
#
# 实测字典序（':'=58  ';'=59  'n'=110）：
#     user:1001:age  <  user:1001:name  <  user:1002:name  <  user;  <  username
#
# 当前以 user: 开头的有 3 个：user:1001:age, user:1001:name, user:1002:name
expect "3"       "COUNT user: 前缀（3 个带冒号的）" count user: "user:$(printf '\xff')"
# 再加上 username，共 4 个
expect "4"       "COUNT user 前缀（含 username 共 4 个）" count user "user$(printf '\xff')"
# 上界用 ';' 会漏掉 username（因为它排在 user; 之后），只命中带冒号的 3 个
expect "3"       "COUNT [user, user;) 只命中带冒号的 3 个（漏掉 username）" count user "user;"

# ---------------------------------------------------------------- 持久化
echo
echo "--- 持久化（string key 走快照 + WAL）---"
expect "OK"      "SNAPSHOT"                  snapshot
redis-cli -p "$PORT" set after_snapshot in_wal > /dev/null
BEFORE=$(redis-cli -p "$PORT" dbsize)

kill "$SRV" 2>/dev/null; sleep 0.8
./bin/kv_server "$PORT" "$DIR" > /tmp/kv_p2b.log 2>&1 &
SRV=$!
sleep 1.2

AFTER=$(redis-cli -p "$PORT" dbsize)
if [ "$AFTER" = "$BEFORE" ]; then ok "重启后 key 数一致（$AFTER）"; else bad "重启后 key 数不一致" "$BEFORE → $AFTER"; fi
expect "alice"   "重启后 GET username"        get username
expect "bob"     "重启后 GET user:1001:name"  get user:1001:name
expect "中文值"   "重启后 GET 中文键"          get 中文键
expect "in_wal"  "重启后 GET after_snapshot（WAL）" get after_snapshot

# ---------------------------------------------------------------- 删除
echo
echo "--- 删除 ---"
expect "1"       "DEL username"              del username
expect ""        "GET username (已删)"        get username
expect "0"       "DEL 不存在的 key"           del nonexist

# ---------------------------------------------------------------- 旧行为已移除
echo
echo "--- 旧限制已移除 ---"
R=$(redis-cli -p "$PORT" get abc 2>&1)
if echo "$R" | grep -q "must be an integer"; then
    bad "非整数 key 仍被拒绝（P2 未生效）" "$R"
else
    ok "非整数 key 不再报 'must be an integer'"
fi

kill "$SRV" 2>/dev/null
pkill -f "kv_server $PORT" 2>/dev/null || true

echo
echo "================================================================"
echo " 通过: $PASS   失败: $FAIL"
if [ "$FAIL" -eq 0 ]; then echo " ✅ 全部通过"; else echo " ❌ 有失败项"; fi
echo "================================================================"
exit $([ "$FAIL" -eq 0 ] && echo 0 || echo 1)
