#include "scan/remote_scan_stream.hpp"

#include "duckdb/common/exception.hpp"

#include <utility>

namespace duckdb {
namespace hbase {

RemoteScanStream::RemoteScanStream(Starter starter, idx_t max_queued)
    : max_queued_(max_queued < 1 ? 1 : max_queued) {
    worker_ = std::thread([this, starter = std::move(starter)]() { Run(std::move(starter)); });
}

RemoteScanStream::~RemoteScanStream() {
    Close();
}

void RemoteScanStream::Run(Starter starter) {
    try {
        starter([this](const string& payload, int64_t row_count, bool finished) {
            Item item;
            item.payload = payload;
            item.row_count = row_count;
            item.finished = finished;
            Push(std::move(item));
        });
    } catch (...) {
        {
            std::lock_guard<std::mutex> guard(lock_);
            error_ = std::current_exception();
            finished_ = true;
        }
        cv_.notify_all();
        return;
    }

    std::lock_guard<std::mutex> guard(lock_);
    finished_ = true;
    cv_.notify_all();
}

void RemoteScanStream::Push(Item item) {
    std::unique_lock<std::mutex> guard(lock_);
    // 有界队列：等待消费者取走，避免把整个结果集读进内存
    cv_.wait(guard, [this]() { return closed_ || queue_.size() < max_queued_; });
    if (closed_) {
        return;
    }
    queue_.push_back(std::move(item));
    guard.unlock();
    cv_.notify_all();
}

bool RemoteScanStream::Next(string& payload, int64_t& row_count) {
    std::unique_lock<std::mutex> guard(lock_);
    cv_.wait(guard, [this]() { return closed_ || !queue_.empty() || finished_; });

    if (error_) {
        auto error = error_;
        guard.unlock();
        std::rethrow_exception(error);
    }
    if (queue_.empty()) {
        return false;
    }

    auto item = std::move(queue_.front());
    queue_.pop_front();
    guard.unlock();
    cv_.notify_all();

    if (item.finished) {
        return false;
    }
    payload = std::move(item.payload);
    row_count = item.row_count;
    return true;
}

void RemoteScanStream::Close() {
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (closed_) {
            // 已经关闭：只负责 join
        } else {
            closed_ = true;
        }
    }
    cv_.notify_all();
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
        worker_.join();
    }
}

}  // namespace hbase
}  // namespace duckdb
