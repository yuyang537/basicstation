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

// 文件功能：Linux平台系统接口实现
// 作用：提供LoRaWAN基站在Linux系统上的底层系统调用封装和进程管理功能

#define _GNU_SOURCE  // 启用GNU扩展功能，如syncfs(fd)系统调用
// 标准C库头文件
#include <limits.h>    // 系统限制常量定义
#include <stdio.h>     // 标准输入输出函数
#include <fcntl.h>     // 文件控制操作
#include <errno.h>     // 错误码定义
#include <time.h>      // 时间相关函数
#include <unistd.h>    // UNIX标准函数
#include <dirent.h>    // 目录操作函数
#include <sys/types.h> // 系统数据类型定义
#include <sys/stat.h>  // 文件状态信息
#include <sys/wait.h>  // 进程等待函数

// 基站专用头文件
#include "argp2.h"     // 命令行参数解析
#include "s2conf.h"    // 基站配置管理
#include "kwcrc.h"     // CRC校验算法
#include "rt.h"        // 运行时系统
#include "uj.h"        // JSON解析器
#include "s2e.h"       // 基站到服务器通信
#include "ral.h"       // 无线电抽象层
#include "timesync.h"  // 时间同步
#include "sys.h"       // 系统接口定义
#include "sys_linux.h" // Linux系统接口
#include "fs.h"        // 文件系统操作
#include "selftests.h" // 自测试功能

#include "mbedtls/version.h" // mbedTLS版本信息

// 外部函数声明 - 来自sys.c文件
extern char* makeFilepath (const char* prefix, const char* suffix, char** pCachedFile, int isReadable); // 构造文件路径
extern int writeFile (str_t file, const char* data, int datalen);        // 写入文件数据
extern dbuf_t readFile (str_t file, int complain);                       // 读取文件内容
extern str_t readFileAsString (str_t basename, str_t suffix, str_t* pCachedValue); // 读取文件为字符串
extern void setupConfigFilenames ();                                     // 设置配置文件名
extern int checkUris ();                                                 // 检查URI配置
extern void checkRollForward ();                                         // 检查版本回滚

// 主从模式配置 - 用于多进程架构
#if defined(CFG_ral_master_slave)
// 从进程环境变量名称数组 - 用于主从进程间通信
static const char* const SLAVE_ENVS[] = {
    "SLAVE_IDX",  // 从进程索引号
    "SLAVE_WRFD", // 从进程写文件描述符
    "SLAVE_RDFD", // 从进程读文件描述符
    NULL          // 数组结束标记
};
#endif // defined(CFG_ral_master_slave)

// 静态全局变量定义
static struct logfile logfile;    // 日志文件配置结构体
static char* gpsDevice = NULL;    // GPS设备路径字符串
static tmr_t startupTmr;          // 启动定时器，用于控制启动流程

// 全局系统配置变量
str_t sys_slaveExec;              // 从进程执行命令模板
u1_t  sys_deviceMode;             // 设备工作模式标志
u1_t  sys_modePPS;                // PPS(脉冲每秒)特殊模式标志
u2_t  sys_webPort;                // Web服务端口号
u1_t  sys_noTC;                   // 禁用TC(Traffic Control)标志
u1_t  sys_noCUPS;                 // 禁用CUPS(Configuration and Update Server)标志

// 外部目录路径变量
extern str_t  homeDir;            // 主目录路径
extern str_t  tempDir;            // 临时目录路径  
extern str_t  webDir;             // Web文件目录路径

// 目录路径来源记录
static str_t  homeDirSrc;         // 主目录路径来源(配置文件/命令行等)
static str_t  tempDirSrc;         // 临时目录路径来源
static str_t  webDirSrc;          // Web目录路径来源

// 进程管理变量
static int    daemonPid;          // 守护进程PID
static int    workerPid;          // 工作进程PID

// 无线电和设备配置
static str_t  radioInit;          // 无线电初始化脚本路径
static str_t  radioDevice;        // 无线电设备路径
static str_t  versionTxt;         // 版本信息文本

// 固件更新相关变量
static char*  updfile;            // 更新文件路径
static char*  temp_updfile;       // 临时更新文件路径
static int    updfd = -1;         // 更新文件描述符

// EUI(Extended Unique Identifier)配置来源
static str_t  protoEuiSrc;        // 协议EUI来源记录
static str_t  prefixEuiSrc;       // EUI前缀来源记录
static str_t  radioInitSrc;       // 无线电初始化脚本来源记录


// 函数功能：信号处理函数，处理系统信号(SIGINT, SIGTERM等)
// 参数说明：signum - 接收到的信号编号
// 返回值：无(函数不返回，直接退出进程)
// 调用时机：当进程接收到注册的信号时被系统调用
static void handle_signal (int signum) {
    // 注意：在信号处理函数中调用exit()是不安全的
    // exit()会运行atexit函数并触发gcda文件写入(gcov/lcov覆盖率测试)
    // 但可能会中断libc中正在进行的IO操作
    // 虽然这种情况不太可能发生，但我们应该在aio_loop中使用pselect
    // 并且只在pselect挂起时启用信号处理
    // 基站的终止代码首先尝试SIGTERM，一段时间后使用SIGKILL
    // 因此我们不应该出现基站进程僵死的情况
    exit(128+signum);  // 以128+信号编号作为退出码退出进程
    // 信号安全但不太方便的替代方案：
    //_exit(128+signum);  // 直接退出，不调用清理函数
}



// 函数功能：更新目录设置，验证目录路径并规范化格式
// 参数说明：path - 目录路径字符串
//          source - 路径来源描述(用于错误信息)
//          pdir - 指向目录路径变量的指针
//          psrc - 指向路径来源变量的指针
// 返回值：成功返回1，失败返回0
// 调用时机：设置home、temp、web等目录时调用
static int updateDirSetting (str_t path, str_t source, str_t* pdir, str_t* psrc) {
    int l = strlen(path);                    // 获取路径长度
    char* p = rt_mallocN(char, l+5);         // 分配内存，预留"./"和"/"及"\0"的空间
    if( path[0] ) {                          // 如果路径不为空
        strcpy(p,path);                      // 复制原路径
    } else {                                 // 如果路径为空
        strcpy(p,"./");                      // 使用当前目录"./"
        l = 2;                               // 更新长度
    }
    if( p[l-1] != '/' ) {                    // 如果路径末尾没有'/'
        p[l++] = '/';                        // 添加'/'并更新长度
    }
    if( p[0] != '/' && (p[0] != '.' || p[1] != '/') ) {  // 如果不是绝对路径且不以"./"开头
        memmove(p+2, p, l+1);                // 向后移动字符串内容
        p[0] = '.';                          // 添加"./"前缀
        p[1] = '/';
    }
    struct stat st;                          // 文件状态结构体
    if( stat(p, &st) == -1 ) {              // 获取文件/目录状态失败
        fprintf(stderr, "%s - Cannot access directory '%s': %s\n", source, p, strerror(errno));
        goto err;                            // 跳转到错误处理
    }
    if( !S_ISDIR(st.st_mode) ) {            // 如果不是目录
        fprintf(stderr, "%s - Not a directory: %s\n", source, p);
  err:                                       // 错误处理标签
        rt_free(p);                          // 释放分配的内存
        return 0;                            // 返回失败
    }

    rt_free((void*)*pdir);                   // 释放旧的目录路径内存
    rt_free((void*)*psrc);                   // 释放旧的路径来源内存
    *pdir = p;                               // 设置新的目录路径
    *psrc = rt_strdup(source);               // 设置新的路径来源
    return 1;                                // 返回成功
}

// 函数功能：设置Web文件目录路径
// 参数说明：path - Web目录路径，source - 路径来源描述
// 返回值：成功返回1，失败返回0
// 调用时机：解析配置文件或命令行参数时调用
static int setWebDir (str_t path, str_t source) {
    return updateDirSetting(path, source, &webDir, &webDirSrc);  // 调用通用目录设置函数
}

// 函数功能：设置基站主目录路径
// 参数说明：path - 主目录路径，source - 路径来源描述  
// 返回值：成功返回1，失败返回0
// 调用时机：解析配置文件或命令行参数时调用
static int setHomeDir (str_t path, str_t source) {
    return updateDirSetting(path, source, &homeDir, &homeDirSrc);  // 调用通用目录设置函数
}

// 函数功能：设置临时文件目录路径
// 参数说明：path - 临时目录路径，source - 路径来源描述
// 返回值：成功返回1，失败返回0  
// 调用时机：解析配置文件或命令行参数时调用
static int setTempDir (str_t path, str_t source) {
    return updateDirSetting(path, source, &tempDir, &tempDirSrc);  // 调用通用目录设置函数
}


// 函数功能：去除字符串尾部的空白字符(空格、制表符、换行符等)
// 参数说明：b - 包含字符串数据的缓冲区结构体
// 返回值：处理后的缓冲区结构体
// 调用时机：读取文件内容后需要清理尾部空白字符时调用
static dbuf_t stripTrailingWsp (dbuf_t b) {
    // 从字符串末尾开始向前检查，如果是空白字符则删除
    while( b.bufsize > 0 && strchr(" \t\r\n", b.buf[b.bufsize-1]) ) {
        b.buf[--b.bufsize] = 0;  // 将空白字符替换为字符串结束符并减少长度
    }
    return b;  // 返回处理后的缓冲区
}


// 函数功能：解析EUI(Extended Unique Identifier)字符串
// 参数说明：s - EUI字符串
//          n - 期望的字符串长度(0表示不限制长度)
//          peui - 输出解析结果的指针
//          nonzero - 是否要求EUI不能为0
// 返回值：成功返回NULL，失败返回错误描述字符串
// 调用时机：解析配置文件中的EUI字段或命令行EUI参数时调用
static str_t parseEui (str_t s, int n, uL_t* peui, int nonzero) {
    str_t p = s;                              // 保存原始字符串指针
    uL_t eui = rt_readEui(&p, n);            // 调用运行时函数解析EUI
    if( p==s || (n==0 ? (p[0] != 0) : (p != s+n)) )  // 检查解析是否成功
        return "syntax error";                // 语法错误：没有解析任何字符或长度不匹配
    if( nonzero && eui == 0 )                // 如果要求非零但EUI为0
        return "must not be zero";            // 返回错误：EUI不能为零
    *peui = eui;                             // 设置解析结果
    return NULL;                             // 返回成功(NULL表示无错误)
}


