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
 * BasicStation运行时系统 - 核心基础设施实现
 * 
 * 功能概述：
 * 本文件是BasicStation系统的运行时基础设施，为整个系统提供底层核心服务。
 * 它位于系统架构的最底层，被所有其他模块依赖和调用。
 * 
 * 核心功能模块：
 * 1. 高精度定时器系统 - 微秒级事件调度和定时任务管理
 * 2. 安全内存管理 - 动态内存分配、调试跟踪和泄漏检测
 * 3. 时间服务系统 - 系统时间、UTC时间转换和日期时间处理
 * 4. 数据转换工具 - 字节序转换、字符串处理、CRC计算等
 * 5. 配置解析器 - 数值、时间跨度、大小单位、EUI地址等解析
 * 6. 系统特性管理 - 功能特性列表维护和查询
 * 
 * 设计特点：
 * - 零依赖设计：仅依赖标准C库和系统抽象层
 * - 高性能优化：关键路径使用ATTR_FASTCODE标记
 * - 内存安全：所有内存分配都有失败检查和泄漏跟踪
 * - 时间精度：微秒级精度的时间管理和定时器系统
 * - 可移植性：通过系统抽象层适配不同平台
 * - 调试支持：详细的调试日志和错误处理机制
 */

#include "sys.h"            // 系统抽象层接口：时间、内存、IO等平台特定功能
#include "rt.h"             // 运行时系统接口定义：数据结构、函数声明、宏定义

// LoRaWAN协议兼容性：设备和服务器EUI字段名称标准化
// LoRaWAN 1.1版本开始使用大写字段名，与早期版本的小写字段名兼容
// 这些字符串用于JSON消息序列化和反序列化中的字段名匹配
str_t rt_deveui  = "DevEui";        // 设备EUI字段名：LoRaWAN设备的唯一64位标识符
str_t rt_joineui = "JoinEui";       // 加入服务器EUI字段名：替代旧版本的AppEui，标识Join Server

// 定时器队列管理：使用单链表实现的优先级队列
// 设计说明：虽然插入复杂度为O(n)，但基站系统中定时器数量有限（通常<100个）
// 简单链表比复杂的堆结构更适合，内存占用小，代码简洁，调试容易
static tmr_t* timerQ = TMR_END;     // 定时器队列头指针，按截止时间升序排列

// 系统功能特性列表：动态构建的特性字符串缓冲区
// 用途：记录和报告系统支持的功能特性，便于客户端了解基站能力
// 格式：逗号分隔的特性名称列表，如"CUPS,LBT,FHDR"
static dbuf_t features;             // 动态缓冲区，自动扩展容量

// UTC时间偏移管理：解决系统时钟与标准时间的转换问题
// 背景：嵌入式系统启动时通常没有网络时间同步，使用相对时间计数
// 解决：通过网络时间协议获取真实UTC时间后，计算并维护偏移量
ustime_t rt_utcOffset;              // UTC时间偏移量（微秒）：UTC时间 - 系统启动时间
ustime_t rt_utcOffset_ts;           // 偏移量设定时的系统时间戳，用于偏移有效性检查

// 微秒级精度睡眠：提供高精度的延时功能
// 参数: us - 延时时间（微秒）
// 用途: 协议时序控制、硬件操作延时、精确定时采样
// 注意: 实际精度取决于系统调度器和硬件定时器精度
void rt_usleep (sL_t us) {
    sys_usleep(us);                                     // 调用系统层实现，避免直接依赖平台API
}

// 获取基站唯一标识符：返回基站的EUI-64地址
// 返回值: 64位基站EUI，通常基于网络接口MAC地址生成
// 用途: 基站身份认证、消息签名、网络注册、日志标识
// 来源: 配置文件指定 → 命令行参数 → 网络接口MAC → 系统默认
uL_t rt_eui() {
    return sys_eui();                                   // 委托给系统层实现，支持不同平台的EUI获取方式
}

// 获取系统运行时间：返回系统启动以来的微秒数
// 返回值: 单调递增的时间戳（微秒），不受系统时间调整影响
// 特性: 单调性保证、高精度、启动归零、溢出安全（64位足够运行30万年）
// 用途: 定时器设置、性能测量、事件时间戳、相对时间计算
ustime_t rt_getTime () {
    return (ustime_t)sys_time();                        // 强制转换确保类型一致性
}

// 系统时间转UTC时间：将相对时间转换为绝对UTC时间
// 参数: ustime - 系统相对时间（微秒）
// 返回值: 对应的UTC绝对时间（微秒，自1970年1月1日）
// 用途: 日志时间戳、网络协议时间字段、外部系统接口
// 前提: 必须先通过网络时间同步设置正确的UTC偏移量
ustime_t rt_ustime2utc (ustime_t ustime) {
    return ustime + rt_utcOffset;                       // 简单加法，高效率的时间转换
}

// 获取当前UTC时间：返回当前的绝对UTC时间
// 返回值: 当前UTC时间戳（微秒，自1970年1月1日）
// 用途: 实时日志记录、消息时间戳、定时任务调度
// 精度: 微秒级，适合高精度时间要求的LoRaWAN协议
ustime_t rt_getUTC () {
    return rt_utcOffset + rt_getTime();                 // 当前相对时间 + UTC偏移量
}

// 平年月份天数表：用于日期时间计算的常量数组
// 注意: 2月份的天数为28天（平年），闰年需要特殊处理
// 用途: 微秒时间戳到年月日的转换计算
static const unsigned char DAYSPERMONTH[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };

