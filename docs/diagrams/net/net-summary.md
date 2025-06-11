# BasicStation网络通信层详细技术文档

## 系统概述

网络通信层(net.c)是BasicStation的核心通信基础设施，为LoRaWAN网关提供完整的网络协议栈实现。该模块实现了从底层TCP/TLS连接到上层WebSocket/HTTP协议的全套网络通信功能，是连接LoRaWAN网关与网络服务器的关键桥梁。

### 核心设计理念

1. **异步非阻塞I/O** - 基于事件驱动的异步I/O模型，提高并发性能
2. **协议无关设计** - 统一的连接对象支持多种上层协议
3. **安全传输优先** - 完整的TLS/SSL支持，确保通信安全
4. **资源高效管理** - 精细的缓冲区管理和内存使用优化
5. **状态机驱动** - 清晰的状态转换逻辑，便于调试和维护

## 核心功能模块

### 1. WebSocket协议实现

WebSocket是BasicStation与LNS(LoRaWAN Network Server)通信的主要协议，提供全双工、低延迟的实时通信能力。

#### 核心特性
- **RFC 6455兼容** - 完整实现WebSocket协议标准
- **帧格式处理** - 支持文本帧、二进制帧、控制帧
- **掩码处理** - 客户端自动添加掩码，服务器验证掩码
- **分片支持** - 大消息自动分片传输
- **心跳机制** - Ping/Pong帧维持连接活性

#### 关键数据结构
```c
typedef struct conn {
    aio_t*   aio;           // 异步I/O对象
    tmr_t    tmr;           // 定时器（超时处理）
    u1_t*    rbuf;          // 接收缓冲区
    u1_t*    wbuf;          // 发送缓冲区
    u1_t     state;         // 连接状态
    evcb_t   evcb;          // 事件回调函数
    netctx_t netctx;        // 网络上下文
    tlsctx_p tlsctx;        // TLS上下文
} conn_t;
```

#### WebSocket状态转换
1. **WS_DEAD** → **WS_TLS_HANDSHAKE** - 开始TLS握手
2. **WS_TLS_HANDSHAKE** → **WS_CLIENT_REQ** - TLS完成，发送HTTP Upgrade
3. **WS_CLIENT_REQ** → **WS_SERVER_RESP** - 等待服务器响应
4. **WS_SERVER_RESP** → **WS_CONNECTED** - 握手成功，进入数据传输
5. **WS_CONNECTED** → **WS_CLOSING_\*** - 开始关闭流程
6. **WS_CLOSING_\*** → **WS_CLOSED** - 关闭完成

#### 性能特征
- **连接建立时间**: 通常50-200ms（含TLS握手）
- **帧解析开销**: ~10μs/帧（小帧）
- **吞吐量**: 可达100MB/s（大消息场景）
- **并发连接**: 支持数千个并发WebSocket连接

### 2. HTTP协议实现

HTTP协议用于配置管理、文件服务、RESTful API等场景，提供标准的Web服务能力。

#### HTTP客户端特性
- **多版本支持** - HTTP/1.0和HTTP/1.1
- **持久连接** - Keep-Alive支持
- **自动重定向** - 3xx响应处理
- **分块传输** - Chunked encoding支持
- **认证支持** - Basic/Bearer认证

#### HTTP服务器特性
- **虚拟主机** - 基于Host头的路由
- **静态文件服务** - 自动MIME类型识别
- **CGI支持** - 动态内容生成
- **访问控制** - IP白名单/黑名单
- **日志记录** - 详细的访问日志

#### 关键API接口
```c
// HTTP客户端
int  http_connect(http_t* conn, char* host, char* port);
void http_request(http_t* conn, dbuf_t* req);
int  http_getStatus(http_t* conn);
dbuf_t http_getBody(http_t* conn);

// HTTP服务器
int  httpd_listen(httpd_t* conn, const char* port);
void httpd_response(httpd_t* conn, dbuf_t* resp);
```

### 3. TLS安全传输层

基于mbed TLS库实现的安全传输层，为所有网络连接提供端到端加密保护。

