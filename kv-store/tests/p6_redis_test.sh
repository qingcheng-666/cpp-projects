#!/bin/bash
# ============================================================================
# [P6] 用真实 redis-cli 验证 KV Server
# ----------------------------------------------------------------------------
# 注意：本文件必须以 UTF-8 保存（含中文测试数据）。
#       用 ASCII 保存会把中文变成 "?????"，而 "?" 在 bash 里是通配符，
#       会被 glob 展开成多个参数，导致 SET 报 "wrong number of arguments"。
#
# 用法：cd fixed && bash tests/p6_redis_test.sh
# ============================================================================
set -u
export LC_ALL=C.UTF-8
cd "$(dirname "$0")/.." || exit 1

PORT=6381
DIR=/tmp/kvdata_p6
PASS=0
FAIL=0

ok()   { PASS=$((PASS+1)); echo "  ✅ $1"; }
bad()  { FAIL=$((FAIL+1)); echo "  ❌ $1  (got: $2)"; }
# 断言：redis-cli 输出等于期望
expect() {  # expect <期望> <说明> <命令...>
    local want="$1"; shift
    local desc="$1"; shift
    local got
    got=$(redis-cli -p "$PORT" "$@" 2>&1)
    if [ "$got" = "$want" ]; then ok "$desc"; else bad "$desc → want[$want]" "$got"; fi
}

echo "================================================================"
echo " P6 KV Server 功能验证（真实 redis-cli）"
echo "================================================================"
echo

pkill -f "kv_server $PORT" 2>/dev/null || true
sleep 0.3
rm -rf "$DIR"

./bin/kv_server "$PORT" "$DIR" > /tmp/kv_p6.log 2>&1 &
SRV=$!
sleep 1.2

if ! kill -0 "$SRV" 2>/dev/null; then
    echo "服务器启动失败："; cat /tmp/kv_p6.log; exit 1
fi
echo "服务器已启动 (pid=$SRV, port=$PORT)"
echo

# ---------------------------------------------------------------- 基本命令
echo "--- 基本命令 ---"
expect "PONG"        "PING"                    ping
expect "OK"          "SET 1 hello"             set 1 hello
expect "hello"       "GET 1"                   get 1
expect "OK"          "SET 100 代码随想录"       set 100 代码随想录
expect "代码随想录"   "GET 100 (UTF-8 中文)"    get 100
expect ""            "GET 999 (不存在→nil)"     get 999
expect "1"           "EXISTS 1"                exists 1
expect "0"           "EXISTS 999"              exists 999
expect "2"           "DBSIZE"                  dbsize
expect "1"           "DEL 1"                   del 1
expect "0"           "DEL 999 (不存在)"         del 999
expect "1"           "DBSIZE after DEL"        dbsize

# ---------------------------------------------------------------- 大小写
echo
echo "--- 命令名大小写不敏感 ---"
expect "OK"          "SET 大写"                SET 7 seven
expect "seven"       "GET 大写"                GET 7
expect "seven"       "GeT 混合大小写"           GeT 7

# ---------------------------------------------------------------- 二进制安全
echo
echo "--- 二进制安全（value 含空格/特殊字符）---"
expect "OK"          "SET 含空格 value"        set 8 "hello world foo"
expect "hello world foo" "GET 含空格 value"     get 8

# ---------------------------------------------------------------- P3 范围查询
echo
echo "--- P3 范围查询能力 ---"
for i in 10 20 30 40 50; do redis-cli -p "$PORT" set $i "v$i" > /dev/null; done
expect "20
v20
30
v30"                 "SCAN 20 40 (左闭右开)"   scan 20 40
expect "3"           "COUNT 20 50"             count 20 50
# 注意：前面 set 过 key=100，所以 [100,200) 里确实有 1 个元素。
# 之前这里写期望 0 是我写错了 —— 用真正空的区间来测。
# [P2] 注意：key 现在是【字符串】，按【字典序】排序。
#   所以 "100"、"777"、"901" 都落在 ["1000","2000") 里 —— 因为
#   "100" < "1000"（短的在前）、"777" < "1000"、"901" < "1000"。
#   想测"真空区间"要用一个任何已有 key 都不在里面的区间：
#   这里用 ["zzz","zzzz")，没有 key 以 z 开头。
expect "0"           "COUNT zzz zzzz (真空区间)" count zzz zzzz

