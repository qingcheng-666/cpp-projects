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

# mysqlcppconn 头文件路径：代码用 <jdbc/mysql_driver.h> / <jdbc/cppconn/*>，
# 但 Ubuntu 的 libmysqlcppconn-dev 把 mysql_driver.h、mysql_connection.h、cppconn/
# 平铺在 /usr/include 下，没有 jdbc/ 这一层。这里在构建目录里自动生成一个带 jdbc/
# 的 shim 目录（软链），让 <jdbc/...> 能被解析 —— 不再需要手工 sudo 建软链。
find_path(MYSQLCPPCONN_ROOT
    NAMES mysql_driver.h
    PATHS /usr/include /usr/local/include /usr/include/mysql-cppconn-8
    NO_DEFAULT_PATH)
if(NOT MYSQLCPPCONN_ROOT)
    find_path(MYSQLCPPCONN_ROOT NAMES mysql_driver.h)
endif()
if(MYSQLCPPCONN_ROOT)
    if(EXISTS "${MYSQLCPPCONN_ROOT}/jdbc/mysql_driver.h")
        list(APPEND THIRD_PARTY_INCLUDE_DIRS "${MYSQLCPPCONN_ROOT}")
        message(STATUS "mysqlcppconn: ${MYSQLCPPCONN_ROOT}（自带 jdbc/）")
    else()
        set(MYSQL_SHIM_DIR "${CMAKE_BINARY_DIR}/mysqlconn_shim")
        file(MAKE_DIRECTORY "${MYSQL_SHIM_DIR}/jdbc")
        foreach(_h mysql_driver.h mysql_connection.h mysql_resultset.h mysql_statement.h)
            if(EXISTS "${MYSQLCPPCONN_ROOT}/${_h}")
                file(CREATE_LINK "${MYSQLCPPCONN_ROOT}/${_h}"
                     "${MYSQL_SHIM_DIR}/jdbc/${_h}" SYMBOLIC)
            endif()
        endforeach()
        if(EXISTS "${MYSQLCPPCONN_ROOT}/cppconn")
            file(CREATE_LINK "${MYSQLCPPCONN_ROOT}/cppconn"
                 "${MYSQL_SHIM_DIR}/jdbc/cppconn" SYMBOLIC)
        endif()
        list(APPEND THIRD_PARTY_INCLUDE_DIRS "${MYSQL_SHIM_DIR}")
        message(STATUS "mysqlcppconn: ${MYSQLCPPCONN_ROOT}（已生成 jdbc/ shim）")
    endif()
else()
    message(WARNING "找不到 mysql_driver.h，mysql 相关服务可能编译失败")
endif()

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
    GateServer/AsioIOServicePool.cpp
    GateServer/ConfigMgr.cpp
    GateServer/CServer.cpp
    GateServer/GateServer.cpp
    GateServer/HttpConnection.cpp
    GateServer/LogicSystem.cpp
    GateServer/MysqlDao.cpp
    GateServer/MysqlMgr.cpp
    GateServer/RedisMgr.cpp
    GateServer/StatusGrpcClient.cpp
    GateServer/VerifyGrpcClient.cpp
    GateServer/message.grpc.pb.cc
    GateServer/message.pb.cc
)
target_include_directories(gate_server PRIVATE
    GateServer
    ${THIRD_PARTY_INCLUDE_DIRS}
    ${GRPC_INCLUDE_DIRS}
    ${PROTOBUF_INCLUDE_DIRS}
)
target_link_libraries(gate_server ${COMMON_LINK_LIBS})

