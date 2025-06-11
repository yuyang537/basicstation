/*
 * --- Revised 3-Clause BSD License ---
 * Copyright Semtech Corporation 2022. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright notice,
 *       this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright notice,
 *       this list of conditions and the following disclaimer in the documentation
 *       and/or other materials provided with the distribution.
 *     * Neither the name of the Semtech corporation nor the names of its
 *       contributors may be used to endorse or promote products derived from this
 *       software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL SEMTECH CORPORATION. BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * SYS 系统核心服务模块头文件
 * ===========================
 * 功能描述：BasicStation系统级服务的统一接口层
 * 核心职责：
 *   1. 硬件抽象层：提供跨平台的硬件访问接口
 *   2. 系统服务：时间、随机数、EUI生成、日志管理
 *   3. 子系统控制：TC/CUPS/Web等模块的生命周期管理
 *   4. 配置管理：凭证存储、固件更新、设备配置
 *   5. 设备管理：射频初始化、GPS控制、设备发现
 * 
 * 架构特点：
 *   - 分层设计：应用层 -> SYS接口层 -> 平台实现层
 *   - 模块化：各子系统独立管理，统一控制接口
 *   - 跨平台：通过条件编译支持不同操作系统
 *   - 事件驱动：基于状态变化的异步通知机制
 */

#ifndef _sys_h_
#define _sys_h_

#include "rt.h"  // 包含运行时基础类型定义

// ==============================================
// 系统模式和设备类型常量定义
// ==============================================

/**
 * 通信接口类型枚举
 * 定义BasicStation支持的硬件通信方式
 */
enum { 
    COMTYPE_SPI = 0,  // SPI总线通信（主要用于SX130x系列芯片）
    COMTYPE_USB       // USB通信（用于某些开发板和调试接口）
};

/**
 * PPS（秒脉冲）信号源类型
 * 用于时间同步的参考信号来源
 */
enum { 
    PPS_GPS = 0,   // GPS模块提供的标准PPS信号
    PPS_FUZZY,     // 模糊PPS信号（低精度，用于测试）
    PPS_TESTPIN    // 测试引脚信号（调试和验证用）
};

// ==============================================
// 全局系统配置变量
// ==============================================

extern u1_t   sys_modePPS;     // PPS模式选择：控制时间同步的信号源类型
extern u1_t   sys_deviceMode;  // 设备模式：用于回归测试和特殊功能模式
extern u1_t   sys_noTC;        // TC禁用标志：禁用与LNS的TC连接
extern u1_t   sys_noCUPS;      // CUPS禁用标志：禁用Configuration and Update Server
extern u2_t   sys_webPort;     // Web服务端口：管理界面的HTTP端口号
extern s1_t   sys_slaveIdx;    // 从进程索引：-1=主进程，>=0表示从进程编号
extern uL_t   protoEUI;        // 协议EUI：LoRaWAN协议相关的标识符
extern uL_t   prefixEUI;       // EUI前缀：设备EUI的前缀部分

// ==============================================
// 系统核心服务API
// ==============================================

/**
 * sys_ini - 系统初始化
 * 功能：
 *   1. 初始化所有系统级服务
 *   2. 设置默认配置参数
 *   3. 准备运行时环境
 *   4. 启动基础设施模块
 * 调用时机：程序启动时首先调用
 */
void  sys_ini ();

/**
 * sys_fatal - 系统致命错误处理
 * 参数：code - 错误代码，用于诊断错误类型
 * 功能：
 *   1. 记录致命错误信息到日志
 *   2. 执行清理操作和资源释放
 *   3. 通知监控系统或上级进程
 *   4. 退出程序（带错误码）
 * 使用场景：无法恢复的系统级错误
 */
void  sys_fatal (int code);

/**
 * sys_addLog - 添加日志条目
 * 参数：
 *   line - 日志内容字符串
 *   len  - 字符串长度
 * 功能：
 *   1. 格式化日志消息（添加时间戳、级别等）
 *   2. 输出到控制台或日志文件
 *   3. 支持日志轮转和大小限制
 *   4. 线程安全的日志写入
 * 注意：输入字符串必须以'\n'结尾
 */
void  sys_addLog (str_t line, int len);