// 函数功能：查找系统默认的EUI，通过扫描网络接口获取MAC地址
// 参数说明：无
// 返回值：无(通过全局变量protoEUI和protoEuiSrc返回结果)
// 调用时机：系统初始化时，如果没有配置EUI则自动查找默认值
static void findDefaultEui () {
    str_t dirname = "/sys/class/net";        // Linux网络接口信息目录
    DIR* D = opendir(dirname);               // 打开网络接口目录
    if( D==NULL )                            // 如果无法打开目录
        return;                              // 直接返回，不设置默认EUI
    char path[NAME_MAX+32];                  // 文件路径缓冲区
    char ifc[64] = {0};                      // 当前选中的网络接口名
    uL_t eui = 0;                           // 当前选中的EUI值
    struct dirent* de;                       // 目录项结构体指针
    while( (de = readdir(D)) ) {            // 遍历网络接口目录中的每个条目
        char* dname = de->d_name;            // 获取目录项名称(网络接口名)
        if( strlen(dname) > sizeof(ifc)-1 )  // 如果接口名太长
            continue;                        // 跳过此接口
        if( strcmp("lo", dname) == 0 )       // 如果是回环接口"lo"
            continue;                        // 跳过回环接口
        snprintf(path, sizeof(path), "%s/%s/address", dirname, dname);  // 构造MAC地址文件路径
        dbuf_t b = stripTrailingWsp(readFile(path, 0));  // 读取MAC地址文件并去除尾部空白
        if( b.buf == NULL )                  // 如果读取失败
            continue;                        // 跳过此接口
        uL_t mac = 0;                       // 临时存储解析的MAC地址
        str_t err = parseEui(b.buf, b.bufsize, &mac, 1);  // 解析MAC地址字符串
        rt_free(b.buf);                      // 释放读取的文件内容内存
        if( err != NULL )                    // 如果解析失败
            continue;                        // 跳过此接口
        // 优先选择以"eth"开头的网络接口
        if( ifc[0] != 0 ) {                 // 如果已经有选中的接口
            if( strncmp(ifc, "eth", 3) == 0 && strncmp(dname, "eth", 3) != 0 )
                continue;                    // eth接口优先于其他接口
            // 否则选择字母序较小的接口名 - 除非eth接口替换其他类型接口
            if( !((strncmp(ifc, "eth", 3) == 0) ^ (strncmp(dname, "eth", 3) == 0))
                && strcmp(ifc, dname) <= 0 )
                continue;                    // 当前接口名不够小，跳过
        }
        strcpy(ifc, dname);                  // 更新选中的接口名
        eui = mac;                           // 更新选中的EUI值
        continue;                            // 继续检查下一个接口
    }
    closedir(D);                             // 关闭目录
    if( eui ) {                             // 如果找到了有效的EUI
        snprintf(path, sizeof(path), "%s/%s/address", dirname, ifc);  // 重新构造路径
        protoEUI = eui;                      // 设置全局协议EUI
        rt_free((void*)protoEuiSrc);         // 释放旧的EUI来源字符串
        protoEuiSrc = rt_strdup(path);       // 设置新的EUI来源为文件路径
    }
}


// 函数功能：设置基站协议EUI，支持从文件读取或直接解析字符串
// 参数说明：spec - EUI规格字符串(可以是文件路径或EUI值)
//          source - EUI来源描述(用于日志记录)
// 返回值：成功返回1，失败返回0
// 调用时机：解析配置文件中的routerid字段或处理命令行EUI参数时调用
static int setEui (str_t spec, str_t source) {
    str_t err;                                   // 错误信息指针
    if( access(spec, R_OK) == 0 ) {             // 如果spec是可读的文件路径
        dbuf_t b = stripTrailingWsp(sys_readFile(spec));  // 读取文件内容并去除尾部空白
        if( b.buf && (err = parseEui(b.buf, b.bufsize, &protoEUI, 1)) == NULL ) {  // 解析EUI成功
            char sbuf[strlen(source)+strlen(spec)+32];  // 构造来源描述缓冲区
            snprintf(sbuf, sizeof(sbuf), "%s file %s", source, spec);  // 格式化来源信息
            rt_free((void*)protoEuiSrc);         // 释放旧的来源字符串
            protoEuiSrc = rt_strdup(sbuf);       // 设置新的来源信息
            rt_free(b.buf);                      // 释放文件内容缓冲区
            return 1;                            // 返回成功
        }
        if( b.buf == NULL ) {                   // 如果文件读取失败
            LOG(MOD_SYS|ERROR, "Station proto EUI %s (%s): Cannot read file", spec, source);
        } else {                                // 如果EUI解析失败
            LOG(MOD_SYS|ERROR, "Station proto EUI '%s' (%s file %s): %s", b.buf, source, spec, err);
        }
        rt_free(b.buf);                         // 释放文件内容缓冲区
        return 0;                               // 返回失败
    }
    if( (err = parseEui(spec, strlen(spec), &protoEUI, 1)) == NULL ) {  // 直接解析EUI字符串成功
        rt_free((void*)protoEuiSrc);            // 释放旧的来源字符串
        protoEuiSrc = rt_strdup(source);        // 设置新的来源信息
        return 1;                               // 返回成功
    }
    LOG(MOD_SYS|ERROR, "Station proto EUI: '%s' (%s): %s", spec, source, err);  // 记录解析错误
    return 0;                                   // 返回失败
}


// 函数功能：查找正在使用指定设备文件的进程ID列表
// 参数说明：device - 设备文件路径(必须是绝对路径)
//          pids - 输出进程ID数组的缓冲区
//          n_pids - pids数组的最大容量
// 返回值：找到的进程数量(可能大于n_pids，表示实际进程数)
// 调用时机：需要检查设备文件是否被其他进程占用时调用
int sys_findPids (str_t device, u4_t* pids, int n_pids) {
    if( device[0] != '/' )                      // 如果不是绝对路径
        return 0;                               // 直接返回0

    char path[NAME_MAX+64];                     // 路径缓冲区
    struct dirent* de;                          // 目录项指针
    DIR *D, *DD;                               // 目录句柄
    int dlen, cnt = 0;                         // 设备路径长度和找到的进程计数

    dlen = strlen(device);                      // 获取设备路径长度
    assert(dlen < NAME_MAX);                    // 确保路径长度不超过限制
    strcpy(path, "/proc");                      // 设置proc文件系统路径
    if( (D = opendir("/proc")) == NULL )        // 打开/proc目录
        return 0;                               // 如果失败则返回0
    while( (de = readdir(D)) ) {               // 遍历/proc目录中的每个条目
        const char* pid_s = de->d_name;         // 获取目录名(进程ID字符串)
        sL_t pid = rt_readDec(&pid_s);         // 解析进程ID数字
        if( pid < 0 )                          // 如果不是有效的进程ID
            continue;                           // 跳过此条目
        int n_prefix = snprintf(path, sizeof(path), "/proc/%s/fd", de->d_name);  // 构造fd目录路径
        if( (DD = opendir(path)) == NULL )      // 打开进程的文件描述符目录
            continue;                           // 如果失败则跳过此进程
        while( (de = readdir(DD)) ) {          // 遍历文件描述符目录
            if( de->d_type != DT_LNK )         // 如果不是符号链接
                continue;                       // 跳过此条目
            char linkpath[NAME_MAX];            // 链接目标路径缓冲区
            snprintf(path+n_prefix, sizeof(path)-n_prefix, "/%s", de->d_name);  // 构造完整fd路径
            int err = readlink(path, linkpath, sizeof(linkpath));  // 读取符号链接目标
            if( err != dlen )                  // 如果链接长度不匹配设备路径长度
                continue;                       // 跳过此文件描述符
            linkpath[err] = 0;                 // 添加字符串结束符
            if( strcmp(device, linkpath) == 0 ) {  // 如果链接目标匹配设备路径
                if( cnt < n_pids )             // 如果输出数组还有空间
                    pids[cnt] = pid;           // 记录进程ID
                cnt++;                         // 增加计数
            }
        }
        closedir(DD);                          // 关闭文件描述符目录
    }
    closedir(D);                               // 关闭/proc目录
    return cnt;                                // 返回找到的进程数量
}


// 函数功能：获取无线电设备路径并解析通信类型
// 参数说明：device - 指定的设备路径(NULL表示使用默认设备)
//          comtype - 输出通信类型的指针(可为NULL)
// 返回值：规范化的设备文件路径(调用者需要释放内存)
// 调用时机：初始化无线电硬件时调用，解析设备路径和通信协议
str_t sys_radioDevice (str_t device, u1_t* comtype) {
    str_t f = device==NULL ? radioDevice : device;  // 使用指定设备或默认设备
    if( f == NULL )                             // 如果没有配置设备
        f = RADIODEV;                           // 使用编译时默认设备路径
    // 检查通信类型前缀
    if( comtype )                               // 如果需要返回通信类型
	*comtype = COMTYPE_SPI;                 // 默认设置为SPI通信
    char *colon = index(f, ':');               // 查找冒号分隔符
    if( colon ) {                              // 如果找到冒号(有通信类型前缀)
	if( strncmp(f, "spi:", 4) == 0 ) {      // 如果是SPI类型前缀
	    if( comtype )                       // 如果需要返回通信类型
		*comtype = COMTYPE_SPI;         // 设置为SPI通信类型
	} else if( strncmp(f, "usb:", 4) == 0 ) {  // 如果是USB类型前缀
	    if( comtype )                       // 如果需要返回通信类型
		*comtype = COMTYPE_USB;         // 设置为USB通信类型
	} else {                                // 如果是未知的通信类型前缀
	    LOG(MOD_SYS|ERROR, "Unknown device comtype '%.*s' (using SPI)", colon-f, f);  // 记录错误并使用SPI
	}
	f = colon + 1;                          // 跳过通信类型前缀，指向实际设备路径
    }
    // 调用者必须释放返回的内存
    return sys_makeFilepath(f, 0);             // 构造完整的设备文件路径并返回
}


