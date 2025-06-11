# TC (Traffic Controller) 传输控制器技术文档

## 模块概述

TC (Traffic Controller) 传输控制器是BasicStation的核心网络连接管理器，负责建立和维护与LoRaWAN网络服务器(LNS)的WebSocket连接。它实现了完整的连接生命周期管理，包括服务发现、连接建立、重连机制和错误恢复。

## 核心功能

### 1. 连接管理
- **INFOS服务发现**: 查询路由器信息获取MUXS连接地址
- **MUXS连接管理**: 建立和维护与数据复用器的WebSocket连接
- **TLS安全连接**: 支持加密的WebSocket连接(WSS)
- **连接状态监控**: 实时监控连接状态和健康度

### 2. 协议处理
- **S2E协议集成**: 与Station-to-Server协议层紧密集成
- **JSON消息处理**: 处理文本格式的控制消息
- **二进制数据传输**: 高效处理LoRaWAN数据包
- **版本协商**: 自动发送版本信息进行协议协商

### 3. 重连机制
- **智能重连策略**: 区分INFOS和MUXS重连类型
- **指数退避算法**: 避免对服务器造成过大压力
- **快速重连**: MUXS连接的快速恢复机制
- **CUPS集成**: 自动触发配置更新

## 关键数据结构

### TC对象 (tc_t)
```c
typedef struct tc {
    ws_t     ws;              // WebSocket连接状态
    tmr_t    timeout;         // 超时定时器
    s1_t     tstate;          // TC引擎状态
    u1_t     credset;         // 凭据集标识
    u1_t     retries;         // 重试次数
    char     muxsuri[MAX_URI_LEN+3];  // 编码的MUXS URI
    tmrcb_t  ondone;          // 完成回调函数
    s2ctx_t  s2ctx;           // S2E协议上下文
} tc_t;
```

### 状态定义
```c
enum {
    // 正常状态
    TC_INI            = 0,    // 初始状态
    TC_INFOS_REQ_PEND,        // INFOS请求待处理
    TC_INFOS_GOT_URI,         // 成功获取MUXS URI
    TC_MUXS_REQ_PEND,         // MUXS连接请求待处理
    TC_MUXS_CONNECTED,        // MUXS已连接
    TC_MUXS_BACKOFF,          // MUXS重连退避
    TC_INFOS_BACKOFF,         // INFOS重连退避

    // 错误状态
    TC_ERR_FAILED        = -1, // 一般失败
    TC_ERR_NOURI         = -2, // 无URI配置
    TC_ERR_TIMEOUT       = -3, // 连接超时
    TC_ERR_REJECTED      = -4, // 服务器拒绝
    TC_ERR_CLOSED        = -5, // 连接关闭
    TC_ERR_DEAD          = -6, // 对象已销毁
};
```

## 连接流程详解

### 1. 初始化阶段
1. **tc_ini()**: 分配TC对象，初始化WebSocket和定时器
2. **设置S2E接口**: 配置发送缓冲区和数据传输回调
3. **参数初始化**: 设置初始状态、凭据集、重试次数

### 2. INFOS查询阶段
1. **tc_start()**: 获取TC配置URI，解析主机名和端口
2. **建立连接**: 连接到INFOS服务(/router-info端点)
3. **发送查询**: 发送包含路由器EUI的JSON查询请求
4. **解析响应**: 提取MUXS URI、路由器ID等信息
5. **URI验证**: 验证MUXS URI格式和长度

### 3. MUXS连接阶段
1. **tc_connect_muxs()**: 解析编码的MUXS URI信息
2. **TLS设置**: 如果需要，配置TLS安全连接
3. **建立连接**: 连接到MUXS服务器
4. **版本协商**: 发送版本信息和功能特性
5. **数据传输**: 开始处理S2E协议消息

## 重连策略详解

### MUXS快速重连
- **适用场景**: 已有MUXS URI，连接临时中断
- **退避算法**: 指数退避 (2^n 秒: 2, 4, 8, 16秒)
- **重试次数**: 最多4次快速重连
- **状态保持**: 保持MUXS URI和S2E上下文

### INFOS重连退避
- **适用场景**: 需要重新查询MUXS地址
- **退避算法**: 线性退避 (min(n,6)*10 秒: 10-60秒)
- **对象重建**: 完全重新创建TC对象
- **状态清理**: 清除所有缓存的连接信息