// 时间戳转日期时间：将Unix时间戳转换为人类可读的日期时间
// 参数: ustime - Unix时间戳（微秒，自1970年1月1日00:00:00 UTC）
// 返回值: datetime结构体，包含年、月、日、时、分、秒、微秒
// 算法: 采用标准的格里高利历算法，正确处理闰年规则
// 用途: 日志格式化、调试输出、时间显示、报告生成
struct datetime rt_datetime (ustime_t ustime) {
    struct datetime dt;
    
    // 处理无效时间戳：负值或异常值
    if( ustime < 0 ) {                                  
        memset(&dt, 0, sizeof(dt));                     // 返回全零的无效日期
        return dt;
    }
    
    // 逐级分解时间单位：从微秒到天的逐步提取
    // 使用除法和取模运算，确保各时间单位在有效范围内
    dt.usec   = ustime%1000000; ustime /= 1000000;     // 微秒部分(0-999999)
    dt.second = ustime%60;      ustime /= 60;          // 秒部分(0-59)
    dt.minute = ustime%60;      ustime /= 60;          // 分部分(0-59)
    dt.hour   = ustime%24;      ustime /= 24;          // 时部分(0-23)

    // 复杂的年份计算：处理格里高利历的闰年规则
    // 闰年规则：4年一闰，100年不闰，400年又闰
    // 算法：先估算年份，然后修正闰日的影响
    int year = (unsigned)(ustime/365) + /*Unix纪元*/1970 - 1;  // 估算年份（不考虑闰年）
    int daysinyear =  (int)(ustime%365)                // 估算年内天数
        // 减去从纪元到估算年份的闰日总数
        - ((year/4)-(year/100)+(year/400))
        + ( 1970/4 - 1970/100 + 1970/400);             // 加上1970年之前的闰日基准
    
    // 修正估算误差：年内天数可能为负，需要调整
    if( daysinyear < 0 ) {                              
        year--;                                         // 年份减1
        daysinyear += 365;                              // 增加一平年的天数
    }
    dt.year = year += 1;                                // 最终年份（修正估算偏差）
    
    // 闰年特殊处理：当前年份如果是闰年，需要考虑2月29日
    if( ((year%4)==0 && (year%100)!=0) || (year%400)==0 ) {
        // 格里高利历闰年判断：能被4整除且不能被100整除，或能被400整除
        if( daysinyear == 31+29-1 ) {                   // 恰好是闰年的2月29日
            dt.day = 29;
            dt.month = 2;
            return dt;                                  // 直接返回2月29日
        }
        if( daysinyear > 31+29-1 )                      // 2月29日之后的日期
            daysinyear--;                               // 减去闰日，按平年计算
    }
    
    // 月份和日期计算：通过逐月减法找到正确的月份和日期
    int month = 0;
    while( daysinyear >= DAYSPERMONTH[month] ) {        // 逐个月份检查
        daysinyear -= DAYSPERMONTH[month];              // 减去当月天数
        month++;                                        // 进入下一个月
    }
    dt.month = month+1;                                 // 月份从1开始（1-12）
    dt.day = daysinyear+1;                              // 日期从1开始（1-31）
    return dt;                                          // 返回完整的日期时间结构
}

// LCOV_EXCL_START - 测试覆盖率工具排除标记（致命错误函数无法正常测试）
// 致命错误处理：记录错误信息并终止程序执行
// 参数: fmt - printf风格的格式字符串
//       ... - 可变参数，对应格式字符串的占位符
// 行为: 记录CRITICAL级别日志后立即终止程序，不返回
// 用途: 处理无法恢复的系统错误、内存分配失败、断言失败等
void rt_fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);                                  // 初始化可变参数列表
    log_vmsg(CRITICAL, fmt, ap);                        // 使用最高级别记录错误日志
    va_end(ap);                                         // 清理可变参数列表
    sys_fatal(0);                                       // 调用系统层终止程序
}
// LCOV_EXCL_STOP - 测试覆盖率工具排除结束标记

// 运行时系统初始化：设置基础运行时环境
// 功能: 初始化UTC时间偏移量，为时间转换做准备
// 调用时机: 系统启动早期，必须在其他运行时功能使用前调用
// 注意: 初始的UTC偏移可能不准确，需要通过网络时间同步更新
void rt_ini() {
    // UTC时间偏移量计算策略：
    // 1. 如果系统提供真实UTC时间，直接计算偏移量
    // 2. 如果系统无UTC时间支持，sys_utc()返回0，暂时使用启动时间
    // 3. 建立网络连接后，通过MuxTime消息更新准确的UTC偏移量
    ustime_t now = rt_getTime();                        // 获取当前系统相对时间
    rt_utcOffset = sys_utc() - now;                     // 计算UTC偏移量
    rt_utcOffset_ts = now;                              // 记录偏移量设定的时间戳
}

