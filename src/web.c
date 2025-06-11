/*
 * BasicStation Web服务器模块 - HTTP服务和管理界面
 * 
 * 主要功能：
 * 1. HTTP服务器 - 提供Web管理界面和API接口
 * 2. 静态文件服务 - 支持HTML、CSS、JS等静态资源服务
 * 3. API接口 - 提供REST API用于系统状态查询和配置
 * 4. 请求路由 - 基于路径CRC的高效路由匹配机制
 * 5. 内容编码支持 - 自动检测和处理gzip压缩文件
 * 
 * 设计特点：
 * - 基于事件驱动的异步HTTP服务器架构
 * - 支持静态文件服务和动态API处理
 * - 使用CRC哈希实现高效的URL路由匹配
 * - 自动检测并处理gzip压缩的静态文件
 * - 集成系统状态监控和版本信息查询接口
 * 
 * --- Revised 3-Clause BSD License ---
 * Copyright Semtech Corporation 2022. All rights reserved.
 */

#include "s2conf.h"   // Station配置系统
#include "web.h"      // Web服务器接口定义
#include "sys.h"      // 系统核心功能（文件服务）
#include "uj.h"       // JSON编码器，用于API响应
#include "kwcrc.h"    // 关键字CRC计算，用于路由匹配

// 全局Web服务器实例 - 单例模式，系统中只有一个Web服务器
static web_t* WEB;

// 本模块请求处理器声明 - 处理API和版本查询请求
static const web_handler_t HANDLERS[];     
// 系统级请求处理器声明 - 由其他模块提供的处理器
extern const web_handler_t SYS_HANDLERS[];

// 功能：Web服务器完成处理，清理资源并停止服务
// 参数：web - Web服务器实例指针
//       wstate - 最终状态码（成功或错误类型）
// 返回值：无
// 调用时机：Web服务器遇到错误或需要停止时
static void web_done (web_t* web, s1_t wstate) {
    // web->wstate = wstate;     // 设置最终状态（当前被注释）
    // http_free(&web->hd);      // 释放HTTP连接资源（当前被注释）
    sys_stopWeb();              // 调用系统级停止函数，清理全局状态
    // rt_yieldTo(&web->timeout, cups_ondone);  // 定时器处理（当前被注释）
}

// 功能：Web服务器超时处理回调函数
// 参数：tmr - 超时定时器指针
// 返回值：无
// 调用时机：Web服务器操作超时时由定时器系统自动调用
static void web_timeout (tmr_t* tmr) {
    web_t* web = timeout2web(tmr);         // 通过定时器指针获取Web实例
    LOG(MOD_WEB|ERROR, "WEB timed out");   // 记录超时错误日志
    web_done(web, WEB_ERR_TIMEOUT);        // 以超时错误状态结束Web服务
}


// 功能：初始化Web服务器实例，分配资源并设置初始状态
// 参数：无
// 返回值：Web服务器实例指针，失败时返回NULL
// 调用时机：系统启动时需要启动Web服务时调用
web_t* web_ini () {
    web_t* web = rt_malloc(web_t);         // 分配Web服务器实例内存
    if ( web == NULL ) {                   // 内存分配失败检查
        LOG(MOD_WEB|ERROR, "Not enough space to initialize WEB.");
        return NULL;                       // 返回空指针表示初始化失败
    }
    httpd_ini(&web->hd, CUPS_BUFSZ);      // 初始化HTTP服务器，使用CUPS缓冲区大小
    rt_iniTimer(&web->timeout, web_timeout);  // 初始化超时定时器，设置回调函数
    web->wstate = WEB_INI;                 // 设置Web服务器状态为已初始化
    return web;                            // 返回初始化成功的Web实例
}

// 功能：释放Web服务器实例，清理所有分配的资源
// 参数：web - 要释放的Web服务器实例指针
// 返回值：无
// 调用时机：Web服务器停止或系统关闭时调用
void web_free (web_t* web) {
    if( web == NULL )                      // 空指针检查，避免无效释放
        return;
    httpd_stop(&web->hd);                  // 停止HTTP服务器，关闭监听和连接
    httpd_free(&web->hd);                  // 释放HTTP服务器占用的资源
    rt_clrTimer(&web->timeout);            // 清除并停止超时定时器
    web->wstate = WEB_ERR_CLOSED;          // 设置状态为已关闭
    rt_free(web);                          // 释放Web实例本身的内存
}

