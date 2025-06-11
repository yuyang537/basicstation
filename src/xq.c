/*
 * BasicStation 扩展队列模块 - 高效的发送和接收数据包队列管理
 * 
 * 主要功能：
 * 1. 发送队列管理 - TXQ发送作业和数据的统一管理
 * 2. 接收队列管理 - RXQ接收帧数据的FIFO缓冲
 * 3. 内存紧凑化 - 动态数据压缩和空间回收机制
 * 4. 链表队列操作 - 高效的单链表作业管理
 * 5. 数据分离存储 - 作业元数据与实际数据分开存储
 * 
 * 设计特点：
 * - TXQ支持非严格FIFO，作业可灵活调度重排
 * - RXQ采用严格FIFO，确保帧序接收处理
 * - 分离式存储：作业描述符与数据载荷独立管理
 * - 内存紧凑化：删除数据时自动压缩空洞
 * - 零拷贝设计：通过偏移量引用避免数据复制
 * - 高效索引：基于数组索引的快速作业查找
 * 
 * --- Revised 3-Clause BSD License ---
 * Copyright Semtech Corporation 2022. All rights reserved.
 */

#include "xq.h"                          // 扩展队列模块接口定义

// ================================================================================
//
// TXQ - 发送队列管理系统
//
// ================================================================================
//
// 发送作业队列特性：
// - 作业不严格按FIFO顺序，可以任意交换位置以优化调度
// - 使用单链表管理：一个空闲作业链表，每个TX单元一个作业链表
// - 作业可选择关联发送数据，数据独立存储在连续缓冲区中
// - 作业释放时关联的数据段将被移除，剩余数据立即压缩
// - txdata缓冲区末尾始终为可用的空闲数据空间
//


// === TXQ初始化和基础操作 ===

// 功能：初始化发送队列 - 设置所有作业为空闲状态并建立空闲链表
// 参数：txq - 发送队列对象指针
// 返回值：无
// 调用时机：系统启动时初始化发送队列系统
void txq_ini (txq_t* txq) {
    memset(txq, 0, sizeof(*txq));         // 清零整个队列结构
    
    // 初始化空闲作业链表：将所有作业链接成单链表
    for( txidx_t i=0; i<MAX_TXJOBS; i++ ) {
        txq->txjobs[i].next = i+1;        // 指向下一个作业索引
        txq->txjobs[i].off = TXOFF_NIL;   // 标记无关联数据
    }
    txq->txjobs[MAX_TXJOBS-1].next = TXIDX_END;  // 最后一个作业标记链表结束
}

// === TXQ索引和指针转换操作 ===

// 功能：索引转作业指针 - 根据作业索引获取作业对象指针
// 参数：txq - 发送队列对象
//       idx - 作业索引
// 返回值：作业对象指针，无效索引时返回NULL
// 调用时机：需要通过索引访问作业对象时
txjob_t* txq_idx2job (txq_t* txq, txidx_t idx) {
    if( idx == TXIDX_NIL || idx == TXIDX_END )  // 检查特殊索引值
        return NULL;
    return &txq->txjobs[idx];             // 返回对应作业的地址
}

// 功能：作业指针转索引 - 根据作业对象指针获取其在数组中的索引
// 参数：txq - 发送队列对象
//       job - 作业对象指针
// 返回值：作业索引，空指针时返回TXIDX_NIL
// 调用时机：需要获取作业索引用于链表操作时
txidx_t txq_job2idx (txq_t* txq, txjob_t* job) {
    if( job==NULL )                       // 空指针检查
        return TXIDX_NIL;
    return job - txq->txjobs;             // 通过指针算术计算索引
}

// === TXQ链表遍历操作 ===

// 功能：获取链表中下一个作业 - 用于遍历作业链表
// 参数：txq - 发送队列对象
//       j - 当前作业对象指针
// 返回值：下一个作业对象指针，链表结束时返回NULL
// 调用时机：遍历特定TX单元的作业链表时
txjob_t* txq_nextJob (txq_t* txq, txjob_t* j) {
    if( j == NULL )                       // 空指针检查
        return NULL;
    assert(j->next != TXIDX_NIL);         // 确保作业在链表中
    if( j->next == TXIDX_END )            // 检查是否为链表末尾
        return NULL;
    return &txq->txjobs[j->next];         // 返回下一个作业
}

// 功能：获取下一个索引的地址 - 用于链表索引操作
// 参数：txq - 发送队列对象
//       pidx - 当前索引指针
// 返回值：下一个索引的地址指针
// 调用时机：需要修改链表结构时获取下一个节点的索引地址
txidx_t* txq_nextIdx (txq_t* txq, txidx_t* pidx) {
    assert(*pidx != TXIDX_NIL);           // 确保索引有效
    if( *pidx == TXIDX_END )              // 如果已到链表末尾
        return pidx;                      // 返回当前位置
    return &(txq->txjobs[*pidx].next);    // 返回下一个索引的地址
}

