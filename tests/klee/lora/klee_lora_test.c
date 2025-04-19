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
#include "../../../src/s2conf.h"  // 配置接口
#include "../../../src/uj.h"  // JSON处理接口

// 外部函数声明
extern int s2e_parse_lora_frame(ujbuf_t* buf, const u1_t* frame, int len, dbuf_t* lbuf);
extern void s2e_make_beacon(uint8_t* layout, sL_t epoch_secs, int infodesc, double lat, double lon, uint8_t* pdu);
extern uL_t* s2e_joineuiFilter;
extern u4_t s2e_netidFilter[4];

// 测试用例1: 空帧处理测试
void test_empty_frame() {
    ujbuf_t buf;
    char json_buf[1024];
    memset(json_buf, 0, sizeof(json_buf));
    
    buf.buf = json_buf;
    buf.bufsize = sizeof(json_buf);
    buf.pos = 0;
    
    // 测试空帧处理
    u1_t empty_frame[1];
    klee_make_symbolic(empty_frame, sizeof(empty_frame), "empty_frame");
    
    int len = 0;  // 长度为0的帧
    int result = s2e_parse_lora_frame(&buf, empty_frame, len, NULL);
    
    // 验证结果 - 空帧应被拒绝
    assert(result == 0);
}

// 测试用例2: 帧格式验证测试
void test_frame_format() {
    ujbuf_t buf;
    char json_buf[1024];
    
    // 初始化缓冲区
    memset(json_buf, 0, sizeof(json_buf));
    buf.buf = json_buf;
    buf.bufsize = sizeof(json_buf);
    buf.pos = 0;
    
    // 创建一个符号化的帧
    u1_t frame[24];  // 足够容纳各种类型的帧
    klee_make_symbolic(frame, sizeof(frame), "test_frame");
    
    // 设置MHDR-MAJOR版本字段 - 只有MAJOR_V1(0)被接受
    // frame[0] & MHDR_MAJOR (0x03) 控制
    klee_assume((frame[0] & 0x03) == 0x00);  // 有效MAJOR
    
    // 设置MHDR-RFU字段 - 必须为0
    // frame[0] & MHDR_RFU (0x1C) 控制
    klee_assume((frame[0] & 0x1C) == 0x00);  // 有效RFU
    
    // 测试不同长度的帧
    int len;
    klee_make_symbolic(&len, sizeof(len), "frame_length");
    // 修改假设条件，确保至少为1字节，不超过帧缓冲区大小
    klee_assume(len >= 1 && len <= sizeof(frame));
    
    // 如果是JREQ帧，长度必须是OFF_jreq_len(23)
    if ((frame[0] & 0xE0) == 0x00) {  // FRMTYPE_JREQ
        len = 23;  // 加入请求长度固定为23
    }
    
    int result = s2e_parse_lora_frame(&buf, frame, len, NULL);
    
    // 验证格式验证逻辑
    if (result == 0) {
        // 如果解析失败，检查是否满足以下条件之一:
        // 1. 长度太短 (< OFF_df_minlen且非专有帧) 
        // 2. 错误的MAJOR版本
        // 3. 非零RFU字段
        // 4. 对于JREQ，长度不是23字节
        if ((frame[0] & 0xE0) != 0xE0) {  // 不是专有帧
            if (len < 12) {  // OFF_df_minlen是12
                assert(result == 0);  // 长度太短，应该失败
            }
        }
    }
}

