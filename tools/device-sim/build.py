"""把 LVGL + 固件的界面代码编成 device.wasm，供 sim.html 使用。

用法：python build.py，然后 python serve.py 打开 http://127.0.0.1:8801/
需要 `pip install ziglang`。LVGL 优先用固件编译时下载的 managed_components（保证版本一致），
没有的话再从 GitHub 拉同一个版本到 third_party/。
"""

import pathlib
import shutil
import subprocess
import sys

LVGL_TAG = "v9.5.0"  # 和 firmware/main/idf_component.yml 解析出的版本一致
HERE = pathlib.Path(__file__).resolve().parent
FIRMWARE = HERE.parent.parent / "firmware"
FONTS = FIRMWARE / "managed_components" / "78__xiaozhi-fonts"
FONT_BIN = "font_noto_sans_common_20_4.bin"  # 设备上 BUILTIN_TEXT_FONT 对应的中文字体
BOARD = FIRMWARE / "main" / "boards" / "spotpear" / "spotpear-esp32s3-max35"
CJSON = FIRMWARE / "managed_components" / "espressif__cjson" / "cJSON"  # 表情脚本解析用，和设备同一份
EXPORTS = [
    "sim_init", "sim_frame", "sim_clear_dirty", "sim_lcd",
    "sim_dirty_x0", "sim_dirty_y0", "sim_dirty_x1", "sim_dirty_y1",
    "sim_font_buffer", "sim_brightness", "sim_volume", "sim_boot_down", "sim_boot_up", "sim_pointer", "sim_force_sleep",
    # 整机流程：网页那一面（serve.py）发生的事、设备当前网络、串口日志
    "sim_str_buf", "sim_str_size", "sim_set_text", "sim_wifi_saved", "sim_ap_exit", "sim_settings_saved",
    "sim_net", "sim_stage", "sim_chat", "sim_talk_key", "sim_request", "sim_notify", "sim_router",
    "sim_log_buf", "sim_log_len", "sim_log_clear",
    # 一轮对话：录音开关、识别 / 情绪 / 逐句合成的结果、出错、工具调用
    "sim_recording", "sim_rec_started", "sim_rec_finished", "sim_turn_id", "sim_vad_end", "sim_turn_thinking", "sim_turn_stt", "sim_turn_empty",
    "sim_turn_emotion", "sim_tts_start", "sim_tts_sentence", "sim_tts_stop", "sim_turn_error", "sim_apply",
    # 表情脚本
    "sim_face_play",
]


def lvgl_dir():
    managed = FIRMWARE / "managed_components" / "lvgl__lvgl"
    if not FONTS.exists():
        sys.exit("先编译一次固件（idf.py build），让 managed_components 里有 LVGL 和小智字体")
    if managed.exists():
        return managed
    local = HERE / "third_party" / "lvgl"
    if not local.exists():
        subprocess.run(["git", "clone", "--depth", "1", "--branch", LVGL_TAG,
                        "https://github.com/lvgl/lvgl.git", str(local)], check=True)
    return local


# 控件开关跟着固件的 sdkconfig 走：模拟器能用的控件，设备上一定也开着
WIDGETS = ["ANIMIMG", "ARC", "BAR", "BUTTON", "BUTTONMATRIX", "CALENDAR", "CANVAS", "CHART",
           "CHECKBOX", "DROPDOWN", "IMAGE", "IMAGEBUTTON", "KEYBOARD", "LABEL", "LED", "LINE",
           "LIST", "MENU", "MSGBOX", "ROLLER", "SCALE", "SLIDER", "SPAN", "SPINBOX", "SPINNER",
           "SWITCH", "TABLE", "TABVIEW", "TEXTAREA", "TILEVIEW", "WIN"]


def write_device_widgets():
    sdkconfig = FIRMWARE / "build" / "config" / "sdkconfig.h"
    if not sdkconfig.exists():
        sys.exit("先编译一次固件，需要 firmware/build/config/sdkconfig.h 里的 LVGL 控件开关")
    text = sdkconfig.read_text(encoding="utf-8")
    lines = ["// 由 build.py 按固件的 sdkconfig.h 生成，不要手改"]
    for w in WIDGETS:
        lines.append(f"#define LV_USE_{w} {1 if f'#define CONFIG_LV_USE_{w} 1' in text else 0}")
    (HERE / "lv_conf_device.h").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    write_device_widgets()
    lvgl = lvgl_dir()
    sources = sorted(str(p) for p in (lvgl / "src").rglob("*.c"))
    sources += [str(HERE / "sim_main.c"), *(str(BOARD / f) for f in ("nomi_ui.c", "face.c", "face_script.c", "raster.c"))]
    sources.append(str(CJSON / "cJSON.c"))
    sources.append(str(FONTS / "src" / "cbin_font.c"))
    shutil.copyfile(FONTS / "cbin" / FONT_BIN, HERE / FONT_BIN)
    args = [
        "-target", "wasm32-wasi", "-mexec-model=reactor", "-O2",
        "-DLV_CONF_INCLUDE_SIMPLE", f"-I{HERE}", f"-I{lvgl}", f"-I{BOARD}", f"-I{FONTS / 'include'}", f"-I{CJSON}",
        *(f"-Wl,--export={name}" for name in EXPORTS),
        *sources, "-o", str(HERE / "device.wasm"),
    ]
    # 几百个源文件会超出 Windows 命令行长度，用响应文件传
    rsp = HERE / "build.rsp"
    rsp.write_text("\n".join(f'"{a}"' if " " in a else a for a in args).replace("\\", "/"), encoding="utf-8")
    try:
        subprocess.run([sys.executable, "-m", "ziglang", "cc", f"@{rsp}"], check=True)
    finally:
        rsp.unlink()
    print(f"built {HERE / 'device.wasm'} from {len(sources)} sources (LVGL: {lvgl})")


if __name__ == "__main__":
    main()