// 函数功能：致命错误退出函数
// 参数说明：code - 错误代码(0表示使用通用致命错误代码)
// 返回值：无(函数不返回)
// 调用时机：遇到无法恢复的致命错误时调用
void sys_fatal (int code) {
    exit(code==0 ? FATAL_GENERIC : code);       // 以指定错误代码退出进程
}

// 函数功能：构造PID文件的完整路径
// 参数说明：无
// 返回值：PID文件路径字符串(调用者需要释放内存)
// 调用时机：需要读取或写入PID文件时调用
static char* makePidFilename() {
    return makeFilepath("~temp/station",".pid",NULL,0);  // 构造临时目录下的station.pid文件路径
}

// 函数功能：从PID文件中读取进程ID
// 参数说明：无
// 返回值：读取到的进程ID(失败返回0)
// 调用时机：启动时检查是否有其他基站进程在运行
static int readPid() {
    char* pidfile = makePidFilename();          // 获取PID文件路径
    dbuf_t b = readFile(pidfile,0);            // 读取PID文件内容(不输出错误信息)
    const char* s = b.buf;                      // 获取文件内容指针
    int pid = rt_readDec(&s);                  // 解析进程ID数字
    rt_free(pidfile);                          // 释放文件路径内存
    rt_free(b.buf);                            // 释放文件内容内存
    return max(0,pid);                         // 返回有效的进程ID(负数转为0)
}

// 函数功能：将当前进程ID写入PID文件
// 参数说明：无
// 返回值：无
// 调用时机：进程启动后调用，记录当前进程ID用于后续检查
static void writePid () {
    char buf[16];                              // PID字符串缓冲区
    dbuf_t b = dbuf_ini(buf);                  // 初始化动态缓冲区
    xprintf(&b, "%d", daemonPid ? daemonPid : getpid());  // 格式化进程ID(优先使用守护进程ID)
    char* pidsfile = makePidFilename();        // 获取PID文件路径
    writeFile(pidsfile, b.buf, b.pos);         // 写入PID到文件
    rt_free(pidsfile);                         // 释放文件路径内存
}


// 函数功能：终止旧的基站进程
// 参数说明：无
// 返回值：无
// 调用时机：启动新基站进程前调用，清理可能存在的旧进程
static void killOldPid () {
    int pid = readPid();                        // 读取PID文件中的进程ID
    if( daemonPid && pid == daemonPid )         // 如果是守护进程启动的工作进程
        return;  // 工作进程在守护进程下启动，不需要杀死
    if( pid > 0 ) {                            // 如果找到有效的进程ID
        pid_t pgid = getpgid(pid);             // 获取进程组ID
        if( pgid == pid ) {                    // 如果是进程组领导者
            fprintf(stderr, "Killing process group %d\n", pid);  // 输出终止进程组信息
            kill(-pid, SIGINT);                // 向整个进程组发送SIGINT信号(优雅终止)
            rt_usleep(2000);                   // 等待2毫秒让进程有时间清理
            kill(-pid, SIGKILL);               // 向整个进程组发送SIGKILL信号(强制终止)
        } else {                               // 如果是普通进程
            fprintf(stderr, "Killing process %d\n", pid);  // 输出终止进程信息
            kill( pid, SIGINT);                // 向进程发送SIGINT信号(优雅终止)
            rt_usleep(2000);                   // 等待2毫秒让进程有时间清理
            kill( pid, SIGKILL);               // 向进程发送SIGKILL信号(强制终止)
        }
    }
}



// 函数功能：关闭LED指示灯，设置基站为死亡状态
// 参数说明：无
// 返回值：无
// 调用时机：进程退出时通过atexit()自动调用，用于清理LED状态
static void leds_off () {
    sys_inState(SYSIS_STATION_DEAD);           // 设置系统状态为基站死亡状态
}

// 函数功能：系统初始化，打印系统信息并初始化随机数种子
// 参数说明：无
// 返回值：无
// 调用时机：主进程启动后，在rt_ini()之前调用
void sys_ini () {
    // 打印日志配置信息
    LOG(MOD_SYS|INFO, "Logging     : %s (maxsize=%d, rotate=%d)\n",
        logfile.path==NULL ? "stderr" : logfile.path, logfile.size, logfile.rotate);
    // 打印基站版本信息
    LOG(MOD_SYS|INFO, "Station Ver : %s",  CFG_version " " CFG_bdate);
    // 打印软件包版本信息
    LOG(MOD_SYS|INFO, "Package Ver : %s",  sys_version());
    // 打印mbedTLS版本信息
    LOG(MOD_SYS|INFO, "mbedTLS Ver : %s",  MBEDTLS_VERSION_STRING);
    // 打印协议EUI和来源
    LOG(MOD_SYS|INFO, "proto EUI   : %:E\t(%s)", protoEUI, protoEuiSrc);
    // 打印EUI前缀和来源
    LOG(MOD_SYS|INFO, "prefix EUI  : %:E\t(%s)", prefixEUI, prefixEuiSrc);
    // 打印最终的基站EUI
    LOG(MOD_SYS|INFO, "Station EUI : %:E", sys_eui());
    // 打印基站主目录和来源
    LOG(MOD_SYS|INFO, "Station home: %s\t(%s)",  homeDir, homeDirSrc);
    // 打印临时目录和来源
    LOG(MOD_SYS|INFO, "Station temp: %s\t(%s)",  tempDir, tempDirSrc);
    if( sys_slaveIdx >= 0 ) {                    // 如果是从进程模式
        LOG(MOD_SYS|INFO, "Station slave: %d", sys_slaveIdx);  // 打印从进程索引
    } else {                                     // 如果是主进程模式
        if( gpsDevice )                          // 如果配置了GPS设备
            LOG(MOD_SYS|INFO, "GPS device: %s", gpsDevice);    // 打印GPS设备路径
    }
    if( sys_noTC || sys_noCUPS ) {              // 如果禁用了TC或CUPS服务
        LOG(MOD_SYS|WARNING, "Station in NO-%s mode", sys_noTC ? "TC" : "CUPS");  // 打印警告信息
    }
    int seed;                                    // 随机数种子变量
    sys_seed((u1_t*)&seed, sizeof(seed));      // 获取加密安全的随机数种子
    srand(seed);                                 // 初始化标准随机数生成器
}


// 函数功能：获取加密安全的随机数种子
// 参数说明：seed - 输出随机数据的缓冲区指针
//          len - 需要的随机数据长度(字节)
// 返回值：无
// 调用时机：系统初始化时调用，为加密算法提供安全的随机数种子
void sys_seed (unsigned char* seed, int len) {
    int fd;                                      // 文件描述符
    if( (fd = open("/dev/urandom", O_RDONLY)) == -1 ) {  // 尝试打开伪随机数设备
        if( (fd = open("/dev/random", O_RDONLY)) == -1 ) {  // 如果失败，尝试真随机数设备
            // 如果都失败，使用备用方案
        fail:
            LOG(MOD_SYS|CRITICAL, "Unable to properly seed cryptographic random number generator!");
            ustime_t t = sys_time();             // 获取当前时间作为备用熵源
            for( int i=0; i<8 && i<len; i++, t>>=8 )  // 使用时间的各个字节
                seed[i] ^= t;                    // 异或操作混合时间数据
            uL_t p = (ptrdiff_t)seed;           // 使用指针地址作为额外熵源
            for( int i=8; i<16 && i <len; i++, p>>=8 )  // 使用指针地址的各个字节
                seed[i] ^= p;                    // 异或操作混合指针数据
            return;                              // 返回(使用了较弱的随机性)
        }
    }
    int n = read(fd, seed, len);                // 从随机数设备读取数据
    if( n != len )                              // 如果读取的长度不符合要求
        goto fail;                              // 跳转到备用方案
    close(fd);                                  // 关闭文件描述符
}


// 函数功能：微秒级精度的睡眠函数
// 参数说明：us - 睡眠时间(微秒)
// 返回值：无
// 调用时机：需要精确延时时调用，可被信号中断后继续睡眠
void sys_usleep (sL_t us) {
    if( us <= 0 )                               // 如果睡眠时间小于等于0
        return;                                 // 直接返回，不睡眠
    struct timespec slp, rem = { .tv_sec = us/1000000, .tv_nsec = us%1000000*1000 }; // 转换为秒和纳秒
    while( rem.tv_sec > 0 || rem.tv_nsec > 0 ) {  // 循环直到睡眠时间用完
        slp = rem;                              // 设置本次睡眠时间
        if( nanosleep(&slp, &rem) == 0 )        // 调用nanosleep，rem返回剩余时间
            break;                              // 如果成功完成睡眠则退出循环
        // 如果被信号中断，rem包含剩余时间，继续循环睡眠
    }
}


// 函数功能：获取单调递增的系统时间(微秒)
// 参数说明：无
// 返回值：自系统启动以来的微秒数(单调时间，不受系统时间调整影响)
// 调用时机：需要测量时间间隔或设置定时器时调用
sL_t sys_time () {
    struct timespec tp;                          // 时间结构体
    int err = clock_gettime(CLOCK_MONOTONIC, &tp);  // 获取单调时钟时间
    if( err == -1 )                             // 如果获取时间失败
        rt_fatal("clock_gettime(2) failed: %s\n", strerror(errno));      // 致命错误退出
    return tp.tv_sec*(sL_t)1000000 + tp.tv_nsec/1000;  // 转换为微秒并返回
}

// 函数功能：获取UTC时间(微秒)
// 参数说明：无
// 返回值：自1970年1月1日以来的微秒数(UTC时间戳)
// 调用时机：需要获取绝对时间或与外部系统同步时间时调用
sL_t sys_utc () {
    struct timespec tp;                          // 时间结构体
    int err = clock_gettime(CLOCK_REALTIME, &tp);  // 获取实时时钟时间
    if( err == -1 )                             // 如果获取时间失败
        rt_fatal("clock_gettime(2) failed: %s\n", strerror(errno));      // 致命错误退出
    return (tp.tv_sec*(sL_t)1000000 + tp.tv_nsec/1000);  // 转换为微秒并返回
}


