// 看图的系统控件和电子相框（图片应用的 ui_thumb、ui_photo，睡着时的「相册」）。解码见 nomi_image。
//
// - 解图都在一个后台任务里（优先级低，栈在 PSRAM），全屏看图和相框排在缩略图前面；控件删了，还没解的就不解了。
// - 缓存：缩略图、全屏图解好都存在卡上 nomi/cache/images/（按路径、文件大小、修改时间、尺寸认），下次直接读
//   （全屏图 300KB 一张，读一下零点几秒；解一张手机照片要好几秒）。
// - 缩略图：方块、铺满裁切，手机照片优先用 EXIF 里自带的小图。
// - 全屏看图：没缓存时先把 EXIF 小图放大当预览（几十毫秒），清楚的解好了再换上；应用可以预读下一张（Preload）。
// - 全屏看图：整张放进屏幕（留黑边）。手势按 flags：kSwipeLr 左滑下一张、右滑上一张；kSwipeUd 上滑下一张、
//   下滑上一张；kTapLr 点左三分之一上一张、右三分之一下一张。点中间那三分之一、长按要菜单。换哪张由应用决定
//   （应用管顺序），控件只报 kNext / kPrev / kMenu。
// - 电子相框：睡着时（设置里「睡着时显示」选了相册）盖在最上面，轮播一个文件夹里的图（打乱顺序）。文件夹和
//   换图间隔由图片应用设（photo_frame），存在 NVS。摸一下就叫醒（交给 on_touch）。
// 除了 Start，都要在显示锁里调。
#pragma once

#include <functional>
#include <string>

#include "lvgl.h"

class Display;

namespace NomiPhoto {

enum Flags { kSwipeLr = 1, kSwipeUd = 2, kTapLr = 4 };
enum Event { kNext, kPrev, kMenu };
using Callback = std::function<void(Event ev)>;

void Start(Display* display);  // 建后台解图任务；板子初始化时调一次

lv_obj_t* CreateThumb(lv_obj_t* parent, const std::string& path, int size);
lv_obj_t* CreateViewer(lv_obj_t* parent, const std::string& path, int flags, Callback cb);
bool IsViewer(lv_obj_t* obj);
void ViewerShow(lv_obj_t* viewer, const std::string& path);
void ViewerSetFlags(lv_obj_t* viewer, int flags);
// 预读：后台（排在别的后面）把这张解成全屏图存进缓存，翻过去就快。界面不用管它
void Preload(const std::string& path);

// 电子相框。dir 空 = 没设
void SetFrame(const std::string& dir, int seconds);  // 存 NVS（排到主循环）
bool FrameConfigured();
bool FrameStart(std::function<void()> on_touch);  // 没设相册、文件夹里没图返回 false
void FrameStop();
bool FrameActive();

// 网页 GET /sd/preview/<宽>x<高>/<卡上的路径（百分号编码）>：当场解成 BMP（最大 480×480），网页预览图片用。
// 不认识的地址返回 false（404）
bool PreviewBmp(const std::string& uri, std::string& type, std::string& out);

}  // namespace NomiPhoto
