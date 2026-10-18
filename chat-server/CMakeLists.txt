cmake_minimum_required(VERSION 3.16)
project(llfcchat CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ============================================================
# 依赖（Ubuntu 22.04 全部 apt 可装）：
#   sudo apt-get install -y libboost-all-dev libjsoncpp-dev libhiredis-dev \
#       libmysqlcppconn-dev libgrpc++-dev libprotobuf-dev \
#       protobuf-compiler protobuf-compiler-grpc
#
# 注意：message.pb.cc / message.grpc.pb.cc 需先用 Linux protoc 重新生成
#（Windows 版 .pb 与 apt 的 protobuf/grpc 版本可能不兼容），
# 见 tools/regen_proto.sh。生成后这些 .cc 就是普通源文件参与编译。
# ============================================================

find_package(PkgConfig REQUIRED)
pkg_check_modules(GRPC REQUIRED grpc++)
pkg_check_modules(PROTOBUF REQUIRED protobuf)
find_package(Boost REQUIRED COMPONENTS system filesystem)

# ---- 第三方头文件路径（若 apt 装完路径不同，改这里）----
# jsoncpp: 头文件在 /usr/include/jsoncpp/json/*.h
# hiredis: 头文件在 /usr/include/hiredis/hiredis.h
set(THIRD_PARTY_INCLUDE_DIRS
    /usr/include/jsoncpp
    /usr/include/hiredis
    ${CMAKE_SOURCE_DIR}/third_include
)

# mysqlcppconn 头文件路径：Windows 版代码用 <jdbc/mysql_driver.h>、
# <jdbc/cppconn/*>，但 Ubuntu 的 libmysqlcppconn-dev 布局不同。
# 装完后执行 dpkg -L libmysqlcppconn-dev | grep include 确认，
# 然后用 tools/fix_mysqlconn_include.sh 建软链让 jdbc/ 能解析。

set(COMMON_LINK_LIBS
    ${GRPC_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    Boost::system
    Boost::filesystem
    jsoncpp
    hiredis
    mysqlcppconn
    pthread
)

# ============================================================
# GateServer — boost.beast HTTP 登录/注册网关（端口 8080）
# ============================================================
add_executable(gate_server
    server/GateServer/AsioIOServicePool.cpp
    server/GateServer/ConfigMgr.cpp
    server/GateServer/CServer.cpp
    server/GateServer/GateServer.cpp
    server/GateServer/HttpConnection.cpp
    server/GateServer/LogicSystem.cpp
    server/GateServer/MysqlDao.cpp
    server/GateServer/MysqlMgr.cpp
    server/GateServer/RedisMgr.cpp
    server/GateServer/StatusGrpcClient.cpp
    server/GateServer/VerifyGrpcClient.cpp
    server/GateServer/message.grpc.pb.cc
    server/GateServer/message.pb.cc
)
target_include_directories(gate_server PRIVATE
    server/GateServer
    ${THIRD_PARTY_INCLUDE_DIRS}
    ${GRPC_INCLUDE_DIRS}
    ${PROTOBUF_INCLUDE_DIRS}
)
target_link_libraries(gate_server ${COMMON_LINK_LIBS})

# ============================================================
# ChatServer — asio TCP 聊天长连接（8990 + gRPC 50055）
# ============================================================
add_executable(chat_server
    server/ChatServer/AsioIOServicePool.cpp
    server/ChatServer/ChatGrpcClient.cpp
    server/ChatServer/ChatServer.cpp
    server/ChatServer/ChatServiceImpl.cpp
    server/ChatServer/ConfigMgr.cpp
    server/ChatServer/CServer.cpp
    server/ChatServer/CSession.cpp
    server/ChatServer/DistLock.cpp
    server/ChatServer/LogicSystem.cpp
    server/ChatServer/MsgNode.cpp
    server/ChatServer/MysqlDao.cpp
    server/ChatServer/MysqlMgr.cpp
    server/ChatServer/RedisMgr.cpp
    server/ChatServer/StatusGrpcClient.cpp
    server/ChatServer/UserMgr.cpp
    server/ChatServer/utils.cpp
    server/ChatServer/message.grpc.pb.cc
    server/ChatServer/message.pb.cc
)
target_include_directories(chat_server PRIVATE
    server/ChatServer
    ${THIRD_PARTY_INCLUDE_DIRS}
    ${GRPC_INCLUDE_DIRS}
    ${PROTOBUF_INCLUDE_DIRS}
)
target_link_libraries(chat_server ${COMMON_LINK_LIBS})

# ============================================================
# StatusServer — gRPC 状态服务（分配 ChatServer + 发 token，50052）
# ============================================================
add_executable(status_server
    server/StatusServer/AsioIOServicePool.cpp
    server/StatusServer/ChatGrpcClient.cpp
    server/StatusServer/ConfigMgr.cpp
    server/StatusServer/DistLock.cpp
    server/StatusServer/MysqlDao.cpp
    server/StatusServer/MysqlMgr.cpp
    server/StatusServer/RedisMgr.cpp
    server/StatusServer/StatusServer.cpp
    server/StatusServer/StatusServiceImpl.cpp
    server/StatusServer/message.grpc.pb.cc
    server/StatusServer/message.pb.cc
)
target_include_directories(status_server PRIVATE
    server/StatusServer
    ${THIRD_PARTY_INCLUDE_DIRS}
    ${GRPC_INCLUDE_DIRS}
    ${PROTOBUF_INCLUDE_DIRS}
)
target_link_libraries(status_server ${COMMON_LINK_LIBS})

# ============================================================
# ResourceServer — 文件/图片传输（断点续传，端口见 config.ini）
# ============================================================
add_executable(resource_server
    server/ResourceServer/AsioIOServicePool.cpp
    server/ResourceServer/base64.cpp
    server/ResourceServer/ChatServerGrpcClient.cpp
    server/ResourceServer/ConfigMgr.cpp
    server/ResourceServer/CServer.cpp
    server/ResourceServer/CSession.cpp
    server/ResourceServer/DistLock.cpp
    server/ResourceServer/FileInfo.cpp
    server/ResourceServer/FileSystem.cpp
    server/ResourceServer/FileWorker.cpp
    server/ResourceServer/LogicSystem.cpp
    server/ResourceServer/LogicWorker.cpp
    server/ResourceServer/MsgNode.cpp
    server/ResourceServer/MysqlDao.cpp
    server/ResourceServer/MysqlMgr.cpp
    server/ResourceServer/RedisMgr.cpp
    server/ResourceServer/ResourceServer.cpp
    server/ResourceServer/UserMgr.cpp
    server/ResourceServer/message.grpc.pb.cc
    server/ResourceServer/message.pb.cc
)
target_include_directories(resource_server PRIVATE
    server/ResourceServer
    ${THIRD_PARTY_INCLUDE_DIRS}
    ${GRPC_INCLUDE_DIRS}
    ${PROTOBUF_INCLUDE_DIRS}
)
target_link_libraries(resource_server ${COMMON_LINK_LIBS})