// 函数功能：获取软件包版本信息
// 参数说明：无
// 返回值：版本字符串(从version.txt文件读取)
// 调用时机：系统初始化时调用，用于日志输出和版本显示
str_t sys_version () {
    return readFileAsString("version", ".txt", &versionTxt);  // 读取version.txt文件内容作为版本信息
}

/* 固件更新功能实现 ************************************ */

// 函数功能：开始固件更新过程，创建临时更新文件
// 参数说明：len - 更新数据长度(0表示取消更新)
// 返回值：无
// 调用时机：接收到固件更新请求时调用
void sys_updateStart (int len) {
    close(updfd);                               // 关闭可能已打开的更新文件描述符
    if( len == 0 ) {                           // 如果长度为0表示取消更新
        updfd = -1;                            // 设置文件描述符为无效
        return;                                // 直接返回
    }
    makeFilepath("/tmp/update", ".bi_", &temp_updfile, 0);  // 构造临时更新文件路径
    updfd = open(temp_updfile, O_CREAT|O_TRUNC|O_WRONLY, S_IRUSR|S_IWUSR|S_IXUSR|S_IRGRP|S_IXGRP);  // 创建临时文件
    if( updfd == -1 )                          // 如果文件创建失败
        LOG(MOD_SYS|ERROR, "Failed to open '%s': %s", temp_updfile, strerror(errno));  // 记录错误日志
}

// 函数功能：写入固件更新数据到临时文件
// 参数说明：data - 固件数据缓冲区指针
//          off - 数据偏移量(当前未使用)
//          len - 数据长度
// 返回值：无
// 调用时机：接收到固件数据块时调用，逐步写入更新文件
void sys_updateWrite (u1_t* data, int off, int len) {
    if( updfd == -1 ) return;                   // 如果文件描述符无效则直接返回
    int err = write(updfd, data, len);          // 写入数据到临时文件
    if( err == -1 ) {                          // 如果写入失败
        LOG(MOD_SYS|ERROR, "Failed to write '%s': %s", temp_updfile, strerror(errno));  // 记录错误日志
        close(updfd);                          // 关闭文件描述符
        updfd = -1;                            // 设置为无效状态
    }
}

// 函数功能：提交固件更新，将临时文件重命名为正式更新文件
// 参数说明：len - 更新数据总长度(0表示取消提交)
// 返回值：成功返回1，失败返回0
// 调用时机：固件数据传输完成后调用，确认提交更新
int sys_updateCommit (int len) {
    // 重命名文件并准备启动更新进程
    if( len == 0 )                             // 如果长度为0表示取消提交
        return 1;                              // 直接返回成功
    if( updfd == -1 ) {                        // 如果文件描述符无效(写入失败)
        if( temp_updfile )                     // 如果临时文件存在
            unlink(temp_updfile);              // 删除临时文件
        return 0;                              // 返回失败
    }
    close(updfd);                              // 关闭临时文件
    sync();                                    // 同步文件系统，确保数据写入磁盘
    updfd = -1;                                // 重置文件描述符
    makeFilepath("/tmp/update", ".bin", &updfile, 0);  // 构造正式更新文件路径
    if( rename(temp_updfile, updfile) == -1 ) { // 将临时文件重命名为正式更新文件
        LOG(MOD_SYS|ERROR, "Rename of update file failed '%s': %s", temp_updfile, strerror(errno));  // 记录重命名失败错误
    }
    sync();                                    // 再次同步文件系统
    return 1;                                  // 返回成功
}


// 函数功能：运行固件更新程序
// 参数说明：无
// 返回值：无
// 调用时机：系统启动时调用，检查并执行待处理的固件更新
void sys_runUpdate () {
    makeFilepath("/tmp/update", ".bin", &updfile, 0);  // 构造更新文件路径
    if( access(updfile, X_OK) != 0 )           // 检查更新文件是否存在且可执行
        return; // 文件不存在或不可执行，直接返回
    str_t argv[2] = { updfile, NULL };         // 构造执行参数数组
    sys_execCommand(0, argv);  // 分离模式执行更新程序，不等待更新完成
}

// 函数功能：中止固件更新，删除更新文件
// 参数说明：无
// 返回值：无
// 调用时机：需要取消固件更新时调用
void sys_abortUpdate () {
    unlink("/tmp/update.bin");                 // 删除更新文件
    sync();                                    // 同步文件系统
}

// 函数功能：运行无线电硬件初始化脚本
// 参数说明：device - 无线电设备路径
// 返回值：成功返回1，失败返回0
// 调用时机：无线电硬件初始化前调用，执行用户自定义的初始化脚本
int sys_runRadioInit (str_t device) {
    setenv("LORAGW_SPI", device, 1);           // 为libloragw库设置SPI设备环境变量
    if( !radioInit )                           // 如果没有配置初始化脚本
        return 1;                              // 直接返回成功
    char buf[16];                              // 从进程索引字符串缓冲区
    str_t argv[4] = { radioInit, device, NULL, NULL };  // 构造执行参数数组
    if( sys_slaveIdx >= 0 ) {                  // 如果是从进程模式
        snprintf(buf, sizeof(buf), "%d", sys_slaveIdx);  // 格式化从进程索引
        argv[2] = buf;                         // 添加从进程索引作为第三个参数
    }
    return sys_execCommand(RADIO_INIT_WAIT, argv) == 0;  // 执行初始化脚本并等待完成
}


// 函数功能：执行外部命令，支持同步和异步模式
// 参数说明：max_wait - 最大等待时间(微秒，0表示分离模式不等待)
//          argv - 命令参数数组(以NULL结尾)
// 返回值：成功返回0，失败返回负数或进程退出码
// 调用时机：需要执行外部脚本、程序或shell命令时调用
int sys_execCommand (ustime_t max_wait, str_t* argv) {
    int argc = 0;                              // 计算参数个数
    while( argv[argc] ) argc++;                // 遍历参数数组直到NULL
    if( argc == 0 || (argc==1 && argv[0][0]==0) )  // 如果没有参数或第一个参数为空
        return 0;                              // 直接返回成功
    sys_flushLog();                            // 刷新日志缓冲区
    pid_t pid1;                                // 第一个子进程ID
    if( (pid1 = fork()) == 0 ) {              // 创建子进程
        pid_t pid2 = 0;                        // 第二个子进程ID(用于分离模式)
        if( max_wait!=0 || (pid2 = fork()) == 0 ) {  // 如果不是分离模式或创建孙进程成功
            if( access(argv[0], X_OK) != 0 ) { // 如果第一个参数不是可执行文件
                // 不是可执行文件，需要通过shell执行
                str_t* argv2 = rt_mallocN(str_t, argc+4);  // 分配新的参数数组
                memcpy(&argv2[3], &argv[0], sizeof(argv[0])*(argc+1)); // 复制原参数(包括NULL)
                if( access(argv[0], F_OK) == -1 ) {  // 如果文件不存在
                    // 假设是shell语句
                    argv2[0] = "/bin/sh";          // 使用sh执行
                    argv2[1] = "-c";               // 命令模式
                    argv2[2] = argv[0];            // 要执行的命令
                    argv = argv2;                  // 更新参数数组
                } else {                           // 如果文件存在但不可执行
                    // 假设是shell脚本文件
                    argv2[1] = "/bin/bash";        // 使用bash执行
                    argv2[2] = argv[0];            // 脚本文件路径
                    argv = &argv2[1];              // 更新参数数组(跳过第一个元素)
                }
            }
            for( int i=0; argv[i]; i++ )           // 调试输出所有参数
                LOG(MOD_SYS|DEBUG, "%s argv[%d]: <%s>\n", i==0?"execvp":"      ", i, argv[i]);
            log_flushIO();                         // 刷新日志输出

            if( execvp(argv[0], (char*const*)argv) == -1 ) {  // 执行程序
                LOG(MOD_SYS|ERROR, "%s: Failed to exec: %s", argv[0], strerror(errno));  // 记录执行失败
                log_flushIO();                     // 刷新日志
                exit(9);                           // 以错误码9退出
            }
        } else if( pid2 < 0 ) {                   // 如果创建孙进程失败
            LOG(MOD_SYS|ERROR, "%s: Fork(2) failed: %s", argv[0], strerror(errno));  // 记录fork失败
            log_flushIO();                         // 刷新日志
            exit(8);                               // 以错误码8退出
        }
        exit(0);                                   // 子进程正常退出
    }
    if( pid1 < 0 ) {                              // 如果创建子进程失败
        LOG(MOD_SYS|ERROR, "%s: Fork failed: %s", argv[0], strerror(errno));  // 记录fork失败
        return -1;                                 // 返回失败
    }
    LOG(MOD_SYS|VERBOSE, "%s: Forked, waiting...", argv[0]);  // 记录进程创建成功
    log_flushIO();                                 // 刷新日志
    int wmode = WNOHANG;                          // 非阻塞等待模式
    if( max_wait == 0 ) {                         // 如果是分离模式
        max_wait = USTIME_MAX;                    // 设置为最大等待时间(基本上是永远)
        wmode = 0;                                // 阻塞等待模式
    }
    for( ustime_t u=0; u < max_wait; u+=rt_millis(1) ) {  // 循环等待进程结束
        int status = 0;                           // 进程状态
        int err = waitpid(pid1, &status, wmode);  // 等待子进程
        if( err == -1 ) {                         // 如果waitpid失败
            LOG(MOD_SYS|ERROR, "Process %s (pid=%d) - waitpid failed: %s", argv[0], pid1, strerror(errno));
            return -1;                            // 返回失败
        }
        if( err == pid1 ) {                       // 如果子进程已结束
            if( WIFEXITED(status) ) {             // 如果是正常退出
                int xcode = WEXITSTATUS(status);  // 获取退出码
                if( xcode == 0 ) {                // 如果退出码为0(成功)
                    LOG(MOD_SYS|INFO, "Process %s (pid=%d) completed", argv[0], pid1);  // 记录成功完成
                    log_flushIO();                // 刷新日志
                    return 0;                     // 返回成功
                }
                LOG(MOD_SYS|ERROR, "Process %s (pid=%d) failed with exit code %d", argv[0], pid1, xcode);  // 记录失败
                return xcode;                     // 返回退出码
            }
            if( WIFSIGNALED(status) ) {           // 如果是被信号终止
                int signo = WTERMSIG(status);     // 获取信号编号
                LOG(MOD_SYS|ERROR, "Process %s (pid=%d) terminated by signal %d", argv[0], pid1, signo);  // 记录信号终止
                return -2;                        // 返回信号终止错误码
            }
            LOG(MOD_SYS|ERROR, "Process %s (pid=%d) with strange exit state 0x%X", argv[0], pid1, status);  // 记录异常状态
            return -4;                            // 返回异常状态错误码
        }
        rt_usleep(rt_millis(2));                  // 等待2毫秒后继续检查
    }
    kill(pid1, SIGTERM);                          // 超时后发送SIGTERM信号终止进程
    LOG(MOD_SYS|ERROR, "Process %s (pid=%d) did not terminate within %ldms - killing it (SIGTERM)",
        argv[0], pid1, max_wait/1000);            // 记录超时终止
    return -3;                                    // 返回超时错误码
}

