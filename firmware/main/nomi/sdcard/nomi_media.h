// 媒体库（系统能力）：整张卡扫一遍，记下歌、书、图片在哪；应用按类型查，不用各自整卡找。
//
// 扫的结果存在卡上 nomi/cache/media/（一类一个文件，一行「sd:/路径 TAB 大小」，按路径排），下次直接用。
// 什么时候重扫：卡的总容量 / 剩余空间变了（换了卡、在电脑上拷进拷出），或者系统自己改过卡上的东西
// （NomiFiles::Changed：网页上传、删、改名，应用 fs_write……）。在电脑上只改名不增删的发现不了：应用可以要求重扫。
// 扫的规矩和文件管理一样：跳过 . 开头的、系统的文件夹（nomi/、System Volume Information），最深 8 层。
#pragma once

#include <cstdint>
#include <string>

namespace NomiMedia {

enum Kind { kAudio = 1, kBook = 2, kImage = 3 };  // mp3 flac wav aac m4a / txt / jpg jpeg png

// 要挂着卡（调用方 NomiSdCard::Use），会卡住（第一次扫要几秒）：别在界面线程里调。
// 返回这一类一共几个（出错为负）；out 里从第 first 个（从 0 起）开始一行一个，放到 cap 字节为止
int32_t List(Kind kind, int32_t first, bool rescan, std::string& out, size_t cap);

}  // namespace NomiMedia