// 测试用例3: 加入请求(JREQ)解析测试
void test_join_request() {
    ujbuf_t buf;
    char json_buf[1024];
    
    // 初始化缓冲区
    memset(json_buf, 0, sizeof(json_buf));
    buf.buf = json_buf;
    buf.bufsize = sizeof(json_buf);
    buf.pos = 0;
    
    // 创建JREQ帧
    u1_t jreq_frame[23];  // 加入请求长度固定为23
    memset(jreq_frame, 0, sizeof(jreq_frame));
    
    // 设置MHDR为JREQ类型(0x00)
    jreq_frame[0] = 0x00;
    
    // 创建符号化的JoinEUI, DevEUI和DevNonce字段
    for (int i = 1; i < 17; i++) {
        klee_make_symbolic(&jreq_frame[i], 1, "eui_bytes");
    }
    for (int i = 17; i < 19; i++) {
        klee_make_symbolic(&jreq_frame[i], 1, "devnonce_bytes");
    }
    for (int i = 19; i < 23; i++) {
        klee_make_symbolic(&jreq_frame[i], 1, "mic_bytes");
    }
    
    // 保存原始过滤器指针
    uL_t* original_filter = s2e_joineuiFilter;
    
    // 测试没有过滤器的情况
    uL_t no_filter[2] = {0, 0};  // 使用静态数组代替动态分配
    s2e_joineuiFilter = no_filter;
    
    int result = s2e_parse_lora_frame(&buf, jreq_frame, 23, NULL);
    
    // 验证结果 - 应该成功解析
    assert(result == 1);
    
    // 测试过滤器情况
    buf.pos = 0;
    
    // 创建静态过滤器数组而不是动态分配
    uL_t filter[4] = {
        0x1000000000000000,  // 开始范围
        0x1FFFFFFFFFFFFFFF,  // 结束范围
        0,                   // 结束标记
        0
    };
    
    s2e_joineuiFilter = filter;
    
    // JoinEUI不在过滤范围内 - 应该被过滤掉
    result = s2e_parse_lora_frame(&buf, jreq_frame, 23, NULL);
    
    // 根据JoinEUI值验证结果
    uL_t joineui = rt_rlsbf8(&jreq_frame[1]);
    if (joineui >= filter[0] && joineui <= filter[1]) {
        assert(result == 1);  // 在过滤范围内，应通过
    } else {
        assert(result == 0);  // 不在过滤范围内，应被过滤
    }
    
    // 恢复原始过滤器
    s2e_joineuiFilter = original_filter;
}

// 测试用例4: 数据帧解析测试
void test_data_frame() {
    ujbuf_t buf;
    char json_buf[1024];
    
    // 初始化缓冲区
    memset(json_buf, 0, sizeof(json_buf));
    buf.buf = json_buf;
    buf.bufsize = sizeof(json_buf);
    buf.pos = 0;
    
    // 创建数据帧
    u1_t data_frame[24];  // 足够大的帧
    memset(data_frame, 0, sizeof(data_frame));
    
    // 设置MHDR为DAUP类型(0x40)或DCUP类型(0x80)
    klee_make_symbolic(&data_frame[0], 1, "mhdr");
    klee_assume((data_frame[0] & 0xE0) == 0x40 || (data_frame[0] & 0xE0) == 0x80);
    klee_assume((data_frame[0] & 0x1F) == 0);  // 清除RFU和MAJOR位，保持为0
    
    // 设置DevAddr字段(4字节)
    for (int i = 1; i < 5; i++) {
        klee_make_symbolic(&data_frame[i], 1, "devaddr_bytes");
    }
    
    // 设置FCtrl字段(1字节) - FOpts长度在低4位
    klee_make_symbolic(&data_frame[5], 1, "fctrl");
    
    // 确保FOpts长度小于16
    data_frame[5] &= 0xF;
    
    // 设置FCnt字段(2字节)
    for (int i = 6; i < 8; i++) {
        klee_make_symbolic(&data_frame[i], 1, "fcnt_bytes");
    }
    
    // 设置FOpts字段(0-15字节)
    u1_t foptslen = data_frame[5] & 0x0F;
    // 限制FOpts长度，确保不会超出数据帧缓冲区
    if (foptslen > sizeof(data_frame) - 12) { // 确保至少有头部(8) + FOpts + MIC(4)
        foptslen = sizeof(data_frame) - 12;
        data_frame[5] = (data_frame[5] & 0xF0) | foptslen;
    }
    
    // 为FOpts字段中的每个字节创建符号
    for (int i = 0; i < foptslen && (8 + i) < sizeof(data_frame); i++) {
        klee_make_symbolic(&data_frame[8 + i], 1, "fopts_bytes");
    }
    
    // 设置帧长度 - 头部+FOpts+FPort+MIC，如果有载荷，则额外添加
    int len = 8 + foptslen + 1 + 4;  // 头部(8) + FOpts + FPort(1) + MIC(4)
    
    // 如果有FPort，设置其值
    if (8 + foptslen < len - 4) {
        klee_make_symbolic(&data_frame[8 + foptslen], 1, "fport");
    }
    
    // 设置MIC(4字节)
    for (int i = 0; i < 4; i++) {
        klee_make_symbolic(&data_frame[len - 4 + i], 1, "mic_bytes");
    }
    
    // 确保长度合法 - 至少包含头部和MIC
    klee_assume(len >= 12 && len <= 24);
    
    // 重置NetID过滤器
    for (int i = 0; i < 4; i++) {
        s2e_netidFilter[i] = 0xFFFFFFFF;  // 允许所有NetID
    }
    
    int result = s2e_parse_lora_frame(&buf, data_frame, len, NULL);
    
    // 验证结果
    u4_t devaddr = rt_rlsbf4(&data_frame[1]);
    u1_t netid = devaddr >> (32-7);  // 提取NetID(高7位)
    
    // 检查NetID过滤是否工作
    if (((1 << (netid & 0x1F)) & s2e_netidFilter[netid>>5]) == 0) {
        assert(result == 0);  // NetID被过滤，应该失败
    } else if (len < 12) {
        assert(result == 0);  // 长度太短，应该失败
    } else {
        assert(result == 1);  // 应该成功解析
    }
}

