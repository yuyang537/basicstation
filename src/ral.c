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

//=============================================================================
// 文件功能：无线电抽象层(RAL)通用实现
// 作用说明：提供RAL层的通用算法实现，主要包括信道分配算法
//          该文件包含与具体硬件无关的通用逻辑
//=============================================================================

#include "kwcrc.h"
#include "sys.h"
#include "ral.h"

//=============================================================================
// SX130X射频前端频率偏移限制常量定义
// 功能：定义不同带宽下射频前端的最大频率偏移范围
// 原理：SX130X芯片的射频前端(RFF)有固定的频率范围限制，超出范围会影响接收性能
//=============================================================================
#define SX130X_RFE_MAXCOFF_125 (( 925000 - 125000)/2)  // 125kHz带宽时的最大中心频率偏移：400kHz
#define SX130X_RFE_MAXCOFF_250 ((1000000 - 250000)/2)  // 250kHz带宽时的最大中心频率偏移：375kHz  
#define SX130X_RFE_MAXCOFF_500 ((1100000 - 500000)/2)  // 500kHz带宽时的最大中心频率偏移：300kHz

//=============================================================================
// SX130X硬件规格常量定义
// 功能：定义SX130X系列芯片的硬件结构参数
//=============================================================================
#define SX130X_RFF_NB 2   // 每个芯片的射频前端数量(2个RFF，支持不同频段)
#define SX130X_IF_NB 10   // 每个芯片的中频信道数量(10个IF信道，支持并发接收)