// 函数功能：设置日志级别
// 参数说明：arg - 日志级别字符串(如"DEBUG"、"INFO"等)
//          source - 设置来源描述(用于错误信息)
// 返回值：成功返回1，失败返回0
// 调用时机：解析配置文件或命令行参数时调用
static int setLogLevel (str_t arg, str_t source) {
    str_t err = log_parseLevels(arg);          // 解析日志级别字符串
    if( err  ) {                               // 如果解析失败
        int n = strlen(err);                   // 获取错误信息长度
        fprintf(stderr, "%s: Failed to parse log level: %.*s%s\n", source, 8, err, n>8 ? ".." : "");  // 输出错误信息(最多8个字符)
        return 0;                              // 返回失败
    }
    return 1;                                  // 返回成功
}

// 函数功能：设置日志文件路径和参数
// 参数说明：logdef - 日志文件定义字符串(格式：path[,size[,rotate]])
//          source - 设置来源描述(用于错误信息)
// 返回值：成功返回1，失败返回0
// 调用时机：解析配置文件或命令行参数时调用
static int setLogFile (str_t logdef, str_t source) {
    // 覆盖内置默认设置
    if( strcmp(logdef, "stderr") == 0 || strcmp(logdef, "-") == 0 ) {  // 如果指定输出到stderr
        logfile.path = NULL;                   // 设置路径为NULL(表示stderr)
        return 1;                              // 返回成功
    }
    free((void*)logfile.path);                 // 释放旧的日志文件路径
    str_t spec = strchr(logdef,',');           // 查找逗号分隔符
    if( spec != NULL ) {                       // 如果有逗号(包含大小和轮转参数)
        str_t path = rt_strdupn(logfile.path, spec-logfile.path);  // 提取文件路径部分
        logfile.path = sys_makeFilepath(path,1);  // 构造完整文件路径
        rt_free((void*)path);                  // 释放临时路径字符串
        sL_t logsz = rt_readDec((str_t*)&spec);  // 解析日志文件大小
        if( logsz > 0 )                        // 如果大小有效
            logfile.size = min(max(logsz, 10000), (sL_t)100e6);  // 限制在10KB到100MB之间
        if( spec[0] == ',' ) {                 // 如果还有第二个逗号(轮转参数)
            int logrot = rt_readDec((str_t*)&spec);  // 解析轮转文件数量
            if( logrot > 0 )                   // 如果轮转数量有效
                logfile.rotate = min(max(logrot, 0), (sL_t)100);  // 限制在0到100之间
        }
        if( !spec[0] ) {                       // 如果解析后字符串不为空(格式错误)
            fprintf(stderr, "%s: Illegal log file spec: %s\n", source, logdef);  // 输出格式错误
            return 0;                          // 返回失败
        }
    } else {                                   // 如果没有逗号(只有文件路径)
        logfile.path = sys_makeFilepath(logdef,0);  // 构造完整文件路径
    }
    return 1;                                  // 返回成功
}

// 函数功能：解析station.conf配置文件
// 参数说明：无
// 返回值：成功返回1，失败返回0
// 调用时机：系统启动时调用，读取并解析基站配置文件
static int parseStationConf () {
    str_t filename = "station.conf";           // 配置文件名
    dbuf_t jbuf = sys_readFile(filename);      // 读取配置文件内容
    if( jbuf.buf == NULL ) {                   // 如果文件读取失败
        LOG(MOD_SYS|ERROR, "No such file (or not readable): %s", filename);  // 记录错误
        return 0;                              // 返回失败
    }
    ujdec_t D;                                 // JSON解码器
    uj_iniDecoder(&D, jbuf.buf, jbuf.bufsize); // 初始化JSON解码器
    if( uj_decode(&D) ) {                      // 如果JSON解析失败
        LOG(MOD_SYS|ERROR, "Parsing of JSON failed - '%s' ignored", filename);  // 记录解析失败
        free(jbuf.buf);                        // 释放文件内容缓冲区
        return 0;                              // 返回失败
    }
    u1_t ccaDisabled=0, dcDisabled=0, dwellDisabled=0;   // 调试功能禁用标志(字段不存在时为0)
    ujcrc_t field;                             // JSON字段标识符
    uj_enterObject(&D);                        // 进入JSON根对象
    while( (field = uj_nextField(&D)) ) {      // 遍历根对象的所有字段
        switch(field) {                        // 根据字段类型处理
        case J_station_conf: {                 // 如果是station_conf字段
            uj_enterObject(&D);                // 进入station_conf对象
            while( (field = uj_nextField(&D)) ) {  // 遍历station_conf对象的所有字段
                switch(field) {                // 根据字段类型处理配置项
                case J_routerid: {             // 路由器ID(基站EUI)
                    if( !setEui(uj_str(&D), filename) )  // 设置基站EUI
                        uj_error(&D, "Illegal EUI");  // 如果EUI格式错误则报错
                    break;
                }
                case J_euiprefix: {            // EUI前缀配置
                    str_t err = parseEui(uj_str(&D), 0, &prefixEUI, 0);  // 解析EUI前缀
                    if( err != NULL )          // 如果解析失败
                        uj_error(&D, "Illegal EUI: %s", err);  // 报告错误
                    free((void*)prefixEuiSrc); // 释放旧的来源字符串
                    prefixEuiSrc = rt_strdup(filename);  // 设置新的来源为配置文件
                    break;
                }
                case J_log_file: {             // 日志文件配置
                    if( !setLogFile(uj_str(&D), filename) )  // 设置日志文件
                        uj_error(&D, "Illegal log file spec: %s", D.str.beg);  // 格式错误则报错
                    break;
                }
                case J_log_size: {             // 日志文件大小限制
                    logfile.size = uj_num(&D); // 设置日志文件最大大小
                    break;
                }
                case J_log_rotate: {           // 日志文件轮转数量
                    logfile.rotate = uj_int(&D);  // 设置日志轮转文件数量
                    break;
                }
                case J_log_level: {            // 日志级别配置
                    if( !setLogLevel(uj_str(&D), filename) )  // 设置日志级别
                        uj_error(&D, "Illegal log level: %s", D.str.beg);  // 级别错误则报错
                    break;
                }
                case J_gps: {                  // GPS设备路径配置
                    makeFilepath(uj_str(&D),"",&gpsDevice,0);  // 构造GPS设备文件路径
                    break;
                }
                case J_pps: {                  // PPS(脉冲每秒)模式配置
                    str_t mode = uj_str(&D);   // 获取PPS模式字符串
                    if( strcmp(mode,"gps") == 0 ) {        // GPS PPS模式
                        sys_modePPS = PPS_GPS;
                    }
                    else if( strcmp(mode,"fuzzy") == 0 ) { // 模糊PPS模式
                        sys_modePPS = PPS_FUZZY;
                    }
                    else if( strcmp(mode,"testpin") == 0 ) {  // 测试引脚PPS模式
                        sys_modePPS = PPS_TESTPIN;
                    }
                    else {                     // 未知PPS模式
                        uj_error(&D, "Illegal pps mode: %s", mode);  // 报告错误
                    }
                    break;
                }
                case J_radio_init: {           // 无线电初始化脚本配置
                    free((void*)radioInit);    // 释放旧的初始化脚本路径
                    radioInit = rt_strdup(uj_str(&D));  // 设置新的初始化脚本路径
                    radioInitSrc = filename;   // 记录配置来源
                    break;
                }
#if defined(CFG_prod)                         // 生产版本编译时
                case J_nocca:                  // 禁用CCA(载波侦听)
                case J_nodc:                   // 禁用占空比控制
                case J_nodwell:                // 禁用驻留时间限制
                case J_device_mode: {          // 设备模式
                    LOG(MOD_S2E|WARNING, "Feature not supported in production level code (station.conf) - ignored: %s", D.field.name);  // 生产版本不支持这些调试功能
                    uj_skipValue(&D);          // 跳过该字段值
                    break;
                }
#else // !defined(CFG_prod)                   // 开发版本编译时
                case J_nocca: {                // 禁用CCA配置
                    ccaDisabled = uj_bool(&D) ? 2 : 1;  // 设置CCA禁用标志(2=禁用,1=启用)
                    break;
                }
                case J_nodc: {                 // 禁用占空比控制配置
                    dcDisabled = uj_bool(&D) ? 2 : 1;   // 设置占空比控制禁用标志
                    break;
                }
                case J_nodwell: {              // 禁用驻留时间限制配置
                    dwellDisabled = uj_bool(&D) ? 2 : 1;  // 设置驻留时间限制禁用标志
                    break;
                }
                case J_device_mode: {          // 设备模式配置
                    sys_deviceMode = uj_bool(&D) ? 1 : 0;  // 设置设备模式标志
                    break;
                }
#endif // !defined(CFG_prod)
                case J_device: {               // 无线电设备路径配置
                    free((void*)radioDevice);  // 释放旧的设备路径
                    radioDevice = rt_strdup(uj_str(&D));  // 设置新的设备路径
                    break;
                }
                case J_web_port: {             // Web服务端口配置
                    sys_webPort = uj_intRange(&D, 1, 65535);  // 设置Web端口(1-65535)
                    break;
                }
                case J_web_dir: {              // Web文件目录配置
                    setWebDir(uj_str(&D), filename);  // 设置Web文件目录
                    break;
                }
                default: {                     // 未知字段处理
                    dbuf_t b = uj_skipValue(&D);  // 跳过字段值并获取其内容
                    int err = s2conf_set(filename, D.field.name, rt_strdupn(b.buf, b.bufsize));  // 尝试设置为通用配置
                    if( err == -1 )            // 如果设置失败
                        LOG(MOD_SYS|WARNING, "Ignoring field: %s", D.field.name);  // 记录忽略的字段
                    break;
                }
                }
            }
            uj_exitObject(&D);                // 退出station_conf对象
            break;
        }
        default: {                            // 根对象中的其他字段
            uj_skipValue(&D);                 // 跳过未知字段
            break;
        }
        }
    }
    uj_exitObject(&D);                        // 退出JSON根对象
    uj_assertEOF(&D);                         // 确认已到达文件末尾
    free(jbuf.buf);                           // 释放文件内容缓冲区
    if( ccaDisabled   ) s2e_ccaDisabled   = ccaDisabled   & 2;  // 应用CCA禁用设置
    if( dcDisabled    ) s2e_dcDisabled    = dcDisabled    & 2;  // 应用占空比控制禁用设置
    if( dwellDisabled ) s2e_dwellDisabled = dwellDisabled & 2;  // 应用驻留时间限制禁用设置
    return 1;                                 // 返回成功
}