// ==============================================
// 随机数和加密服务
// ==============================================

#if defined(CFG_sysrandom)  // 如果系统提供硬件随机数生成器
/**
 * sys_random - 获取系统随机数
 * 参数：
 *   buf - 输出缓冲区
 *   len - 需要的随机字节数
 * 返回值：实际生成的随机字节数
 * 功能：从系统熵源获取高质量随机数
 */
int  sys_random (u1_t* buf, int len);
#else  // 否则使用软件随机数生成器
/**
 * sys_seed - 设置随机数种子
 * 参数：
 *   seed - 种子数据
 *   len  - 种子长度
 * 功能：
 *   1. 使用高质量熵源初始化随机数生成器
 *   2. 混合多种熵源（时间、硬件状态等）
 *   3. 确保随机数的不可预测性
 */
void  sys_seed (u1_t* seed, int len);
#endif

// ==============================================
// 时间服务API
// ==============================================

/**
 * sys_time - 获取系统单调时间
 * 返回值：微秒级时间戳（单调递增）
 * 功能：
 *   1. 提供高精度时间测量
 *   2. 不受系统时间调整影响
 *   3. 用于超时计算和性能测量
 * 精度：微秒级，适合网络超时和调度
 */
sL_t  sys_time ();

/**
 * sys_utc - 获取UTC时间
 * 返回值：UTC时间戳，0表示不可用
 * 功能：
 *   1. 获取当前UTC时间
 *   2. 用于日志时间戳和协议时间字段
 *   3. 依赖系统时间同步状态
 */
sL_t  sys_utc ();

/**
 * sys_eui - 获取设备EUI
 * 返回值：设备的唯一标识符（8字节）
 * 功能：
 *   1. 从硬件MAC地址或配置文件获取
 *   2. 确保设备在网络中的唯一性
 *   3. 用于LoRaWAN协议的设备识别
 */
uL_t  sys_eui ();

/**
 * sys_usleep - 微秒级睡眠
 * 参数：us - 睡眠微秒数
 * 功能：
 *   1. 提供高精度延迟
 *   2. 不阻塞其他线程
 *   3. 用于精确的时序控制
 */
void  sys_usleep(sL_t us);

// ==============================================
// 系统状态通知
// ==============================================

/**
 * 系统状态事件类型
 * 用于通知系统各模块的状态变化
 */
enum {
    SYSIS_TC_CONNECTED    = 1,  // TC连接已建立
    SYSIS_TC_DISCONNECTED = 2,  // TC连接已断开
    SYSIS_STATION_DEAD    = 3,  // 基站进程未启动或已死亡
    SYSIS_CUPS_INTERACT   = 4,  // CUPS交互进行中
    SYSIS_CUPS_DONE       = 5,  // CUPS操作完成
};

/**
 * sys_inState - 系统状态变化通知
 * 参数：state - 新的系统状态（SYSIS_xxx常量）
 * 功能：
 *   1. 通知系统状态监控器
 *   2. 触发相关的状态处理逻辑
 *   3. 更新状态指示器和日志
 *   4. 支持状态变化的链式反应
 */
void sys_inState (int state);

// ==============================================
// 凭证和配置管理
// ==============================================

/**
 * 凭证类别定义
 * 区分不同类型的凭证和配置数据
 */
enum { 
    SYS_CRED_CUPS,      // CUPS服务相关凭证
    SYS_CRED_TC,        // TC(Traffic Controller)凭证
    SYS_CRED_MAX        // 凭证类别数量上限
};

/**
 * 凭证集合定义
 * 同一类别凭证的不同版本或备份
 */
enum { 
    SYS_CRED_REG,       // 常规凭证集合
    SYS_CRED_BAK,       // 备份凭证集合
    SYS_CRED_BOOT       // 启动凭证集合
};

/**
 * 凭证元素类型
 * 每个凭证包含的具体内容
 */
enum { 
    SYS_CRED_TRUST,     // 信任根证书（CA证书）
    SYS_CRED_MYCERT,    // 客户端证书
    SYS_CRED_MYKEY,     // 客户端私钥
    SYS_CRED_NELEMS     // 凭证元素数量
};

/**
 * 认证模式定义
 * 配置TLS连接的认证要求
 */
