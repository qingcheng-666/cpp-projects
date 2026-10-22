#include "VarifyServiceImpl.h"

#include <iostream>
#include <random>
#include <string>

#include "const.h"
#include "RedisMgr.h"

// 生成 6 位数字验证码
std::string VarifyServiceImpl::generateCode(int length) {
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_int_distribution<int> dist(0, 9);

	std::string code;
	code.reserve(length);
	for (int i = 0; i < length; ++i) {
		code.push_back(static_cast<char>('0' + dist(gen)));
	}
	return code;
}

// 简单校验邮箱格式，避免把任意字符串当 Redis key 写进去
bool VarifyServiceImpl::isValidEmail(const std::string& email) {
	auto at = email.find('@');
	if (at == std::string::npos || at == 0 || at + 1 >= email.size()) {
		return false;
	}
	auto dot = email.find('.', at);
	return dot != std::string::npos && dot + 1 < email.size();
}

// 真正发邮件的地方。教学项目没有接 SMTP，先打到日志里，联调时直接看服务端输出；
// 生产环境把这里换成 SMTP / 云厂商邮件 API，周围逻辑不用改。
void VarifyServiceImpl::sendMail(const std::string& email, const std::string& code) {
	std::cout << "[VarifyServer] code generated: email = " << email
		<< ", code = " << code
		<< ", ttl = " << VARIFY_CODE_TTL << "s" << std::endl;
}

Status VarifyServiceImpl::GetVarifyCode(ServerContext* context, const GetVarifyReq* request,
	GetVarifyRsp* reply)
{
	const std::string email = request->email();
	reply->set_email(email);

	if (!isValidEmail(email)) {
		std::cout << "[VarifyServer] invalid email: " << email << std::endl;
		reply->set_error(ErrorCodes::Error_Json);
		return Status::OK;
	}

	// 防刷：同一邮箱在 VARIFY_SEND_INTERVAL 秒内只允许发一次
	const std::string throttle_key = VARIFY_THROTTLE_PREFIX + email;
	if (RedisMgr::GetInstance()->ExistsKey(throttle_key)) {
		std::cout << "[VarifyServer] too frequent: " << email << std::endl;
		reply->set_error(ErrorCodes::VarifyTooFrequent);
		return Status::OK;
	}

	const std::string code = generateCode(6);

	// 关键：这里写的 key 必须和 GateServer 校验时读的 key 一致（CODEPREFIX + email），
	// 并且必须带 TTL，否则验证码永久有效
	if (!RedisMgr::GetInstance()->SetWithExpire(CODEPREFIX + email, code, VARIFY_CODE_TTL)) {
		std::cout << "[VarifyServer] write redis failed: " << email << std::endl;
		reply->set_error(ErrorCodes::RPCFailed);
		return Status::OK;
	}

	// 记录发送时间，用于上面的防刷
	RedisMgr::GetInstance()->SetWithExpire(throttle_key, "1", VARIFY_SEND_INTERVAL);

	sendMail(email, code);

	reply->set_code(code);
	reply->set_error(ErrorCodes::Success);
	return Status::OK;
}
