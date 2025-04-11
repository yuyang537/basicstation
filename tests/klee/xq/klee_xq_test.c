#include <klee/klee.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

// 项目头文件
#include "../../../src/rt.h"  // 基础类型定义
#include "../../../src/sys.h"  // 系统接口
#include "../../../src/xq.h"  // 测试目标模块

// 测试用例1: 发送队列初始化和基本操作
void test_txq_initialization() {
    txq_t* txq = (txq_t*)malloc(sizeof(txq_t));
    assert(txq != NULL);
    
    // 初始化队列
    txq_ini(txq);
    
    // 验证初始化后的状态
    for(txidx_t i=0; i<MAX_TXJOBS-1; i++) {
        assert(txq->txjobs[i].next == i+1);
        assert(txq->txjobs[i].off == TXOFF_NIL);
    }
    assert(txq->txjobs[MAX_TXJOBS-1].next == TXIDX_END);
    assert(txq->txjobs[MAX_TXJOBS-1].off == TXOFF_NIL);
    
    // 测试 txq_idx2job 和 txq_job2idx 之间的一致性
    for(txidx_t i=0; i<MAX_TXJOBS; i++) {
        txjob_t* job = txq_idx2job(txq, i);
        assert(job != NULL);
        assert(txq_job2idx(txq, job) == i);
    }
    
    assert(txq_idx2job(txq, TXIDX_NIL) == NULL);
    assert(txq_idx2job(txq, TXIDX_END) == NULL);
    
    free(txq);
}

// 测试用例2: 任务预留和提交
void test_txq_job_reserve_commit() {
    txq_t* txq = (txq_t*)malloc(sizeof(txq_t));
    assert(txq != NULL);
    
    // 初始化队列
    txq_ini(txq);
    
    // 预留任务
    txjob_t* job = txq_reserveJob(txq);
    assert(job != NULL);
    
    // 填充任务数据
    job->txtime = 1000000;
    job->deveui = 0x1122334455667788;
    job->diid = 123;
    job->freq = 868100000;
    job->txpow = 14;
    job->dr = 5;
    job->prio = 1;
    job->len = 20;
    
    // 预留数据空间
    u1_t* data = txq_reserveData(txq, 20);
    assert(data != NULL);
    
    // 填充数据（符号化）
    u1_t symbolicData[20];
    klee_make_symbolic(symbolicData, 20, "symbolicData");
    memcpy(data, symbolicData, 20);
    
    // 提交任务
    txq_commitJob(txq, job);
    
    // 验证任务状态
    assert(job->off != TXOFF_NIL);
    assert(job->len == 20);
    
    // 释放任务
    txq_freeJob(txq, job);
    
    // 验证释放后任务回到空闲队列
    txidx_t idx = txq_job2idx(txq, job);
    txidx_t freeJobs = txq->freeJobs;
    int found = 0;
    
    // 在空闲队列中查找
    while(freeJobs != TXIDX_END) {
        if(freeJobs == idx) {
            found = 1;
            break;
        }
        freeJobs = txq->txjobs[freeJobs].next;
    }
    
    assert(found == 1);
    
    free(txq);
}

// 测试用例3: 队列操作 - 插入和删除
void test_txq_queue_operations() {
    txq_t* txq = (txq_t*)malloc(sizeof(txq_t));
    assert(txq != NULL);
    
    // 初始化队列
    txq_ini(txq);
    
    // 创建一个空队列
    txidx_t queue = TXIDX_END;
    
    // 测试数据
    int jobCount = 5;
    txjob_t* jobs[5];
    
    // 向队列中添加任务
    for(int i=0; i<jobCount; i++) {
        jobs[i] = txq_reserveJob(txq);
        assert(jobs[i] != NULL);
        
        // 设置一些符号化数据
        jobs[i]->txtime = 1000000 + i*1000;
        jobs[i]->diid = i;
        jobs[i]->prio = klee_range(0, 3, "job_priority");
        
        txq_insJob(txq, &queue, jobs[i]);
    }
    
    // 验证队列不为空
    assert(queue != TXIDX_END);
    
    // 从队列中移除任务并验证
    txjob_t* job = txq_unqJob(txq, &queue);
    assert(job != NULL);
    assert(job->next == TXIDX_NIL);
    assert(job->diid == 4);  // 最后添加的
    
    // 继续移除更多任务
    job = txq_unqJob(txq, &queue);
    assert(job != NULL);
    assert(job->diid == 3);
    
    // 释放所有任务
    txq_freeJob(txq, jobs[4]);
    txq_freeJob(txq, jobs[3]);
    
    while((job = txq_unqJob(txq, &queue)) != NULL) {
        txq_freeJob(txq, job);
    }
    
    // 验证队列为空
    assert(queue == TXIDX_END);
    
    free(txq);
}