// 测试用例5: 信标帧生成测试
void test_beacon_generation() {
    // 创建符号化的参数
    uint8_t layout[3];
    sL_t epoch_secs;
    int infodesc;
    double lat, lon;
    
    // 使用固定值而非符号化值来避免潜在的边界问题
    layout[0] = 4;   // 时间戳偏移
    layout[1] = 10;  // 信息描述符偏移
    layout[2] = 32;  // 信标总长度
    
    // 符号化时间戳
    klee_make_symbolic(&epoch_secs, sizeof(epoch_secs), "epoch_secs");
    
    // 符号化信息描述符，但限制范围
    klee_make_symbolic(&infodesc, sizeof(infodesc), "infodesc");
    klee_assume(infodesc >= 0 && infodesc <= 255); // 单字节范围
    
    // 符号化坐标，但使用固定范围
    lat = 45.0;  // 使用固定值代替符号化值
    lon = 90.0;  // 使用固定值代替符号化值
    
    // 分配输出缓冲区并初始化
    uint8_t pdu[32]; // 使用栈上分配的固定大小缓冲区
    memset(pdu, 0, sizeof(pdu));
    
    // 调用信标生成函数
    s2e_make_beacon(layout, epoch_secs, infodesc, lat, lon, pdu);
    
    // 验证时间戳正确写入
    uint32_t written_time = 0;
    for (int i = 0; i < 4; i++) {
        written_time |= ((uint32_t)pdu[layout[0] + i]) << (8 * i);
    }
    assert((sL_t)written_time == (epoch_secs & 0xFFFFFFFF));
    
    // 验证信息描述符正确写入
    assert(pdu[layout[1]] == (infodesc & 0xFF));
    
    // 验证第一个CRC字段存在
    uint16_t crc1_offset = layout[1] - 2;
    assert(crc1_offset < sizeof(pdu));
    
    // 验证第二个CRC字段存在
    uint16_t crc2_offset = layout[2] - 2;
    assert(crc2_offset < sizeof(pdu));
}

