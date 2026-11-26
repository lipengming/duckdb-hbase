#include "scan/remote_scan_stream.hpp"

#include "catch.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

using namespace duckdb;
using namespace duckdb::hbase;

TEST_CASE("关闭未完成的远端流时触发一次取消回调", "[scan][lifecycle]") {
    std::atomic<bool> cancelled {false};
    std::atomic<idx_t> cancel_calls {0};
    std::promise<void> started;
    auto started_future = started.get_future();

    RemoteScanStream stream(
        [&](const RemoteBatchCallback &) {
            started.set_value();
            while (!cancelled.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        },
        1,
        [&]() {
            cancel_calls.fetch_add(1);
            cancelled.store(true);
        });

    started_future.wait();
    stream.Close();
    stream.Close();

    REQUIRE(cancel_calls.load() == 1);
}

TEST_CASE("正常结束的远端流不发送取消回调", "[scan][lifecycle]") {
    std::atomic<idx_t> cancel_calls {0};
    RemoteScanStream stream(
        [](const RemoteBatchCallback &callback) { callback(string(), 0, true); },
        1,
        [&]() { cancel_calls.fetch_add(1); });

    string payload;
    int64_t row_count = 0;
    REQUIRE_FALSE(stream.Next(payload, row_count));
    stream.Close();

    REQUIRE(cancel_calls.load() == 0);
}
