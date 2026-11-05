// 图片解码（系统能力：图片应用、缩略图、电子相框用）。从文件解成指定大小的 RGB565。
//
// - JPEG：TJpgDec R0.03（esp_jpeg 组件带的，查表解 Huffman，比 ROM 里那份快几倍），边读边解，不把整个文件
//   读进内存（手机拍的一两千万像素的照片也行）。
//   按要的大小先 1/2、1/4、1/8 缩着解（1/8 时只解直流分量，很快），解出来的像素直接按最近邻落到目标图上
//   （顺手按 EXIF 方向转正、按 Fit 留边或裁切），不另开整张大图的内存。缩略图优先用 EXIF 里自带的小图。
//   渐进式 JPEG 不支持（TJpgDec 只认基线）。
// - PNG：ROM 里的 tinfl 一块块解压、一行行还原，直接落到目标图上，只留两行在内存里，多大都行；隔行扫描的不支持。
// 慢（大照片一两秒），别在界面任务里调。SD 卡上的文件要调用方挂着卡。
#pragma once

#include <cstdint>
#include <string>

namespace NomiImage {

enum Fit {
    kContain,  // 整张放进去，四周留黑
    kCover,    // 铺满，多的裁掉（缩略图）
};

struct Image {
    uint16_t* pixels = nullptr;  // w × h 的 RGB565（PSRAM）
    int w = 0, h = 0;
    int src_w = 0, src_h = 0;  // 原图多大（转正以后的）

    Image() = default;
    ~Image();
    Image(Image&& o) noexcept;
    Image& operator=(Image&& o) noexcept;
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
};

enum Source {
    kMain,        // 解主图
    kThumbFirst,  // 缩略图：EXIF 里自带的小图够大就用它，不然解主图
    kThumbOnly,   // 预览：只要 EXIF 小图（多小都行，放大了看个大概），没有就失败
};

// 解成正好 w × h。失败返回 false，error 里是给人看的原因
bool Load(const std::string& path, int w, int h, Fit fit, Source source, Image& out, std::string& error);

}  // namespace NomiImage
