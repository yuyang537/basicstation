# Basic Station 主进程启动从进程流程详解

## 概述

Basic Station 采用主从架构来管理多个无线电硬件单元。主进程负责网络通信和协议处理，从进程负责具体的无线电硬件操作。本文档详细解释主进程如何启动和管理从进程的完整流程。

## 主要阶段

### 1. 主进程启动和初始化阶段

```c
// 在 main() 函数中
int main(int argc, char **argv) {
    // 解析命令行参数
    // 初始化各子系统 (aio, logging, sys, rt, timesync)
    // 启动主控制器
    rt_yieldTo(&startupTmr, startupMaster);
    aio_loop();
}
```

主进程首先完成基础的系统初始化，然后调用 `startupMaster()` 函数开始真正的业务初始化。

### 2. RAL 初始化和从进程发现阶段

```c
// 在 startupMaster() 中调用
void ral_ini() {
    // 扫描配置文件发现从进程
    int slaveCnt = 0;
    while(1) {
        char cfname[64];
        snprintf(cfname, sizeof(cfname), "slave-%d.conf", slaveCnt);
        dbuf_t b = sys_checkFile(cfname);
        if( b.buf == NULL ) break;  // 没有更多配置文件
        free(b.buf);
        slaveCnt += 1;
    }
    
    // 验证从进程数量
    if( slaveCnt == 0 || slaveCnt > MAX_TXUNITS )
        rt_fatal("配置文件数量不合法");
}
```

**关键步骤：**
- 从 `slave-0.conf` 开始逐个检查配置文件
- 统计有效的从进程配置文件数量
- 验证数量在合法范围内（1 到 MAX_TXUNITS）

### 3. 从进程管理结构初始化

```c
// 分配从进程管理数组
n_slaves = slaveCnt;
slaves = rt_mallocN(slave_t, n_slaves);

// 为每个从进程解析配置
for( int sidx=0; sidx < n_slaves; sidx++ ) {
    struct sx130xconf sx1301conf;
    if( !sx130xconf_parse_setup(&sx1301conf, sidx, "sx1301/1", "{}", 2) ) {
        allok = 0;
    } else {
        slaves[sidx].antennaType = sx1301conf.antennaType;
    }
    slaves[sidx].last_expcmd = -1;
}

// 设置主进程相关参数
master_pid = getpid();
atexit(killAllSlaves);          // 注册退出处理
signal(SIGPIPE, SIG_IGN);       // 忽略管道信号
```

**关键步骤：**
- 为每个从进程分配管理结构 `slave_t`
- 预解析配置文件获取基本参数
- 设置进程管理相关的信号和退出处理

### 4. 逐个启动从进程

```c
// 为每个从进程初始化定时器并启动
for( int i=0; i<n_slaves; i++ ) {
    rt_iniTimer(&slaves[i].tmr, NULL);
    rt_iniTimer(&slaves[i].tsync, req_timesync);
    rt_yieldTo(&slaves[i].tmr, restart_slave);  // 异步启动
}
```

每个从进程都通过 `restart_slave()` 函数异步启动，确保主进程不会被阻塞。

### 5. restart_slave() 核心启动逻辑

#### 5.1 重启控制和资源清理

```c
static void restart_slave (tmr_t* tmr) {
    slave_t* slave = memberof(slave_t, tmr, tmr);
    
    // 检查重启次数，防止无限重启
    if( ++slave->restartCnt > 4 ) {
        rt_fatal("Slave %d restarted %d times without successful interaction",
                 slaveIdx, slave->restartCnt);
    }
    
    // 清理旧的资源
    rt_clrTimer(&slave->tmr);
    rt_clrTimer(&slave->tsync);
    aio_close(slave->up);
    aio_close(slave->dn);
    slave->up = slave->dn = NULL;
}
```

#### 5.2 处理已存在的从进程

```c
// 如果从进程还在运行，先终止它
if( is_slave_alive(slave) ) {
    int err = kill(pid, slave->killCnt <= 2 ? SIGTERM : SIGKILL);
    slave->killCnt += 1;
    rt_setTimerCb(&slave->tmr, rt_micros_ahead(RETRY_KILL_INTV), restart_slave);
    return;  // 等待下次重试
}
```

使用渐进式终止策略：先发送 SIGTERM，如果多次失败则使用 SIGKILL。

#### 5.3 创建管道和fork子进程

```c
// 创建双向管道
int up[2] = { -1, -1 };  // 从进程 -> 主进程
int dn[2] = { -1, -1 };  // 主进程 -> 从进程
if( pipe2(up, O_NONBLOCK) == -1 || pipe2(dn, O_NONBLOCK) == -1 ) {
    rt_fatal("Failed to create pipe: %s", strerror(errno));
}

// Fork 创建子进程
if( (pid = fork()) == 0 ) {
    // 子进程路径
    execSlave(slaveIdx, dn[0], up[1]);
} else if( pid > 0 ) {
    // 父进程路径
    close(up[1]); close(dn[0]);  // 关闭子进程使用的管道端
    slave->pid = pid;
    // 继续父进程处理...
}
```

### 6. 子进程执行路径 (execSlave)

