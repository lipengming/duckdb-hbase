#pragma once

#include "duckdb.hpp"
#include "duckdb/main/client_context_state.hpp"

#include <cstdint>
#include <map>
#include <mutex>

namespace duckdb {
namespace hbase {

//! 一条已注册连接的静态配置。
//! 注意：这里只保存连接参数，不持有任何网络资源；
//! 真正的 Gateway 客户端由 Provider 在需要时创建（见 docs/gateway-design.md）。
struct ConnectionConfig {
    //! 连接名，SQL 中的首参
    string name;
    //! 规范化后的端点，例如 grpc://127.0.0.1:18080
    string endpoint;
    //! 协议方案：grpc / http
    string scheme;
    //! 主机名或 IP
    string host;
    //! 端口
    uint16_t port = 0;
};

//! 连接表：生命周期绑定到 ClientContext（DuckDB 连接），随会话销毁自动释放。
//! 所有方法线程安全。实现见 docs/extension-design.md §2。
class ConnectionRegistry : public ClientContextState {
public:
    //! 注册或覆盖同名连接；返回 true 表示覆盖了已存在的连接。
    bool Upsert(ConnectionConfig config);
    //! 删除连接；返回是否删除成功。
    bool Remove(const string& name);
    //! 查询连接；存在时写入 out 并返回 true。
    bool TryGet(const string& name, ConnectionConfig& out) const;
    //! 当前连接数量。
    idx_t Count() const;

private:
    mutable mutex lock_;
    std::map<string, ConnectionConfig> connections_;
};

//! 取得（必要时创建）当前会话的连接表。
ConnectionRegistry& GetConnectionRegistry(ClientContext& context);

//! 解析并校验端点字符串（scheme://host:port）。
//! 非法输入抛 InvalidInputException，错误信息包含连接名与原始端点。
ConnectionConfig ParseEndpoint(const string& connection_name, const string& endpoint);

}  // namespace hbase
}  // namespace duckdb