// 定时器队列处理：检查并执行到期的定时器回调
// 返回值: 下一个定时器的截止时间（timerfd模式）或剩余时间（轮询模式）
// 调用时机: 主事件循环中调用，是整个定时器系统的核心调度函数
// 性能: 使用ATTR_FASTCODE标记，优化为高频调用路径
// 算法: 链表头部总是最早到期的定时器，O(1)时间复杂度的检查和执行
ATTR_FASTCODE
ustime_t rt_processTimerQ () {
    while(1) {
        // 队列空检查：没有活动定时器时返回最大值
        if( timerQ == TMR_END )                         
            return USTIME_MAX;                          // 表示无定时器需要处理
            
#if defined(CFG_timerfd)                                // Linux timerfd模式：使用内核定时器
        ustime_t deadline = timerQ->deadline;           // 获取队列头的截止时间
        if( (deadline - rt_getTime()) > 0 )             // 检查是否还未到期
            return deadline;                            // 返回绝对截止时间，用于设置timerfd
#else // !defined(CFG_timerfd)                          // 轮询模式：上层负责延时控制
        ustime_t ahead;
        if( (ahead = timerQ->deadline - rt_getTime()) > 0 )  // 计算剩余等待时间
            return ahead;                               // 返回相对剩余时间，用于select/poll超时
#endif // !defined(CFG_timerfd)

        // 执行到期定时器：从队列头部取出并执行
        tmr_t* expired = timerQ;                        // 获取到期的定时器对象
        timerQ = expired->next;                         // 将队列头指向下一个定时器
        expired->next = TMR_NIL;                        // 标记为非活动状态，防止重复处理
        
        // 安全的回调执行：检查回调函数有效性
        if (expired->callback) {                        
            expired->callback(expired);                 // 执行用户定义的回调函数
        } else {
            LOG(ERROR, "Timer due with NULL callback (tmr %p)", expired);  // 记录错误但继续执行
        }
        // 循环继续：检查是否有更多到期的定时器需要处理
    }
}

// 定时器初始化：设置定时器的初始状态和回调函数
// 参数: tmr - 要初始化的定时器对象指针
//       callback - 定时器到期时的回调函数指针
// 用途: 定时器对象在使用前必须初始化，设置基本属性
// 注意: 初始化后定时器处于非活动状态，需要调用rt_setTimer激活
void rt_iniTimer (tmr_t* tmr, tmrcb_t callback) {
    tmr->next     = TMR_NIL;                            // 标记为非活动状态（未加入队列）
    tmr->deadline = rt_getTime();                       // 设置当前时间为初始截止时间
    tmr->callback = callback;                           // 设置用户回调函数
    tmr->ctx      = NULL;                               // 清空用户上下文指针
}

// 设置定时器时间和回调：同时更新定时器的截止时间和回调函数
// 参数: tmr - 定时器对象指针
//       deadline - 绝对截止时间（微秒时间戳）
//       callback - 新的回调函数指针
// 用途: 当需要同时修改定时器时间和回调函数时使用
// 优化: 避免分别调用两个函数，减少队列操作次数
void rt_setTimerCb (tmr_t* tmr, ustime_t deadline, tmrcb_t callback) {
    tmr->callback = callback;                           // 先更新回调函数
    rt_setTimer(tmr, deadline);                         // 再设置定时器时间并激活
}

// 设置定时器截止时间：激活定时器并插入到优先级队列
// 参数: tmr - 定时器对象指针
//       deadline - 绝对截止时间（微秒时间戳）
// 算法: 使用有序链表维护定时器队列，按截止时间升序排列
// 性能: 插入复杂度O(n)，但基站定时器数量有限，实际性能良好
// 注意: 如果定时器已激活，会先清除再重新插入
ATTR_FASTCODE
void rt_setTimer (tmr_t* tmr, ustime_t deadline) {
    assert(tmr != NULL && tmr != TMR_END && tmr != TMR_NIL);  // 参数有效性断言
    
    // 清理已激活的定时器：避免重复插入造成队列损坏
    if( tmr->next != TMR_NIL )                          
        rt_clrTimer(tmr);                               // 从当前位置移除
        
    tmr->deadline = deadline;                           // 设置新的截止时间
    
    // 有序插入算法：在队列中找到正确的插入位置
    // 维持队列按截止时间升序排列的不变性
    tmr_t *p, **pp = &timerQ;                           // 使用指针的指针简化插入操作
    while( (p = *pp) != TMR_END ) {                     // 遍历队列直到末尾
        if( deadline < p->deadline )                    // 找到第一个截止时间更晚的定时器
            break;                                      // 在此位置插入
        pp = &p->next;                                  // 移动到下一个位置
    }
    tmr->next = p;                                      // 新定时器指向后续节点
    *pp = tmr;                                          // 前驱节点指向新定时器
}

// 协作式调度：让出CPU控制权并设置立即执行的回调
// 参数: tmr - 用于调度的定时器对象
//       callback - 让出CPU后立即执行的回调函数
// 用途: 在长时间运行的任务中让出CPU，避免饿死其他任务
// 机制: 通过设置当前时间为截止时间，确保在下次定时器处理时立即执行
void rt_yieldTo (tmr_t* tmr, tmrcb_t callback) {
    tmr->callback = callback;                           // 设置恢复执行时的回调函数
    rt_setTimer(tmr, rt_getTime());                     // 设置当前时间为截止时间（立即触发）
}