enum { 
    SYS_AUTH_NONE,      // 无认证（仅加密）
    SYS_AUTH_SERVER,    // 仅服务器认证
    SYS_AUTH_BOTH,      // 双向认证
    SYS_AUTH_TOKEN      // 基于令牌的认证
};

// 凭证管理API函数声明
str_t sys_credcat2str (int cred_cat);    // 凭证类别转字符串
str_t sys_credset2str (int cred_set);    // 凭证集合转字符串
int   sys_cred (int cred_cat, int cred_set, str_t* elems, int* elemslen);  // 获取凭证
void  sys_credStart (int cred_cat, int len);     // 开始凭证写入
void  sys_credWrite  (int cred_cat, u1_t* data, int off, int len);  // 写入凭证数据
void  sys_credComplete (int cred_cat, int len);  // 完成凭证写入

// ==============================================
// 固件更新管理
// ==============================================

void  sys_updateStart  (int len);                // 开始固件更新
void  sys_updateWrite  (u1_t* data, int off, int len);  // 写入固件数据
int   sys_updateCommit (int len);                // 提交固件更新
void  sys_resetConfigUpdate ();                  // 重置配置更新
void  sys_commitConfigUpdate ();                 // 提交配置更新
void  sys_backupConfig (int cred_cat);           // 备份配置

// ==============================================
// 系统信息和工具函数
// ==============================================

str_t sys_version ();                            // 获取版本信息
str_t sys_uri     (int cred_cat, int cred_set);  // 获取服务URI
void  sys_saveUri (int cred_cat, str_t uri);     // 保存服务URI
u4_t  sys_crcCred (int cred_cat, int cred_set);  // 计算凭证CRC

void   sys_runUpdate ();                         // 执行更新
void   sys_abortUpdate ();                       // 中止更新
str_t  sys_radioDevice (str_t device, u1_t* comtype);  // 解析射频设备
int    sys_runRadioInit (str_t device);          // 初始化射频设备
int    sys_execCommand (ustime_t max_wait, str_t* argv);  // 执行系统命令

dbuf_t sys_sigKey (int key_id);                  // 获取签名密钥
u4_t   sys_crcSigkey (int key_id);               // 计算签名密钥CRC
dbuf_t sys_readFile (str_t filename);            // 读取文件内容
str_t  sys_makeFilepath (str_t fn, int complain); // 构造文件路径

// ==============================================
// 子系统生命周期管理API
// ==============================================

/**
 * TC（Traffic Controller）模块管理
 * 负责与LoRaWAN网络服务器的连接和通信
 */
void   sys_iniTC ();      // 初始化TC模块
void   sys_stopTC ();     // 停止TC模块
void   sys_startTC ();    // 启动TC模块
s1_t   sys_statusTC ();   // 获取TC模块状态

/**
 * CUPS（Configuration and Update Server）模块管理
 * 负责配置获取和固件更新
 */
void   sys_iniCUPS ();         // 初始化CUPS模块
void   sys_triggerCUPS (int delay);  // 触发CUPS操作（带延迟）
void   sys_clearCUPS ();       // 清除CUPS状态
void   sys_delayCUPS ();       // 延迟CUPS操作
s1_t   sys_statusCUPS ();      // 获取CUPS模块状态

/**
 * Web管理界面模块管理
 * 提供HTTP服务和配置界面
 */
void   sys_iniWeb ();     // 初始化Web模块
void   sys_stopWeb ();    // 停止Web模块

// ==============================================
// 网络和设备管理
// ==============================================

/**
 * sys_keepAlive - 设置TCP连接保活
 * 参数：fd - 套接字文件描述符
 * 功能：配置TCP_KEEPALIVE选项，确保连接活性检测
 */
void   sys_keepAlive (int fd);

/**
 * sys_getLatLon - 获取地理位置坐标
 * 参数：
 *   lat - 纬度输出指针
 *   lon - 经度输出指针
 * 返回值：0=成功，非0=失败
 * 功能：
 *   1. 从GPS模块获取当前位置
 *   2. 用于Class B信标和位置报告
 *   3. 支持多种定位源（GPS、配置文件等）
 */
int    sys_getLatLon (double* lat, double* lon);

#endif // _sys_h_
