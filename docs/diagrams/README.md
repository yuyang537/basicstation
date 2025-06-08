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

### 6. startupMaster-flow.mmd
描述了 `startupMaster` 函数的启动流程，包括日志线程启动、自检、进程管理、更新处理、无线电初始化、退出处理和定时器设置等步骤。

### 7. startupMaster2-flow.mmd
描述了 `startupMaster2` 函数的子系统初始化流程，包括特性管理、命令FIFO启用、GPS设备检测、时间同步、CUPS和Web子系统的初始化过程。

### 8. startupMaster2-architecture.mmd
展示了 `startupMaster2` 函数的架构设计，包括特性管理模块、通信子系统、GPS子系统、时间同步子系统、CUPS子系统和Web子系统的组织结构和相互关系。

### 9. tc-subsystem-architecture.mmd
详细描述了时间同步子系统（TC）的架构，包括初始化流程、连接管理、重连策略和状态监控机制。展示了TC引擎如何连接INFOS和MUXS服务器，以及失败时的恢复策略。

### 10. cups-subsystem-architecture.mmd
展示了CUPS（配置和更新服务器）子系统的完整架构，包括会话触发、连接管理、数据处理（URI、凭证、固件更新）和重试机制。还包括独占模式下与TC子系统的交互。

### 11. web-subsystem-architecture.mmd
描述了Web子系统的架构设计，包括HTTP服务器初始化、请求路由处理、静态文件服务、API接口处理和认证模块。展示了Web服务器如何处理不同类型的HTTP事件。

### 12. subsystem-interaction-flow.mmd
综合展示了 `startupMaster2` 中各个子系统的初始化流程和相互交互关系。包括时间同步、CUPS和Web子系统的启动顺序，以及它们之间的依赖关系和故障恢复机制。

### 13. main-process-startup-flow.mmd
展示了整个程序从启动到运行的完整流程，包括参数解析、配置验证、守护进程处理、子系统初始化和最终进入主事件循环的全过程。覆盖了所有启动模式（普通模式、守护进程模式、从进程模式）。

### 14. startup-master-detailed-flow.mmd
详细描述了 `startupMaster` 和 `startupMaster2` 函数的执行流程，包括日志线程启动、自检、PID管理、更新处理、无线电初始化、各个子系统的启动顺序，以及最终进入事件循环的过程。

### 15. daemon-process-flow.mmd
专门展示守护进程模式的工作机制，包括双层fork架构、工作进程监控、自动重启机制和进程间的协调。详细说明了守护进程如何确保服务的高可用性和故障恢复能力。

### 16. slave-mode-flow.mmd
展示从模式启动和运行的完整流程图，包括环境变量检查、管道通信初始化、命令处理循环和RX数据处理。详细描述了从进程如何响应主进程的各种命令（CONFIG、TX、TXSTATUS、TXABORT、TIMESYNC、STOP）。

### 17. master-slave-interaction.mmd  
展示主从模式交互架构图，详细描述主进程和从进程之间的通信机制。包括从进程的创建和管理、管道通信、监控重启逻辑、配置文件管理和环境变量传递机制。

### 18. complete-startup-flow.mmd
展示完整的主进程启动流程图，包括所有启动模式的详细处理过程。特别详细描述了主进程如何发现、创建、配置和监控从进程，以及各个子系统的初始化顺序和最终的协同工作状态。

### 19. master-startup-slave-process.mmd
专门聚焦于主进程启动从进程的详细机制图。深入展示了从配置文件发现、fork创建、环境变量设置、管道通信建立、配置发送、进程监控到故障恢复的完整过程。是理解主从架构实现细节的核心图表。

### 20. master-startup-summary.md
主进程启动从进程流程的详细文字解释文档。包含完整的代码示例和技术分析，深入解释主从架构的设计原理、可靠性机制、通信协议和错误处理策略。是理解Basic Station主从架构的重要技术文档。

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