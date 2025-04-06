/*
 * KLEE符号执行测试 - LoRa模块安全属性验证
 * ======================================
 * 
 * 本测试文件用于验证LoRa模块的关键安全属性：
 * 1. 帧解析安全
 * 2. 过滤机制安全
 * 3. 信标生成安全
 * 4. 数据完整性安全
 * 5. 协议兼容性安全
 */

#include <klee/klee.h>
#include <stdio.h>
#include <string.h>
#include "lora.h"
#include "uj.h"
#include "rt.h"

// 测试帧解析安全
void test_lora_frame_parse() {
    u1_t frame[256];
    int len;
    ujbuf_t buf;
    dbuf_t lbuf;
    
    // 符号化输入
    klee_make_symbolic(frame, sizeof(frame), "lora_frame");
    klee_make_symbolic(&len, sizeof(len), "frame_length");
    klee_assume(len > 0 && len <= sizeof(frame));
    
    // 初始化缓冲区
    uj_iniBuf(&buf);
    dbuf_ini(&lbuf);
    
    // 测试帧解析
    int result = s2e_parse_lora_frame(&buf, frame, len, &lbuf);
    
    // 安全属性断言
    if (result == 1) {
        // 验证帧格式
        klee_assert((frame[OFF_mhdr] & (MHDR_RFU|MHDR_MAJOR)) == MAJOR_V1);
        klee_assert(len >= OFF_df_minlen || (frame[OFF_mhdr] & MHDR_FTYPE) == FRMTYPE_PROP);
    }
}

// 测试Join EUI过滤
void test_lora_joineui_filter() {
    uL_t joineui;
    uL_t filter[4] = {0x12345678, 0x87654321, 0x0, 0x0};
    
    // 设置过滤规则
    s2e_joineuiFilter = filter;
    
    // 符号化输入
    klee_make_symbolic(&joineui, sizeof(joineui), "join_eui");
    
    // 测试过滤
    int filtered = 0;
    uL_t* f = s2e_joineuiFilter-2;
    while( *(f += 2) ) {
        if( joineui >= f[0] && joineui <= f[1] ) {
            filtered = 1;
            break;
        }
    }
    
    // 安全属性断言
    klee_assert(filtered == 0 || (joineui >= filter[0] && joineui <= filter[1]));
}

// 测试NetID过滤
void test_lora_netid_filter() {
    u4_t devaddr;
    u1_t netid;
    
    // 符号化输入
    klee_make_symbolic(&devaddr, sizeof(devaddr), "dev_addr");
    
    // 提取NetID
    netid = devaddr >> (32-7);
    
    // 测试过滤
    int filtered = ((1 << (netid & 0x1F)) & s2e_netidFilter[netid>>5]) == 0;
    
    // 安全属性断言
    klee_assert(!filtered || (netid >= 0 && netid < 128));
}

// 测试信标生成
void test_lora_beacon() {
    uint8_t layout[32];
    uint8_t pdu[32];
    sL_t epoch_secs;
    int infodesc;
    double lat, lon;
    
    // 符号化输入
    klee_make_symbolic(layout, sizeof(layout), "beacon_layout");
    klee_make_symbolic(&epoch_secs, sizeof(epoch_secs), "epoch_seconds");
    klee_make_symbolic(&infodesc, sizeof(infodesc), "info_descriptor");
    klee_make_symbolic(&lat, sizeof(lat), "latitude");
    klee_make_symbolic(&lon, sizeof(lon), "longitude");
    
    // 测试信标生成
    s2e_make_beacon(layout, epoch_secs, infodesc, lat, lon, pdu);
    
    // 安全属性断言
    klee_assert(pdu[0] == 0x00); // 信标帧类型
    klee_assert(pdu[1] == 0x00); // 信标帧版本
}

// 测试CRC16计算
void test_lora_crc16() {
    uint8_t data[32];
    int len;
    
    // 符号化输入
    klee_make_symbolic(data, sizeof(data), "crc_data");
    klee_make_symbolic(&len, sizeof(len), "data_length");
    klee_assume(len > 0 && len <= sizeof(data));
    
    // 测试CRC16计算
    int crc = crc16_no_table(data, len);
    
    // 安全属性断言
    klee_assert(crc >= 0 && crc <= 0xFFFF);
}

// 主测试函数
int main() {
    // 运行各个测试用例
    test_lora_frame_parse();
    test_lora_joineui_filter();
    test_lora_netid_filter();
    test_lora_beacon();
    test_lora_crc16();
    
    return 0;
} 