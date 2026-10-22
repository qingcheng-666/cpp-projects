// VarifyServer.cpp : 验证码服务的入口（gRPC）
//
// 职责：给 GateServer 的 /get_varifycode 提供验证码
//   1. 校验邮箱格式
//   2. 同一邮箱 60 秒内只发一次（Redis 里放一个带 TTL 的标记）
//   3. 生成 6 位数字码，写 Redis：SETEX code_<email> 300 <code>
//   4. 发邮件（当前实现只打日志，可替换成 SMTP）
//
// 注意：GateServer 注册/找回密码时读的就是 code_<email>，两边 key 必须一致。

#include <iostream>
#include <memory>
#include <string>

#include <boost/asio.hpp>
#include <grpcpp/grpcpp.h>

#include "ConfigMgr.h"
#include "RedisMgr.h"
#include "const.h"
#include "VarifyServiceImpl.h"

void RunServer() {
	auto& cfg = ConfigMgr::Inst();

	std::string server_address(cfg["VarifyServer"]["Host"] + ":" + cfg["VarifyServer"]["Port"]);

	VarifyServiceImpl service;

	grpc::ServerBuilder builder;
	builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
	builder.RegisterService(&service);

	std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
	std::cout << "[VarifyServer] listening on " << server_address << std::endl;

	// 提前初始化 Redis 连接池，避免第一个请求进来时才建连
	RedisMgr::GetInstance();

	// 捕获 Ctrl+C / SIGTERM，优雅退出
	boost::asio::io_context io_context;
	boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
	signals.async_wait([&server, &io_context](const boost::system::error_code& error, int) {
		if (!error) {
			std::cout << "[VarifyServer] shutting down..." << std::endl;
			server->Shutdown();
			io_context.stop();
		}
		});

	io_context.run();
}

int main(int argc, char** argv) {
	try {
		RunServer();
	}
	catch (std::exception& e) {
		std::cerr << "[VarifyServer] exception: " << e.what() << std::endl;
		return 1;
	}
	return 0;
}