// === TXQ链表操作函数 ===

// 功能：从链表中取出作业 - 移除链表头部的第一个作业
// 参数：txq - 发送队列对象
//       pidx - 链表头索引指针
// 返回值：被取出的作业对象指针，空链表时返回NULL
// 调用时机：从空闲链表或TX单元链表中取出作业时
txjob_t* txq_unqJob (txq_t* txq, txidx_t* pidx) {
    assert(*pidx != TXIDX_NIL);           // 确保链表头有效
    if( *pidx == TXIDX_END )              // 空链表检查
        return NULL;
    
    txjob_t* j = &txq->txjobs[*pidx];     // 获取头部作业
    *pidx = j->next;                      // 更新链表头为下一个作业
    j->next = TXIDX_NIL;                  // 标记作业不在任何链表中
    return j;                             // 返回取出的作业
}

// 功能：向链表中插入作业 - 将作业插入到链表头部
// 参数：txq - 发送队列对象
//       pidx - 链表头索引指针
//       j - 要插入的作业对象
// 返回值：无
// 调用时机：将作业加入空闲链表或TX单元链表时
void txq_insJob (txq_t* txq, txidx_t* pidx, txjob_t* j) {
    assert(*pidx != TXIDX_NIL && j->next == TXIDX_NIL);  // 确保参数有效
    j->next = *pidx;                      // 作业指向当前链表头
    *pidx = j - txq->txjobs;              // 更新链表头为新作业的索引
}

// 功能：释放作业 - 清理作业数据并返回到空闲链表
// 参数：txq - 发送队列对象
//       j - 要释放的作业对象
// 返回值：无
// 调用时机：作业完成或取消时释放资源
void txq_freeJob (txq_t* txq, txjob_t* j) {
    txq_freeData(txq, j);                 // 先释放关联的数据
    txq_insJob(txq, &txq->freeJobs, j);   // 将作业返回空闲链表
}


// === TXQ数据管理系统 ===

// 功能：预留作业 - 从空闲链表取出作业供调用者填充，支持取消操作
// 参数：txq - 发送队列对象
// 返回值：预留的作业对象指针，无可用作业时返回NULL
// 调用时机：需要创建新的发送作业时
// 设计说明：调用者可以开始填充数据但可以不调用commitJob就放弃，
//          只有调用commitJob()后数据才会被正式保留
txjob_t* txq_reserveJob (txq_t* txq) {
    txidx_t idx = txq->freeJobs;          // 获取空闲链表头
    assert(idx != TXIDX_NIL);             // 确保空闲链表已初始化
    if( idx == TXIDX_END )                // 检查是否有空闲作业
        return NULL;                      // 无可用作业
    
    txjob_t* j = &txq->txjobs[idx];       // 获取空闲作业对象
    
    // 验证作业状态：字段可能已被填充但由于未提交而应该是这样的状态
    assert(j->next != TXIDX_NIL);         // 确保作业在空闲链表中
    assert(j->off  == TXOFF_NIL);         // 确保无关联数据
    
    // 重置所有字段为零 - 之前的reserveJob调用可能部分填充了数据然后放弃
    idx = j->next;                        // 保存下一个空闲作业索引
    memset(j, 0, sizeof(*j));             // 清零作业结构
    j->off = TXOFF_NIL;                   // 标记无数据关联
    j->next = idx;                        // 保持在空闲链表中的位置
    return j;                             // 返回预留的作业
}

// 功能：预留数据空间 - 为作业分配连续的数据缓冲区空间
// 参数：txq - 发送队列对象
//       maxlen - 需要的最大数据长度
// 返回值：数据缓冲区指针，空间不足时返回NULL
// 调用时机：预留作业后需要存储发送数据时
u1_t* txq_reserveData (txq_t* txq, txoff_t maxlen) {
    if( maxlen > MAX_TXDATA - txq->txdataInUse )  // 检查可用空间
        return NULL;                      // 数据空间不足
    return &txq->txdata[txq->txdataInUse]; // 返回可用数据区域起始地址
}