// 命令行选项结构体 - 存储解析后的命令行参数
static struct opts {
    str_t logLevel;    // 日志级别设置
    str_t logFile;     // 日志文件路径设置
    str_t homeDir;     // 基站主目录路径
    str_t tempDir;     // 临时文件目录路径
    str_t radioInit;   // 无线电初始化脚本路径
    str_t euiprefix;   // EUI前缀设置
    int   slaveMode;   // 从进程模式标志
    str_t slaveExec;   // 从进程执行命令模板
    u1_t  params;      // 打印参数标志
    u1_t  daemon;      // 守护进程模式标志
    u1_t  force;       // 强制模式标志(杀死旧进程)
    u1_t  kill;        // 仅杀死进程标志
    u1_t  notc;        // 禁用TC(Traffic Control)标志
} *opts;


static struct argp_option options[] = {
    { "log-file", 'L', "FILE[,SIZE[,ROT]]", 0,
      ("Write log entries to FILE. If FILE is '-' then write to stderr. "
       "Optionally followed by a max file SIZE and a number of rotation files. "
       "If ROT is 0 then keep only FILE. If ROT is 1 then keep one more old "
       "log file around. "
       "Overrides environment STATION_LOGFILE.")
    },
    { "log-level", 'l', "LVL|0..7", 0,
      ("Set a log level LVL=#loglvls# or use a numeric value. "
       "Overrides environment STATION_LOGLEVEL.")
    },
    { "home", 'h', "DIR", 0,
      ("Home directory for configuration files. "
       "Default is the current working directory. "
       "Overrides environment STATION_DIR.")
    },
    { "temp", 't', "DIR", 0,
      ("Temp directory for frequently written files. "
       "Default is /tmp. "
       "Overrides environment STATION_TEMPDIR.")
    },
    { "radio-init", 'i', "cmd", 0,
      ("Program/script to run before reinitializing radio hardware. "
       "By default nothing is being executed. "
       "Overrides environment STATION_RADIOINIT.")
    },
    { "eui-prefix", 'x', "id6", 0,
      ("Turn MAC address into EUI by adding this prefix. If the argument has value "
       "ff:fe00:0 then the EUI is formed by inserting FFFE in the middle. "
       "If absent use MAC or routerid as is. "
       "Overrides environment STATION_EUIPREFIX.")
    },
    { "params", 'p', NULL, 0,
      ("Print current parameter settings.")
    },
    { "version", 'v', NULL, 0,
      ("Print station version."),
    },
    { "daemon", 'd', NULL, 0,
      ("First check if another process is still alive. If so do nothing and exit. "
       "Otherwise fork a worker process to operate the radios and network protocols. "
       "If the subprocess died respawn it with an appropriate back off.")
    },
    { "force", 'f', NULL, 0,
      ("If a station process is already running, kill it before continuing with requested operation mode.")
    },
    { "kill", 'k', NULL, 0,
      ("Kill a currently running station process.")
    },
    { "no-tc", 'N', NULL, 0,
      ("Do not connect to a LNS. Only run CUPS functionality.")
    },
    { "slave", 'S', NULL, OPTION_HIDDEN,
      ("Station process is slave to a master process. For internal use only."),
    },
    { "exec", 'X', "CMD", OPTION_HIDDEN,
      ("Template for exec of slave processes. For internal/test use only."),
    },
    { "selftests", 256, NULL, OPTION_HIDDEN,
      ("If compiled with builtin selftests run them. For internal/test use only."),
    },
    { "fscmd", 257, "cmdline", OPTION_HIDDEN,
      ("Run a command on the simulated flash."),
    },
    { "fskey", 258, "hex", OPTION_HIDDEN,
      ("Specify an encryption key for the simulated flash."),
    },
    { "fscd",  259, "dir", OPTION_HIDDEN,
      ("Specify an current working dir for the simulated flash."),
    },
    { 0 }
};


// 函数功能：命令行参数解析回调函数，处理各种命令行选项
// 参数说明：key - 选项键值(字符或数字)
//          arg - 选项参数值(如果有)
//          state - argp解析状态结构体
// 返回值：成功返回0，未知选项返回ARGP_ERR_UNKNOWN
// 调用时机：argp_parse()解析每个命令行选项时调用
static int parse_opt (int key, char* arg, struct argp_state* state) {
    switch(key) {                                 // 根据选项键值进行处理
    case 259: {                                   // --fscd选项：设置模拟文件系统当前目录(隐藏选项)
        int err = fs_chdir(arg);                  // 切换模拟文件系统目录
        if( err != 0 ) {                          // 如果切换失败
            fprintf(stderr, "Failed --fscd: %s\n", strerror(errno));  // 输出错误信息
            exit(8);                              // 以错误码8退出
        }
        return 0;                                 // 返回成功
    }
    case 258: {                                   // --fskey选项：设置模拟文件系统加密密钥(隐藏选项)
        u4_t key[4] = {0};                        // 128位密钥数组(4个32位整数)
        for( int ki=0; ki<16; ki++ ) {            // 解析32个十六进制字符(16字节)
            int b = (rt_hexDigit(arg[2*ki])<<4) | rt_hexDigit(arg[2*ki+1]);  // 解析两个十六进制字符为一个字节
            if( b < 0 ) {                         // 如果解析失败(非法十六进制字符)
                fprintf(stderr, "Illegal --fskey argument - expecting 32 hex digits\n");  // 输出错误信息
                exit(7);                          // 以错误码7退出
            }
            key[ki/4] |= b<<(24 - ki%4*8);        // 将字节存入密钥数组(大端序)
        }
        fs_ini(key);                              // 使用密钥初始化模拟文件系统
        return 0;                                 // 返回成功
    }
    case 257: {                                   // --fscmd选项：在模拟文件系统上运行命令(隐藏选项)
        fs_ini(NULL);                             // 初始化模拟文件系统(无加密)
        exit(fs_shell(arg));                      // 运行文件系统shell命令并退出
    }
    case 256: {                                   // --selftests选项：启用内置自测试(隐藏选项)
        setenv("STATION_SELFTESTS", "1", 1);      // 设置自测试环境变量
        return 0;                                 // 返回成功
    }
    case 'S': {                                   // -S选项：从进程模式(隐藏选项，内部使用)
        opts->slaveMode = 1;                      // 设置从进程模式标志
        return 0;                                 // 返回成功
    }
    case 'X': {                                   // -X选项：从进程执行命令模板(隐藏选项)
        free((void*)sys_slaveExec);               // 释放旧的执行命令模板
        sys_slaveExec = rt_strdup(arg);           // 设置新的执行命令模板
        return 0;                                 // 返回成功
    }
    case 'x': {                                   // -x选项：--eui-prefix，设置EUI前缀
        opts->euiprefix = arg;                    // 保存EUI前缀参数
        return 0;                                 // 返回成功
    }
    case 'l': {                                   // -l选项：--log-level，设置日志级别
        opts->logLevel = arg;                     // 保存日志级别参数
        return 0;                                 // 返回成功
    }
    case 'L': {                                   // -L选项：--log-file，设置日志文件
        opts->logFile = arg;                      // 保存日志文件参数
        return 0;                                 // 返回成功
    }
    case 'h': {                                   // -h选项：--home，设置主目录
        opts->homeDir = arg;                      // 保存主目录参数
        return 0;                                 // 返回成功
    }
    case 't': {                                   // -t选项：--temp，设置临时目录
        opts->tempDir = arg;                      // 保存临时目录参数
        return 0;                                 // 返回成功
    }
    case 'i': {                                   // -i选项：--radio-init，设置无线电初始化脚本
        opts->radioInit = arg;                    // 保存无线电初始化脚本参数
        return 0;                                 // 返回成功
    }
    case 'p': {                                   // -p选项：--params，打印当前参数设置
        opts->params = 1;                         // 设置打印参数标志
        return 0;                                 // 返回成功
    }
    case 'd': {                                   // -d选项：--daemon，守护进程模式
        opts->daemon = 1;                         // 设置守护进程模式标志
        return 0;                                 // 返回成功
    }
    case 'f': {                                   // -f选项：--force，强制模式(杀死旧进程)
        opts->force = 1;                          // 设置强制模式标志
        return 0;                                 // 返回成功
    }
    case 'k': {                                   // -k选项：--kill，杀死当前运行的基站进程
        opts->kill = 1;                           // 设置杀死进程标志
        return 0;                                 // 返回成功
    }
    case 'N': {                                   // -N选项：--no-tc，禁用TC(Traffic Control)
        opts->notc = 1;                           // 设置禁用TC标志
        return 0;                                 // 返回成功
    }
    case 'v': {                                   // -v选项：--version，打印版本信息
        fputs("Station: " CFG_version " " CFG_bdate "\n", stdout);  // 输出基站版本和构建日期
        readFileAsString("version", ".txt", &versionTxt);  // 读取软件包版本文件
        fprintf(stdout, "Package: %s\n", versionTxt);     // 输出软件包版本
        exit(0);                                  // 打印版本后退出程序
    }
    case ARGP_KEY_END: {                          // 参数解析结束标志
        return 0;                                 // 返回成功，继续程序执行
    }
    case ARGP_KEY_ARG: {                          // 非选项参数(位置参数)
        break;                                    // 跳出switch，返回未知错误(基站不接受位置参数)
    }
    }
    return ARGP_ERR_UNKNOWN;                      // 返回未知选项错误
}