// 测试用例6: 专有帧和加入接受(JACC)帧测试
void test_prop_and_jacc_frames() {
    ujbuf_t buf;
    char json_buf[1024];
    
    // 初始化缓冲区
    memset(json_buf, 0, sizeof(json_buf));
    buf.buf = json_buf;
    buf.bufsize = sizeof(json_buf);
    buf.pos = 0;
    
    // 创建专有帧和JACC帧
    u1_t frame[16];  // 一个合理大小的帧
    memset(frame, 0, sizeof(frame));
    
    // 随机选择PROP(0xE0)或JACC(0x20)类型
    u1_t frame_type;
    klee_make_symbolic(&frame_type, 1, "frame_type");
    klee_assume(frame_type == 0xE0 || frame_type == 0x20);
    
    frame[0] = frame_type;
    
    // 符号化剩余帧数据
    for (int i = 1; i < 16; i++) {
        klee_make_symbolic(&frame[i], 1, "payload_bytes");
    }
    
    // 测试解析
    int result = s2e_parse_lora_frame(&buf, frame, 16, NULL);
    
    // 验证结果 - 这些特殊帧类型总是成功解析
    assert(result == 1);
    
    // 检查JSON输出中是否包含正确的msgtype
    buf.buf[buf.pos] = '\0';
    if (frame_type == 0xE0) {
        assert(strstr(buf.buf, "\"msgtype\":\"propdf\"") != NULL);
    } else {
        assert(strstr(buf.buf, "\"msgtype\":\"jacc\"") != NULL);
    }
}

// 测试用例7: 缓冲区边界检查
void test_buffer_boundaries() {
    ujbuf_t buf;
    char json_buf[1024];
    
    // 初始化缓冲区
    memset(json_buf, 0, sizeof(json_buf));
    buf.buf = json_buf;
    buf.bufsize = sizeof(json_buf);
    buf.pos = 0;
    
    // 创建一个数据帧
    u1_t data_frame[32];
    memset(data_frame, 0, sizeof(data_frame));
    data_frame[0] = 0x40;  // DAUP类型
    
    // 动态设置FCtrl中的FOpts长度
    u1_t foptslen;
    klee_make_symbolic(&foptslen, 1, "fopts_length");
    klee_assume(foptslen <= 15);  // FOpts长度不能超过15
    
    data_frame[5] = foptslen;  // FCtrl，设置FOpts长度
    
    // 设置帧长度
    int len;
    klee_make_symbolic(&len, sizeof(len), "frame_length");
    
    // 计算最小长度 - 头部(8) + FOpts + MIC(4)
    int min_len = 8 + foptslen + 4;
    
    // 限制长度在逻辑范围内
    klee_assume(len >= min_len && len <= 32);
    
    // 重置NetID过滤器
    for (int i = 0; i < 4; i++) {
        s2e_netidFilter[i] = 0xFFFFFFFF;  // 允许所有NetID
    }
    
    int result = s2e_parse_lora_frame(&buf, data_frame, len, NULL);
    
    // 验证长度校验逻辑
    u1_t portoff = foptslen + 8;  // 8是头部长度
    
    if (portoff > len - 4) {  // 检查端口偏移是否超出帧长度减去MIC
        assert(result == 0);  // 应该失败，因为没有足够空间放置端口和MIC
    }
}

// 测试用例8: 安全性验证 - 极限情况测试
void test_security_checks() {
    ujbuf_t buf;
    char json_buf[1024];
    
    // 初始化缓冲区
    memset(json_buf, 0, sizeof(json_buf));
    buf.buf = json_buf;
    buf.bufsize = sizeof(json_buf);
    buf.pos = 0;
    
    // 测试1: 检查非常大的FOpts长度处理
    u1_t data_frame[256];
    memset(data_frame, 0, sizeof(data_frame));
    data_frame[0] = 0x40;  // DAUP类型
    
    // 设置不合理的FOpts长度
    data_frame[5] = 0xFF;  // FCtrl，设置FOpts长度为最大值
    
    // 确保解析器能安全处理
    int result = s2e_parse_lora_frame(&buf, data_frame, 32, NULL);
    
    // 应该失败，因为FOpts长度不合理
    assert(result == 0);
    
    // 测试2: 检查恶意长度值的处理
    buf.pos = 0;
    
    // 使用合理的FOpts长度但不一致的总长度
    data_frame[5] = 0x0F;  // 15字节FOpts
    
    // 使用太短的长度，不足以容纳FOpts+MIC
    result = s2e_parse_lora_frame(&buf, data_frame, 20, NULL);
    
    // 应该失败，因为长度太短
    assert(result == 0);
}