```c
static void execSlave (int idx, int rdfd, int wrfd) {
    // 设置环境变量传递给新进程
    char idxbuf[12], rdfdbuf[12], wrfdbuf[12];
    snprintf(idxbuf,  sizeof(idxbuf),  "%d", idx);
    snprintf(rdfdbuf, sizeof(rdfdbuf), "%d", rdfd);
    snprintf(wrfdbuf, sizeof(wrfdbuf), "%d", wrfd);
    setenv("SLAVE_IDX" , idxbuf , 1);
    setenv("SLAVE_RDFD", rdfdbuf, 1);
    setenv("SLAVE_WRFD", wrfdbuf, 1);
    
    // 解析执行命令模板
    wordexp_t wexp;
    int fail = wordexp(sys_slaveExec, &wexp, WRDE_DOOFFS|WRDE_NOCMD|WRDE_UNDEF|WRDE_SHOWERR);
    
    // 执行从进程程序 (通常是自己的可执行文件)
    execvp(wexp.we_wordv[0], wexp.we_wordv);
}
```

**关键点：**
- 通过环境变量传递从进程索引和管道文件描述符
- 使用 `execvp()` 替换当前进程映像
- 新进程重新从 `main()` 开始，但会检测到从模式环境变量

### 7. 从进程重新启动和识别

```c
// 在新进程的 main() 中
#if defined(CFG_ral_master_slave)
int slave_rdfd = -1, slave_wrfd = -1;
if( opts->slaveMode ) {
    // 解析环境变量
    str_t const* sn = SLAVE_ENVS;
    while( *sn ) {
        str_t sv = getenv(*sn);
        if( sv == NULL )
            rt_fatal("Missing mandatory env var: %s", *sn);
        // 解析 SLAVE_IDX, SLAVE_RDFD, SLAVE_WRFD
    }
}

// 如果是从模式，启动从进程
if( isSlave ) {
    sys_startupSlave(slave_rdfd, slave_wrfd);
    assert(0);  // 不应该返回
}
#endif
```

### 8. 父进程配置发送和监控

#### 8.1 异步IO设置和配置发送

```c
// 父进程设置异步IO
slave->up = aio_open(slave, up[0], pipe_read, NULL);
slave->dn = aio_open(slave, dn[1], NULL, NULL);

// 发送配置到从进程
static void send_config (slave_t* slave) {
    struct ral_config_req req = { .cmd = RAL_CMD_CONFIG, .rctx = 0 };
    strcpy(req.hwspec, "sx1301/1");
    req.region = region;
    req.jsonlen = slave->sx1301confJson.bufsize;
    req.upchs = slave->upchs;
    memcpy(req.json, slave->sx1301confJson.buf, req.jsonlen);
    
    if( !write_slave_pipe(slave, &req, sizeof(req)) )
        rt_fatal("Failed to send sx1301conf");
}
```

#### 8.2 进程监控

```c
// 启动进程监控
rt_yieldTo(&slave->tmr, recheck_slave);

static void recheck_slave (tmr_t* tmr) {
    slave_t* slave = memberof(slave_t, tmr, tmr);
    if( is_slave_alive(slave) ) {
        rt_setTimer(&slave->tmr, rt_micros_ahead(WAIT_SLAVE_PID_INTV));
        return;
    }
    restart_slave(tmr);  // 如果进程死亡，重新启动
}
```

**监控机制：**
- 500ms 定时检查从进程状态
- 使用 `waitpid(WNOHANG)` 非阻塞检查
- 自动重启异常退出的从进程
- 对致命错误进行特殊处理

### 9. 从进程初始化和运行

```c
void sys_startupSlave (int rdfd, int wrfd) {
    // 初始化异步IO管道
    rd_aio = aio_open(&rxpoll_tmr, rdfd, pipe_read, NULL);
    wr_aio = aio_open(&rxpoll_tmr, wrfd, NULL, NULL);
    rt_iniTimer(&rxpoll_tmr, NULL);
    
    // 开始监听主进程命令
    pipe_read(rd_aio);
    LOG(MOD_RAL|INFO, "Slave LGW (%d) - started.", sys_slaveIdx);
    
    // 进入事件循环
    aio_loop();
}
```

从进程进入事件循环后，会：
- 接收并处理主进程的各种命令（CONFIG, TX, TXSTATUS 等）
- 管理具体的无线电硬件
- 处理 LoRa 数据包的收发
- 向主进程报告状态和数据

## 技术特点

### 1. 可靠性设计
- **重启限制**：最多重启 4 次，防止无限循环
- **渐进式终止**：先 SIGTERM，后 SIGKILL
- **进程监控**：500ms 定时检查，及时发现异常

### 2. 通信机制
- **双向管道**：分离上行和下行数据流
- **非阻塞IO**：使用 O_NONBLOCK 防止阻塞
- **异步处理**：基于事件循环的通信

### 3. 配置管理
- **文件驱动**：通过 slave-N.conf 文件配置
- **动态发现**：自动检测配置文件数量
- **参数传递**：环境变量传递运行时参数

### 4. 错误处理
- **分层处理**：区分致命错误和可恢复错误
- **日志记录**：详细的启动和错误日志
- **资源清理**：异常时的完整资源清理

## 总结

Basic Station 的主从架构通过精心设计的进程管理机制，实现了：

1. **高可用性**：自动监控和重启机制确保服务连续性
2. **可扩展性**：支持多个从进程管理多个无线电单元
3. **稳定性**：完善的错误处理和资源管理
4. **效率**：异步IO和事件驱动架构提高并发性能

这种架构使得 Basic Station 能够可靠地管理复杂的 LoRaWAN 网关硬件，同时保持良好的可维护性和扩展性。 