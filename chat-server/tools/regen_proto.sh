#!/usr/bin/env bash
# 为所有服务重新生成 protobuf / gRPC 的 C++ 代码。
#
# 背景：message.pb.* / message.grpc.pb.* 由 .proto 生成，已在 .gitignore 中排除，
#       因此首次构建、或修改过任何 .proto 之后，都必须先跑一遍本脚本。
#
# 依赖（Ubuntu/Debian）：
#   sudo apt install -y protobuf-compiler protobuf-compiler-grpc \
#                       libprotobuf-dev libgrpc++-dev
#
# 用法：
#   ./tools/regen_proto.sh            # 生成全部服务
#   ./tools/regen_proto.sh ChatServer # 只生成某一个服务
set -euo pipefail

cd "$(dirname "$0")/.."

PROTOC="${PROTOC:-protoc}"
if ! command -v "$PROTOC" >/dev/null 2>&1; then
    echo "错误：找不到 protoc，请先安装 protobuf-compiler" >&2
    exit 1
fi

GRPC_CPP_PLUGIN="${GRPC_CPP_PLUGIN:-$(command -v grpc_cpp_plugin || true)}"
if [ -z "$GRPC_CPP_PLUGIN" ] || [ ! -x "$GRPC_CPP_PLUGIN" ]; then
    echo "错误：找不到 grpc_cpp_plugin，请先安装 protobuf-compiler-grpc" >&2
    exit 1
fi

echo "protoc             : $("$PROTOC" --version)"
echo "grpc_cpp_plugin    : $GRPC_CPP_PLUGIN"

SERVICES=("ChatServer" "ChatServer2" "GateServer" "ResourceServer" "StatusServer" "VarifyServer")
if [ "$#" -gt 0 ]; then
    SERVICES=("$@")
fi

for service in "${SERVICES[@]}"; do
    proto="${service}/message.proto"
    if [ ! -f "$proto" ]; then
        echo "跳过 ${service}：找不到 ${proto}"
        continue
    fi
    echo "==> 生成 ${service}"
    "$PROTOC" -I "${service}" \
        --cpp_out="${service}" \
        --grpc_out="${service}" \
        --plugin=protoc-gen-grpc="${GRPC_CPP_PLUGIN}" \
        "${proto}"
done

echo "完成。生成的文件（.pb.h/.pb.cc/.grpc.pb.h/.grpc.pb.cc）不进版本库，属正常现象。"