### CUPS触发条件
- 服务器拒绝连接 (TC_ERR_REJECTED)
- 无URI配置 (TC_ERR_NOURI)
- 重试次数达到上限 (retries >= 10)

## S2E协议接口

### 发送接口
```c
// 获取发送缓冲区
static dbuf_t tc_getSendbuf(s2ctx_t* s2ctx, int minsize)

// 发送文本数据 (JSON消息)
static void tc_sendText(s2ctx_t* s2ctx, dbuf_t* buf)

// 发送二进制数据 (LoRaWAN包)
static void tc_sendBinary(s2ctx_t* s2ctx, dbuf_t* buf)
```

### 接收处理
- **WSEV_TEXTRCVD**: 接收JSON消息，转发给s2e_onMsg()
- **WSEV_BINARYRCVD**: 接收二进制数据，转发给s2e_onBinary()
- **WSEV_DATASENT**: 数据发送完成，刷新待发送队列

## 错误处理机制

### 统一错误处理
```c
static void tc_done(tc_t* tc, s1_t tstate)
```
- 设置最终状态
- 释放WebSocket资源
- 触发完成回调
- 更新系统状态

### 超时处理
```c
static void tc_timeout(tmr_t* tmr)
```
- 连接超时检测
- 记录超时错误
- 触发重连流程

### 重连控制
```c
void tc_continue(tc_t* tc)
```
- 分析错误类型
- 选择重连策略
- 实施退避算法
- 条件触发CUPS

## 系统集成接口

### 生命周期管理
```c
void sys_startTC()    // 启动TC引擎
void sys_stopTC()     // 停止TC引擎
void sys_iniTC()      // 初始化TC系统
s1_t sys_statusTC()   // 查询TC状态
```

### 配置依赖
- **sys_uri()**: 获取TC配置URI
- **sys_eui()**: 获取基站EUI
- **sys_version()**: 获取版本信息
- **sys_*CUPS()**: CUPS管理接口

## 性能特点

### 内存管理
- **动态分配**: 仅在需要时分配TC对象
- **资源复用**: WebSocket缓冲区复用
- **及时释放**: 连接关闭时立即释放资源

### 网络优化
- **连接复用**: 保持长连接减少握手开销
- **退避控制**: 避免网络风暴和服务器压力
- **快速重连**: 减少连接中断时间

### 协议效率
- **二进制传输**: LoRaWAN数据包直接传输
- **JSON压缩**: 控制消息使用紧凑格式
- **队列管理**: 智能管理待发送数据队列

## 调试和监控

### 日志输出
- **连接状态**: 详细记录连接建立过程
- **错误信息**: 完整的错误原因和处理
- **重连统计**: 重试次数和退避时间
- **性能指标**: 连接延迟和数据吞吐

### 状态查询
- **实时状态**: sys_statusTC()查询当前状态
- **历史状态**: 保存最后状态用于调试
- **连接参数**: 当前使用的URI和凭据信息

## 设计原则

### 可靠性
- **故障自恢复**: 自动处理各种网络异常
- **状态一致性**: 严格的状态机控制
- **资源保护**: 防止内存泄漏和资源耗尽

### 可扩展性
- **模块化设计**: 清晰的接口分离
- **协议无关**: 可适配不同的传输协议
- **配置灵活**: 支持多种部署模式

### 可维护性
- **清晰架构**: 分层设计便于理解
- **完整注释**: 详细的功能说明
- **调试友好**: 丰富的日志和状态信息

## 常见问题处理

### 连接问题
- **URI配置错误**: 检查station.conf中的TC URI
- **网络不通**: 验证防火墙和路由配置
- **TLS证书问题**: 检查证书文件和权限

### 性能问题
- **频繁重连**: 检查网络稳定性和服务器负载
- **内存泄漏**: 监控TC对象的创建和销毁
- **CPU占用高**: 分析重连频率和退避参数

### 协议问题
- **版本不匹配**: 检查协议版本兼容性
- **消息解析失败**: 验证JSON格式和字段
- **二进制数据错误**: 检查数据包格式和长度

这个文档为TC模块提供了全面的技术参考，涵盖了设计原理、实现细节、使用方法和问题排查，为开发者提供了深入理解和维护TC模块的完整指南。 