// 功能：提交作业 - 正式确认作业和数据，从空闲链表移除并分配数据空间
// 参数：txq - 发送队列对象
//       j - 要提交的作业对象
// 返回值：无
// 调用时机：完成作业填充并确认提交时
void txq_commitJob (txq_t* txq, txjob_t*j) {
    assert(j == &txq->txjobs[txq->freeJobs]);     // 确保是空闲链表头部作业
    assert(j->len <= MAX_TXDATA - txq->txdataInUse); // 确保数据长度合法
    assert(j->off == TXOFF_NIL);          // 确保尚未分配数据偏移
    
    // 从空闲链表移除头部作业
    txq->freeJobs = j->next;              // 更新空闲链表头
    j->next = TXIDX_NIL;                  // 标记作业不在任何链表中
    j->off = txq->txdataInUse;            // 设置数据在缓冲区中的偏移
    txq->txdataInUse += j->len;           // 增加已使用数据空间计数
}

// === TXQ内存紧凑化系统 ===

// 功能：释放数据 - 清理作业关联数据并进行内存紧凑化
// 参数：txq - 发送队列对象
//       j - 要释放数据的作业对象
// 返回值：无
// 调用时机：作业完成或取消时释放其占用的数据空间
// 设计说明：采用即时紧凑化策略，删除数据时立即移动后续数据填补空洞，
//          并同步更新所有受影响作业的数据偏移量
void txq_freeData (txq_t* txq, txjob_t* j) {
    
    // 如果作业有关联数据则进行紧凑化处理并修正所有TX队列中作业的偏移量
    txoff_t freeOff = j->off;             // 获取要释放的数据起始偏移
    if( freeOff == TXOFF_NIL )            // 如果作业无关联数据
        return;                           // 直接返回，无需处理
    
    u1_t freeLen = j->len;                // 获取要释放的数据长度
    
    // 遍历所有作业，调整受影响作业的数据偏移量
    for( txidx_t idx=0; idx<MAX_TXJOBS; idx++ ) {
        txjob_t* fixjob = &txq->txjobs[idx]; // 获取当前检查的作业
        // 如果作业有数据且偏移量大于等于被释放的位置
        if( fixjob->off != TXOFF_NIL && fixjob->off >= freeOff )
            fixjob->off -= freeLen;       // 向前调整偏移量
    }
    
    // 执行数据紧凑化：将释放位置后的数据向前移动
    txoff_t freeEnd = freeOff + freeLen;  // 计算释放区域结束位置
    if( freeEnd < txq->txdataInUse )      // 如果后面还有数据需要移动
        memcpy(&txq->txdata[freeOff], &txq->txdata[freeEnd], 
               txq->txdataInUse - freeEnd); // 将后续数据前移填补空洞
    
    txq->txdataInUse -= freeLen;          // 减少已使用数据空间计数
    j->off = TXOFF_NIL;                   // 清除作业的数据偏移
    j->len = 0;                           // 清除作业的数据长度
}


// ================================================================================
//
// RXQ - 接收队列管理系统  
//
// ================================================================================

// 接收队列系统特性：
// RX状态维护两个FIFO队列：rxjobs(作业描述符)和rxdata(帧数据)
// 
// FIFO出队操作：将rxjob/rxdata序列化为JSON并传递给websocket
// FIFO入队操作：从无线电层获取帧数据，填充rxjob并追加rxdata
// 
// 已使用区域在可用数组中向后游走。当剩余空间耗尽时，已使用区域
// 会移动到数组开头并调整索引指针。
//
// 内存布局示意图：
//      off     last
//      first   next
//       |      |                      |      |  压缩操作
//  |----|xxxxxx|----|         |-------|xxxxxx|    ==>  |xxxxxx|-------|
//  空闲  已使用  空闲           空闲     已使用        压缩后  可用空间
//

// === RXQ初始化系统 ===

// 功能：初始化接收队列 - 设置队列为空状态
// 参数：rxq - 接收队列对象指针
// 返回值：无
// 调用时机：系统启动时初始化接收队列系统
void rxq_ini (rxq_t* rxq) {
    rxq->first = rxq->next = 0;           // 设置队列为空：首尾指针相等
}

// === RXQ智能分配系统 ===