// 清除定时器：从队列中移除定时器并标记为非活动状态
// 参数: tmr - 要清除的定时器对象指针
// 功能: 取消已设置的定时器，防止回调函数被执行
// 安全性: 支持清除未激活的定时器，支持空指针输入
// 复杂度: O(n)遍历查找，但定时器数量有限
void rt_clrTimer (tmr_t* tmr) {
    // 参数有效性和状态检查：避免无效操作
    if( (tmr == NULL || tmr == TMR_END) || tmr->next == TMR_NIL )  
        return;                                         // 定时器无效或未激活，安全返回
        
    // 链表删除算法：遍历队列找到目标定时器并移除
    tmr_t *p, **pp = &timerQ;                           // 使用指针的指针简化删除操作
    while( (p = *pp) != TMR_END ) {                     // 遍历整个队列
        if( p == tmr ) {                                // 找到目标定时器
            *pp = tmr->next;                            // 将前驱节点指向后继节点
            tmr->next = TMR_NIL;                        // 标记为非活动状态
            return;                                     // 清除成功
        }
        pp = &p->next;                                  // 移动到下一个节点
    }
    assert(0);                                          // 断言失败：定时器应该在队列中但未找到
}

// 函数功能：从字节缓冲区读取2字节小端序数值
// 参数说明：buf - 字节缓冲区指针
// 返回值：转换后的16位无符号整数
// 调用时机：解析网络协议、二进制数据格式时调用
u2_t rt_rlsbf2 (const u1_t* buf) {
    return (u2_t)(buf[0] | (buf[1]<<8));                // 低字节在前的16位数据读取
}

// 函数功能：从字节缓冲区读取2字节大端序数值
// 参数说明：buf - 字节缓冲区指针
// 返回值：转换后的16位无符号整数
// 调用时机：解析网络协议（通常使用大端序）时调用
u2_t rt_rmsbf2 (const u1_t* buf) {
    return (u2_t)((buf[0]<<8) | buf[1]);                // 高字节在前的16位数据读取
}

// 函数功能：从字节缓冲区读取4字节小端序数值
// 参数说明：buf - 字节缓冲区指针
// 返回值：转换后的32位无符号整数
// 调用时机：解析小端序的32位数据（如x86架构数据）时调用
u4_t rt_rlsbf4 (const u1_t* buf) {
    return (u4_t)(buf[0] | (buf[1]<<8) | ((u4_t)buf[2]<<16) | ((u4_t)buf[3]<<24));  // 小端序32位读取
}

// 函数功能：从字节缓冲区读取8字节小端序数值
// 参数说明：buf - 字节缓冲区指针
// 返回值：转换后的64位无符号整数
// 调用时机：解析小端序的64位数据（如时间戳、EUI等）时调用
uL_t rt_rlsbf8 (const u1_t* buf) {
    return rt_rlsbf4(buf) | ((uL_t)rt_rlsbf4(buf+4) << 32);  // 低32位+高32位组合成64位
}

// 函数功能：安全的内存分配，带零初始化选项
// 参数说明：size - 要分配的字节数
//          zero - 是否零初始化内存
// 返回值：分配的内存指针（失败时程序终止）
// 调用时机：需要动态分配内存时调用，提供比malloc更安全的接口
void* _rt_malloc(int size, int zero) {
    void* p = malloc(size);                             // 调用标准库分配内存
    if( p == NULL )                                     // 分配失败检查
        rt_fatal("Out of memory - requesting %d bytes", size);  // 致命错误终止程序
    if( zero )                                          // 如果需要零初始化
        memset(p, 0, size);                             // 清零内存内容
    return p;                                           // 返回有效的内存指针
}

// 函数功能：调试版本的内存分配，记录分配位置
// 参数说明：size - 要分配的字节数
//          zero - 是否零初始化内存
//          f - 调用文件名（调试用）
//          l - 调用行号（调试用）
// 返回值：分配的内存指针
// 调用时机：DEBUG模式下通过宏调用，用于内存泄漏跟踪
void* _rt_malloc_d(int size, int zero, const char* f, int l) {
    void* p = _rt_malloc(size, zero);                   // 调用基础分配函数
    // LOG (XDEBUG, "  rt_malloc(%d) %s:%d -> %p", size, f, l, p);  // 可选的调试日志
    return p;                                           // 返回分配的内存
}

// 函数功能：调试版本的内存释放，记录释放位置
// 参数说明：p - 要释放的内存指针
//          f - 调用文件名（调试用）
//          l - 调用行号（调试用）
// 返回值：无
// 调用时机：DEBUG模式下通过宏调用，用于内存管理跟踪
void _rt_free_d (void* p, const char* f, int l) {
    // LOG (XDEBUG, "  rt_free() %s:%d -> %p", f, l, p);  // 可选的调试日志
    free(p);                                            // 调用标准库释放内存
}

// 函数功能：复制字符串到新分配的内存中
// 参数说明：s - 源字符串指针
// 返回值：新分配的字符串副本指针，NULL输入返回NULL
// 调用时机：需要创建字符串副本且源字符串生命周期可能结束时调用
char* rt_strdup (str_t s) {
    if( s == NULL ) return NULL;                        // 处理NULL输入
    return strcpy(_rt_malloc(strlen(s)+1, 0), s);       // 分配内存并复制字符串
}

// 函数功能：复制字符串的前n个字符到新分配的内存中
// 参数说明：s - 源字符串指针
//          n - 要复制的最大字符数
// 返回值：新分配的字符串副本指针，自动添加结束符
// 调用时机：需要复制部分字符串或限制复制长度时调用
char* rt_strdupn (str_t s, int n) {
    if( s == NULL ) return NULL;                        // 处理NULL输入
    char* s2 = strncpy(_rt_malloc(n+1, 0), s, n);       // 分配内存并复制指定长度
    s2[n] = 0;                                          // 确保字符串以NULL结尾
    return s2;                                          // 返回复制的字符串
}

