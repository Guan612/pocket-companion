// TXT 书：认编码、按文件里的字节位置读一段转成排版用的 UTF-8、找章节。阅读器（nomi_reader）和应用的
// text_chapters 用它。不依赖设备，电脑上也能编（测试用）。
//
// 编码：开头有 UTF-8 BOM、或者开头 64KB 是合法的 UTF-8 → UTF-8；不然当 GBK（对照表在资源分区 gbk.tab，
// tools/gen_gbk_table.py 生成）。UTF-16 的不认。
// 转出来的字：\r 去掉；空行去掉；每段开头的空白换成两个全角空格（章名不缩进）；行尾空白去掉；解不出来的字节换成 ?。
// 位置一律是文件里的字节位置：书签、翻页、章节都用它，换了排版（字号、屏幕）也还对得上。
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

class NomiBook {
public:
    enum Encoding { kUtf8, kGbk };
    struct Chapter {
        uint32_t offset;  // 章名那行（去掉开头空白后）在文件里的位置
        std::string title;
    };

    // GBK 对照表（tools/gen_gbk_table.py 的格式）。设备上第一次用时从资源分区取；电脑上测试时自己给
    static void SetGbkTable(const uint8_t* table, size_t size);

    NomiBook() = default;
    ~NomiBook();
    NomiBook(const NomiBook&) = delete;
    NomiBook& operator=(const NomiBook&) = delete;

    bool Open(const std::string& path);  // 设备上的路径；SD 卡上的要先挂着卡
    uint32_t size() const { return size_; }
    uint32_t body() const { return body_; }  // 正文从哪开始（跳过 BOM）
    Encoding encoding() const { return enc_; }

    // 把 [from, to) 转成排版用的字：text 的第 i 个字节属于文件里从 src[i] 开始的那个字。src 比 text 多一个，
    // 最后一个是读到的结尾。from 要在字的开头（书签、翻页、章节给的位置都是）
    bool Decode(uint32_t from, uint32_t to, std::string& text, std::vector<uint32_t>& src);
    // 段落开头：pos 往前至少退 back 字节后，再往前找到的第一个行首（往回翻页从这里排）；退到正文开头为止
    uint32_t ParagraphStartBefore(uint32_t pos, uint32_t back);
    // 把任意位置对齐到字的开头（往后挪）
    uint32_t AlignChar(uint32_t pos);

    // 整本书读一遍找章节（第 X 章 / 回 / 节 / 卷、楔子、尾声、Chapter N……）
    bool ScanChapters(std::vector<Chapter>& out);
    // 一行（UTF-8，已去掉首尾空白）是不是章名
    static bool IsChapterTitle(const std::string& line);

private:
    FILE* f_ = nullptr;
    uint32_t size_ = 0, body_ = 0;
    Encoding enc_ = kUtf8;

    bool ReadAt(uint32_t pos, uint8_t* buf, size_t n, size_t& got);
    // 一个字：返回占几个字节（不完整返回 0），cp 是 Unicode（解不了的给 '?'）
    size_t DecodeChar(const uint8_t* p, size_t avail, uint32_t& cp) const;
    bool AtParagraphStart(uint32_t pos);
};