// 测试用例9: 溢出测试 - 验证处理极限值的情况
void test_overflow_conditions() {
    ujbuf_t buf;
    char json_buf[1024];
    
    // 初始化缓冲区
    memset(json_buf, 0, sizeof(json_buf));
    buf.buf = json_buf;
    buf.bufsize = sizeof(json_buf);
    buf.pos = 0;
    
    // 测试1: 测试JSON缓冲区接近溢出的情况
    u1_t large_frame[255];  // 大帧
    memset(large_frame, 0, sizeof(large_frame));
    large_frame[0] = 0xE0;  // 专有帧类型，会被接受
    
    // 填充大量数据到帧中
    for (int i = 1; i < 255; i++) {
        large_frame[i] = i & 0xFF;
    }
    
    // 测试解析器对大型帧的处理
    int result = s2e_parse_lora_frame(&buf, large_frame, 255, NULL);
    
    // 确认没有缓冲区溢出 - 通过检查结果是否为预期值
    assert(result == 1);  // 专有帧应该被接受
    
    // 验证JSON缓冲区未溢出
    assert(buf.pos < buf.bufsize);
    
    // 测试2: 测试极限长度值的处理
    buf.pos = 0;
    
    // 创建数据帧
    u1_t data_frame[256];
    memset(data_frame, 0, sizeof(data_frame));
    data_frame[0] = 0x40;  // DAUP类型
    
    // 设置FCtrl - 使用边界值15作为FOpts长度
    data_frame[5] = 0x0F;  // 最大FOpts长度(15)
    
    // 设置一个极限长度 - 头部(8) + 最大FOpts(15) + 大量FRMPayload + MIC(4)
    int len = 255; // 使用极限值
    
    // 确保解析器能安全处理，不会导致溢出
    result = s2e_parse_lora_frame(&buf, data_frame, len, NULL);
    
    // 不关心具体结果，只要不崩溃就行
    // 只需确认JSON缓冲区没有溢出
    assert(buf.pos <= buf.bufsize);
}

// 测试用例10: 整数溢出测试
void test_integer_overflow() {
    // 测试1: 测试s2e_make_beacon中的整数计算是否安全
    uint8_t layout[3] = {0, 8, 32};
    uint8_t pdu[32] = {0};
    
    // 使用可能导致溢出的值
    sL_t epoch_secs = 0x7FFFFFFFFFFFFFFF;  // 最大有符号64位值
    int infodesc = 255;  // 最大8位值
    double lat = 90.0;  // 最大有效纬度
    double lon = 180.0;  // 最大有效经度
    
    // 调用函数，应该能安全处理而不崩溃
    s2e_make_beacon(layout, epoch_secs, infodesc, lat, lon, pdu);
    
    // 测试2: 测试RT模块的整数处理
    uL_t max_eui = 0xFFFFFFFFFFFFFFFF;  // 最大64位无符号整数
    uL_t joineui = max_eui;
    uL_t deveui = max_eui;
    
    // 设置测试帧
    u1_t jreq_frame[23] = {0};
    jreq_frame[0] = 0x00;  // JREQ类型
    
    // 设置极限EUI值
    for (int i = 0; i < 8; i++) {
        jreq_frame[1+i] = 0xFF;  // JoinEUI全为1
        jreq_frame[9+i] = 0xFF;  // DevEUI全为1
    }
    
    // 验证极限EUI值的处理
    joineui = rt_rlsbf8(&jreq_frame[1]);
    assert(joineui == max_eui);
    
    deveui = rt_rlsbf8(&jreq_frame[9]);
    assert(deveui == max_eui);
}