struct argp argp = { options, parse_opt, "", NULL, NULL, NULL, NULL };


// 函数功能：主进程启动第二阶段，初始化各种服务和功能模块
// 参数说明：tmr - 定时器指针(未使用)
// 返回值：无
// 调用时机：主进程启动200毫秒后调用，确保从进程已启动
static void startupMaster2 (tmr_t* tmr) {
#if !defined(CFG_no_rmtsh)                        // 如果未禁用远程shell功能
    rt_addFeature("rmtsh");                       // 添加远程shell功能特性
#endif
#if defined(CFG_prod)                             // 如果是生产版本编译
    rt_addFeature("prod");                        // 添加生产版本特性(某些开发/测试/调试功能不被接受)
#endif
    sys_enableCmdFIFO(makeFilepath("~/cmd",".fifo",NULL,0));  // 启用命令FIFO管道，用于外部命令控制
    if( gpsDevice ) {                             // 如果配置了GPS设备
        rt_addFeature("gps");                     // 添加GPS功能特性
        sys_enableGPS(gpsDevice);                 // 启用GPS设备和时间同步
    }
    sys_iniTC();                                  // 初始化TC(Traffic Control)流量控制模块
    sys_startTC();                                // 启动TC服务，开始与LNS(LoRaWAN Network Server)通信
    sys_iniCUPS();                                // 初始化CUPS(Configuration and Update Server)配置更新服务
    sys_triggerCUPS(0);                           // 触发CUPS服务，开始配置同步
    sys_iniWeb();                                 // 初始化Web管理界面服务
}

// 函数功能：主进程启动函数，执行基站核心初始化流程
// 参数说明：tmr - 定时器指针，用于延迟启动第二阶段
// 返回值：无
// 调用时机：主进程模式下由事件循环调用，或守护进程的子进程调用
static void startupMaster (tmr_t* tmr) {
    sys_startLogThread();                         // 启动日志线程，处理异步日志输出
    if( getenv("STATION_SELFTESTS") ) {           // 如果设置了自测试环境变量
        selftests();                              // 运行内置自测试程序
        // NOT REACHED                            // 自测试完成后程序会退出，不会返回
    }
    // 清理旧进程并创建PID文件记录当前进程ID
    writePid();                                   // 将当前进程ID写入PID文件
    // 如果有待处理的固件更新则运行它
    sys_runUpdate();                              // 检查并执行待处理的固件更新
    ral_ini();                                    // 初始化RAL(Radio Abstraction Layer)无线电抽象层
    atexit(leds_off);                             // 注册退出时的LED清理函数
    // 等待从进程启动完成
    //startupMaster2(tmr);                        // 直接调用第二阶段(已注释)
    rt_setTimerCb(tmr, rt_millis_ahead(200), startupMaster2);  // 延迟200毫秒后启动第二阶段，确保从进程已启动
}


// 前向声明
static void startupDaemon (tmr_t* tmr);

// 函数功能：等待工作进程状态，监控子进程是否异常退出
// 参数说明：tmr - 定时器指针，用于定期检查进程状态
// 返回值：无
// 调用时机：守护进程模式下定期调用，监控工作进程健康状态
// 注意：这里使用轮询而不是SIGCHLD信号，因为在aio中使用pselect的可移植性较差
// (例如FreeRTOS上的LWIP)。轮询不是问题，因为我们也希望减慢重启速度，
// 避免系统在紧密的重启循环中阻塞。
static void waitForWorker (tmr_t* tmr) {
    int wstatus;                                  // 进程退出状态
    pid_t wpid = waitpid(workerPid, &wstatus, WNOHANG);  // 非阻塞等待工作进程
    //NOT-NEEDED sys_inState(SYSIS_STATION_DEAD); // 不需要设置基站死亡状态
    if( wpid < 0 || wpid == workerPid ) {         // 如果waitpid失败或工作进程已退出
        LOG(MOD_SYS|ERROR, "DAEMON: Station process %d died (exit code 0x%X)", workerPid, wstatus);  // 记录进程死亡
        workerPid = 0;                            // 清除工作进程ID
        startupDaemon(&startupTmr);               // 重新启动守护进程流程
    } else {                                      // 如果工作进程仍在运行
        rt_setTimer(&startupTmr, rt_millis_ahead(500));  // 500毫秒后再次检查
    }
}


// 函数功能：守护进程启动函数，创建并监控工作进程
// 参数说明：tmr - 定时器指针，用于进程间协调
// 返回值：无
// 调用时机：守护进程模式下调用，负责创建和重启工作进程
static void startupDaemon (tmr_t* tmr) {
    int subprocPid;                               // 子进程ID
    // 重新生成基站工作进程
    sys_inState(SYSIS_STATION_DEAD);              // 设置基站状态为死亡(重启前)
    sys_flushLog();                               // 刷新日志缓冲区
    if( (subprocPid = fork()) == -1 )             // 创建子进程
        rt_fatal("DAEMON: Failed to fork station: %s", strerror(errno));  // fork失败则致命错误退出
    if( subprocPid == 0 ) {                       // 子进程分支
        // 子进程：基站工作进程
        sys_iniLogging(&logfile, 1);              // 重新初始化日志系统(子进程独立日志)
        LOG(MOD_SYS|INFO, "DAEMON: Station process %d started...", getpid());  // 记录工作进程启动
        rt_yieldTo(&startupTmr, startupMaster);   // 切换到主进程启动流程
    } else {                                      // 父进程分支
        // 父进程：守护进程
        workerPid = subprocPid;                   // 记录工作进程ID
        rt_yieldTo(&startupTmr, waitForWorker);   // 切换到工作进程监控流程
    }
}