// 函数功能：复制字符串并在两端添加双引号
// 参数说明：s - 源字符串指针
// 返回值：新分配的带引号的字符串副本
// 调用时机：生成JSON字符串或需要引号包围的字符串时调用
char* rt_strdupq (str_t s) {
    if( s == NULL ) return NULL;                        // 处理NULL输入
    int n = strlen(s);                                  // 获取原字符串长度
    char* s2 = (char*)memcpy((char*)_rt_malloc(n+3, 0)+1, s, n)-1;  // 分配内存并复制（预留引号空间）
    s2[0] = s2[n+1] = '"';                              // 添加前后双引号
    s2[n+2] = 0;                                        // 添加字符串结束符
    return s2;                                          // 返回带引号的字符串
}

// 函数功能：复制动态缓冲区，创建独立的副本
// 参数说明：b - 源动态缓冲区结构
// 返回值：新的动态缓冲区副本，包含独立的内存空间
// 调用时机：需要创建缓冲区副本且源缓冲区可能被修改时调用
dbuf_t dbuf_dup (dbuf_t b) {
    dbuf_t b2 = b;                                      // 复制缓冲区结构
    b2.buf = _rt_malloc(b.bufsize+1, 0);               // 为副本分配新的内存空间
    if( b.buf == NULL )                                 // 如果源缓冲区为空
        memset(b2.buf, 0, b2.bufsize);                 // 清零新缓冲区
    else
        memcpy(b2.buf, b.buf, b.bufsize);              // 复制源缓冲区数据
    b2.buf[b.bufsize] = 0;                             // 添加字符串结束符（安全措施）
    return  b2;                                        // 返回新的缓冲区副本
}

// 函数功能：释放动态缓冲区的内存并重置结构
// 参数说明：b - 要释放的动态缓冲区指针
// 返回值：无
// 调用时机：不再需要缓冲区时调用，防止内存泄漏
void dbuf_free (dbuf_t* b) {
    rt_free(b->buf);                                   // 释放缓冲区内存
    b->buf = NULL;                                     // 清空缓冲区指针
    b->bufsize = b->pos = 0;                           // 重置大小和位置
}

// 函数功能：将字符转换为对应的十六进制数值
// 参数说明：c - 字符（'0'-'9', 'a'-'f', 'A'-'F'）
// 返回值：对应的十六进制数值（0-15），无效字符返回-1
// 调用时机：解析十六进制字符串、EUI地址、MAC地址时调用
int rt_hexDigit (int c) {
    /**/ if( c >= '0' && c <= '9') return c - '0';       // 数字字符0-9
    else if( c >= 'a' && c <= 'f') return c - ('a'-10); // 小写字母a-f
    else if( c >= 'A' && c <= 'F') return c - ('A'-10); // 大写字母A-F
    else return -1;                                     // 无效字符
}

// 函数功能：从字符串中解析十进制或十六进制数值
// 参数说明：pp - 字符串指针的指针，解析后会更新位置
// 返回值：解析得到的数值，解析失败返回-1
// 调用时机：配置文件解析、命令行参数解析、协议数据解析时调用
sL_t rt_readDec (str_t* pp) {
    str_t p = *pp;                                      // 获取当前解析位置
    if( p == NULL )                                     // 空指针检查
        return -1;
        
    int c, n=0;                                         // 字符和计数器
    sL_t v=0;                                           // 累积的数值
    
    if( p[0] == '0' && (p[1] == 'X' || p[1] == 'x') ) { // 十六进制格式（0x或0X前缀）
        p += 2;                                         // 跳过0x前缀
        while( (c=rt_hexDigit(p[n])) >= 0 ) {           // 解析十六进制数字
            v = (v<<4) + c;                             // 累积数值（左移4位 + 新数字）
            n++;
        }
    } else {                                            // 十进制格式
        while( (c=p[n]) >= '0' && c <= '9' ) {          // 解析十进制数字
            v = v*10 + (c-'0');                         // 累积数值（乘10 + 新数字）
            n++;
        }
    }
    *pp = p+n;                                          // 更新解析位置
    return n==0 ? -1 : v;                               // 没有解析到数字返回-1，否则返回数值
}

// 函数功能：解析时间跨度字符串，支持多种时间单位
// 参数说明：pp - 字符串指针的指针，解析后会更新位置
//          defaultUnit - 没有单位时使用的默认单位
// 返回值：解析得到的时间跨度（微秒），解析失败返回-1
// 调用时机：解析配置中的超时时间、延时设置、时间间隔时调用
sL_t rt_readSpan (str_t* pp, ustime_t defaultUnit) {
    str_t p = *pp;                                      // 获取当前解析位置
    if( p == NULL )                                     // 空指针检查
        return -1;
        
    sL_t span = -1;                                     // 累积的时间跨度
    while(1) {                                          // 循环解析多个时间单位
        sL_t u, v = rt_readDec(&p);                     // 解析数值部分
        if( v < 0 ) {                                   // 解析完成或失败
            *pp = p;                                    // 更新解析位置
            return span;                                // 返回累积的时间跨度
        }
        
        // 根据单位字符确定时间单位
        switch(p[0]) {
        case 'd': {                                     // 天 (day)
            u = rt_seconds(24*3600);                    // 1天 = 24小时 * 3600秒
            break;
        }
        case 'h': {                                     // 小时 (hour)
            u = rt_seconds(3600);                       // 1小时 = 3600秒
            break;
        }
        case 'm': {                                     // 分钟 (minute) 或毫秒 (millisecond)
            if( p[1]=='s' ) {                           // 'ms' - 毫秒
                u = rt_millis(1); p++;                  // 1毫秒，多跳过一个字符
            } else {                                    // 'm' - 分钟
                u = rt_seconds(60);                     // 1分钟 = 60秒
            }
            break;
        }
        case 's': {                                     // 秒 (second)
            u = rt_seconds(1);                          // 1秒
            break;
        }
        default: {                                      // 没有单位，使用默认单位
            if( defaultUnit == 0 )                      // 如果没有默认单位
                return -1;                              // 解析失败
            u = defaultUnit;                            // 使用默认单位
            p--;                                        // 回退一个字符（因为没有单位字符）
            break;
        }
        }
        p++;                                            // 跳过单位字符
        span = (span < 0 ? 0 : span) + v*u;             // 累积时间跨度
    }
}

