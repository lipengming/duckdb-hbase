#pragma once

#include "duckdb.hpp"
#include "provider/remote_scan_provider.hpp"

#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>

namespace duckdb {
namespace hbase {

//! 后台线程拉取 Gateway 流，主线程按批消费：有界队列保证内存不随结果集增长。
//!
//! 用法：构造时传入一个"启动函数"（内部调用 provider->Scan/BatchGet），
//! 该函数通过回调把批次推入队列；Next() 阻塞直到有批次或流结束。
class RemoteScanStream {
public:
    using Starter = std::function<void(const RemoteBatchCallback&)>;

    explicit RemoteScanStream(Starter starter, idx_t max_queued = 2);
    ~RemoteScanStream();

    RemoteScanStream(const RemoteScanStream&) = delete;
    RemoteScanStream& operator=(const RemoteScanStream&) = delete;

    //! 取下一条 IPC 载荷；返回 false 表示流已正常结束。
    bool Next(string& payload, int64_t& row_count);

    //! 请求停止并等待工作线程退出（幂等）。
    void Close();

private:
    struct Item {
        string payload;
        int64_t row_count = 0;
        bool finished = false;
    };

    void Push(Item item);
    void Run(Starter starter);

    idx_t max_queued_;
    mutable std::mutex lock_;
    std::condition_variable cv_;
    std::deque<Item> queue_;
    bool finished_ = false;
    bool closed_ = false;
    std::exception_ptr error_;
    std::thread worker_;
};

}  // namespace hbase
}  // namespace duckdb