// 功能：Web请求路由处理，根据路径分发到相应的处理器
// 参数：pstate - HTTP请求解析状态，包含路径、方法等信息
//       hd - HTTP服务器实例
//       buf - 响应数据缓冲区，由处理器填充响应内容
// 返回值：HTTP状态码（200=成功，404=未找到，其他=错误）
// 调用时机：收到HTTP请求并解析完成后调用
static int web_route(httpd_pstate_t* pstate, httpd_t* hd, dbuf_t* buf) {
    char* path = pstate->path;             // 获取请求路径
    LOG(MOD_WEB|VERBOSE, "Requested Path: %s (crc=0x%08x) [%s]",
        path, pstate->pathcrc, pstate->meth);  // 记录请求路径和CRC哈希值
    
    // 处理空路径请求，默认返回主页
    if ( path[0] == 0 ) {                  // 空路径检查
        path = "index.html";               // 设置默认主页文件名
        pstate->contentType = "text/html"; // 设置内容类型为HTML
    }
    
    *buf = sys_webFile(path);              // 尝试从文件系统读取静态文件

    if ( buf->buf != NULL) {               // 静态文件存在
        // 检查文件是否为gzip压缩格式（magic number: 1f 8b 08）
        if( buf->pos >= 4 && (rt_rlsbf4((u1_t*)buf->buf) & 0x00ffffff) == 0x088b1f ) {
            pstate->contentEnc = "gzip";   // 设置内容编码为gzip
        }
        return 200;                        // 返回HTTP 200 OK状态
    }

    // 静态文件不存在，尝试动态处理器
    const web_handler_t * const handlers[] = {
        SYS_HANDLERS,                      // 系统级处理器（系统状态、配置等）
        HANDLERS,                          // 本模块处理器（API、版本等）
        AUTH_HANDLERS,                     // 认证相关处理器
        NULL                               // 处理器数组结束标记
    };

    // 遍历所有处理器组，查找匹配的路径CRC
    for( int k=0; k<sizeof(handlers)/sizeof(*handlers) && handlers[k]; k++ )
        for( const web_handler_t* hdlr=handlers[k]; hdlr && hdlr->pathcrc; hdlr++ )
            if( hdlr->pathcrc == pstate->pathcrc )  // CRC匹配检查
                return hdlr->f(pstate,hd,buf);      // 调用匹配的处理器函数

    return 404;                            // 没有找到匹配的处理器，返回404
}