//=============================================================================
// 函数功能：信道分配算法实现
// 参数说明：upchs - 上行信道定义列表，包含所有需要分配的信道
//          alloc_cb - 分配回调函数，用于通知分配进度和结果
//          ctx - 用户上下文指针，传递给回调函数
// 返回值：成功返回1，失败返回0
// 调用时机：RAL配置时调用，将逻辑信道映射到物理硬件芯片
// 算法原理：遍历所有待分配信道，按125kHz、FSK、快速LoRa的顺序分配到芯片，
//          考虑射频前端频率范围限制，确保信道可以正确接收
// 分配策略：
//   1. 优先分配125kHz LoRa信道，因为它们占用带宽小，容易安排
//   2. 然后分配FSK信道，使用固定的调制解调器
//   3. 最后分配快速LoRa信道(250/500kHz)，带宽大，对频率范围要求更严格
//=============================================================================
int ral_challoc (chdefl_t* upchs, challoc_cb alloc_cb, void* ctx) {
    // 射频前端频率跨度数组：每个芯片2个RFF，每个RFF记录[最小频率,最大频率]
    // 数组结构：[chip0_rff0_min, chip0_rff0_max, chip0_rff1_min, chip0_rff1_max, ...]
    u4_t rfe_spans[MAX_130X*SX130X_RFF_NB*2] = {0};
    for( int chip=0; chip<MAX_130X; chip++ ) {
        // 初始化每个芯片的射频前端频率范围：最小值设为无穷大，最大值设为0
        rfe_spans[2*SX130X_RFF_NB*chip] = rfe_spans[2*SX130X_RFF_NB*chip+2] = INT_MAX;  // 初始化最小频率为最大值
        // rfe_spans[2*SX130X_RFF_NB*chip+1] = rfe_spans[2*SX130X_RFF_NB*chip+3] = 0;   // 最大频率初始化为0
    }

    // 分配状态跟踪变量，用于记录当前分配进度
    uint modem_idx = 0;      // 当前芯片内调制解调器索引(0-9)
    uint chip_idx = 0;       // 当前芯片索引(0到MAX_130X-1)
    uint multi_chslot = 0;   // 多速率信道槽位索引(125kHz LoRa，支持多个扩频因子)
    uint fsk_chslot = 0;     // FSK信道槽位索引(FSK调制专用)
    uint fl_chslot = 0;      // 快速LoRa信道槽位索引(250/500kHz LoRa)

    alloc_cb(ctx, NULL, CHALLOC_START);  // 通知回调函数开始分配过程

    // 主分配循环：遍历所有芯片，为每个芯片分配信道
    // 继续分配，直到芯片用完或所有信道都已分配
    while( chip_idx < MAX_130X && (multi_chslot < MAX_UPCHNLS || fsk_chslot < MAX_UPCHNLS || fl_chslot < MAX_UPCHNLS) ) {
        alloc_cb(ctx, &(challoc_t) {.chip = chip_idx}, CHALLOC_CHIP_START);  // 通知开始当前芯片的分配

        //=========================================================================
        // 第一阶段：分配125kHz LoRa信道
        // 策略：优先分配125kHz信道，因为它们占用带宽小，容易安排
        // 原理：125kHz信道对射频前端的频率偏移要求最宽松，可以更灵活地组合
        //=========================================================================
        modem_idx = 0;  // 重置当前芯片的调制解调器索引
        while( multi_chslot < MAX_UPCHNLS && modem_idx < SX130X_IF_NB-2 ) {  // 保留最后2个modem给FSK和快速LoRa
            // 跳过非125kHz或FSK信道，只处理125kHz LoRa信道
            if( !upchs->freq[multi_chslot] ||                               // 跳过空信道
                upchs->rps[multi_chslot].maxSF == FSK ||                    // 跳过FSK信道
                upchs->rps[multi_chslot].bw != BW125 ) {                    // 跳过非125kHz信道
                multi_chslot++;  // 移动到下一个信道槽位
                continue;
            }
            u4_t ch_freq = upchs->freq[multi_chslot];  // 获取待分配信道的频率

            // 尝试在当前芯片的2个射频前端中找到合适位置
            for( int rfe_idx=0; rfe_idx<SX130X_RFF_NB; rfe_idx++ ) {
                // 获取对应射频前端的频率跨度指针：[最小频率, 最大频率]
                u4_t* rfe_span = &rfe_spans[2*rfe_idx+4*chip_idx];
                
                // 检查信道频率是否在射频前端覆盖范围内，或者射频前端还未使用
                // 条件1：射频前端还未使用(rfe_span[1] == 0)
                // 条件2：新信道频率在射频前端的频率偏移范围内
                if( rfe_span[1] == 0 ||                                     // 射频前端未使用
                    (ch_freq - rfe_span[0])/2 <= SX130X_RFE_MAXCOFF_125 ) { // 频率偏移在125kHz范围内
                    
                    // 更新射频前端频率跨度，扩展到包含新信道
                    rfe_span[0] = min(ch_freq, rfe_span[0]);                // 更新最小频率
                    rfe_span[1] = max(ch_freq, rfe_span[1]);                // 更新最大频率
                    
                    // 通知信道分配成功，提供详细的分配信息
                    alloc_cb(ctx, &(challoc_t) {
                        .chip = chip_idx,                                   // 分配到的芯片编号
                        .chan = modem_idx++,                                // 分配到的调制解调器编号(后递增)
                        .rff = rfe_idx,                                     // 分配到的射频前端编号
                        .rff_freq = (rfe_span[0]+rfe_span[1])/2,           // 射频前端中心频率
                        .chdef = {.freq = ch_freq, .rps = upchs->rps[multi_chslot]}  // 信道定义
                    }, CHALLOC_CH);
                    
                    ch_freq = 0;      // 标记信道已分配
                    multi_chslot++;   // 移动到下一个信道槽位
                    break;            // 跳出射频前端循环
                }
            }
            if( ch_freq ) break;  // 如果信道未分配成功，当前芯片空间不足，尝试下一个芯片
        }

        //=========================================================================
        // 第二阶段：分配FSK信道
        // 策略：FSK使用固定的调制解调器(最后一个)，每个芯片只分配一个FSK信道
        // 原理：FSK调制与LoRa调制不同，需要专用的调制解调器
        //=========================================================================
        while( fsk_chslot < MAX_UPCHNLS ) {
            // 跳过非FSK信道，只处理FSK调制的信道
            if( !upchs->freq[fsk_chslot] ||                                 // 跳过空信道
                upchs->rps[fsk_chslot].maxSF != FSK ) {                     // 跳过非FSK信道
                fsk_chslot++;  // 移动到下一个FSK槽位
                continue;
            }
            if( chip_idx >= MAX_130X ) break; // 芯片用完了，无法分配更多FSK信道
            
            u4_t ch_freq = upchs->freq[fsk_chslot];  // 获取FSK信道频率
            
            // 尝试在当前芯片的射频前端中找到位置
            for( int rfe_idx=0; rfe_idx<SX130X_RFF_NB; rfe_idx++ ) {
                u4_t* rfe_span = &rfe_spans[2*rfe_idx+4*chip_idx];
                
                // FSK对频率偏移的容忍度较大，可以在更宽的频率范围内工作
                // 检查FSK信道频率是否与当前射频前端兼容
                if( rfe_span[1] == 0 ||                                     // 射频前端未使用
                    (ch_freq >= rfe_span[1] - 2*SX130X_RFE_MAXCOFF_125 &&  // 频率在扩展范围内
                     ch_freq <= rfe_span[0] + 2*SX130X_RFE_MAXCOFF_125) ) {
                    
                    // 更新射频前端频率跨度
                    rfe_span[0] = min(ch_freq, rfe_span[0]);                // 更新最小频率
                    rfe_span[1] = max(ch_freq, rfe_span[1]);                // 更新最大频率
                    
                    // 通知FSK信道分配成功(使用最后一个调制解调器)
                    alloc_cb(ctx, &(challoc_t) {
                        .chip = chip_idx,                                   // 分配到的芯片编号
                        .chan = SX130X_IF_NB-1,                            // FSK专用调制解调器(第9号)
                        .rff = rfe_idx,                                     // 分配到的射频前端编号
                        .rff_freq = (rfe_span[0]+rfe_span[1])/2,           // 射频前端中心频率
                        .chdef = {.freq = ch_freq, .rps = upchs->rps[fsk_chslot]}  // FSK信道定义
                    }, CHALLOC_CH);
                    
                    fsk_chslot++;   // 移动到下一个FSK槽位
                    modem_idx++;    // 增加调制解调器计数
                    break;          // 跳出射频前端循环
                }
            }
            break;  // 只分配一个FSK信道到当前芯片，然后处理下一阶段
        }

        //=========================================================================
        // 第三阶段：分配快速LoRa信道(250kHz/500kHz)
        // 策略：快速LoRa使用倒数第二个调制解调器，带宽大，对频率范围要求更严格
        // 原理：大带宽信道需要更精确的频率控制，对射频前端的频率偏移要求更严格
        //=========================================================================
        while( fl_chslot < MAX_UPCHNLS ) {
            // 跳过非快速LoRa信道，只处理250kHz和500kHz LoRa信道
            if( !upchs->freq[fl_chslot] ||                                  // 跳过空信道
                (upchs->rps[fl_chslot].bw != BW250 &&                       // 跳过非250kHz信道
                 upchs->rps[fl_chslot].bw != BW500) ) {                     // 跳过非500kHz信道
                fl_chslot++;  // 移动到下一个快速LoRa槽位
                continue;
            }
            if( chip_idx >= MAX_130X ) break; // 芯片用完了，无法分配更多快速LoRa信道
            
            u4_t ch_freq = upchs->freq[fl_chslot];  // 获取快速LoRa信道频率
            
            // 根据带宽确定最大频率偏移：不同带宽有不同的频率偏移限制
            int maxcoff = upchs->rps[fl_chslot].bw == BW250 ? SX130X_RFE_MAXCOFF_250 : SX130X_RFE_MAXCOFF_500;
            
            // 尝试在当前芯片的射频前端中找到位置
            for( int rfe_idx=0; rfe_idx<SX130X_RFF_NB; rfe_idx++ ) {
                u4_t* rfe_span = &rfe_spans[2*rfe_idx+4*chip_idx];
                
                // 计算当前射频前端的频率范围
                u4_t cmin = rfe_span[1] - SX130X_RFE_MAXCOFF_125;          // 当前射频前端的最小中心频率
                u4_t cmax = rfe_span[0] + SX130X_RFE_MAXCOFF_125;          // 当前射频前端的最大中心频率
                
                // 检查快速LoRa信道是否与当前射频前端兼容
                // 需要确保快速LoRa信道的频率范围不会超出射频前端的能力
                if( rfe_span[1] == 0 ||                                     // 射频前端未使用
                    (ch_freq >= cmin - maxcoff &&                          // 信道频率在射频前端范围内
                     ch_freq <= cmax + maxcoff) ) {
                    
                    // 更新射频前端频率跨度
                    rfe_span[0] = min(ch_freq, rfe_span[0]);                // 更新最小频率
                    rfe_span[1] = max(ch_freq, rfe_span[1]);                // 更新最大频率
                    
                    // 通知快速LoRa信道分配成功(使用倒数第二个调制解调器)
                    alloc_cb(ctx, &(challoc_t) {
                        .chip = chip_idx,                                   // 分配到的芯片编号
                        .chan = SX130X_IF_NB-2,                            // 快速LoRa专用调制解调器(第8号)
                        .rff = rfe_idx,                                     // 分配到的射频前端编号
                        // 优化的射频前端中心频率：在满足约束条件下选择最优值
                        .rff_freq = (max(cmin, ch_freq-maxcoff)+min(cmax, ch_freq+maxcoff))/2,
                        .chdef = { .freq = ch_freq, .rps = upchs->rps[fl_chslot]}  // 快速LoRa信道定义
                    }, CHALLOC_CH);
                    
                    fl_chslot++;    // 移动到下一个快速LoRa槽位
                    modem_idx++;    // 增加调制解调器计数
                    break;          // 跳出射频前端循环
                }
            }
            break;  // 只分配一个快速LoRa信道到当前芯片，然后处理下一个芯片
        }

        // 通知当前芯片分配完成，提供芯片统计信息
        alloc_cb(ctx, &(challoc_t) {
            .chipid = chip_idx,                                                      // 芯片ID
            .chans  = modem_idx,                                                     // 分配到该芯片的信道数量
            .minFreq= modem_idx ? rfe_spans[4*chip_idx] : 0,                        // 芯片覆盖的最小频率
            .maxFreq= modem_idx ? max(rfe_spans[4*chip_idx+3],rfe_spans[4*chip_idx+1]) : 0,  // 芯片覆盖的最大频率
        }, CHALLOC_CHIP_DONE);
        
        chip_idx++;  // 移动到下一个芯片
    }

    // 通知整个分配过程完成
    alloc_cb(ctx, NULL, CHALLOC_DONE);
    return 1;  // 分配成功
}