#### 安全特性
- **TLS 1.2/1.3支持** - 最新的TLS协议版本
- **证书验证** - X.509证书链验证
- **密码套件选择** - 安全的加密算法组合
- **Perfect Forward Secrecy** - 前向安全保证
- **证书固定** - 防止中间人攻击

#### 配置管理
```c
// TLS配置创建
tlsconf_t* tls_makeConf(mbedtls_x509_crt* trust, 
                        mbedtls_x509_crt* mycert, 
                        mbedtls_pk_context* mykey);

// 设置信任CA
int tls_setTrustedCAs(tlsconf_t* conf, const char* ca, int calen);

// 设置客户端证书
int tls_setMyCert(tlsconf_t* conf, const char* cert, int certlen,
                  const char* key, int keylen, const char* pwd);
```

#### 性能优化
- **会话复用** - TLS会话缓存减少握手开销
- **硬件加速** - 支持硬件密码学引擎
- **内存优化** - 最小化TLS内存占用
- **连接池** - 复用TLS连接减少开销

### 4. URI解析引擎

完整的URI解析实现，支持IPv4/IPv6地址、域名、端口、路径等全部URI组件。

#### 解析能力
- **协议识别** - http/https/ws/wss等协议
- **IPv6支持** - [2001:db8::1]:8080格式
- **端口解析** - 默认端口推断
- **路径提取** - 查询参数和片段标识符
- **URL编码** - %编码的解码支持

#### 解析示例
```c
// 解析: wss://example.com:8443/ws/gateway?id=123
struct uri_info u;
dbuf_t buf;
dbuf_str(buf, "wss://example.com:8443/ws/gateway?id=123");
uri_parse(&buf, &u, 0);

// 结果:
// scheme: "wss"
// host: "example.com"  
// port: "8443"
// path: "/ws/gateway?id=123"
```

### 5. 连接管理系统

统一的连接对象管理，提供连接生命周期的完整控制。

#### 连接对象特性
- **状态跟踪** - 详细的连接状态信息
- **超时管理** - 连接、读写、空闲超时
- **错误处理** - 网络错误的检测和恢复
- **资源清理** - 自动资源释放和内存管理
- **事件通知** - 异步事件回调机制

#### 缓冲区管理
- **动态调整** - 根据数据量动态调整缓冲区大小
- **零拷贝** - 减少数据拷贝操作
- **内存池** - 预分配内存池提高性能
- **溢出保护** - 防止缓冲区溢出攻击

## 高级特性

### 1. 异步I/O架构

基于epoll/kqueue的高性能异步I/O实现，支持大量并发连接。

#### 事件处理模型
```c
// 异步I/O事件循环
while (!shutdown) {
    int nevents = aio_poll(aio_ctx, events, MAX_EVENTS, timeout);
    for (int i = 0; i < nevents; i++) {
        conn_t* conn = events[i].data.ptr;
        if (events[i].events & EPOLLIN) {
            handle_read(conn);
        }
        if (events[i].events & EPOLLOUT) {
            handle_write(conn);
        }
    }
    rt_processTimerQ();  // 处理定时器事件
}
```

### 2. 错误处理和恢复

完整的错误处理机制，确保网络异常情况下的稳定运行。

#### 错误分类
- **网络错误** - 连接断开、超时、DNS解析失败
- **协议错误** - 格式错误、版本不匹配、认证失败
- **资源错误** - 内存不足、文件描述符耗尽
- **应用错误** - 逻辑错误、配置错误

#### 恢复策略
- **自动重连** - 指数退避算法
- **降级服务** - 功能降级保证核心服务
- **错误隔离** - 防止错误传播
- **监控报警** - 错误统计和告警

### 3. 性能监控

内置的性能监控系统，提供详细的运行时统计信息。

#### 监控指标
- **连接统计** - 活跃连接数、连接建立/断开速率
- **吞吐量** - 发送/接收字节数、消息数
- **延迟统计** - RTT、处理延迟分布
- **错误率** - 各类错误的发生频率
- **资源使用** - 内存、CPU、文件描述符使用情况

## 配置和调优

### 1. 网络参数调优

