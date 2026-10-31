#pragma once
#include "Singleton.h"
#include <queue>
#include <thread>
#include "CSession.h"
#include <queue>
#include <map>
#include <functional>
#include "const.h"
#include <json/json.h>
#include <json/value.h>
#include <json/reader.h>
#include <unordered_map>
#include <condition_variable>
#include <memory>
#include "LogicWorker.h"
#include "FileInfo.h"


typedef  function<void(shared_ptr<CSession>, const short &msg_id, const string &msg_data)> FunCallBack;

// singleflight（单飞）：同一个 md5 的并发回源只让一个线程真去查 Redis，
// 其余线程等它的结果，避免"缓存未命中时多个线程重复回源"（cache stampede）
struct FileFlight {
	std::mutex mtx;
	std::condition_variable cv;
	bool done = false;                            // leader 是否已完成
	std::shared_ptr<FileInfo> result;             // leader 查到的结果（可能为 nullptr）
};

class LogicSystem:public Singleton<LogicSystem>
{
	friend class Singleton<LogicSystem>;
public:
	~LogicSystem();
	void PostMsgToQue(shared_ptr < LogicNode> msg, int index);
	void AddMD5File(std::string md5, std::shared_ptr<FileInfo> fileinfo);
	std::shared_ptr<FileInfo> GetFileInfo(std::string md5);
private:
	LogicSystem();
	std::vector<std::shared_ptr<LogicWorker> > _workers;
	std::mutex _file_mtx;
	std::unordered_map<std::string, std::shared_ptr<FileInfo>> _map_md5_files;

	// singleflight 的登记表：md5 -> 正在进行中的那次回源
	std::mutex _flight_mtx;
	std::unordered_map<std::string, std::shared_ptr<FileFlight>> _inflight;
};

