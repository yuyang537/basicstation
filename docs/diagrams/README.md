# Basic Station 架构图文档

本目录包含Basic Station项目的各种架构图和流程图，用于帮助理解系统的设计和工作原理。

## 架构图列表

### 1. 进程架构图 (`process-architecture.mmd`)
展示Basic Station的进程结构和组件间的交互关系，包括：
- **进程管理层**：主进程、守护进程控制器、进程监控
- **核心工作进程**：Station核心、配置管理、无线电管理
- **网络通信进程**：TC协议、CUPS协议、TLS处理、HTTP服务
- **平台服务进程**：Linux系统服务、日志管理、命令FIFO、远程Shell
- **无线电抽象层进程**：RAL主从进程、驱动程序、时间同步
- **外部接口**：SPI接口、LNS连接、CUPS服务器连接

### 2. 数据流架构图 (`data-flow-architecture.mmd`)
展示LoRa数据包在系统中的完整处理流程，包括：
- **系统启动和配置阶段**：配置加载、远程配置获取
- **无线电初始化阶段**：硬件初始化、接口配置
- **网络连接建立阶段**：LNS连接、安全握手
- **上行数据流**：从LoRa设备到网络服务器的数据处理
- **下行数据流**：从网络服务器到LoRa设备的数据传输
- **配置动态更新**：运行时配置更新流程
- **错误处理和恢复**：异常情况的处理机制

### 3. 守护进程启动流程图 (`startup-daemon-flow.mmd`)
详细展示startupDaemon函数的完整执行流程，包括：
- **启动模式检查**：普通模式vs守护进程模式的判断
- **双层fork架构**：第一次fork创建守护进程，第二次fork创建工作进程
- **进程监控循环**：工作进程状态监控和自动重启机制
- **系统初始化**：各子系统的初始化顺序和依赖关系

### 4. 守护进程架构图 (`startup-daemon-architecture.mmd`)
展示守护进程的完整架构和各组件的交互关系，包括：
- **系统启动流程**：不同启动模式的处理逻辑
- **守护进程初始化**：会话独立和子系统初始化
- **进程监控循环**：500ms轮询检查和重启机制
- **PID管理功能**：进程ID文件的管理和旧进程清理

### 5. 守护进程功能特点图 (`startup-daemon-features.mmd`)
总结startupDaemon的核心功能和技术特点，包括：
- **自动重启**：工作进程异常时的自动重启能力
- **进程隔离**：双层fork实现的进程隔离架构
- **会话独立**：通过setsid()实现的终端脱离
- **资源管理**：PID文件和进程生命周期管理
- **错误恢复**：完整的监控、日志和错误处理机制

## 如何查看架构图

这些架构图使用Mermaid格式编写，可以通过以下方式查看：

1. **GitHub直接查看**：在GitHub仓库中直接打开`.mmd`文件
2. **Mermaid Live Editor**：访问 https://mermaid.live/ 并粘贴文件内容
3. **VS Code插件**：安装Mermaid Preview插件
4. **本地渲染**：使用mermaid-cli工具转换为图片格式

## 文件组织

```
docs/diagrams/
├── README.md                      # 本说明文件
├── process-architecture.mmd       # 进程架构图
├── data-flow-architecture.mmd     # 数据流架构图
├── startup-daemon-flow.mmd        # 守护进程启动流程图
├── startup-daemon-architecture.mmd # 守护进程架构图
└── startup-daemon-features.mmd    # 守护进程功能特点图
```

## 贡献指南

如需添加新的架构图或更新现有图表：

1. 所有流程图应使用Mermaid格式
2. 文件命名使用小写字母和连字符
3. 图表应包含适当的中文注释
4. 更新本README文件以包含新图表的说明

## 相关文档

- [Basic Station官方文档](https://doc.sm.tc/station)
- [项目根目录的架构图](../../sturture.mmd)
- [源代码目录](../../src/)
- [Linux平台代码](../../src-linux/) 