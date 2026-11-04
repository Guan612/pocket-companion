// m4a（MP4 封装的 AAC）解析：读 moov 里音频轨的目录表，按时间找到样本在文件里的位置，
// 再给每帧补上 7 字节 ADTS 头，交给和电台同一个 AAC 解码器。
//
// 小宇宙等播客平台的节目都是 m4a。有了目录表，暂停后接着放、快进快退都只要从对应字节开始下载，不用从头解码。
//
// 目录表不整块读进内存：每帧的大小（stsz）、每块的位置（stco）跟着节目长度涨，1 个半小时的节目约 1MB，
// PSRAM 放不下几期。现在 moov 只顺着读一遍、记下各个表在文件里的位置，每个表只在内存里留一段（窗口），
// 放到窗口外面再按需去文件里读下一段。内存最多约 100KB（常见的节目约 64KB），和节目多长无关。
#pragma once

#include <cstddef>
#include <cstdint>

// 读 m4a 文件的一段（网上就是一个 Range 请求）。同一时间只开着一段：Open 新的一段前，实现要先关掉旧的。
// 下载音频和按需读目录表共用这一条（设备的内部 RAM 只够同时一条 TLS）
class Mp4Source {
public:
    static constexpr uint64_t kToEnd = UINT64_MAX;
    virtual ~Mp4Source() = default;

    // 开 [from, to) 这一段；to 为 kToEnd 时一直读到文件结尾
    bool Open(uint64_t from, uint64_t to) {
        opens_++;
        return DoOpen(from, to);
    }
    // >0 读到的字节数；0 这一段读完了；-1 出错或被叫停；-2 一阵子没数据（可以接着读）
    virtual int Read(uint8_t* buf, size_t len) = 0;
    virtual void Close() = 0;
    // 开过几次。读目录表会占用这条连接：调用方看这个数变了，就知道正在下的音频那段被关掉了，要重新开
    uint32_t opens() const { return opens_; }

protected:
    virtual bool DoOpen(uint64_t from, uint64_t to) = 0;

private:
    uint32_t opens_ = 0;
};

// 一张目录表：所有条目在文件里的位置，和内存里的一段窗口
struct Mp4Table {
    uint64_t offset = 0;  // 第 0 个条目在文件里的位置
    uint32_t count = 0;   // 条目数
    uint32_t entry = 0;   // 每个条目几字节
    uint32_t cap = 0;     // 窗口最多放几个条目
    uint8_t* buf = nullptr;
    uint32_t first = 0, n = 0;  // 窗口里是第 first 个起的 n 个条目
};

// 顺序遍历样本用的位置
struct Mp4Cursor {
    uint32_t sample = 0;     // 第几个样本（帧）
    uint32_t chunk = 0;      // 第几个块（从 0 数）
    uint32_t in_chunk = 0;   // 块里的第几个
    uint32_t per_chunk = 0;  // 这个块有几个样本
    uint32_t stsc_i = 0;     // 当前用的 stsc 条目
    uint64_t offset = 0;     // 这个样本在文件里的位置
    bool new_chunk = false;  // 刚走进下一个块，块的位置还没查
};

class Mp4Track {
public:
    Mp4Track() = default;
    ~Mp4Track();
    Mp4Track(const Mp4Track&) = delete;
    Mp4Track& operator=(const Mp4Track&) = delete;

    // 找到 moov、解析第一条 AAC 音频轨。只顺着读一遍盒子头和小的表；大表只读开头一个窗口，
    // 剩下的远的话直接跳过去（重新开一段），之后按需读
    bool Open(Mp4Source& src);
    // 之后按需读目录表用哪条连接（每次播放换一个；不用了设回 nullptr）
    void Attach(Mp4Source* src) { src_ = src; }

    uint32_t DurationSeconds();
    // 第 second 秒是第几个样本（超出结尾给样本总数）；读目录表出错返回 false
    bool SampleAtSecond(uint32_t second, uint32_t& sample);
    // 定位到第 sample 个样本：1 好了；0 已经在结尾；-1 读目录表出错
    int Seek(uint32_t sample, Mp4Cursor& c);
    // 当前样本的大小（位置在 c.offset）：1 好了；0 放完了；-1 读目录表出错
    int Sample(Mp4Cursor& c, uint32_t& size);
    // 走到下一个样本（只算数，不读东西）
    void Advance(Mp4Cursor& c, uint32_t size);
    void AdtsHeader(uint32_t payload, uint8_t out[7]) const;

    uint32_t samples() const { return stsz_const_ ? stsz_count_ : stsz_.count; }
    size_t MemoryBytes() const;    // 几个窗口一共占多少
    uint32_t fetches() const { return fetches_; }  // 按需读过几次窗口

private:
    Mp4Source* src_ = nullptr;
    uint32_t timescale_ = 0;
    uint64_t mdhd_duration_ = 0;
    int profile_ = 1;  // ADTS 的 profile = AAC audio object type - 1（LC 为 1）
    int sf_index_ = 4;
    int channels_ = 2;
    Mp4Table stts_, stsc_, stsz_, stco_;
    uint32_t stsz_const_ = 0;  // 所有样本一样大时的大小（这时没有逐个大小的表）
    uint32_t stsz_count_ = 0;  // 这时的样本数
    uint32_t fetches_ = 0;

    bool Load(Mp4Table& t, uint32_t i);
    const uint8_t* At(Mp4Table& t, uint32_t i) { return Load(t, i) ? t.buf + size_t(i - t.first) * t.entry : nullptr; }
    bool ChunkOffset(uint32_t chunk, uint64_t& offset);
    bool SampleSize(uint32_t sample, uint32_t& size);
    bool StscEntry(uint32_t i, uint32_t& first_chunk, uint32_t& per);
};