// 函数功能：解析大小字符串，支持多种大小单位（K/M/G等）
// 参数说明：pp - 字符串指针的指针，解析后会更新位置
//          defaultUnit - 没有单位时使用的默认单位
// 返回值：解析得到的大小（字节），解析失败返回-1
// 调用时机：解析配置中的缓冲区大小、文件大小限制等时调用
sL_t rt_readSize (str_t* pp, ustime_t defaultUnit) {
    str_t p = *pp;                                      // 获取当前解析位置
    if( p == NULL )                                     // 空指针检查
        return -1;
        
    sL_t size = -1;                                     // 累积的大小
    while(1) {                                          // 循环解析多个大小单位
        sL_t u, v = rt_readDec(&p);                     // 解析数值部分
        if( v < 0 ) {                                   // 解析完成或失败
            *pp = p;                                    // 更新解析位置
            return size;                                // 返回累积的大小
        }
        
        sL_t uu = 1000;                                 // 默认使用十进制单位（1000）
        if( p[0] && (p[1]=='b' || p[1]=='B') )          // 如果下一个字符是'b'或'B'
            uu = 1024;                                  // 使用二进制单位（1024）
            
        // 根据单位字符确定大小单位
        switch(p[0]) {
        case 'k': case 'K': { u = uu; break; }         // KB/KiB（千字节/千二进制字节）
        case 'm': case 'M': { u = uu*uu; break; }      // MB/MiB（兆字节/兆二进制字节）
        case 'g': case 'G': { u = uu*uu*uu; break; }   // GB/GiB（吉字节/吉二进制字节）
        default: {                                      // 没有单位，使用默认单位
            if( defaultUnit == 0 )                      // 如果没有默认单位
                return -1;                              // 解析失败
            u = defaultUnit;                            // 使用默认单位
            uu = 1000;                                  // 重置为十进制
            p--;                                        // 回退一个字符（因为没有单位字符）
            break;
        }
        }
        p += uu==1000 ? 1 : 2;                          // 跳过单位字符（十进制1个，二进制2个）
        size = (size < 0 ? 0 : size) + v*u;             // 累积大小
    }
}

// 函数功能：解析ID6格式的EUI片段（IPv6风格的压缩格式）
// 参数说明：p - 字符串指针
//          n - 要解析的字符数
//          peui - 输出的EUI值指针
// 返回值：解析的有效位数，失败返回0
// 调用时机：解析IPv6风格的EUI地址时内部调用
static int parseId6Fragment (str_t p, int n, uL_t* peui) {
    uL_t group = 0;                                     // 累积的EUI组值
    int bits = 16;                                      // 当前处理的位数
    
    // 从右向左解析十六进制数字
    for( int i=n-1, s=0; i >= -1; i-- ) {
        int x = i<0 ? -1 : rt_hexDigit(p[i]);           // 获取十六进制数字
        if( x < 0 ) {                                   // 遇到非十六进制字符
            if( s == 0 || s > 16 )                      // 检查位数有效性
                return 0;                               // 无效格式
            if( i<0 ) break;                            // 到达字符串开头
            bits += 16;                                 // 增加16位
            s = 0;                                      // 重置位计数
        } else {
            group |= (uL_t)x << (s+bits-16);            // 累积十六进制数字
            s += 4;                                     // 每个十六进制数字4位
        }
    }
    *peui = group;                                      // 输出解析结果
    return bits;                                        // 返回有效位数
}

