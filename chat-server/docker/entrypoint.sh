#!/bin/sh
# ============================================================================
# 容器启动脚本
#
# 通过 SERVICE_NAME 环境变量决定启动哪个服务，这样同一镜像可以跑多个服务，
# 不用为每个服务单独构建镜像（节省构建时间和镜像体积）。
#
# 用法：
#   docker run -e SERVICE_NAME=GateServer ...
#   docker compose up   # 见 docker-compose.yml
# ============================================================================
set -e

SERVICE_NAME="${SERVICE_NAME:-GateServer}"
CONFIG_DIR="${CONFIG_DIR:-/app/config}"

echo "[entrypoint] 启动服务: ${SERVICE_NAME}"
echo "[entrypoint] 配置目录: ${CONFIG_DIR}"

# 等待 MySQL / Redis 就绪（用 nc 探测，最多等 60 秒）
wait_for() {
    host="$1"
    port="$2"
    name="$3"
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

# 从配置里读 MySQL / Redis 地址（简化处理，实际可用 envsubst 生成配置）
if [ -n "${MYSQL_HOST:-}" ]; then
    wait_for "${MYSQL_HOST}" "${MYSQL_PORT:-3306}" "MySQL"
fi
if [ -n "${REDIS_HOST:-}" ]; then
    wait_for "${REDIS_HOST}" "${REDIS_PORT:-6379}" "Redis"
fi

# 找可执行文件（构建产物的路径可能因 CMake 配置而异）
BIN=$(find /app/build -type f -name "${SERVICE_NAME}" -perm -u+x 2>/dev/null | head -1)

if [ -z "$BIN" ]; then
    echo "[entrypoint] 错误: 找不到可执行文件 ${SERVICE_NAME}"
    echo "[entrypoint] /app/build 内容:"
    find /app/build -maxdepth 3 -type f -perm -u+x 2>/dev/null | head -20
    exit 1
fi

echo "[entrypoint] 可执行文件: ${BIN}"

# 切到配置目录再启动（程序默认读取当前目录的 config.ini）
cd "${CONFIG_DIR}" 2>/dev/null || cd /app

exec "$BIN"
