// 表情脚本解析：把大模型写的 JSON 变成 face_script_t，交给 face_play 播放。
//
// 格式：1～6 步的数组（也接受单个对象或 {"steps":[…]}），每步的字段都可以不写：
//   dur    这一步多少秒，0.3～4，默认 1.5；整段最多 10 秒
//   eyes   capsule 平常 | arc 笑眯眯 | ring 睁大的空心眼 | dash 平静 | dot 小眼 | closed 闭眼 | heart | star | x | shades 墨镜 | spiral 晕
//   look   [x, y] 视线，-1～1      tilt  歪头，-1～1
//   brow   up 挑眉 | down 皱眉     sweat true 汗滴      blush 0～1（或 true）腮红
//   hands  none | chin 托腮 | ear 手放耳边 | wave 挥手 | up 举双手 | cover 捂眼 | mic 拿麦克风 | cheeks 双手托腮
//   motion nod 点头 | bounce 蹦 | shake 摇头 | sway 摇摆
//   prop   道具 {"icon": 内置图标名, "strokes": 自己画的线条, "anim": bob|swing|spin|float|pop|orbit, "at": mouth|left|right|top}
//          strokes 是坐标 0～100 的数组：[x1,y1,x2,y2,…] 是一条折线，[x,y,r] 是圆圈，r 为负是实心圆点
// 不认识的字段和取值直接忽略，数值超范围会截断；一个能播的步骤都没有才算失败。
#pragma once

#include <stddef.h>

#include "face.h"

#ifdef __cplusplus
extern "C" {
#endif

// 成功返回 1；失败返回 0，原因（给大模型看的中文）写进 err
int face_script_parse(const char *json, face_script_t *out, char *err, size_t err_size);

// 内置图标名，逗号分隔（写提示词 / 工具说明用）
const char *face_script_icon_names(void);

#ifdef __cplusplus
}
#endif
