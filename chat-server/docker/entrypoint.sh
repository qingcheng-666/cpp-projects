#!/bin/sh
# ============================================================================
# 容器启动脚本
#
# 通过 SERVICE_NAME 环境变量决定启动哪个服务，同一镜像可跑多个服务，
# 不用为每个服务单独构建镜像（节省构建时间和镜像体积）。
#
# SERVICE_NAME 取值（对应 CMake 产物名）：
#   gate_server / chat_server / status_server / resource_server
#
# 用法：
#   docker run -e SERVICE_NAME=gate_server ...
#   docker compose up   # 见 docker-compose.yml
# ============================================================================
set -e

SERVICE_NAME="${SERVICE_NAME:-gate_server}"
CONFIG_DIR="${CONFIG_DIR:-/app/config}"

echo "[entrypoint] 启动服务: ${SERVICE_NAME}"
echo "[entrypoint] 配置目录: ${CONFIG_DIR}"

# 等待依赖服务就绪（最多 60 秒）
wait_for() {
    host="$1"; port="$2"; name="$3"
    i=0
    while [ $i -lt 60 ]; do
        if nc -z "$host" "$port" 2>/dev/null; then
            echo "[entrypoint] ${name} (${host}:${port}) 已就绪"
            return 0
        fi
        i=$((i + 1))
        sleep 1
    done
    echo "[entrypoint] 警告: ${name} (${host}:${port}) 60 秒内未就绪，继续启动"
    return 0
}

[ -n "${MYSQL_HOST:-}" ] && wait_for "${MYSQL_HOST}" "${MYSQL_PORT:-3306}" "MySQL"
[ -n "${REDIS_HOST:-}" ] && wait_for "${REDIS_HOST}" "${REDIS_PORT:-6379}" "Redis"

# 定位可执行文件（CMake 产物在 build/ 根目录）
BIN=""
for cand in "/app/build/${SERVICE_NAME}" "/app/build/bin/${SERVICE_NAME}" \
            "/app/build/Release/${SERVICE_NAME}"; do
    if [ -x "$cand" ]; then BIN="$cand"; break; fi
done

if [ -z "$BIN" ]; then
    BIN=$(find /app/build -maxdepth 3 -type f -name "${SERVICE_NAME}" -perm -u+x 2>/dev/null | head -1)
fi

if [ -z "$BIN" ]; then
    echo "[entrypoint] 错误: 找不到可执行文件 ${SERVICE_NAME}"
    echo "[entrypoint] /app/build 下可执行文件："
    find /app/build -maxdepth 3 -type f -perm -u+x 2>/dev/null | head -20
    exit 1
fi

echo "[entrypoint] 可执行文件: ${BIN}"

# 切到配置目录（程序从当前目录读取 config.ini）
cd "${CONFIG_DIR}" 2>/dev/null || cd /app

exec "$BIN"
