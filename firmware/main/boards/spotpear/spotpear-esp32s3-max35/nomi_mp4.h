// m4a（MP4 封装的 AAC）解析：读 moov 里音频轨的目录表，按时间找到样本在文件里的位置，
// 再给每帧补上 7 字节 ADTS 头，交给和电台同一个 AAC 解码器。
//
// 小宇宙等播客平台的节目都是 m4a。moov 在文件开头（faststart）时一次 Range 请求就能拿到；
// 有了目录表，暂停后接着放、快进快退都只要从对应字节开始下载，不用从头解码。
#pragma once

#include <cstddef>
#include <cstdint>

struct Mp4Track {
    uint32_t timescale = 0;
    int profile = 1;  // ADTS 的 profile = AAC audio object type - 1（LC 为 1）
    int sf_index = 4;
    int channels = 2;
    const uint8_t *stts = nullptr, *stsc = nullptr, *stsz = nullptr, *stco = nullptr;
    uint32_t stts_n = 0, stsc_n = 0, stsz_n = 0, stco_n = 0;
    uint32_t stsz_const = 0;  // 所有样本一样大时的大小（这时没有逐个大小的表）
    bool co64 = false;
};

// 顺序遍历样本用的位置
struct Mp4Cursor {
    uint32_t sample = 0;      // 第几个样本（帧）
    uint32_t chunk = 0;       // 第几个块（从 0 数）
    uint32_t in_chunk = 0;    // 块里的第几个
    uint32_t per_chunk = 0;   // 这个块有几个样本
    uint32_t stsc_i = 0;      // 当前用的 stsc 条目
    uint64_t offset = 0;      // 这个样本在文件里的位置
};

// 在文件开头的一段数据里找顶层 moov：找到返回 true 和它的位置、大小；
// 没在这段里时 next 是下一个顶层盒子的位置（moov 在文件末尾的情况，要再去那里读）
bool Mp4FindMoov(const uint8_t* data, size_t len, uint64_t base, uint64_t& moov_offset, uint64_t& moov_size,
                 uint64_t& next);

// 解析整个 moov（含 8 字节盒子头），找第一条音频轨
bool Mp4ParseMoov(const uint8_t* moov, size_t size, Mp4Track& track);

uint32_t Mp4SampleAtSecond(const Mp4Track& track, uint32_t second);
uint32_t Mp4DurationSeconds(const Mp4Track& track);  // 总长：stts 表里"样本数 × 每个样本多长"加起来
bool Mp4Seek(const Mp4Track& track, uint32_t sample, Mp4Cursor& cursor);
// 当前样本的大小；返回 false 表示放完了
bool Mp4SampleSize(const Mp4Track& track, const Mp4Cursor& cursor, uint32_t& size);
void Mp4Advance(const Mp4Track& track, Mp4Cursor& cursor, uint32_t size);
void Mp4AdtsHeader(const Mp4Track& track, uint32_t payload, uint8_t out[7]);
