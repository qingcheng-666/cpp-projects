#include "LogicWorker.h"
#include <json/json.h>
#include <json/value.h>
#include <json/reader.h>
#include "FileSystem.h"
#include "CSession.h"
#include "LogicSystem.h"
#include "ConfigMgr.h"
#include "RedisMgr.h"
#include "MysqlMgr.h"

// 文件名安全校验：只允许纯文件名，禁止路径分隔符与上跳，防止路径穿越
// 文件名来自客户端 JSON，若不校验，name="../../etc/xx" 会写到文件目录之外
static bool IsSafeFileName(const std::string& name) {
	if (name.empty() || name == "." || name == "..") {
		return false;
	}
	if (name.size() > 255) {
		return false;
	}
	for (char c : name) {
		if (c == '/' || c == '\\' || static_cast<unsigned char>(c) < 0x20) {
			return false;
		}
	}
	return true;
}

LogicWorker::LogicWorker():_b_stop(false)
{
	RegisterCallBacks();

	_work_thread = std::thread([this]() {
		while (!_b_stop) {
			std::unique_lock<std::mutex> lock(_mtx);
			_cv.wait(lock, [this]() {
				if(_b_stop) {
					return true;
				}

				if (_task_que.empty()) {
					return false;
				}

				return true;

			});

			if (_b_stop) {
				return;
			}

			auto task = _task_que.front();
			task_callback(task);
			_task_que.pop();
		}
	});

}

LogicWorker::~LogicWorker()
{
	_b_stop = true;
	_cv.notify_one();
	_work_thread.join();
}

void LogicWorker::PostTask(std::shared_ptr<LogicNode> task)
{
	std::lock_guard<std::mutex> lock(_mtx);
	_task_que.push(task);
	_cv.notify_one();
}