// 测试用例4: 接收队列初始化和基本操作
void test_rxq_initialization() {
    rxq_t* rxq = (rxq_t*)malloc(sizeof(rxq_t));
    assert(rxq != NULL);
    
    // 初始化接收队列
    rxq_ini(rxq);
    
    // 验证初始状态
    assert(rxq->first == 0);
    assert(rxq->next == 0);
    
    free(rxq);
}

// 测试用例5: 接收任务提交和处理
void test_rxq_job_commit() {
    rxq_t* rxq = (rxq_t*)malloc(sizeof(rxq_t));
    assert(rxq != NULL);
    
    // 初始化接收队列
    rxq_ini(rxq);
    
    // 获取下一个可用接收任务
    rxjob_t* job = rxq_nextJob(rxq);
    assert(job != NULL);
    
    // 填充符号化数据
    job->rctx = klee_range(1, 100, "rctx");
    job->xtime = klee_range(1000000, 2000000, "xtime");
    job->freq = 868100000;
    job->rssi = klee_range(0, 255, "rssi");
    job->snr = klee_range(-128, 127, "snr");
    job->dr = klee_range(0, 15, "dr");
    job->len = klee_range(10, 50, "len");
    
    // 验证长度在合理范围内
    klee_assume(job->len <= 255);
    
    // 提交任务
    rxq_commitJob(rxq, job);
    
    // 获取下一个任务，应该是我们刚刚提交的
    job = rxq_nextJob(rxq);
    assert(job != NULL);
    
    // 丢弃该任务
    job = rxq_dropJob(rxq, job);
    
    // 验证下一个任务为NULL（队列为空）
    assert(rxq_nextJob(rxq) == NULL);
    
    free(rxq);
}

// 测试用例6: 边界条件 - 内存安全测试
void test_boundary_conditions() {
    txq_t* txq = (txq_t*)malloc(sizeof(txq_t));
    assert(txq != NULL);
    
    // 初始化队列
    txq_ini(txq);
    
    // 尝试预留所有可用的任务
    txjob_t* jobs[MAX_TXJOBS];
    int count = 0;
    
    for(int i=0; i<MAX_TXJOBS; i++) {
        jobs[count] = txq_reserveJob(txq);
        if(jobs[count] != NULL) {
            count++;
        } else {
            break;
        }
    }
    
    // 验证我们已分配所有任务
    assert(count == MAX_TXJOBS);
    
    // 尝试再分配一个应该失败
    txjob_t* extraJob = txq_reserveJob(txq);
    assert(extraJob == NULL);
    
    // 释放所有任务
    for(int i=0; i<count; i++) {
        txq_freeJob(txq, jobs[i]);
    }
    
    // 现在应该能够再次分配任务
    txjob_t* newJob = txq_reserveJob(txq);
    assert(newJob != NULL);
    txq_freeJob(txq, newJob);
    
    free(txq);
}

// 测试用例7: 数据缓冲区管理安全测试
void test_data_buffer_management() {
    txq_t* txq = (txq_t*)malloc(sizeof(txq_t));
    assert(txq != NULL);
    
    // 初始化队列
    txq_ini(txq);
    
    // 尝试分配较大的数据块
    int maxSize = MAX_TXDATA / 2;
    u1_t* data = txq_reserveData(txq, maxSize);
    assert(data != NULL);
    
    // 符号化数据并填充，注意边界
    u1_t symbolicData[100];  // 使用较小的符号化数据
    klee_make_symbolic(symbolicData, 100, "largeDataBlock");
    
    // 只复制最多100字节，确保不会越界
    int copySize = (maxSize < 100) ? maxSize : 100;
    memcpy(data, symbolicData, copySize);
    
    // 预留一个任务并将其与数据关联
    txjob_t* job = txq_reserveJob(txq);
    assert(job != NULL);
    job->len = copySize;
    txq_commitJob(txq, job);
    
    // 验证任务与数据正确关联
    assert(job->off != TXOFF_NIL);
    
    // 释放任务应该同时释放关联的数据
    txq_freeJob(txq, job);
    
    // 现在应该可以再次分配相同大小的数据
    data = txq_reserveData(txq, maxSize);
    assert(data != NULL);
    
    // 创建新任务
    job = txq_reserveJob(txq);
    assert(job != NULL);
    job->len = maxSize;
    txq_commitJob(txq, job);
    
    // 释放任务
    txq_freeJob(txq, job);
    
    free(txq);
}

// 主测试函数
int main() {
    // 运行所有测试用例
    test_txq_initialization();
    test_txq_job_reserve_commit();
    test_txq_queue_operations();
    test_rxq_initialization();
    test_rxq_job_commit();
    test_boundary_conditions();
    test_data_buffer_management();
    
    return 0;
}