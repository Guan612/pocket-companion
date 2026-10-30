// 浏览器模拟器的导出层：把固件的表情代码编成 WebAssembly，由 sim.html 驱动。
#include "face.h"

static uint16_t s_fb[FACE_W * FACE_H];
static rrect_t s_dirty;

uint16_t *sim_fb(void) { return s_fb; }
int sim_w(void) { return FACE_W; }
int sim_h(void) { return FACE_H; }

void sim_init(uint32_t seed) { face_init(seed); }
void sim_set_mode(int mode) { face_set_mode((face_mode_t)mode); }
int sim_get_mode(void) { return (int)face_get_mode(); }
void sim_set_level(float level) { face_set_level(level); }
void sim_step(float dt) { face_step(dt); }

// 返回本帧脏区面积，脏区坐标用 sim_dirty_* 取
int sim_render(void)
{
    s_dirty = face_render(s_fb);
    return (s_dirty.x1 - s_dirty.x0) * (s_dirty.y1 - s_dirty.y0);
}
int sim_dirty_x0(void) { return s_dirty.x0; }
int sim_dirty_y0(void) { return s_dirty.y0; }
int sim_dirty_x1(void) { return s_dirty.x1; }
int sim_dirty_y1(void) { return s_dirty.y1; }