# ============================================================
# ChatServer — asio TCP 聊天长连接（8990 + gRPC 50055）
# ============================================================
add_executable(chat_server
    ChatServer/AsioIOServicePool.cpp
    ChatServer/ChatGrpcClient.cpp
    ChatServer/ChatServer.cpp
    ChatServer/ChatServiceImpl.cpp
    ChatServer/ConfigMgr.cpp
    ChatServer/CServer.cpp
    ChatServer/CSession.cpp
    ChatServer/DistLock.cpp
    ChatServer/LogicSystem.cpp
    ChatServer/MsgNode.cpp
    ChatServer/MysqlDao.cpp
    ChatServer/MysqlMgr.cpp
    ChatServer/RedisMgr.cpp
    ChatServer/StatusGrpcClient.cpp
    ChatServer/UserMgr.cpp
    ChatServer/utils.cpp
    ChatServer/message.grpc.pb.cc
    ChatServer/message.pb.cc
)
target_include_directories(chat_server PRIVATE
    ChatServer
    ${THIRD_PARTY_INCLUDE_DIRS}
    ${GRPC_INCLUDE_DIRS}
    ${PROTOBUF_INCLUDE_DIRS}
)
target_link_libraries(chat_server ${COMMON_LINK_LIBS})

# ============================================================
# VarifyServer — gRPC 验证码服务（50051）
#   给 GateServer 的 /get_varifycode 用：生成 6 位码 -> SETEX code_<email> 300
# ============================================================
add_executable(varify_server
    VarifyServer/ConfigMgr.cpp
    VarifyServer/RedisMgr.cpp
    VarifyServer/VarifyServer.cpp
    VarifyServer/VarifyServiceImpl.cpp
    VarifyServer/message.grpc.pb.cc
    VarifyServer/message.pb.cc
)
target_include_directories(varify_server PRIVATE
    VarifyServer
    ${THIRD_PARTY_INCLUDE_DIRS}
    ${GRPC_INCLUDE_DIRS}
    ${PROTOBUF_INCLUDE_DIRS}
)
target_link_libraries(varify_server ${COMMON_LINK_LIBS})

# ============================================================
# StatusServer — gRPC 状态服务（分配 ChatServer + 发 token，50052）
# ============================================================
add_executable(status_server
    StatusServer/AsioIOServicePool.cpp
    StatusServer/ChatGrpcClient.cpp
    StatusServer/ConfigMgr.cpp
    StatusServer/DistLock.cpp
    StatusServer/MysqlDao.cpp
    StatusServer/MysqlMgr.cpp
    StatusServer/RedisMgr.cpp
    StatusServer/StatusServer.cpp
    StatusServer/StatusServiceImpl.cpp
    StatusServer/message.grpc.pb.cc
    StatusServer/message.pb.cc
)
target_include_directories(status_server PRIVATE
    StatusServer
    ${THIRD_PARTY_INCLUDE_DIRS}
    ${GRPC_INCLUDE_DIRS}
    ${PROTOBUF_INCLUDE_DIRS}
)
target_link_libraries(status_server ${COMMON_LINK_LIBS})

# ============================================================
# ResourceServer — 文件/图片传输（断点续传，端口见 config.ini）
# ============================================================
add_executable(resource_server
    ResourceServer/AsioIOServicePool.cpp
    ResourceServer/base64.cpp
    ResourceServer/ChatServerGrpcClient.cpp
    ResourceServer/ConfigMgr.cpp
    ResourceServer/CServer.cpp
    ResourceServer/CSession.cpp
    ResourceServer/DistLock.cpp
    ResourceServer/FileInfo.cpp
    ResourceServer/FileSystem.cpp
    ResourceServer/FileWorker.cpp
    ResourceServer/LogicSystem.cpp
    ResourceServer/LogicWorker.cpp
    ResourceServer/MsgNode.cpp
    ResourceServer/MysqlDao.cpp
    ResourceServer/MysqlMgr.cpp
    ResourceServer/RedisMgr.cpp
    ResourceServer/ResourceServer.cpp
    ResourceServer/UserMgr.cpp
    ResourceServer/message.grpc.pb.cc
    ResourceServer/message.pb.cc
)
target_include_directories(resource_server PRIVATE
    ResourceServer
    ${THIRD_PARTY_INCLUDE_DIRS}
    ${GRPC_INCLUDE_DIRS}
    ${PROTOBUF_INCLUDE_DIRS}
)
target_link_libraries(resource_server ${COMMON_LINK_LIBS})
