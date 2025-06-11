/*
 * SYS_LINUX - Linux平台系统接口头文件
 * ===================================
 * 功能：BasicStation在Linux平台的系统特定功能实现
 * 核心：日志管理、进程控制、文件操作、设备管理
 */

#ifndef _sys_linux_h_
#define _sys_linux_h_

#include "rt.h"  // 运行时基础类型

// 退出码定义
#define EXIT_NOP          6   // 无操作退出：程序正常退出但无实际操作
#define FATAL_GENERIC    30   // 一般致命错误：通用的致命错误退出码
#define FATAL_PTHREAD    31   // 线程致命错误：pthread相关致命错误
#define FATAL_NOLOGGING  32   // 日志致命错误：日志系统初始化失败
#define FATAL_MAX        40   // 最大致命错误码：致命错误码的上限

/**
 * struct logfile - 日志文件配置结构
 * 功能：定义日志文件的路径、大小和轮转配置
 */
struct logfile {
    str_t path;         // 日志路径：日志文件的完整路径
    int   size;         // 文件大小：单个日志文件的最大字节数
    int   rotate;       // 轮转数量：保留的历史日志文件数量
};

extern str_t  sys_slaveExec;  // 子进程模板：启动子进程的命令行模板

// 日志管理API
void     sys_startLogThread ();                        // 启动日志线程：创建异步日志处理线程
void     sys_iniLogging (struct logfile* lf, int captureStdio);  // 初始化日志：配置日志文件和标准IO捕获
void     sys_flushLog ();                              // 刷新日志：强制写入所有缓存的日志数据

// 进程管理API
int      sys_findPids (str_t device, u4_t* pids, int n_pids);  // 查找进程：根据设备名查找相关进程PID
void     sys_startupSlave (int rdfd, int wrfd);        // 启动子进程：以指定文件描述符启动RAL子进程

// 文件操作API
dbuf_t   sys_checkFile (str_t filename);               // 检查文件：读取文件内容到缓冲区
void     sys_writeFile (str_t filename, dbuf_t* data); // 写入文件：将缓冲区数据写入文件

// 设备管理API
int      sys_enableGPS (str_t device);                 // 启用GPS：初始化指定GPS设备
void     sys_enableCmdFIFO (str_t file);              // 启用命令FIFO：初始化命令管道接口

#endif // _sys_linux_h_