// 函数功能：解析EUI/MAC地址，支持多种格式（-分隔、:分隔、ID6格式）
// 参数说明：pp - 字符串指针的指针，解析后会更新位置
//          len - 最大解析字符数，0表示无限制
// 返回值：解析得到的EUI/MAC地址（64位），解析失败返回0
// 调用时机：解析配置中的设备EUI、网关MAC地址等时调用
uL_t rt_readEui (const char** pp, int len) {
    str_t p = *pp;                                      // 获取当前解析位置
    if( p == NULL )                                     // 空指针检查
        return 0;
        
    int c, n=0;                                         // 字符和计数器
    int dashes=0, colons=0, hexdigits=0;                // 分隔符和十六进制数字计数
    
    // 扫描字符串，统计各种字符类型
    while( (len==0 || n < len) && (c=p[n]) != 0 ) {
        /**/ if( c == '-' ) dashes++;                   // 短横线分隔符
        else if( c == ':' ) colons++;                   // 冒号分隔符
        else if( rt_hexDigit(c) >= 0 ) hexdigits++;     // 十六进制数字
        else break;                                     // 遇到其他字符停止
        n++;
    }
    
    // 格式有效性检查
    if( hexdigits==0 || (dashes && colons) || dashes > 7 || colons > 7 || hexdigits > 16 )
        return 0;                                       // 格式异常
        
    if( colons == 2 || colons == 3 ) {                  // ID6格式（IPv6风格）
        uL_t eui;
        // 查找双冒号（::）进行压缩格式处理
        for( int i=1; i<n; i++ ) {
            if( p[i] == ':' && p[i-1] == ':' ) {        // 找到双冒号
                if( i==1 ) {                            // ::123 格式
                    if( !parseId6Fragment(p+2, n-2, &eui) )
                        goto id6err;
                    goto id6exit;
                }
                if( i==n-1 ) {                          // 123:: 格式
                    int bits = parseId6Fragment(p, n-2, &eui);
                    if( !bits ) goto id6err;
                    eui <<= (64-bits);                  // 左移到高位
                    goto id6exit;
                }{                                      // 1::2 格式
                    uL_t euix;
                    int bits = parseId6Fragment(p, i-1, &euix);
                    if( !bits || !parseId6Fragment(p+i+1, n-i-1, &eui) )
                        goto id6err;
                    eui |= (euix << (64-bits));         // 合并前后部分
                    goto id6exit;
                }
            }
        }
        // 没有双冒号的完整格式
        if( !parseId6Fragment(p, n, &eui) )
            goto id6err;
    id6exit:
        *pp = p+n;                                      // 更新解析位置
        return eui;                                     // 返回解析结果
    id6err:
        return 0;                                       // ID6解析失败
    }
    
    // 标准格式：缺失的十六进制数字假定为前导零
    uL_t eui = 0;
    for( int i=0; i<n; i++ ) {
        int c = rt_hexDigit(p[i]);                      // 获取十六进制数字
        if( c >= 0 )                                    // 有效的十六进制数字
            eui = (eui<<4) | c;                         // 累积到EUI值
    }
    *pp = p+n;                                          // 更新解析位置
    return eui;                                         // 返回解析结果
}

// CRC-32查找表：预计算的CRC-32多项式查找表
// 使用标准的CRC-32多项式(0x04C11DB7)进行数据完整性校验
// 表大小为256个32位值，提供高效的CRC计算
static const uint32_t crc_table[256] = {
    0x00000000,0x77073096,0xEE0E612C,0x990951BA,0x076DC419,0x706AF48F,0xE963A535,0x9E6495A3,
    0x0EDB8832,0x79DCB8A4,0xE0D5E91E,0x97D2D988,0x09B64C2B,0x7EB17CBD,0xE7B82D07,0x90BF1D91,
    0x1DB71064,0x6AB020F2,0xF3B97148,0x84BE41DE,0x1ADAD47D,0x6DDDE4EB,0xF4D4B551,0x83D385C7,
    0x136C9856,0x646BA8C0,0xFD62F97A,0x8A65C9EC,0x14015C4F,0x63066CD9,0xFA0F3D63,0x8D080DF5,
    0x3B6E20C8,0x4C69105E,0xD56041E4,0xA2677172,0x3C03E4D1,0x4B04D447,0xD20D85FD,0xA50AB56B,
    0x35B5A8FA,0x42B2986C,0xDBBBC9D6,0xACBCF940,0x32D86CE3,0x45DF5C75,0xDCD60DCF,0xABD13D59,
    0x26D930AC,0x51DE003A,0xC8D75180,0xBFD06116,0x21B4F4B5,0x56B3C423,0xCFBA9599,0xB8BDA50F,
    0x2802B89E,0x5F058808,0xC60CD9B2,0xB10BE924,0x2F6F7C87,0x58684C11,0xC1611DAB,0xB6662D3D,
    0x76DC4190,0x01DB7106,0x98D220BC,0xEFD5102A,0x71B18589,0x06B6B51F,0x9FBFE4A5,0xE8B8D433,
    0x7807C9A2,0x0F00F934,0x9609A88E,0xE10E9818,0x7F6A0DBB,0x086D3D2D,0x91646C97,0xE6635C01,
    0x6B6B51F4,0x1C6C6162,0x856530D8,0xF262004E,0x6C0695ED,0x1B01A57B,0x8208F4C1,0xF50FC457,
    0x65B0D9C6,0x12B7E950,0x8BBEB8EA,0xFCB9887C,0x62DD1DDF,0x15DA2D49,0x8CD37CF3,0xFBD44C65,
    0x4DB26158,0x3AB551CE,0xA3BC0074,0xD4BB30E2,0x4ADFA541,0x3DD895D7,0xA4D1C46D,0xD3D6F4FB,
    0x4369E96A,0x346ED9FC,0xAD678846,0xDA60B8D0,0x44042D73,0x33031DE5,0xAA0A4C5F,0xDD0D7CC9,
    0x5005713C,0x270241AA,0xBE0B1010,0xC90C2086,0x5768B525,0x206F85B3,0xB966D409,0xCE61E49F,
    0x5EDEF90E,0x29D9C998,0xB0D09822,0xC7D7A8B4,0x59B33D17,0x2EB40D81,0xB7BD5C3B,0xC0BA6CAD,
    0xEDB88320,0x9ABFB3B6,0x03B6E20C,0x74B1D29A,0xEAD54739,0x9DD277AF,0x04DB2615,0x73DC1683,
    0xE3630B12,0x94643B84,0x0D6D6A3E,0x7A6A5AA8,0xE40ECF0B,0x9309FF9D,0x0A00AE27,0x7D079EB1,
    0xF00F9344,0x8708A3D2,0x1E01F268,0x6906C2FE,0xF762575D,0x806567CB,0x196C3671,0x6E6B06E7,
    0xFED41B76,0x89D32BE0,0x10DA7A5A,0x67DD4ACC,0xF9B9DF6F,0x8EBEEFF9,0x17B7BE43,0x60B08ED5,
    0xD6D6A3E8,0xA1D1937E,0x38D8C2C4,0x4FDFF252,0xD1BB67F1,0xA6BC5767,0x3FB506DD,0x48B2364B,
    0xD80D2BDA,0xAF0A1B4C,0x36034AF6,0x41047A60,0xDF60EFC3,0xA867DF55,0x316E8EEF,0x4669BE79,
    0xCB61B38C,0xBC66831A,0x256FD2A0,0x5268E236,0xCC0C7795,0xBB0B4703,0x220216B9,0x5505262F,
    0xC5BA3BBE,0xB2BD0B28,0x2BB45A92,0x5CB36A04,0xC2D7FFA7,0xB5D0CF31,0x2CD99E8B,0x5BDEAE1D,
    0x9B64C2B0,0xEC63F226,0x756AA39C,0x026D930A,0x9C0906A9,0xEB0E363F,0x72076785,0x05005713,
    0x95BF4A82,0xE2B87A14,0x7BB12BAE,0x0CB61B38,0x92D28E9B,0xE5D5BE0D,0x7CDCEFB7,0x0BDBDF21,
    0x86D3D2D4,0xF1D4E242,0x68DDB3F8,0x1FDA836E,0x81BE16CD,0xF6B9265B,0x6FB077E1,0x18B74777,
    0x88085AE6,0xFF0F6A70,0x66063BCA,0x11010B5C,0x8F659EFF,0xF862AE69,0x616BFFD3,0x166CCF45,
    0xA00AE278,0xD70DD2EE,0x4E048354,0x3903B3C2,0xA7672661,0xD06016F7,0x4969474D,0x3E6E77DB,
    0xAED16A4A,0xD9D65ADC,0x40DF0B66,0x37D83BF0,0xA9BCAE53,0xDEBB9EC5,0x47B2CF7F,0x30B5FFE9,
    0xBDBDF21C,0xCABAC28A,0x53B39330,0x24B4A3A6,0xBAD03605,0xCDD70693,0x54DE5729,0x23D967BF,
    0xB3667A2E,0xC4614AB8,0x5D681B02,0x2A6F2B94,0xB40BBE37,0xC30C8EA1,0x5A05DF1B,0x2D02EF8D,
};

