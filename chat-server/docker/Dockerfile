# ============================================================================
# 聊天服务器 Dockerfile —— 多阶段构建
#
# 设计说明：
#   1. 构建阶段安装全部依赖（Boost/gRPC/protobuf/MySQL/Redis 客户端），体积大但只用于编译
#   2. 运行阶段只拷贝可执行文件 + 运行时动态库，镜像更小
#   3. 服务通过 SERVICE_NAME 环境变量区分，同一镜像可跑多个服务
# ============================================================================

# ---------------------------------------------------------------------------
# 阶段一：构建
# ---------------------------------------------------------------------------
FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

# 编译依赖（这一层变动少，放前面利用缓存）
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

# 先拷 CMakeLists，利用 Docker 层缓存
COPY CMakeLists.txt ./
COPY server/ ./server/

# 生成 protobuf 代码并编译
# 注意：仓库未包含 *.pb.h / *.grpc.pb.h，需先从 .proto 生成
RUN find . -name "*.proto" -exec sh -c '\
        for p in "$@"; do \
            protoc -I"$(dirname "$p")" --cpp_out="$(dirname "$p")" "$p"; \
            protoc -I"$(dirname "$p")" --grpc_out="$(dirname "$p")" \
                   --plugin=protoc-gen-grpc=$(which grpc_cpp_plugin) "$p"; \
        done' sh {} +

RUN mkdir -p build && cd build \
    && cmake .. -DCMAKE_BUILD_TYPE=Release \
    && make -j"$(nproc)"

# ---------------------------------------------------------------------------
# 阶段二：运行
# ---------------------------------------------------------------------------
FROM ubuntu:22.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

# 运行时只需动态库，不需要编译工具
RUN apt-get update && apt-get install -y --no-install-recommends \
        libboost-system1.74.0 \
        libboost-filesystem1.74.0 \
        libjsoncpp25 \
        libhiredis0.14 \
        libmysqlcppconn7v5 \
        libprotobuf23 \
        libgrpc++1 \
        libgrpc9 \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# 从构建阶段拷贝产物
COPY --from=builder /build/build/ /app/build/

# 启动脚本（按 SERVICE_NAME 选择可执行文件）
COPY docker/entrypoint.sh /app/entrypoint.sh
RUN chmod +x /app/entrypoint.sh

# 各服务端口（仅文档作用，实际映射见 docker-compose.yml）
# GateServer:8080  ChatServer:8090/50055  ChatServer2:8091/50056
# StatusServer:50052  VerifyServer:50051  ResourceServer:9090

ENTRYPOINT ["/app/entrypoint.sh"]
