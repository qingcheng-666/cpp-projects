#include "AsioIOServicePool.h"
#include <iostream>
using namespace std;
AsioIOServicePool::AsioIOServicePool(std::size_t size):_ioServices(size),
_works(size), _nextIOService(0){
	for (std::size_t i = 0; i < size; ++i) {
		_works[i] = std::unique_ptr<Work>(new Work(_ioServices[i]));
	}

	//遍历多个ioservice，创建多个线程，每个线程内部启动ioservice
	for (std::size_t i = 0; i < _ioServices.size(); ++i) {
		_threads.emplace_back([this, i]() {
			_ioServices[i].run();
			});
	}
}

AsioIOServicePool::~AsioIOServicePool() {
	std::cout << "AsioIOServicePool destruct" << endl;
	// [修复] 原来只有一句打印：_threads 里的线程在析构时仍 joinable，
	//   std::thread 析构函数会直接 std::terminate()（进程异常退出）。
	//   这里补上 Stop()：先 stop 各 io_context 让 run() 返回，再 join 回收线程。
	Stop();
}

boost::asio::io_context& AsioIOServicePool::GetIOService() {
	auto& service = _ioServices[_nextIOService++];
	if (_nextIOService == _ioServices.size()) {
		_nextIOService = 0;
	}
	return service;
}

void AsioIOServicePool::Stop(){
	//因为仅仅执行work.reset并不能让iocontext从run的状态中退出
	//当iocontext已经绑定了读或写的监听事件后，还需要手动stop该服务。
	// [修复] 让 Stop() 可重入：析构里也会调它，而 work 被 reset 后是 nullptr，
	//   重复 stop 会解引用空指针；已 join 过的 thread 再 join 会抛 system_error。
	for (auto& work : _works) {
		if (work) {
			//把服务先停止
			work->get_io_context().stop();
			work.reset();
		}
	}

	for (auto& t : _threads) {
		if (t.joinable()) {
			t.join();
		}
	}
}