// 功能：分配下一个作业 - 智能分配接收作业并在需要时进行空间压缩
// 参数：rxq - 接收队列对象
// 返回值：分配的作业对象指针，空间不足时返回NULL
// 调用时机：从无线电层接收到新帧需要创建作业时
// 设计说明：作业仅预留标记状态：
//          - 出错时调用者不会再回来，作业自动废弃
//          - 数据填充完成后调用者必须调用rxq_commitJob确认
rxjob_t* rxq_nextJob (rxq_t* rxq) {
    rxjob_t* jobs = rxq->rxjobs;          // 获取作业数组
    rxidx_t first = rxq->first;           // 获取队列首部索引
    rxidx_t next = rxq->next;             // 获取队列尾部索引
    
    // 如果队列为空，重置到数组开头
    if( first==next ) {
        rxq->first = rxq->next = 0;       // 重置队列指针
        jobs[0].off = jobs[0].len = 0;    // 初始化第一个作业的数据位置
        jobs[0].fts = -1;                 // 设置帧时间戳为无效值
        return &jobs[0];                  // 返回第一个作业位置
    }
    
    // 检查作业数组空间是否用尽
    if( next >= MAX_RXJOBS ) {
        // 作业或缓冲区空间不足 - 需要压缩？
        if( first == 0 ) {                // 如果已经在数组开头无法压缩
            LOG(MOD_S2E|WARNING, "RX out of jobs");  // 记录作业耗尽警告
            return NULL;                  // 返回失败
        }
        // 将已使用的作业移动到数组开头
        memmove(&jobs[0], &jobs[first], sizeof(jobs[0])*(next-first));
        rxq->next = next -= first;        // 调整尾部指针
        rxq->first = first = 0;           // 重置首部指针到开头
    }

    // 计算数据空间需求和分配
    rxjob_t* last = &jobs[next-1];        // 获取最后一个有效作业
    rxoff_t end = last->off + last->len;  // 计算数据区域结束位置
    
 check_rxdata:  // 数据空间检查点，压缩后可能需要重新检查
    // 检查是否有足够的数据缓冲区空间
    if( end + MAX_RXFRAME_LEN > MAX_RXDATA ) {
        // 数据或缓冲区空间不足 - 需要压缩？
        rxoff_t off = jobs[first].off;    // 获取第一个作业的数据偏移
        if( off == 0 ) {                  // 如果已经在数据开头无法压缩
            LOG(MOD_S2E|WARNING, "RX out of data space");  // 记录数据空间耗尽
            return NULL;                  // 返回失败
        }
        // 将数据向前移动压缩空间
        memmove(&rxq->rxdata[0], &rxq->rxdata[off], end-off);
        // 调整所有作业的数据偏移量
        for( int i=first; i < next; i++ )
            jobs[i].off -= off;           // 减去压缩的偏移量
        end -= off;                       // 调整数据结束位置
        goto check_rxdata;                // 重新检查数据空间
    }
    
    // 分配新的作业位置
    last += 1;                            // 移动到下一个作业位置
    last->off = end;                      // 设置数据起始偏移
    last->len = 0;                        // 初始化数据长度
    last->fts = -1;                       // 初始化帧时间戳
    return last;                          // 返回新分配的作业
}

// === RXQ作业确认系统 ===

// 功能：提交作业 - 确认作业有效并正式加入队列
// 参数：rxq - 接收队列对象
//       p - 要提交的作业对象
// 返回值：无
// 调用时机：完成接收作业数据填充后确认提交
void rxq_commitJob (rxq_t* rxq, rxjob_t* p) {
    assert(p == &rxq->rxjobs[rxq->next]);  // 确保是下一个待提交的作业
    rxq->next += 1;                       // 向前移动队列尾部指针
}

// === RXQ作业删除系统 ===

// 功能：删除作业 - 从队列中删除指定作业并返回新的末尾作业指针
// 参数：rxq - 接收队列对象
//       p - 要删除的作业对象
// 返回值：删除后新的末尾作业指针
// 调用时机：删除影子帧等无效作业时使用
rxjob_t* rxq_dropJob (rxq_t* rxq, rxjob_t* p) {
    rxjob_t* jobs = rxq->rxjobs;          // 获取作业数组
    u1_t* rxdata = rxq->rxdata;           // 获取数据缓冲区
    rxjob_t* last = &jobs[--rxq->next];   // 获取新的末尾作业并调整队列尾部
    
    // 计算被删除作业的数据范围
    rxoff_t poff = p->off;                // 被删除作业的数据起始偏移
    rxoff_t pend = poff + p->len;         // 被删除作业的数据结束偏移
    assert(p >= jobs && p < last);        // 确保作业在有效范围内
    
    // 移动数据：将删除位置后的数据向前移动
    memmove(&rxdata[poff], &rxdata[pend], 
            last->off + last->len - pend); // 移动后续数据填补空洞
    
    // 移动作业：将删除位置后的作业向前移动
    memmove(&p[0], &p[1], sizeof(jobs[0])*(last-p+1));
    
    // 调整受影响作业的数据偏移量
    poff = pend-poff;                     // 计算偏移调整量
    while( p <= last ) {                  // 遍历后续所有作业
        p->off -= poff;                   // 向前调整数据偏移
        p++;                              // 移动到下一个作业
    }
    return last-1;                        // 返回新的末尾作业指针
}
