// 在电脑上跑表情代码：每个状态推进一段时间后存一帧 PPM，用来对照浏览器预览。
// 用法：render_frames <输出目录>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "face.h"

static void save_ppm(const char *path, const uint16_t *fb)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(1);
    }
    fprintf(f, "P6\n%d %d\n255\n", FACE_W, FACE_H);
    for (int i = 0; i < FACE_W * FACE_H; i++) {
        uint16_t c = fb[i];
        unsigned char px[3] = {(unsigned char)((c >> 11) << 3), (unsigned char)(((c >> 5) & 63) << 2),
                               (unsigned char)((c & 31) << 3)};
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    static uint16_t fb[FACE_W * FACE_H];
    face_init(1234);
    int total_dirty = 0, frames = 0;
    for (int m = 0; m < FACE_MODE_COUNT; m++) {
        face_set_mode((face_mode_t)m);
        // 每个状态推进 1.2 秒，每 2 步渲染一次，模拟设备上 30fps
        for (int step = 0; step < 72; step++) {
            face_step(1.0f / 60);
            if (step % 2) {
                rrect_t d = face_render(fb);
                total_dirty += (d.x1 - d.x0) * (d.y1 - d.y0);
                frames++;
            }
        }
        char path[512];
        snprintf(path, sizeof(path), "%s/%d-%s.ppm", dir, m, face_mode_name((face_mode_t)m));
        save_ppm(path, fb);
    }
    printf("frames=%d avg_dirty=%.1f%% of screen\n", frames,
           100.0 * total_dirty / frames / (FACE_W * FACE_H));
    return 0;
}
