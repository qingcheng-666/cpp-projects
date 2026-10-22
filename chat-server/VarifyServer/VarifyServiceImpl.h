#pragma once
#include <string>

#include <grpcpp/grpcpp.h>
#include "message.grpc.pb.h"

using grpc::ServerContext;
using grpc::Status;

using message::GetVarifyReq;
using message::GetVarifyRsp;
using message::VarifyService;

// 验证码服务：GateServer 的 /get_varifycode 通过 gRPC 调用这里
class VarifyServiceImpl final : public VarifyService::Service {
public:
	VarifyServiceImpl() = default;

	Status GetVarifyCode(ServerContext* context, const GetVarifyReq* request,
		GetVarifyRsp* reply) override;

private:
	static std::string generateCode(int length);
	static bool isValidEmail(const std::string& email);
	void sendMail(const std::string& email, const std::string& code);
};
