# ============================================================================
# 聊天服务器 Dockerfile —— 多阶段构建
#
# 设计说明：
#   1. 构建阶段安装全部依赖（Boost/gRPC/protobuf/MySQL/Redis 客户端），体积大但只用于编译
#   2. 运行阶段只拷贝可执行文件 + 运行时动态库，镜像更小
#   3. 服务通过 SERVICE_NAME 环境变量区分，同一镜像可跑多个服务
#
# 仓库根目录结构（chat-server/）：
#   CMakeLists.txt
#   ChatServer/ ChatServer2/ GateServer/ ResourceServer/ StatusServer/ VarifyServer/
#   docker/
#
# 注意：CMakeLists.txt 引用的是 server/XxxServer/... 路径，
#       所以这里先把各服务目录拷到 server/ 下再编译。
# ============================================================================

# ---------------------------------------------------------------------------
# 阶段一：构建
# ---------------------------------------------------------------------------
FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        pkg-config \
        libboost-all-dev \
        libjsoncpp-dev \
        libhiredis-dev \
        libmysqlcppconn-dev \
        protobuf-compiler \
        libprotobuf-dev \
        libgrpc++-dev \
        protobuf-compiler-grpc \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

COPY CMakeLists.txt ./
COPY ChatServer/     ./server/ChatServer/
COPY ChatServer2/    ./server/ChatServer2/
COPY GateServer/     ./server/GateServer/
COPY ResourceServer/ ./server/ResourceServer/
COPY StatusServer/   ./server/StatusServer/
COPY VarifyServer/   ./server/VarifyServer/

# ---------------------------------------------------------------------------
# mysqlcppconn 头文件布局修正
# ---------------------------------------------------------------------------
# 源码里写的是 #include <jdbc/mysql_driver.h>（Windows 版 MySQL Connector 的布局），
# 但 Ubuntu 的 libmysqlcppconn-dev 把头文件直接放在 /usr/include/ 下，
# 没有 jdbc/ 子目录，导致编译报错 "jdbc/mysql_driver.h: No such file"。
#
# 修法：建 /usr/include/jdbc/ 目录，把 mysql_driver.h、cppconn/ 等软链进去。
RUN set -eux; \
    if [ -d /usr/include/jdbc ]; then \
        echo "jdbc 目录已存在"; \
    else \
        mkdir -p /usr/include/jdbc; \
        # 单个头文件
        for f in /usr/include/mysql_driver.h /usr/include/mysql_connection.h \
                 /usr/include/mysql_error.h /usr/include/mysql_exception.h \
                 /usr/include/mysql_metadata.h /usr/include/mysql_parameter_metadata.h \
                 /usr/include/mysql_prepared_statement.h /usr/include/mysql_resultset.h \
                 /usr/include/mysql_resultset_metadata.h /usr/include/mysql_statement.h \
                 /usr/include/mysql_warning.h /usr/include/mysqlx* ; do \
            [ -e "$f" ] && ln -sf "$f" /usr/include/jdbc/ || true; \
        done; \
        # cppconn 子目录
        [ -d /usr/include/cppconn ] && ln -sfn /usr/include/cppconn /usr/include/jdbc/cppconn || true; \
        # 若在别的路径（如 mysql-cppconn-8）
        OTHER=$(find /usr/include -maxdepth 2 -type d -name "mysql-cppconn*" 2>/dev/null | head -1); \
        if [ -n "$OTHER" ]; then \
            for f in "$OTHER"/*.h; do [ -e "$f" ] && ln -sf "$f" /usr/include/jdbc/ || true; done; \
            [ -d "$OTHER"/cppconn ] && ln -sfn "$OTHER"/cppconn /usr/include/jdbc/cppconn || true; \
        fi; \
        echo "=== jdbc 目录内容 ==="; ls -la /usr/include/jdbc | head -20; \
    fi

# ---------------------------------------------------------------------------
# 生成 protobuf / gRPC 代码
# ---------------------------------------------------------------------------
# 每个服务目录下各有一份 message.proto，需在各自目录内生成（-I 指向该目录）
RUN set -eux; \
    for p in $(find server -name "message.proto"); do \
        d=$(dirname "$p"); \
        echo "--- 生成: $d ---"; \
        protoc -I"$d" --cpp_out="$d" "$p"; \
        protoc -I"$d" --grpc_out="$d" \
               --plugin=protoc-gen-grpc="$(which grpc_cpp_plugin)" "$p"; \
    done; \
    echo "=== 生成的 pb 文件数 ==="; \
    find server -name "*.pb.h" | wc -l

# ---------------------------------------------------------------------------
# 编译
# ---------------------------------------------------------------------------
RUN mkdir -p build && cd build \
    && cmake .. -DCMAKE_BUILD_TYPE=Release \
    && make -j"$(nproc)" \
    && echo "=== 产物 ===" \
    && find . -maxdepth 2 -type f -perm -u+x ! -name "*.cmake" ! -name "*.sh"

# ---------------------------------------------------------------------------
# 阶段二：运行
# ---------------------------------------------------------------------------
FROM ubuntu:22.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        libboost-system1.74.0 \
        libboost-filesystem1.74.0 \
        libjsoncpp25 \
        libhiredis0.14 \
        libmysqlcppconn7v5 \
        libprotobuf23 \
        libgrpc++1 \
        libgrpc9 \
        netcat-openbsd \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=builder /build/build/ /app/build/
COPY docker/entrypoint.sh /app/entrypoint.sh
RUN chmod +x /app/entrypoint.sh

# 端口：gate_server:8080  chat_server:8090/50055
#       status_server:50052  resource_server:9090

ENTRYPOINT ["/app/entrypoint.sh"]