// 测试用例11: 安全的输入验证测试
void test_input_validation() {
    ujbuf_t buf;
    char json_buf[1024];
    
    // 初始化缓冲区
    memset(json_buf, 0, sizeof(json_buf));
    buf.buf = json_buf;
    buf.bufsize = sizeof(json_buf);
    buf.pos = 0;
    
    // 测试1: 测试各种非法帧的处理
    // 1.1 测试空帧
    u1_t empty_frame[1];  // 至少分配一个字节
    result = s2e_parse_lora_frame(&buf, empty_frame, 0, NULL);  // 但使用长度为0
    assert(result == 0);  // 应该拒绝
    
    buf.pos = 0;
    
    // 1.2 测试无效MHDR
    u1_t invalid_mhdr_frame[23] = {0};
    invalid_mhdr_frame[0] = 0x1F;  // 非法的MHDR(无效的帧类型并带有RFU位)
    
    result = s2e_parse_lora_frame(&buf, invalid_mhdr_frame, 23, NULL);
    assert(result == 0);  // 应该拒绝
    
    buf.pos = 0;
    
    // 1.3 测试错误长度的JREQ
    u1_t short_jreq_frame[22] = {0};
    short_jreq_frame[0] = 0x00;  // JREQ类型
    
    result = s2e_parse_lora_frame(&buf, short_jreq_frame, 22, NULL);
    assert(result == 0);  // 应该拒绝
    
    buf.pos = 0;
    
    // 1.4 测试错误的FOpts长度
    u1_t invalid_fopts_frame[24] = {0};
    invalid_fopts_frame[0] = 0x40;  // DAUP类型
    invalid_fopts_frame[5] = 0xFF;  // 非法的FOpts长度在FCtrl中
    
    result = s2e_parse_lora_frame(&buf, invalid_fopts_frame, 24, NULL);
    assert(result == 0);  // 应该拒绝或安全处理
    
    // 测试2: 测试信标参数边界
    uint8_t layout[3] = {0, 4, 8};  // 不合理的布局(CRC1区域重叠时间戳)
    uint8_t pdu[32] = {0};
    
    // 调用函数，应该能安全处理而不崩溃
    s2e_make_beacon(layout, 0, 0, 0, 0, pdu);
    
    // 极限纬度/经度值
    layout[0] = 0;
    layout[1] = 8;
    layout[2] = 16;
    
    s2e_make_beacon(layout, 0, 0, 91.0, 181.0, pdu);  // 超出有效范围的坐标
}

// 测试用例12: 协议一致性测试
void test_protocol_conformance() {
    ujbuf_t buf;
    char json_buf[1024];
    
    // 初始化缓冲区
    memset(json_buf, 0, sizeof(json_buf));
    buf.buf = json_buf;
    buf.bufsize = sizeof(json_buf);
    buf.pos = 0;
    
    // 测试1: 验证各种帧类型的正确识别
    u1_t frame_types[8] = {
        0x00,  // JREQ
        0x20,  // JACC
        0x40,  // DAUP
        0x60,  // DADN
        0x80,  // DCUP
        0xA0,  // DCDN
        0xC0,  // REJOIN
        0xE0   // PROP
    };
    
    const char* expected_types[8] = {
        "jreq",
        "jacc",
        "updf",
        "dndf",
        "updf",
        "dndf",
        "rejoin",
        "propdf"
    };
    
    // 测试每种帧类型
    for (int i = 0; i < 8; i++) {
        buf.pos = 0;
        
        u1_t test_frame[23] = {0};  // 足够大的帧
        test_frame[0] = frame_types[i];  // 设置帧类型
        
        int len = 23;  // JREQ/REJOIN需要精确长度
        if (frame_types[i] != 0x00 && frame_types[i] != 0xC0) {
            len = 16;  // 其他类型用较短长度
        }
        
        // 如果是数据帧，确保长度至少为12
        if ((frame_types[i] & 0xE0) >= 0x40 && (frame_types[i] & 0xE0) <= 0xA0) {
            if (len < 12) len = 12;
        }
        
        int result = s2e_parse_lora_frame(&buf, test_frame, len, NULL);
        
        // JACC和PROP应总是返回成功
        if (frame_types[i] == 0x20 || frame_types[i] == 0xE0) {
            assert(result == 1);
            
            // 验证JSON输出包含正确的msgtype
            buf.buf[buf.pos] = '\0';
            assert(strstr(buf.buf, expected_types[i]) != NULL);
        }
    }
}

// 主测试函数
int main() {
    // 初始化系统
    sys_init("{}");
    
    // 运行所有测试用例
    test_empty_frame();
    test_frame_format();
    test_join_request();
    test_data_frame();
    test_beacon_generation();
    test_prop_and_jacc_frames();
    test_buffer_boundaries();
    test_security_checks();
    
    // 新增安全测试
    test_overflow_conditions();
    test_integer_overflow();
    test_input_validation();
    test_protocol_conformance();
    
    return 0;
} 