void LogicWorker::RegisterCallBacks()
{
	_fun_callbacks[ID_TEST_MSG_REQ] = [this](shared_ptr<CSession> session, const short& msg_id,
		const string& msg_data) {
			Json::Reader reader;
			Json::Value root;
			reader.parse(msg_data, root);
			auto data = root["data"].asString();
			std::cout << "recv test data is  " << data << std::endl;

			Json::Value  rtvalue;
			Defer defer([this, &rtvalue, session]() {
				std::string return_str = rtvalue.toStyledString();
				session->Send(return_str, ID_TEST_MSG_RSP);
				});

			rtvalue["error"] = ErrorCodes::Success;
			rtvalue["data"] = data;
	};

	_fun_callbacks[ID_UPLOAD_FILE_REQ] = [this](shared_ptr<CSession> session, const short& msg_id,
		const string& msg_data) {
			Json::Reader reader;
			Json::Value root;
			reader.parse(msg_data, root);
			auto md5 = root["md5"].asString();
			auto seq = root["seq"].asInt();
			auto name = root["name"].asString();
			if (!IsSafeFileName(name)) {
				Json::Value err_val;
				err_val["error"] = ErrorCodes::FileNotExists;
				session->Send(err_val.toStyledString(), msg_id + 1);
				return;
			}
			auto total_size = root["total_size"].asInt();
			auto trans_size = root["trans_size"].asInt();
			auto last = root["last"].asInt();
			auto file_data = root["data"].asString();
			auto file_path = ConfigMgr::Inst().GetFileOutPath();
			auto uid = root["uid"].asInt();
			//杞寲涓哄瓧绗︿覆
			auto uid_str = std::to_string(uid);
			auto file_path_str = (file_path / uid_str/ name).string();
			Json::Value  rtvalue;

			auto callback = [=](const Json::Value& result) {

				// 鍦ㄥ紓姝ヤ换鍔″畬鎴愬悗璋冪敤
				Json::Value rtvalue = result;
				rtvalue["error"] = ErrorCodes::Success;
				rtvalue["total_size"] = total_size;
				rtvalue["seq"] = seq;
				rtvalue["name"] = name;
				rtvalue["trans_size"] = trans_size;
				rtvalue["last"] = last;
				rtvalue["md5"] = md5;
				rtvalue["uid"] = uid;
				std::string return_str = rtvalue.toStyledString();
				session->Send(return_str, ID_UPLOAD_FILE_RSP);
			};
			
			// 浣跨敤 std::hash 瀵瑰瓧绗︿覆杩涜鍝堝笇
			std::hash<std::string> hash_fn;
			size_t hash_value = hash_fn(name); // 鐢熸垚鍝堝笇鍊�
			int index = hash_value % FILE_WORKER_COUNT;
			std::cout << "Hash value: " << hash_value << std::endl;

			//绗竴涓寘
			if (seq == 1) {
				//鏋勯�犳暟鎹瓨鍌�
				auto file_info = std::make_shared<FileInfo>();
				file_info->_file_path_str = file_path_str;
				file_info->_name = name;
				file_info->_seq = seq;
				file_info->_total_size = total_size;
				file_info->_trans_size = trans_size;
				//todo... 鍚庢湡鏀逛负redis,浠ュ強mysql 鎸佷箙鍖栧瓨鍌�
				bool success = RedisMgr::GetInstance()->SetFileInfo(md5, file_info);
				if (!success) {
					rtvalue["error"] = ErrorCodes::FileSaveRedisFailed;
					std::string return_str = rtvalue.toStyledString();
					session->Send(return_str, ID_UPLOAD_HEAD_ICON_RSP);
					return;
				}
			}
			else {
				auto file_info = RedisMgr::GetInstance()->GetFileInfo(md5);
				if (file_info == nullptr) {
					rtvalue["error"] = ErrorCodes::FileNotExists;
					std::string return_str = rtvalue.toStyledString();
					session->Send(return_str, ID_UPLOAD_FILE_RSP);
					return;
				}
				file_info->_seq = seq;
				file_info->_trans_size = trans_size;
				bool success = RedisMgr::GetInstance()->SetFileInfo(md5, file_info);
				if (!success) {
					rtvalue["error"] = ErrorCodes::FileSaveRedisFailed;
					std::string return_str = rtvalue.toStyledString();
					session->Send(return_str, ID_UPLOAD_FILE_RSP);
					return;
				}
			}


			FileSystem::GetInstance()->PostMsgToQue(
				std::make_shared<FileTask>(session, ID_UPLOAD_FILE_REQ, uid, file_path_str, name, seq, total_size,
					trans_size, last, file_data, callback),
				index
			);
	};



	_fun_callbacks[ID_SYNC_FILE_REQ] = [this](shared_ptr<CSession> session, const short& msg_id,
		const string& msg_data) {

			Json::Reader reader;
			Json::Value root;
			reader.parse(msg_data, root);

			Json::Value  rtvalue;
			Defer defer([this, &rtvalue, session]() {
				std::string return_str = rtvalue.toStyledString();
				session->Send(return_str, ID_SYNC_FILE_RSP);
				});

			auto md5 = root["md5"].asString();

			auto file = LogicSystem::GetInstance()->GetFileInfo(md5);
			if (file == nullptr) {
				rtvalue["error"] = ErrorCodes::FileNotExists;
				return;
			}

			rtvalue["error"] = ErrorCodes::Success;
			rtvalue["total_size"] = std::to_string(file->_total_size);
			rtvalue["seq"] = file->_seq;
			rtvalue["name"] = file->_name;
			rtvalue["trans_size"] = std::to_string(file->_trans_size);
			rtvalue["md5"] = md5;

	};


	_fun_callbacks[ID_UPLOAD_HEAD_ICON_REQ] = [this](shared_ptr<CSession> session, const short& msg_id,
		const string& msg_data) {
			Json::Reader reader;
			Json::Value root;
			reader.parse(msg_data, root);
			auto md5 = root["md5"].asString();
			auto seq = root["seq"].asInt();
			auto name = root["name"].asString();
			if (!IsSafeFileName(name)) {
				Json::Value err_val;
				err_val["error"] = ErrorCodes::FileNotExists;
				session->Send(err_val.toStyledString(), msg_id + 1);
				return;
			}
			auto total_size = root["total_size"].asInt();
			auto trans_size = root["trans_size"].asInt();
			auto last = root["last"].asInt();
			auto file_data = root["data"].asString();
			auto uid = root["uid"].asInt();
			auto token = root["token"].asString();
			auto last_seq = root["last_seq"].asInt();
			//杞寲涓哄瓧绗︿覆
			auto uid_str = std::to_string(uid);

			auto file_path = ConfigMgr::Inst().GetFileOutPath();
			auto file_path_str = (file_path / uid_str / name).string();
			Json::Value  rtvalue;
			auto callback = [=](const Json::Value& result) {

				// 鍦ㄥ紓姝ヤ换鍔″畬鎴愬悗璋冪敤
				Json::Value rtvalue = result;
				rtvalue["total_size"] = total_size;
				rtvalue["seq"] = seq;
				rtvalue["name"] = name;
				rtvalue["trans_size"] = trans_size;
				rtvalue["last"] = last;
				rtvalue["md5"] = md5;
				rtvalue["uid"] = uid;
				rtvalue["last_seq"] = last_seq;
				std::string return_str = rtvalue.toStyledString();
				session->Send(return_str, ID_UPLOAD_HEAD_ICON_RSP);
			};

			//绗竴涓寘鏍￠獙涓�涓媡oken鏄惁鍚堢悊
			if (seq == 1) {
				//浠巖edis鑾峰彇鐢ㄦ埛token鏄惁姝ｇ‘
				std::string uid_str = std::to_string(uid);
				std::string token_key = USERTOKENPREFIX + uid_str;
				std::string token_value = "";
				bool success = RedisMgr::GetInstance()->Get(token_key, token_value);
				if (!success) {
					rtvalue["error"] = ErrorCodes::UidInvalid;
					std::string return_str = rtvalue.toStyledString();
					session->Send(return_str, ID_UPLOAD_HEAD_ICON_RSP);
					return;
				}

				if (token_value != token) {
					rtvalue["error"] = ErrorCodes::TokenInvalid;
					std::string return_str = rtvalue.toStyledString();
					session->Send(return_str, ID_UPLOAD_HEAD_ICON_RSP);
					return;
				}
			}

			// 浣跨敤 std::hash 瀵瑰瓧绗︿覆杩涜鍝堝笇
			std::hash<std::string> hash_fn;
			size_t hash_value = hash_fn(name); // 鐢熸垚鍝堝笇鍊�
			int index = hash_value % FILE_WORKER_COUNT;
			std::cout << "Hash value: " << hash_value << std::endl;

			//绗竴涓寘
			if (seq == 1) {
				//鏋勯�犳暟鎹瓨鍌�
				auto file_info = std::make_shared<FileInfo>();
				file_info->_file_path_str = file_path_str;
				file_info->_name = name;
				file_info->_seq = seq;
				file_info->_total_size = total_size;
				file_info->_trans_size = trans_size;
				//LogicSystem::GetInstance()->AddMD5File(md5, file_info);
				//鏀逛负鐢╮edis瀛樺偍
				bool success = RedisMgr::GetInstance()->SetFileInfo(name, file_info);
				if (!success) {
					rtvalue["error"] = ErrorCodes::FileSaveRedisFailed;
					std::string return_str = rtvalue.toStyledString();
					session->Send(return_str, ID_UPLOAD_HEAD_ICON_RSP);
					return;
				}
			}
			else {
				//auto file_info = LogicSystem::GetInstance()->GetFileInfo(md5);
				//鏀逛负浠巖edis涓姞杞�
				auto file_info = RedisMgr::GetInstance()->GetFileInfo(name);
				if (file_info == nullptr) {
					rtvalue["error"] = ErrorCodes::FileNotExists;
					std::string return_str = rtvalue.toStyledString();
					session->Send(return_str, ID_UPLOAD_HEAD_ICON_RSP);
					return;
				}
				file_info->_seq = seq;
				file_info->_trans_size = trans_size;
				bool success = RedisMgr::GetInstance()->SetFileInfo(name, file_info);
				if (!success) {
					rtvalue["error"] = ErrorCodes::FileSaveRedisFailed;
					std::string return_str = rtvalue.toStyledString();
					session->Send(return_str, ID_UPLOAD_HEAD_ICON_RSP);
					return;
				}
			}


			FileSystem::GetInstance()->PostMsgToQue(
				std::make_shared<FileTask>(session, ID_UPLOAD_HEAD_ICON_REQ, uid, file_path_str, name, seq, total_size,
					trans_size, last, file_data, callback),
				index
			);

	};

	_fun_callbacks[ID_DOWN_LOAD_FILE_REQ] = [this](shared_ptr<CSession> session, const short& msg_id,
		const string& msg_data) {
			Json::Reader reader;
			Json::Value root;
			reader.parse(msg_data, root);
			auto seq = root["seq"].asInt();
			auto name = root["name"].asString();
			if (!IsSafeFileName(name)) {
				Json::Value err_val;
				err_val["error"] = ErrorCodes::FileNotExists;
				session->Send(err_val.toStyledString(), msg_id + 1);
				return;
			}
			auto uid = root["uid"].asInt();
			auto token = root["token"].asString();
			auto client_path = root["client_path"].asString();
			auto req_type = root["req_type"].asString();
			//杞寲涓哄瓧绗︿覆
			auto uid_str = std::to_string(uid);

			auto file_path = ConfigMgr::Inst().GetFileOutPath();
			auto file_path_str = (file_path / uid_str / name).string();
			Json::Value  rtvalue;
			auto callback = [=](const Json::Value& result) {

				// 鍦ㄥ紓姝ヤ换鍔″畬鎴愬悗璋冪敤
				Json::Value rtvalue = result;
				rtvalue["client_path"] = client_path;
				rtvalue["name"] = name;
				rtvalue["req_type"] = req_type;
				std::string return_str = rtvalue.toStyledString();
				session->Send(return_str, ID_DOWN_LOAD_FILE_RSP);
			};

			//绗竴涓寘鏍￠獙涓�涓媡oken鏄惁鍚堢悊
			if (seq == 1) {
				//浠巖edis鑾峰彇鐢ㄦ埛token鏄惁姝ｇ‘
				std::string uid_str = std::to_string(uid);
				std::string token_key = USERTOKENPREFIX + uid_str;
				std::string token_value = "";
				bool success = RedisMgr::GetInstance()->Get(token_key, token_value);
				if (!success) {
					rtvalue["error"] = ErrorCodes::UidInvalid;
					std::string return_str = rtvalue.toStyledString();
					session->Send(return_str, ID_DOWN_LOAD_FILE_RSP);
					return;
				}

				if (token_value != token) {
					rtvalue["error"] = ErrorCodes::TokenInvalid;
					std::string return_str = rtvalue.toStyledString();
					session->Send(return_str, ID_DOWN_LOAD_FILE_RSP);
					return;
				}
			}

			// 浣跨敤 std::hash 瀵瑰瓧绗︿覆杩涜鍝堝笇
			std::hash<std::string> hash_fn;
			size_t hash_value = hash_fn(name); // 鐢熸垚鍝堝笇鍊�
			int index = hash_value % FILE_WORKER_COUNT;
			std::cout << "Hash value: " << hash_value << std::endl;

			FileSystem::GetInstance()->PostDownloadTaskToQue(
				std::make_shared<DownloadTask>(session, uid, name, seq, file_path_str, callback),
				index
			);

	};

	_fun_callbacks[ID_IMG_CHAT_UPLOAD_REQ] = [this](shared_ptr<CSession> session, const short& msg_id,
		const string& msg_data) {
			Json::Reader reader;
			Json::Value root;
			reader.parse(msg_data, root);
			auto md5 = root["md5"].asString();
			auto seq = root["seq"].asInt();
			auto name = root["name"].asString();
			if (!IsSafeFileName(name)) {
				Json::Value err_val;
				err_val["error"] = ErrorCodes::FileNotExists;
				session->Send(err_val.toStyledString(), msg_id + 1);
				return;
			}
			auto total_size_str = root["total_size"].asString();
			auto trans_size_str = root["trans_size"].asString();
			int64_t total_size = std::stoll(total_size_str);
			int64_t trans_size = std::stoll(trans_size_str);
			auto last = root["last"].asInt();
			auto file_data = root["data"].asString();
			auto file_path = ConfigMgr::Inst().GetFileOutPath();
			auto uid = root["uid"].asInt();
			auto sender = root["sender"].asInt();
			auto receiver = root["receiver"].asInt();
			auto message_id = root["message_id"].asInt();
			//杞寲涓哄瓧绗︿覆
			auto uid_str = std::to_string(uid);
			auto file_path_str = (file_path / uid_str / name).string();
			Json::Value  rtvalue;

			auto callback = [=](const Json::Value& result) {

				// 鍦ㄥ紓姝ヤ换鍔″畬鎴愬悗璋冪敤
				Json::Value rtvalue = result;
				rtvalue["error"] = ErrorCodes::Success;
				rtvalue["total_size"] = std::to_string(total_size);
				rtvalue["seq"] = seq;
				rtvalue["name"] = name;
				rtvalue["trans_size"] = std::to_string(trans_size);
				rtvalue["last"] = last;
				rtvalue["md5"] = md5;
				rtvalue["uid"] = uid;
				rtvalue["sender"] = sender;
				rtvalue["receiver"] = receiver;
				std::string return_str = rtvalue.toStyledString();
				session->Send(return_str, ID_IMG_CHAT_UPLOAD_RSP);
			};

			// 浣跨敤 std::hash 瀵瑰瓧绗︿覆杩涜鍝堝笇
			std::hash<std::string> hash_fn;
			size_t hash_value = hash_fn(name); // 鐢熸垚鍝堝笇鍊�
			int index = hash_value % FILE_WORKER_COUNT;
			std::cout << "Hash value: " << hash_value << std::endl;

			//绗竴涓寘
			if (seq == 1) {
				//鏋勯�犳暟鎹瓨鍌�
				auto file_info = std::make_shared<FileInfo>();
				file_info->_file_path_str = file_path_str;
				file_info->_name = name;
				file_info->_seq = seq;
				file_info->_total_size = total_size;
				file_info->_trans_size = trans_size;
				bool success = RedisMgr::GetInstance()->SetFileInfo(name, file_info);
				if (!success) {
					rtvalue["error"] = ErrorCodes::FileSaveRedisFailed;
					std::string return_str = rtvalue.toStyledString();
					session->Send(return_str, ID_IMG_CHAT_UPLOAD_RSP);
					return;
				}
			}
			else {
				auto file_info = RedisMgr::GetInstance()->GetFileInfo(name);
				if (file_info == nullptr) {
					rtvalue["error"] = ErrorCodes::FileNotExists;
					std::string return_str = rtvalue.toStyledString();
					session->Send(return_str, ID_IMG_CHAT_UPLOAD_RSP);
					return;
				}
				file_info->_seq++;
				file_info->_trans_size = trans_size;
				bool success = RedisMgr::GetInstance()->SetFileInfo(name, file_info);
				if (!success) {
					rtvalue["error"] = ErrorCodes::FileSaveRedisFailed;
					std::string return_str = rtvalue.toStyledString();
					session->Send(return_str, ID_IMG_CHAT_UPLOAD_RSP);
					return;
				}
			}


			FileSystem::GetInstance()->PostMsgToQue(
				std::make_shared<FileTask>(session, ID_IMG_CHAT_UPLOAD_REQ, uid, file_path_str, name, seq, total_size,
					trans_size, last, file_data, callback, message_id,sender,receiver),
				index
			);
	};	


	_fun_callbacks[ID_FILE_INFO_SYNC_REQ] = [this](shared_ptr<CSession> session, const short& msg_id,
		const string& msg_data) {
			Json::Reader reader;
			Json::Value root;
			reader.parse(msg_data, root);
			auto md5 = root["md5"].asString();
			auto seq = root["seq"].asInt();
			auto name = root["name"].asString();
			if (!IsSafeFileName(name)) {
				Json::Value err_val;
				err_val["error"] = ErrorCodes::FileNotExists;
				session->Send(err_val.toStyledString(), msg_id + 1);
				return;
			}
			auto total_size_str = root["total_size"].asString();
			auto trans_size_str = root["trans_size"].asString();
			auto total_size = std::stoll(total_size_str);
			auto trans_size = std::stoll(trans_size_str);
			auto last = root["last"].asInt();
			auto file_data = root["data"].asString();
			auto file_path = ConfigMgr::Inst().GetFileOutPath();
			auto uid = root["uid"].asInt();
			auto message_id = root["message_id"].asInt();
			auto sender = root["sender"].asInt();
			auto receiver = root["receiver"].asInt();
			//杞寲涓哄瓧绗︿覆
			auto uid_str = std::to_string(uid);
			auto file_path_str = (file_path / uid_str / name).string();
			Json::Value  rtvalue;

			auto callback = [=](const Json::Value& result) {

				// 鍦ㄥ紓姝ヤ换鍔″畬鎴愬悗璋冪敤
				Json::Value rtvalue = result;
				rtvalue["error"] = ErrorCodes::Success;		
				rtvalue["seq"] = seq;
				rtvalue["name"] = name;
				rtvalue["last"] = last;
				rtvalue["md5"] = md5;
				rtvalue["uid"] = uid;
				rtvalue["sender"] = sender;
				rtvalue["receiver"] = receiver;
				std::string return_str = rtvalue.toStyledString();
				session->Send(return_str, ID_FILE_INFO_SYNC_RSP);
			};

			// 浣跨敤 std::hash 瀵瑰瓧绗︿覆杩涜鍝堝笇
			std::hash<std::string> hash_fn;
			size_t hash_value = hash_fn(name); // 鐢熸垚鍝堝笇鍊�
			int index = hash_value % FILE_WORKER_COUNT;
			std::cout << "Hash value: " << hash_value << std::endl;

			//绗竴涓寘
			if (seq == 1) {
				//鏋勯�犳暟鎹瓨鍌�
				auto file_info = std::make_shared<FileInfo>();
				file_info->_file_path_str = file_path_str;
				file_info->_name = name;
				file_info->_seq = seq;
				file_info->_total_size = total_size;
				file_info->_trans_size = trans_size;
				bool success = RedisMgr::GetInstance()->SetFileInfo(name, file_info);
				if (!success) {
					rtvalue["error"] = ErrorCodes::FileSaveRedisFailed;
					std::string return_str = rtvalue.toStyledString();
					session->Send(return_str, ID_FILE_INFO_SYNC_RSP);
					return;
				}
			}
			else {
				auto file_info = RedisMgr::GetInstance()->GetFileInfo(name);
				if (file_info == nullptr) {
					rtvalue["error"] = ErrorCodes::FileNotExists;
					std::string return_str = rtvalue.toStyledString();
					session->Send(return_str, ID_FILE_INFO_SYNC_RSP);
					return;
				}
				file_info->_seq = seq;
				file_info->_trans_size = trans_size;
				bool success = RedisMgr::GetInstance()->SetFileInfo(name, file_info);
				if (!success) {
					rtvalue["error"] = ErrorCodes::FileSaveRedisFailed;
					std::string return_str = rtvalue.toStyledString();
					session->Send(return_str, ID_FILE_INFO_SYNC_RSP);
					return;
				}
			}


			FileSystem::GetInstance()->PostMsgToQue(
				std::make_shared<FileTask>(session, ID_FILE_INFO_SYNC_REQ, uid, file_path_str, name, seq, total_size,
					trans_size, last, file_data, callback, message_id,sender,receiver),
				index
			);
	};



	_fun_callbacks[ID_IMG_CHAT_CONTINUE_UPLOAD_REQ] = [this](shared_ptr<CSession> session, const short& msg_id,
		const string& msg_data) {
			Json::Reader reader;
			Json::Value root;
			reader.parse(msg_data, root);
			auto md5 = root["md5"].asString();
			auto seq = root["seq"].asInt();
			auto name = root["name"].asString();
			if (!IsSafeFileName(name)) {
				Json::Value err_val;
				err_val["error"] = ErrorCodes::FileNotExists;
				session->Send(err_val.toStyledString(), msg_id + 1);
				return;
			}
			auto total_size = root["total_size"].asInt();
			auto trans_size = root["trans_size"].asInt();
			auto last = root["last"].asInt();
			auto file_data = root["data"].asString();
			auto file_path = ConfigMgr::Inst().GetFileOutPath();
			auto uid = root["uid"].asInt();
			auto message_id = root["message_id"].asInt();
			auto sender = root["sender"].asInt();
			auto receiver = root["receiver"].asInt();
			//杞寲涓哄瓧绗︿覆
			auto uid_str = std::to_string(uid);
			auto file_path_str = (file_path / uid_str / name).string();
			Json::Value  rtvalue;

			auto callback = [=](const Json::Value& result) {

				// 鍦ㄥ紓姝ヤ换鍔″畬鎴愬悗璋冪敤
				Json::Value rtvalue = result;
				rtvalue["error"] = ErrorCodes::Success;
				rtvalue["total_size"] = total_size;
				rtvalue["seq"] = seq;
				rtvalue["name"] = name;
				rtvalue["trans_size"] = trans_size;
				rtvalue["last"] = last;
				rtvalue["md5"] = md5;
				rtvalue["uid"] = uid;
				rtvalue["sender"] = sender;
				rtvalue["receiver"] = receiver;
				std::string return_str = rtvalue.toStyledString();
				session->Send(return_str, ID_IMG_CHAT_CONTINUE_UPLOAD_RSP);
			};

			// 浣跨敤 std::hash 瀵瑰瓧绗︿覆杩涜鍝堝笇
			std::hash<std::string> hash_fn;
			size_t hash_value = hash_fn(name); // 鐢熸垚鍝堝笇鍊�
			int index = hash_value % FILE_WORKER_COUNT;
			std::cout << "Hash value: " << hash_value << std::endl;

			//绗竴涓寘
			if (seq == 1) {
				//鏋勯�犳暟鎹瓨鍌�
				auto file_info = std::make_shared<FileInfo>();
				file_info->_file_path_str = file_path_str;
				file_info->_name = name;
				file_info->_seq = seq;
				file_info->_total_size = total_size;
				file_info->_trans_size = trans_size;
				bool success = RedisMgr::GetInstance()->SetFileInfo(name, file_info);
				if (!success) {
					rtvalue["error"] = ErrorCodes::FileSaveRedisFailed;
					std::string return_str = rtvalue.toStyledString();
					session->Send(return_str, ID_IMG_CHAT_CONTINUE_UPLOAD_RSP);
					return;
				}
			}
			else {
				auto file_info = RedisMgr::GetInstance()->GetFileInfo(name);
				if (file_info == nullptr) {
					rtvalue["error"] = ErrorCodes::FileNotExists;
					std::string return_str = rtvalue.toStyledString();
					session->Send(return_str, ID_IMG_CHAT_CONTINUE_UPLOAD_RSP);
					return;
				}
				file_info->_seq = seq;
				file_info->_trans_size = trans_size;
				bool success = RedisMgr::GetInstance()->SetFileInfo(name, file_info);
				if (!success) {
					rtvalue["error"] = ErrorCodes::FileSaveRedisFailed;
					std::string return_str = rtvalue.toStyledString();
					session->Send(return_str, ID_IMG_CHAT_CONTINUE_UPLOAD_RSP);
					return;
				}
			}


			FileSystem::GetInstance()->PostMsgToQue(
				std::make_shared<FileTask>(session, ID_IMG_CHAT_CONTINUE_UPLOAD_REQ, uid, file_path_str, name, seq, total_size,
					trans_size, last, file_data, callback, message_id,sender,receiver),
				index
			);
	};

	_fun_callbacks[ID_IMG_CHAT_DOWN_INFO_SYNC_REQ] = [this](std::shared_ptr<CSession> session, const short& msg_id,
		const string& msg_data) {
			Json::Reader reader;
			Json::Value root;
			reader.parse(msg_data, root);
			auto message_id = root["message_id"].asInt();
			auto chat_msg = MysqlMgr::GetInstance()->GetChatMsgById(message_id);
			if (chat_msg == nullptr) {
				Json::Value rtvalue;
				rtvalue["error"] = ErrorCodes::MsgIdErr;
				return;
			}

			// 璧勬簮鏂囦欢璺緞
			auto file_dir = ConfigMgr::Inst().GetFileOutPath();
			//璇ユ秷鎭槸鎺ユ敹鏂瑰鎴风鍙戦�佽繃鏉ョ殑,鏈嶅姟鍣ㄥ皢璧勬簮瀛樺偍鍦ㄥ彂閫佹柟鐨勬枃浠跺す涓�
			auto uid_str = std::to_string(chat_msg->sender_id);
			auto file_path = (file_dir / uid_str / chat_msg->content);
			boost::uintmax_t file_size = boost::filesystem::file_size(file_path);

			// 鍦ㄥ紓姝ヤ换鍔″畬鎴愬悗璋冪敤
			Json::Value rtvalue ;
			rtvalue["error"] = ErrorCodes::Success;
			rtvalue["message_id"] = chat_msg->message_id;
			rtvalue["thread_id"] = chat_msg->thread_id;
			rtvalue["sender_id"] = chat_msg->sender_id;
			rtvalue["recv_id"] = chat_msg->recv_id;
			rtvalue["name"] = chat_msg->content;
			rtvalue["msg_type"] = chat_msg->msg_type;
			rtvalue["status"] = chat_msg->status;
			rtvalue["total_size"] = std::to_string(file_size);
 			std::string return_str = rtvalue.toStyledString();
			session->Send(return_str, ID_IMG_CHAT_DOWN_INFO_SYNC_RSP);
	};

	_fun_callbacks[ID_IMG_CHAT_DOWN_REQ] = [this](std::shared_ptr<CSession> session, const short& msg_req_id,
		const string& msg_data) {

			Json::Reader reader;
			Json::Value root;
			reader.parse(msg_data, root);

			auto seq = root["seq"].asInt();
			auto name = root["name"].asString();
			if (!IsSafeFileName(name)) {
				Json::Value err_val;
				err_val["error"] = ErrorCodes::FileNotExists;
				session->Send(err_val.toStyledString(), msg_req_id + 1);
				return;
			}
			auto total_size_str = root["total_size"].asString();
			auto trans_size_str = root["trans_size"].asString();
			auto file_path = ConfigMgr::Inst().GetFileOutPath();
			auto message_id = root["message_id"].asInt();
			auto sender = root["sender_id"].asInt();
			auto receiver = root["receiver_id"].asInt();
			auto token = root["token"].asString();
			auto uid = root["uid"].asInt();
			
			auto callback = [=](const Json::Value& result) {
				// 鍦ㄥ紓姝ヤ换鍔″畬鎴愬悗璋冪敤
				Json::Value rtvalue = result;
				rtvalue["error"] = ErrorCodes::Success;
				rtvalue["name"] = name;
				rtvalue["sender_id"] = sender;
				rtvalue["receiver_id"] = receiver;
				std::string return_str = rtvalue.toStyledString();
				session->Send(return_str, ID_IMG_CHAT_DOWN_RSP);
			};

			// 浣跨敤 std::hash 瀵瑰瓧绗︿覆杩涜鍝堝笇
			std::hash<std::string> hash_fn;
			size_t hash_value = hash_fn(name); // 鐢熸垚鍝堝笇鍊�
			int index = hash_value % DOWN_LOAD_WORKER_COUNT;
			std::cout << "Hash value: " << hash_value << std::endl;


			//绗竴涓寘鏍￠獙涓�涓媡oken鏄惁鍚堢悊
			if (seq == 1) {
				//浠巖edis鑾峰彇鐢ㄦ埛token鏄惁姝ｇ‘
				std::string uid_str = std::to_string(uid);
				std::string token_key = USERTOKENPREFIX + uid_str;
				std::string token_value = "";
				bool success = RedisMgr::GetInstance()->Get(token_key, token_value);
				Json::Value  rtvalue;
				if (!success) {
					rtvalue["error"] = ErrorCodes::UidInvalid;
					std::string return_str = rtvalue.toStyledString();
					session->Send(return_str, ID_IMG_CHAT_DOWN_RSP);
					return;
				}

				if (token_value != token) {
					rtvalue["error"] = ErrorCodes::TokenInvalid;
					std::string return_str = rtvalue.toStyledString();
					session->Send(return_str, ID_IMG_CHAT_DOWN_RSP);
					return;
				}
			}

			auto sender_str = std::to_string(sender);
			//杞寲涓哄瓧绗︿覆
			auto uid_str = std::to_string(uid);
			auto file_path_str = (file_path / sender_str / name).string();

		    auto down_load_task = std::make_shared<DownloadTask>(session, uid, name, seq, file_path_str, callback);

			FileSystem::GetInstance()->PostDownloadTaskToQue(down_load_task,index);
	};
	
}

void LogicWorker::task_callback(std::shared_ptr<LogicNode> task)
{
	cout << "recv_msg id  is " << task->_recvnode->_msg_id << endl;
	auto call_back_iter = _fun_callbacks.find(task->_recvnode->_msg_id);
	if (call_back_iter == _fun_callbacks.end()) {
		return;
	}
	call_back_iter->second(task->_session, task->_recvnode->_msg_id,
		std::string(task->_recvnode->_data, task->_recvnode->_cur_len));
}
