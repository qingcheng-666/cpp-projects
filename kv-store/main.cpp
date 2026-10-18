/* ************************************************************************
> File Name:     main.cpp
> Description:   修复版 demo（对应 fixed/skiplist.h）
>
> 与原版 main.cpp 的区别：
>   1. 用 --std=c++17 编译（需要 std::shared_mutex）
>   2. dump_file() 现在返回 bool，所以检查了返回值
>   3. 末尾加了并发读写自测，验证 [修复 3] 的读写锁确实生效
 ************************************************************************/
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include "skiplist.h"
#define FILE_PATH "./store/dumpFile"

int main() {

    // 键值中的key用int型，如果用其他类型，需要自定义比较函数
    // 而且如果修改key的类型，同时需要修改skipList.load_file函数
    SkipList<int, std::string> skipList(6);
	skipList.insert_element(1, "学"); 
	skipList.insert_element(3, "算法"); 
	skipList.insert_element(7, "认准"); 
	skipList.insert_element(8, "微信公众号：代码随想录"); 
	skipList.insert_element(9, "学习"); 
	skipList.insert_element(19, "算法不迷路"); 
	skipList.insert_element(19, "赶快关注吧你会发现详见很晚！"); 

    std::cout << "skipList size:" << skipList.size() << std::endl;

    // [修复 4] dump_file 现在返回 bool，必须检查
    if (!skipList.dump_file()) {
        std::cerr << "!! 落盘失败，请检查 store/ 目录是否存在" << std::endl;
    }

    // skipList.load_file();

    skipList.search_element(9);
    skipList.search_element(18);

    skipList.display_list();

    skipList.delete_element(3);
    skipList.delete_element(7);

    std::cout << "skipList size:" << skipList.size() << std::endl;

    skipList.display_list();

    // ------------------------------------------------------------------
    // [修复 3] 并发自测：1 个写线程 + 3 个读线程
    // 原版这里会产生数据竞争（TSan 会报 data race），修复版应干净通过。
    // ------------------------------------------------------------------
    std::cout << "\n===== 并发读写自测（修复 3 验证）=====" << std::endl;
    std::atomic<bool> stop{false};

    std::thread writer([&] {
        int k = 1000;
        while (!stop) skipList.insert_element(k++, "w");
    });
    std::vector<std::thread> readers;
    for (int i = 0; i < 3; i++) {
        readers.emplace_back([&] {
            while (!stop) skipList.search_element(9);
        });
    }

    std::this_thread::sleep_for(std::chrono::seconds(2));
    stop = true;
    writer.join();
    for (auto& t : readers) t.join();

    std::cout << "并发读写 2 秒无崩溃，最终 size=" << skipList.size() << std::endl;
    std::cout << "（若用 TSan 编译运行，原版会报 data race，修复版不会）" << std::endl;
    return 0;
}
