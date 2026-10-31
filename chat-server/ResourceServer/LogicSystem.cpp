#include "LogicSystem.h"
#include "const.h"
#include "base64.h"
#include <fstream>
#include <boost/filesystem.hpp>
#include "ConfigMgr.h"
#include "FileSystem.h"
#include "RedisMgr.h"

using namespace std;

LogicSystem::LogicSystem(){
	for (int i = 0; i < LOGIC_WORKER_COUNT; i++) {
		_workers.push_back(std::make_shared<LogicWorker>());
	}
}

LogicSystem::~LogicSystem(){

}

void LogicSystem::PostMsgToQue(shared_ptr < LogicNode> msg, int index) {
	_workers[index]->PostTask(msg);
}


void LogicSystem::AddMD5File(std::string md5, std::shared_ptr<FileInfo> fileinfo) {
	std::lock_guard<std::mutex> lock(_file_mtx);
	_map_md5_files[md5] = fileinfo;
}

std::shared_ptr<FileInfo> LogicSystem::GetFileInfo(std::string md5) {
	// ① 第一层：内存缓存（命中就直接返回，不进单飞流程）
	{
		std::lock_guard<std::mutex> lock(_file_mtx);
		auto iter = _map_md5_files.find(md5);
		if (iter != _map_md5_files.end()) {
			return iter->second;
		}
	}

	// ② 第二层：singleflight —— 同一个 md5 只让一个线程去查 Redis
	std::shared_ptr<FileFlight> flight;
	bool is_leader = false;
	{
		std::lock_guard<std::mutex> lock(_flight_mtx);
		auto it = _inflight.find(md5);
		if (it != _inflight.end()) {
			flight = it->second;                        // 已经有人在查 → 我是跟随者
		}
		else {
			flight = std::make_shared<FileFlight>();    // 没人查 → 我是 leader
			_inflight[md5] = flight;
			is_leader = true;
		}
	}

	// ③ 跟随者：睡在这里等 leader 的结果，醒来直接复用
	if (!is_leader) {
		std::unique_lock<std::mutex> lk(flight->mtx);
		flight->cv.wait(lk, [&flight] { return flight->done; });
		return flight->result;
	}

	// ④ leader：真正回源 Redis（在锁外做网络 IO）
	std::shared_ptr<FileInfo> file_info = nullptr;
	try {
		file_info = RedisMgr::GetInstance()->GetFileInfo(md5);
		if (file_info != nullptr) {
			AddMD5File(md5, file_info);                 // 回填内存缓存
		}
	}
	catch (...) {
		file_info = nullptr;                           // 回源失败也要唤醒跟随者，否则他们会一直睡
	}

	// ⑤ 通知所有跟随者，并撤销登记
	{
		std::lock_guard<std::mutex> lk(flight->mtx);
		flight->result = file_info;
		flight->done = true;
	}
	flight->cv.notify_all();
	{
		std::lock_guard<std::mutex> lock(_flight_mtx);
		_inflight.erase(md5);
	}
	return file_info;
}