// 函数功能：计算数据的CRC-32校验和
// 参数说明：crc - 初始CRC值（通常为0或之前的CRC值）
//          buf - 要计算CRC的数据缓冲区
//          size - 数据大小（字节）
// 返回值：计算得到的32位CRC校验和
// 调用时机：数据完整性校验、协议校验码计算时调用
u4_t rt_crc32 (u4_t crc, const void* buf, int size) {
    const u1_t *p = (u1_t*)buf;                         // 字节指针用于逐字节处理

    crc = crc ^ ~0U;                                    // CRC初始化（反转所有位）
    while( size-- > 0 )                                 // 逐字节处理数据
        crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);  // 查表计算CRC
    return crc ^ ~0U;                                   // 返回最终CRC（再次反转）
}

// 函数功能：向系统特性列表添加新特性
// 参数说明：s - 要添加的特性字符串
// 返回值：无
// 调用时机：系统初始化时添加支持的功能特性，供客户端查询
void rt_addFeature (str_t s) {
    int l = strlen(s);                                  // 特性字符串长度
    int n = features.pos+l+1;                           // 需要的总空间
    
    // 扩展特性缓冲区
    if( features.buf == NULL || n > features.bufsize ) {
        int sz = max(features.bufsize + 32, n);         // 计算新的缓冲区大小
        char* b = rt_mallocN(char, sz);                 // 分配新缓冲区
        if( features.pos > 0 )                          // 如果有旧数据
            memcpy(b, features.buf, features.pos);      // 复制旧数据
        rt_free(features.buf);                          // 释放旧缓冲区
        features.buf = b;                               // 设置新缓冲区
        features.bufsize = sz;                          // 更新缓冲区大小
    }
    
    int i = 0;                                          // 搜索位置
    char* p = features.buf;                             // 缓冲区指针
    n = features.pos;                                   // 当前数据长度
    
    // 检查特性是否已存在
    while( i < n ) {
        if( strncmp(&p[i], s, l) == 0 && (p[i+l] == ' ' || p[i+l] == 0) )
            return;                                     // 特性已存在，直接返回
        while( p[i] != ' ' && p[i] != 0 ) i++;          // 跳过当前特性
        i += 1;                                         // 跳过空格
    }
    
    // 添加新特性
    if( i > 0 )                                         // 如果不是第一个特性
        features.buf[i-1] = ' ';                        // 添加空格分隔符
    memcpy(&features.buf[i], s, l+1);                   // 复制新特性字符串
    features.pos = i+l+1;                               // 更新数据位置
}

// 函数功能：获取系统特性列表字符串
// 参数说明：无
// 返回值：包含所有特性的字符串，特性间用空格分隔
// 调用时机：客户端查询系统支持的功能特性时调用
str_t rt_features () {
    return features.buf==NULL ? "" : features.buf;     // 返回特性字符串或空字符串
}
