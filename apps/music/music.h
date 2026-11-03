// 音乐应用几个文件共用的东西。
#pragma once

#include "nomi_sdk.h"
#include "nomi_text.h"

#define NAME_LEN 128
#define PATH_LEN 256

// 现在放的（或上次放的）是哪一路：上一首 / 下一首、放完了接什么、"接着放"都看它
enum { SRC_NONE, SRC_LOCAL, SRC_RADIO, SRC_PODCAST, SRC_ONLINE };
extern int g_src;
void set_source(int src);  // 记下来（kv "last"）
void update_context(void);

// 网络、网页共用的大缓冲（RSS、网页、搜索结果都往这里收；同一时间只做一件事）
#define NET_BUF (224 * 1024)
extern char g_net[NET_BUF];

// 界面上可点的一行：kind 自己定，index、name 带着走。page 是在第几页（1 是首页）
int32_t add_row(int32_t id, int kind, int index, const char *name);
void push_page(const char *title);
void back_home(void);
extern int g_pages;

// 播客（podcast.c）
enum { ROW_SUB = 40, ROW_EPISODE, ROW_POD_RESUME };
void pod_load(void);
int pod_count(void);
void pod_show_subs(void);                // 一页：订阅列表
int pod_click(int kind, int index);      // 点了 ROW_SUB / ROW_EPISODE，处理了返回 1
int pod_play_latest(const char *query);  // 按名字放最新一期（订阅里找不到就去目录搜）；空 = 接着听上次；返回 <0 失败
int pod_resume(void);
int pod_subscribe(const char *query, char *name_out, int size);  // 去目录搜，订第一个；返回 <0 失败
void pod_media(int value);               // NOMI_EV_MEDIA
void pod_save_progress(int done);        // 放的是播客时记进度
void pod_web(const char *body, char *out, int size);  // 网页请求（cmd 以 pod_ 开头）
void pod_context(char *out, int size);

// 在线音乐（online.c）
enum { ROW_ONLINE_SONG = 60 };
void online_load(void);
int online_ready(void);  // 填了网站
void online_show(void);  // 一页：上次搜到的
int online_click(int kind, int index);
int online_search_play(const char *query, char *title_out, int size);  // 搜、放第一首能放的；返回 <0 失败
int online_resume(void);
void online_media(int value);
void online_web(const char *body, char *out, int size);
void online_context(char *out, int size);

// 回给网页的 JSON：接一段转义好的字符串（带引号）
int32_t json_str(char *out, int32_t size, const char *s);
