# ============================================================================
# 修复版 makefile（C++17，因为用了 std::shared_mutex）
# 用法：
#   make         编译 demo
#   make stress  编译压测
#   make p3      编译并运行 P3 范围查询测试
#   make bench   编译并运行 P3 性能基准
#   make p4      编译并运行 P4 WAL 测试
#   make crash   运行 kill -9 崩溃恢复演示
#   make compare 原版 vs WAL 崩溃对照实验
#   make test    编译并运行 P1 验证
#   make all-test 运行全部测试
#   make clean
# ============================================================================
CC      = g++
STD     = -std=c++17
FLAGS   = $(STD) -pthread -O2 -Wall
BIN     = ./bin

.PHONY: all stress test p3 bench p4 crash compare all-test clean

all: $(BIN)/main

$(BIN)/main: main.cpp skiplist.h
	@mkdir -p $(BIN) store
	$(CC) main.cpp -o $(BIN)/main $(FLAGS)

stress: $(BIN)/stress

$(BIN)/stress: stress-test/stress_test.cpp skiplist.h
	@mkdir -p $(BIN)
	$(CC) stress-test/stress_test.cpp -o $(BIN)/stress $(FLAGS)

test: $(BIN)/main
	@bash tests/run_p1_verify.sh

p3: $(BIN)/p3_test
	@./$(BIN)/p3_test

$(BIN)/p3_test: tests/p3_scan_test.cpp skiplist.h
	@mkdir -p $(BIN)
	$(CC) tests/p3_scan_test.cpp -o $(BIN)/p3_test $(FLAGS)

bench: $(BIN)/p3_bench
	@./$(BIN)/p3_bench

$(BIN)/p3_bench: tests/p3_bench.cpp skiplist.h
	@mkdir -p $(BIN)
	$(CC) tests/p3_bench.cpp -o $(BIN)/p3_bench $(FLAGS)

# ---------------------------------------------------------------- P4
p4: $(BIN)/p4_test
	@./$(BIN)/p4_test

$(BIN)/p4_test: tests/p4_wal_test.cpp kvstore.h skiplist.h
	@mkdir -p $(BIN)
	$(CC) tests/p4_wal_test.cpp -o $(BIN)/p4_test $(FLAGS)

crash:
	@bash tests/crash_demo.sh 3

compare:
	@bash tests/crash_compare.sh

# ---------------------------------------------------------------- P2
p2: $(BIN)/p2_test
	@./$(BIN)/p2_test

$(BIN)/p2_test: tests/p2_codec_test.cpp codec.h kvstore.h skiplist.h
	@mkdir -p $(BIN)
	$(CC) tests/p2_codec_test.cpp -o $(BIN)/p2_test $(FLAGS)

# P2 端到端：需要先有 server
p2e2e: server
	@bash tests/p2_redis_test.sh

# ---------------------------------------------------------------- P5
p5: $(BIN)/p5_test
	@./$(BIN)/p5_test

$(BIN)/p5_test: tests/p5_reverse_test.cpp skiplist.h
	@mkdir -p $(BIN)
	$(CC) tests/p5_reverse_test.cpp -o $(BIN)/p5_test $(FLAGS)

# ---------------------------------------------------------------- P6
server: $(BIN)/kv_server

$(BIN)/kv_server: kv_server.cpp resp.h kvstore.h skiplist.h codec.h
	@mkdir -p $(BIN)
	$(CC) kv_server.cpp -o $(BIN)/kv_server $(FLAGS)

resp: $(BIN)/resp_test
	@./$(BIN)/resp_test

$(BIN)/resp_test: tests/resp_test.cpp resp.h
	@mkdir -p $(BIN)
	$(CC) tests/resp_test.cpp -o $(BIN)/resp_test $(STD) -O2 -Wall

p6: server resp
	@bash tests/p6_redis_test.sh

p6bench: server
	@bash tests/p6_bench.sh

# 前台启动服务器（Ctrl-C 退出）
run: server
	@./$(BIN)/kv_server 6380 /tmp/kvdata

all-test: test p2 p3 p4 p5 resp p6
	@echo ""
	@echo "===== 全部测试完成 ====="

clean:
	rm -f $(BIN)/* *.o