// 功能：Web服务器事件处理回调函数，处理HTTP连接的各种事件
// 参数：_conn - 连接实例指针
//       ev - 事件类型（请求、连接关闭等）
// 返回值：无
// 调用时机：HTTP连接状态发生变化时由事件循环自动调用
static void web_onev (conn_t* _conn, int ev) {
    web_t* web = conn2web(_conn);          // 通过连接指针获取Web实例
    httpd_t* hd = &web->hd;                // 获取HTTP服务器实例
    LOG(MOD_WEB|XDEBUG, "Web Event: %d", ev);  // 记录详细的事件调试信息
    switch(ev) {
    
    // HTTP请求事件：客户端发送了完整的HTTP请求
    // HTTP请求事件：客户端发送了完整的HTTP请求
    case HTTPDEV_REQUEST: {
        dbuf_t hdr = httpd_getHdr(hd);     // 获取HTTP请求头缓冲区
        LOG(MOD_WEB|XDEBUG, "Client request: content-length=%d\n%.*s", 
            hd->extra.clen, hdr.bufsize, hdr.buf);  // 记录请求详细信息
        
        httpd_pstate_t pstate;             // HTTP请求解析状态
        int r = 500;                       // 默认响应状态为500（内部错误）
        // 注意：写入响应缓冲区会覆盖请求头缓冲区！
        dbuf_t respbuf = httpd_getRespbuf(hd);  // 获取HTTP响应缓冲区
        dbuf_t fbuf = {0};                 // 文件内容缓冲区，用于存储响应数据
        
        if( !httpd_parseReqLine(&pstate, &hdr) ) {  // 解析HTTP请求行
            LOG(MOD_WEB|ERROR, "Failed to parse request header");
            r = 400;                       // 解析失败，返回400错误（请求格式错误）
        } else {
            r = web_route(&pstate, hd, &fbuf);  // 路由请求到相应处理器
        }
        char* path = rt_strdup(pstate.path);   // 复制路径字符串用于日志记录
        // 根据处理结果生成相应的HTTP响应
        switch(r) {
        case 200:  // 请求成功处理
            // 生成HTTP 200 OK响应头
            xprintf(&respbuf,
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: %s\r\n"                    // 设置内容类型
                    "Content-Encoding: %s\r\n"               // 设置内容编码
                    "\r\n", 
                    pstate.contentType, 
                    (pstate.contentEnc && pstate.contentEnc[0] != 0) ? pstate.contentEnc : "identity");
            
            // 检查响应缓冲区是否有足够空间存放响应内容
            if( respbuf.bufsize - respbuf.pos < fbuf.pos ) {
                LOG(MOD_WEB|ERROR, "Too big: %s (size=%d, bufsize=%d)", 
                    path, fbuf.pos, respbuf.bufsize - respbuf.pos);
                respbuf.pos = 0;               // 重置响应缓冲区
                // 发送507错误（存储空间不足）
                xprintf(&respbuf,
                        "HTTP/1.1 507 Insufficient Storage\r\n\r\n"
                        "Resource too big!\r\n");
            } else {
                LOG(MOD_WEB|VERBOSE, "Sending response: %s (%d bytes)", path, fbuf.pos);
                // 将文件内容复制到响应缓冲区
                memcpy(respbuf.buf + respbuf.pos, fbuf.buf, fbuf.pos);
                respbuf.pos += fbuf.pos;       // 更新响应缓冲区位置
            }
            rt_free((void*)fbuf.buf);          // 释放文件内容缓冲区
            break;
        case 400:  // 请求格式错误
            xprintf(&respbuf, "HTTP/1.1 400 Bad Request\r\n\r\n");
            break;
        case 401:  // 未授权访问
            xprintf(&respbuf, "HTTP/1.1 401 Unauthorized\r\n\r\n");
            break;
        case 404:  // 资源未找到
            xprintf(&respbuf,
                        "HTTP/1.1 404 Not Found\r\n\r\n"
                        "Resource not found!\r\n");
            break;
        case 405:  // HTTP方法不允许
            xprintf(&respbuf, "HTTP/1.1 405 Method Not Allowed\r\n\r\n");
            break;
        case 500:  // 内部服务器错误
            xprintf(&respbuf, "HTTP/1.1 500 Internal Server Error\r\n\r\n");
            break;
        }
        free(path);                        // 释放路径字符串内存
        httpd_response(hd, &respbuf);      // 发送HTTP响应给客户端
        break;
    }
    // 客户端连接死亡事件：网络连接异常断开
    case HTTPDEV_DEAD: {
        LOG(MOD_WEB|INFO, "Web client dead");  // 记录客户端连接死亡信息
        httpd_close(hd);                       // 关闭HTTP连接，清理资源
        break;
    }
    
    // 客户端连接正常关闭事件：客户端主动关闭连接
    case HTTPDEV_CLOSED: {
        LOG(MOD_WEB|DEBUG, "Web client closed");  // 记录客户端关闭信息
        web->hd.c.evcb = (evcb_t)web_onev;        // 重置事件回调函数（httpd_close会设置为默认回调）
        break;
    }
    
    // 未知事件类型：记录错误并忽略
    default: {
        LOG(MOD_WEB|ERROR, "Web - unknown event: %d", ev);  // 记录未知事件错误
        break;
    }
    }
}