// 函数功能：基站主入口函数，负责完整的系统初始化和启动流程
// 参数说明：argc - 命令行参数个数，argv - 命令行参数数组
// 返回值：成功返回0，失败返回错误码
// 调用时机：程序启动时由操作系统调用
int sys_main (int argc, char** argv) {
    // 因为我们在rt_ini()之前就开始记录日志...
    rt_utcOffset = sys_utc() - rt_getTime();     // 计算UTC时间偏移量，用于时间同步

    signal(SIGHUP,  SIG_IGN);                    // 忽略SIGHUP信号(终端断开)
    signal(SIGINT,  handle_signal);              // 注册SIGINT信号处理函数(Ctrl+C)
    signal(SIGTERM, handle_signal);              // 注册SIGTERM信号处理函数(终止信号)

    char cwd[MAX_FILEPATH_LEN];                  // 当前工作目录缓冲区
    if( getcwd(cwd, sizeof(cwd)) != NULL )       // 获取当前工作目录
        fs_chdir(cwd);                           // 设置文件系统当前目录

    s2conf_ini();                                // 初始化S2配置系统
    logfile.size = LOGFILE_SIZE;                 // 设置默认日志文件大小
    logfile.rotate = LOGFILE_ROTATE;             // 设置默认日志轮转数量
    setHomeDir(".", "builtin");                  // 设置默认主目录为当前目录
    // setWebDir("./web", "builtin");            // 设置默认Web目录(已注释)
    setTempDir(access("/var/tmp", W_OK) < 0 ? "/tmp" : "/var/tmp", "builtin");  // 设置临时目录(优先使用/var/tmp)
    prefixEuiSrc = rt_strdup("builtin");         // 设置EUI前缀来源为内置
    findDefaultEui();                            // 查找默认EUI(从网络接口MAC地址)

    opts = rt_malloc(struct opts);               // 分配命令行选项结构体内存
    int err = argp_parse (&argp, argc, argv, 0, NULL, NULL);  // 解析命令行参数
    if( err != 0 )                               // 如果参数解析失败
        return err;                              // 返回错误码

#if defined(CFG_ral_master_slave)                   // 如果编译时启用了主从进程模式
    int slave_rdfd = -1, slave_wrfd = -1;           // 从进程读写文件描述符
    if( opts->slaveMode ) {                         // 如果是从进程模式
        str_t const* sn = SLAVE_ENVS;               // 从进程环境变量名称数组
        while( *sn ) {                              // 遍历所有必需的环境变量
            str_t sv = getenv(*sn);                 // 获取环境变量值
            if( sv == NULL )                        // 如果环境变量不存在
                rt_fatal("Missing mandatory env var: %s", *sn);  // 致命错误：缺少必需的环境变量
            str_t sve = sv;                         // 保存环境变量值指针
            sL_t v = rt_readDec(&sve);             // 解析环境变量为数字
            if( v < 0 )                            // 如果解析失败
                rt_fatal("Env var %s has illegal value: %s", *sn, sv);  // 致命错误：环境变量值非法
            switch(sn[0][6]) {                     // 根据环境变量名的第7个字符判断类型
            case 'I': log_setSlaveIdx(sys_slaveIdx = v); break;  // SLAVE_IDX：设置从进程索引
            case 'R': slave_rdfd = v; break;       // SLAVE_RDFD：设置读文件描述符
            case 'W': slave_wrfd = v; break;       // SLAVE_WRFD：设置写文件描述符
            }
            sn++;                                  // 移动到下一个环境变量名
        }
    }
    if( sys_slaveExec == NULL ) {                  // 如果没有设置从进程执行命令模板
        sys_slaveExec = rt_strdup("/proc/self/exe -S");  // 使用默认模板(当前程序路径 + -S参数)
    }
#endif // defined(CFG_ral_master_slave)

    {   // EUI前缀配置处理块
        str_t prefix = opts->euiprefix;             // 获取命令行指定的EUI前缀
        str_t source = "--eui-prefix";              // 设置来源为命令行参数
        if( prefix == NULL ) {                      // 如果命令行没有指定EUI前缀
            source = "STATION_EUIPREFIX";           // 尝试从环境变量获取
            prefix = getenv(source);                // 读取环境变量值
        } else {                                    // 如果命令行指定了EUI前缀
            setenv("STATION_EUIPREFIX", prefix, 1); // 设置环境变量供子进程使用
        }
        if( prefix ) {                              // 如果有EUI前缀配置
            str_t err = parseEui(prefix, 0, & prefixEUI, 0);  // 解析EUI前缀
            if( err )                               // 如果解析失败
                rt_fatal("%s has illegal EUI value: %s", source, err);  // 致命错误退出
            free((void*)prefixEuiSrc);              // 释放旧的来源字符串
            prefixEuiSrc = rt_strdup(source);       // 设置新的来源信息
        }
    }
    // 临时目录配置处理：命令行参数优先于环境变量
    if( opts->tempDir ) {                           // 如果命令行指定了临时目录
        if( !setTempDir(opts->tempDir, "--temp") )  // 设置临时目录
            return 1;                               // 设置失败则退出
        setenv("STATION_TEMPDIR", opts->tempDir, 1);  // 设置环境变量供子进程使用
    } else {                                        // 如果命令行没有指定临时目录
        str_t source = "STATION_TEMPDIR";           // 尝试从环境变量获取
        str_t v = getenv(source);                   // 读取环境变量值
        if( v && !setTempDir(v, source) )           // 如果环境变量存在但设置失败
            return 1;                               // 退出程序
    }

    // 主目录配置处理：命令行参数优先于环境变量
    if( opts->homeDir ) {                           // 如果命令行指定了主目录
        if( !setHomeDir(opts->homeDir, "--home") )  // 设置主目录
            return 1;                               // 设置失败则退出
        setenv("STATION_HOME", opts->homeDir, 1);   // 设置环境变量供子进程使用
    } else {                                        // 如果命令行没有指定主目录
        str_t source = "STATION_HOME";              // 尝试从环境变量获取
        str_t v = getenv(source);                   // 读取环境变量值
        if( v && !setHomeDir(v, source) )           // 如果环境变量存在但设置失败
            return 1;                               // 退出程序
    }

    // 解析station.conf配置文件
    if( !parseStationConf() )                       // 解析配置文件
        return 1;                                   // 解析失败则退出
    if( opts->params ) {                            // 如果指定了打印参数选项
        s2conf_printAll();                          // 打印所有配置参数并退出
    }

    // 日志文件配置处理：命令行参数优先于环境变量
    if( opts->logFile ) {                           // 如果命令行指定了日志文件
        if( !setLogFile(opts->logFile, "--log-file") )  // 设置日志文件
            return 1;                               // 设置失败则退出
        setenv("STATION_LOGFILE", opts->logFile, 1);  // 设置环境变量供子进程使用
    } else {                                        // 如果命令行没有指定日志文件
        str_t source = "STATION_LOGFILE";           // 尝试从环境变量获取
        str_t v = getenv(source);                   // 读取环境变量值
        if( v && !setLogFile(v, source) )           // 如果环境变量存在但设置失败
            return 1;                               // 退出程序
    }
    // 无线电初始化脚本配置处理：命令行参数优先于环境变量
    if( opts->radioInit ) {                         // 如果命令行指定了无线电初始化脚本
        radioInitSrc = "--radio-init";              // 设置来源为命令行参数
        free((char*)radioInit);                     // 释放旧的脚本路径
        radioInit = rt_strdup(opts->radioInit);     // 设置新的脚本路径
        setenv("STATION_RADIOINIT", radioInit, 1);  // 设置环境变量供子进程使用
    } else {                                        // 如果命令行没有指定初始化脚本
        str_t s = "STATION_RADIOINIT";              // 尝试从环境变量获取
        str_t v = getenv(s);                        // 读取环境变量值
        if( v ) {                                   // 如果环境变量存在
            radioInitSrc = s;                       // 设置来源为环境变量
            free((char*)radioInit);                 // 释放旧的脚本路径
            radioInit = rt_strdup(v);               // 设置新的脚本路径
        }
    }
    // 日志级别配置处理：命令行参数优先于环境变量
    if( opts->logLevel ) {                          // 如果命令行指定了日志级别
        if( !setLogLevel(opts->logLevel, "--log-level") )  // 设置日志级别
            return 1;                               // 设置失败则退出
        setenv("STATION_LOGLEVEL", opts->logLevel, 1);  // 设置环境变量供子进程使用
    } else {                                        // 如果命令行没有指定日志级别
        str_t source = "STATION_LOGLEVEL";          // 尝试从环境变量获取
        str_t v = getenv(source);                   // 读取环境变量值
        if( v && !setLogLevel(v, source) )          // 如果环境变量存在但设置失败
            return 1;                               // 退出程序
    }
    {   // TLS调试级别配置处理
        str_t source = "STATION_TLSDBG";            // TLS调试级别环境变量
        str_t v = getenv(source);                   // 读取环境变量值
        if( v && (v[0]&0xF0) == '0' )              // 如果环境变量存在且是数字字符(0-9)
            tls_dbgLevel = v[0] - '0';              // 设置TLS调试级别
    }

    // 处理杀死进程选项
    if( opts->kill ) {                              // 如果指定了杀死进程选项
        if( opts->daemon || opts->force ) {         // 如果同时指定了守护进程或强制选项
            fprintf(stderr, "Option -k is incompatible with -d/-f\n");  // 输出选项冲突错误
            return 1;                               // 退出程序
        }
        killOldPid();                               // 杀死旧的基站进程
        return 0;                                   // 完成杀死操作后退出
    }
    sys_noTC = opts->notc;                          // 设置禁用TC标志

    // 保存选项值并释放选项结构体
    int daemon = opts->daemon;                      // 保存守护进程模式标志
    int force = opts->force;                        // 保存强制模式标志
    free(opts);                                     // 释放命令行选项结构体内存
    opts = NULL;                                    // 清空指针

#if defined(CFG_ral_master_slave)                   // 如果编译时启用了主从进程模式
    int isSlave = (sys_slaveIdx >= 0);              // 判断是否为从进程(从进程索引>=0)
#else
    int isSlave = 0;                                // 如果未启用主从模式，则不是从进程
#endif

    // 进程冲突检查和处理(仅对主进程)
    if( !isSlave ) {                                // 如果不是从进程
        if( !force ) {                              // 如果没有指定强制模式
            int pid = readPid();                    // 读取PID文件中的进程ID
            if( pid && kill(pid, 0) == 0 ) {        // 如果进程存在且仍在运行
                // 有进程仍在运行
                fprintf(stderr, "A station with pid=%d is still running (use -f to take over)\n", pid);  // 输出进程冲突信息
                exit(EXIT_NOP);                     // 以无操作退出码退出
            }
        } else {                                    // 如果指定了强制模式
            killOldPid();                           // 强制杀死旧进程
        }
    }

    // 完成配置设置和验证
    setupConfigFilenames();                         // 设置配置文件名
    checkRollForward();                             // 检查版本回滚
    if( !checkUris() )                              // 检查URI配置
        return 1;                                   // URI检查失败则退出

    // 守护进程模式处理
    if( daemon ) {                                  // 如果指定了守护进程模式
        if( logfile.path == NULL ) {                // 如果没有指定日志文件
            setLogFile("~temp/station.log", "builtin");  // 将默认stderr改为文件输出
            setenv("STATION_TEMPDIR", tempDir, 1);  // 设置临时目录环境变量
        }
        int subprocPid;                             // 子进程ID
        if( (subprocPid = fork()) == -1 )           // 创建守护进程
            rt_fatal("Daemonize fork failed: %s\n", strerror(errno));  // fork失败则致命错误
        if( subprocPid != 0 ) {                     // 父进程分支
            fprintf(stderr, "Daemon pid=%d running...\n", subprocPid);  // 输出守护进程ID
            daemonPid = subprocPid;                 // 记录守护进程ID
            writePid();                             // 写入PID文件
            exit(0);                                // 父进程退出
        }
        // 子进程成为守护进程
        daemonPid = getpid();                       // 获取当前进程ID作为守护进程ID
        setsid();                                   // 创建新的会话，脱离终端
    }

    // 系统核心初始化
    aio_ini();                                      // 初始化异步IO系统
    sys_iniLogging(&logfile, !isSlave && !daemon); // 初始化日志系统(从进程和守护进程不输出到控制台)
    sys_ini();                                      // 系统初始化，打印系统信息
    rt_ini();                                       // 运行时系统初始化
    ts_iniTimesync();                               // 初始化时间同步系统

#if defined(CFG_ral_master_slave)                   // 如果编译时启用了主从进程模式
    if( isSlave ) {                                 // 如果是从进程
        sys_startupSlave(slave_rdfd, slave_wrfd);   // 启动从进程，传入读写文件描述符
        // NOT REACHED                              // 从进程启动后不会返回
        assert(0);                                  // 断言失败，不应该到达这里
    }
#endif // defined(CFG_ral_master_slave)

    // 启动主事件循环
    rt_yieldTo(&startupTmr, daemon ? startupDaemon : startupMaster);  // 根据模式选择启动函数(守护进程或主进程)
    aio_loop();                                     // 进入主事件循环，处理所有IO和定时器事件
    // NOT REACHED                                  // 主事件循环不会返回
    assert(0);                                      // 断言失败，不应该到达这里
}