# ---------------------------------------------------------------- P4 持久化
echo
echo "--- P4 持久化能力 ---"
expect "OK"          "SNAPSHOT 触发快照"        snapshot
if [ -f "$DIR/snapshot" ]; then ok "快照文件已生成 ($(stat -c%s "$DIR/snapshot") 字节)"; else bad "快照文件未生成" "-"; fi
WAL_SIZE=$(stat -c%s "$DIR/wal.log" 2>/dev/null || echo "?")
if [ "$WAL_SIZE" = "0" ]; then ok "快照后 WAL 已清空"; else bad "WAL 未清空" "$WAL_SIZE"; fi

# ---------------------------------------------------------------- 管道
echo
echo "--- 管道（pipelining，验证粘包处理）---"
# 一次性发 4 条命令，验证服务端能正确处理粘包
PIPED=$(printf 'set 901 a\r\nset 902 b\r\nset 903 c\r\nget 902\r\n' | redis-cli -p "$PORT" --pipe 2>&1)
if echo "$PIPED" | grep -q "errors: 0"; then
    ok "管道 4 条命令（含粘包）全部成功，errors: 0"
else
    bad "管道处理异常" "$(echo "$PIPED" | tail -1)"
fi
expect "b"           "管道写入后 GET 902"        get 902
expect "3"           "管道写入的 3 个 key 都在"    count 901 904

# ---------------------------------------------------------------- ECHO
echo
echo "--- ECHO / SELECT（客户端兼容性）---"
expect "hello"       "ECHO hello"              echo hello
expect "OK"          "SELECT 0"                select 0
R=$(redis-cli -p "$PORT" select 5 2>&1)
if echo "$R" | grep -q "out of range"; then ok "SELECT 5 被拒绝（只有 db 0）"; else bad "SELECT 5 处理异常" "$R"; fi

# ---------------------------------------------------------------- 错误处理
echo
echo "--- 错误处理 ---"
# [P2] 这里原来测的是「非整数 key 被拒绝」。
#   P2 之后 key 是任意字符串，所以 `GET abc` 不再报错，
#   而是正常返回空（key 不存在）—— 这正是 P2 想要的效果。
R=$(redis-cli -p "$PORT" get abc 2>&1)
if [ -z "$R" ]; then
    ok "非整数 key 正常处理（返回空，不再是 'must be an integer'）"
else
    bad "非整数 key 处理异常" "$R"
fi
# 反向确认：旧限制确实已移除
R2=$(redis-cli -p "$PORT" set abc xyz 2>&1)
if [ "$R2" = "OK" ]; then ok "string key 可以正常写入（P2 生效）"; else bad "string key 写入失败" "$R2"; fi

R=$(redis-cli -p "$PORT" set onlykey 2>&1)
if echo "$R" | grep -q "wrong number of arguments"; then ok "参数个数错误被拒绝"; else bad "参数错误处理异常" "$R"; fi

R=$(redis-cli -p "$PORT" nosuchcmd 2>&1)
if echo "$R" | grep -q "unknown command"; then ok "未知命令被拒绝"; else bad "未知命令处理异常" "$R"; fi

# ---------------------------------------------------------------- 重启恢复
echo
echo "--- 重启后数据恢复（快照 + WAL）---"
BEFORE=$(redis-cli -p "$PORT" dbsize)
# 快照之后再写几条（只进 WAL）
redis-cli -p "$PORT" set 777 in_wal > /dev/null
kill "$SRV" 2>/dev/null; sleep 0.8

./bin/kv_server "$PORT" "$DIR" > /tmp/kv_p6b.log 2>&1 &
SRV=$!
sleep 1.2
AFTER=$(redis-cli -p "$PORT" dbsize)
V777=$(redis-cli -p "$PORT" get 777)
V100=$(redis-cli -p "$PORT" get 100)

if [ "$V777" = "in_wal" ]; then ok "WAL 中的写入已恢复 (key=777)"; else bad "WAL 恢复失败" "$V777"; fi
if [ "$V100" = "代码随想录" ]; then ok "快照中的中文值已恢复 (key=100)"; else bad "快照恢复失败" "$V100"; fi
if [ "$AFTER" -gt "$BEFORE" ]; then ok "重启后 size 从 $BEFORE 增长到 $AFTER"; else bad "size 异常" "$AFTER"; fi

# ---------------------------------------------------------------- INFO
echo
echo "--- INFO ---"
redis-cli -p "$PORT" info | sed 's/^/    /'

kill "$SRV" 2>/dev/null
pkill -f "kv_server $PORT" 2>/dev/null || true

echo
echo "================================================================"
echo " 通过: $PASS   失败: $FAIL"
if [ "$FAIL" -eq 0 ]; then echo " ✅ 全部通过"; else echo " ❌ 有失败项"; fi
echo "================================================================"
exit $([ "$FAIL" -eq 0 ] && echo 0 || echo 1)