// 功能：启动Web服务器，开始监听指定端口
// 参数：web - 已初始化的Web服务器实例
// 返回值：无
// 调用时机：Web服务器初始化完成后调用启动监听
static void web_start (web_t* web) {
    assert(web->wstate == WEB_INI);        // 确保Web服务器处于初始化状态
    char port[10];                         // 端口号字符串缓冲区
    snprintf(port, sizeof(port), "%d", sys_webPort);  // 将端口号转换为字符串

    if( !httpd_listen(&web->hd, port) ) { // 启动HTTP服务器监听
        LOG(MOD_WEB|ERROR, "Web listen failed on port %d", sys_webPort);
        goto errexit;                      // 监听失败，跳转到错误处理
    }
    // rt_setTimerCb(&web->timeout, rt_micros_ahead(WEB_CONN_TIMEOUT), web_timeout);  // 超时设置（当前被注释）
    web->hd.c.evcb = (evcb_t)web_onev;     // 设置事件回调函数

    LOG(MOD_WEB|INFO, "Web server listening on port %d (fd=%d)...", 
        sys_webPort, web->hd.listen.netctx.fd);  // 记录启动成功信息
    return;

 errexit:
    web_done(web, WEB_ERR_FAILED);         // 启动失败，清理并设置错误状态
    return;
}


// 功能：系统级Web服务初始化接口，启动Web服务器
// 参数：无
// 返回值：无
// 调用时机：系统启动时，如果配置了Web端口则启动Web服务
void sys_iniWeb () {
    if( !sys_webPort )                     // 检查是否配置了Web端口
        return;                            // 未配置端口，不启动Web服务
    if ( (WEB = web_ini()) )               // 初始化Web服务器实例
        web_start(WEB);                    // 启动Web服务器监听
    web_authini();                         // 初始化Web认证系统
}

// 功能：系统级Web服务停止接口，关闭Web服务器
// 参数：无
// 返回值：无
// 调用时机：系统关闭或需要停止Web服务时调用
void sys_stopWeb () {
    web_free(WEB);                         // 释放Web服务器实例和资源
    WEB = NULL;                            // 清空全局Web实例指针
}

/*
 * HTTP请求处理器集合
 * 
 * 处理器函数规范：
 *   - 返回HTTP状态码（200=成功，500=错误，其他=具体错误）
 *   - dbuf_t *b 缓冲区未初始化，需要处理器自行分配
 *   - 负责设置适当的 contentType 和 contentEnc
 */

// 功能：API根路径处理器，目前返回成功状态
// 参数：pstate - HTTP请求解析状态
//       hd - HTTP服务器实例
//       b - 响应数据缓冲区（需要处理器填充）
// 返回值：HTTP状态码
// 调用时机：客户端访问 /api 路径时
int handle_api(httpd_pstate_t* pstate, httpd_t* hd, dbuf_t* b) {
    return 200;                            // 暂时只返回成功状态，未实现具体功能
}

// 功能：版本信息查询处理器，返回系统版本和特性信息
// 参数：pstate - HTTP请求解析状态
//       hd - HTTP服务器实例  
//       b - 响应数据缓冲区（需要处理器填充）
// 返回值：HTTP状态码
// 调用时机：客户端访问 /version 路径时
int handle_version(httpd_pstate_t* pstate, httpd_t* hd, dbuf_t* b) {
    if ( pstate->method != HTTP_GET )      // 只支持GET方法
        return 405;                        // 返回方法不允许错误

    b->buf = _rt_malloc(200,0);            // 分配200字节响应缓冲区
    b->bufsize = 200;                      // 设置缓冲区大小
    
    // 使用JSON编码器生成版本信息响应
    uj_encOpen(b, '{');                    // 开始JSON对象
        uj_encKV(b, "msgtype",  's', "version");           // 消息类型
        uj_encKV(b, "firmware", 's', sys_version());       // 固件版本
        uj_encKV(b, "station",  's', CFG_version);         // Station版本
        uj_encKV(b, "protocol", 'i', MUXS_PROTOCOL_VERSION); // 协议版本
        uj_encKV(b, "features", 's', rt_features());       // 运行时特性
    uj_encClose(b, '}');                   // 结束JSON对象
    
    pstate->contentType = "application/json";  // 设置响应内容类型为JSON
    return 200;                            // 返回成功状态
}

// 本模块URL处理器映射表 - 基于路径CRC哈希快速匹配
static const web_handler_t HANDLERS[] = {
    { J_api,     handle_api     },         // API根路径处理器
    { J_version, handle_version },         // 版本信息处理器
    { 0,         NULL           },         // 处理器表结束标记
};
