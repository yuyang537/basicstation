# μJSON (MicroJSON) 模块技术详解

## 概述

μJSON是BasicStation项目的核心JSON处理引擎，提供高性能、内存优化的JSON解析和编码功能。设计目标是在嵌入式环境中实现完整的JSON支持，同时针对LoRaWAN协议的特定需求进行优化。

## 系统架构

### 核心组件结构

```
μJSON系统
├── 解析引擎 (Parsing Engine)
│   ├── 词法分析器 (nextChar, skipWsp)
│   ├── 语法分析器 (uj_nextValue)
│   ├── 类型专用解析器 (parseString, parseNumber)
│   └── 嵌套结构管理 (do_enter, do_exit)
├── 编码引擎 (Encoding Engine)
│   ├── 缓冲区管理 (ujbuf_t, addChar)
│   ├── 基础类型编码器 (uj_encInt, uj_encStr)
│   ├── LoRaWAN特化编码器 (uj_encEui, uj_encMac)
│   └── 扩展printf系统 (xprintf, vxprintf)
└── 类型安全接口 (Type-Safe Interface)
    ├── 值提取器 (uj_int, uj_str, uj_bool)
    ├── 专用转换器 (uj_eui, uj_hexstr)
    └── 错误处理器 (uj_error, longjmp)
```

## 核心技术实现

### 1. 流式解析算法

**设计原理:**
- 单遍解析，最小内存占用
- 递归下降解析器，支持任意嵌套深度
- 状态机驱动，精确错误定位

### 2. 高性能字符串处理

**CRC校验优化:**
- 实时计算字符串CRC32，用于快速比较
- 支持常量时间字符串匹配
- 避免strcopy操作，就地处理

### 3. LoRaWAN专用编码

**EUI-64编码:** 支持大端序8字节格式
**MAC地址编码:** 标准6字节格式  
**时间戳处理:** UTC时间和ISO日期格式

## API使用指南

### 解析API示例

```c
ujdec_t dec;
uj_iniDecoder(&dec, json_text, strlen(json_text));

if( uj_nextValue(&dec) == UJ_OBJECT ) {
    uj_enterObject(&dec);
    while( uj_nextField(&dec) ) {
        if( dec.field.crc == J_devEUI ) {
            uL_t devEUI = uj_eui(&dec);
        }
    }
    uj_exitObject(&dec);
}
```

### 编码API示例

```c
ujbuf_t buf = {.buf = buffer, .bufsize = sizeof(buffer)};
uj_encOpen(&buf, '{');
uj_encKV(&buf, "devEUI", 'E', devEUI);
uj_encKV(&buf, "timestamp", 'T', timestamp);
uj_encClose(&buf, '}');
```

## 性能特征

| 操作 | 性能 | 优化点 |
|------|------|--------|
| 字符串解析 | ~50MB/s | 就地处理，避免拷贝 |
| 数值解析 | ~30MB/s | 直接算术运算 |
| CRC计算 | ~100MB/s | 硬件加速表查找 |

## 最佳实践

1. **错误处理**: 使用setjmp/longjmp机制
2. **性能优化**: 预计算字段CRC，使用switch匹配
3. **内存管理**: 栈上分配小缓冲区，大JSON动态分配 