```c
// WebSocket配置
#define WS_RBUF_SIZE    (64*1024)   // 接收缓冲区大小
#define WS_WBUF_SIZE    (64*1024)   // 发送缓冲区大小
#define WS_CONNECT_TMO  (30*1000)   // 连接超时(ms)
#define WS_IDLE_TMO     (300*1000)  // 空闲超时(ms)

// HTTP配置  
#define HTTP_MAX_HDR    (8*1024)    // 最大头部大小
#define HTTP_MAX_BODY   (1024*1024) // 最大消息体大小
#define HTTP_KEEPALIVE  (60*1000)   // Keep-Alive超时

// TLS配置
#define TLS_MAX_FRAG    512         // 最大片段长度
#define TLS_VERIFY_MODE MBEDTLS_SSL_VERIFY_REQUIRED
```

### 2. 性能优化建议

#### 连接优化
- 启用TCP_NODELAY减少小包延迟
- 调整TCP窗口大小适应网络带宽
- 使用连接池避免频繁建立连接
- 启用Keep-Alive维持长连接

#### 内存优化
- 合理设置缓冲区大小
- 及时释放不用的连接
- 使用内存池减少分配开销
- 监控内存泄漏问题

#### TLS优化
- 启用会话复用
- 选择高效的密码套件
- 使用硬件加速（如AES-NI）
- 优化证书链长度

## 故障诊断

### 1. 常见问题

#### 连接问题
- **连接超时** - 检查网络连通性、防火墙配置
- **TLS握手失败** - 验证证书、检查时钟同步
- **WebSocket升级失败** - 检查Upgrade头、协议版本

#### 性能问题
- **高延迟** - 检查网络质量、缓冲区配置
- **低吞吐量** - 调整TCP窗口、启用并发
- **内存泄漏** - 使用内存分析工具检查

### 2. 调试工具

#### 日志系统
```c
// 启用详细日志
LOG(MOD_AIO|DEBUG, "WebSocket frame: type=%d, len=%d", type, len);
LOG(MOD_AIO|VERBOSE, "HTTP request: %s %s", method, uri);
```

#### 网络抓包
```bash
# 抓取WebSocket流量
tcpdump -i any -w ws.pcap 'port 8080'

# 分析TLS握手
openssl s_client -connect example.com:443 -msg
```

## 安全考虑

### 1. 输入验证
- 严格验证所有网络输入
- 防止缓冲区溢出攻击
- 限制请求大小和频率
- 过滤恶意字符和格式

### 2. 加密传输
- 强制使用TLS 1.2+
- 禁用弱密码套件
- 启用证书验证
- 实施证书固定

### 3. 访问控制
- IP白名单/黑名单
- 基于Token的认证
- 限制并发连接数
- 实施速率限制

## 扩展和集成

### 1. 协议扩展
网络通信层设计为可扩展架构，支持添加新的协议实现：

```c
// 协议接口定义
typedef struct protocol_ops {
    int (*connect)(conn_t* conn, const char* uri);
    int (*send)(conn_t* conn, const void* data, size_t len);
    int (*recv)(conn_t* conn, void* buf, size_t len);
    void (*close)(conn_t* conn);
} protocol_ops_t;
```

### 2. 中间件支持
支持插件式中间件，可以在协议层之间添加功能：

- **压缩中间件** - 数据压缩减少带宽
- **加密中间件** - 应用层加密
- **监控中间件** - 性能监控和统计
- **缓存中间件** - 响应缓存优化

## 总结

BasicStation网络通信层是一个功能完整、性能优异的网络协议栈实现。它不仅提供了WebSocket和HTTP的标准实现，还包含了TLS安全层、URI解析、连接管理等完整的网络基础设施。

### 主要优势
1. **高性能** - 异步I/O和零拷贝优化
2. **安全性** - 完整的TLS支持和安全机制
3. **可靠性** - 完善的错误处理和恢复机制
4. **可扩展** - 模块化设计支持协议扩展
5. **易维护** - 清晰的状态机和丰富的调试信息

该模块为BasicStation提供了稳定可靠的网络通信能力，是实现高质量LoRaWAN网关的关键技术基础。 