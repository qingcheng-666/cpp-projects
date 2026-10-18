# Docker 部署

一条命令拉起整套服务（MySQL + Redis + 6 个业务服务），不用再手动装依赖。

---

## 为什么需要 Docker

这个项目依赖 **Boost、gRPC、protobuf、hiredis、MySQL Connector、MySQL Server、Redis Server**。

手动部署的痛点：
```
装 boost → 装 grpc → 版本不匹配 → 装 protobuf → 编译报错
→ 装 MySQL → 建库建表 → 装 Redis → 改配置里的 IP
→ 启动顺序不对 → 服务连不上 → ...
```

**Docker 把「编译好的程序 + 运行时依赖 + 数据库」全部打包**，一条命令全起来。

> 注意区分两类依赖：
> - **编译期**：头文件、Boost 源码 —— 只在构建镜像时需要
> - **运行期**：`.so` 动态库、MySQL/Redis **服务进程** —— 运行时需要
>
> 镜像里放的是**编译好的程序 + 运行期依赖**，不是源码。

---

## 快速开始

```bash
cd chat-server/docker

# 构建镜像（首次约 10-20 分钟，主要是编译 Boost/gRPC 依赖）
docker compose build

# 启动全部服务
docker compose up -d

# 查看状态
docker compose ps

# 看日志
docker compose logs -f gate
```

访问：`http://localhost:8080`

停止：
```bash
docker compose down          # 停止并删除容器（保留数据）
docker compose down -v       # 连数据卷一起删（重置数据库）
```

---

## 服务与端口

| 服务 | 容器名 | 端口 | 说明 |
|---|---|---|---|
| **MySQL** | chat-mysql | 3306 | 用户、好友、消息持久化 |
| **Redis** | chat-redis | 6379 | Token、在线状态、分布式锁 |
| **GateServer** | chat-gate | 8080 | HTTP 网关入口 |
| **StatusServer** | chat-status | 50052 | 服务发现、token 签发 |
| **ChatServer** | chat-server1 | 8090 / 50055 | 长连接 + gRPC |
| **ChatServer2** | chat-server2 | 8091 / 50056 | 第二个实例（演示跨服） |
| **ResourceServer** | chat-resource | 9090 | 文件上传下载 |

---

## 设计要点

### 1. 多阶段构建

```dockerfile
FROM ubuntu:22.04 AS builder    # 装全部编译依赖，体积大
...
FROM ubuntu:22.04 AS runtime    # 只装运行时动态库，体积小
COPY --from=builder /build/build/ /app/build/
```

**为什么**：编译工具（gcc、cmake、头文件）在运行阶段完全用不到，
分阶段能让最终镜像小很多（从约 2GB 降到约 400MB）。

### 2. 一个镜像跑多个服务

所有业务服务共用 `chat-server:latest` 镜像，通过 `SERVICE_NAME` 环境变量区分：

```yaml
status:
  image: chat-server:latest
  environment:
    SERVICE_NAME: StatusServer
```

**为什么**：6 个服务如果各建一个镜像，要重复构建 6 次，浪费时间和磁盘。
它们本来就共用同一套代码和依赖，只是启动不同的可执行文件。

### 3. 启动顺序控制

```yaml
depends_on:
  mysql:
    condition: service_healthy    # 等 MySQL 健康检查通过
  redis:
    condition: service_healthy
```

MySQL 首次启动要初始化，**光 `depends_on` 不够**（那只保证容器启动，不保证服务就绪），
必须配 `healthcheck`。

`entrypoint.sh` 里还有一层保险——用 `nc` 探测端口，最多等 60 秒。

### 4. 配置分离

`docker/config/*.ini` 是 Docker 专用配置，与源码里的 `config.ini` 区别在于：

| 源码配置 | Docker 配置 |
|---|---|
| `Host = 127.0.0.1` | `Host = mysql`（容器名） |
| `Host = 192.168.88.130` | `Host = chatserver1`（服务名） |

**Docker Compose 会自动把服务名解析成容器 IP**，所以配置里直接写服务名即可。

配置通过 volume 只读挂载，改配置不用重新构建镜像：
```yaml
volumes:
  - ./config/StatusServer.ini:/app/config/config.ini:ro
```

---

## 常见问题

### Q: 构建时提示找不到 `*.pb.h`

仓库未包含 protobuf 生成文件（`.gitignore` 里排除了），Dockerfile 会在构建时
用 `protoc` 从 `message.proto` 自动生成：

```dockerfile
RUN find . -name "*.proto" -exec sh -c '...' sh {} +
```

如果本地构建也想生成：
```bash
find . -name "*.proto" -exec protoc -I. --cpp_out=. {} \;
```

### Q: 服务启动后连不上 MySQL

检查 MySQL 是否真的就绪：
```bash
docker compose exec mysql mysqladmin ping -uroot -proot123456
docker compose logs mysql | tail -20
```

### Q: 想改数据库密码

改两处：
1. `docker-compose.yml` 里 mysql 的 `MYSQL_PASSWORD`
2. `docker/config/*.ini` 里的 `Passwd`

然后 `docker compose down -v && docker compose up -d` 重建。

### Q: 只想启动 MySQL 和 Redis 做本地调试

```bash
docker compose up -d mysql redis
```

然后用本地编译的程序连 `127.0.0.1:3306` / `127.0.0.1:6379`。

---

## 镜像体积优化（可选）

如果觉得镜像太大，可以进一步优化：

1. **用 Alpine 基础镜像**（需要自己编译 Boost/gRPC，较麻烦）
2. **静态链接**，运行阶段用 `scratch` 或 `alpine`（约 50MB）
3. **只保留用到的动态库**，用 `ldd` 查依赖后逐个拷贝

本项目为了可读性没有做极致优化。
