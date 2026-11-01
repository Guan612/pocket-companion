"""整机模拟的本地服务器：一个进程开三个端口，扮演设备在网络上的那一面。

  8801  模拟器页面（sim.html + device.wasm），以及它和"设备存储"之间的接口 /sim/*
  8802  配网热点上的网页（设备在配网模式时的 192.168.4.1）：固件里小智 esp-wifi-connect 的同一份 HTML
  8803  设备连上 Wi-Fi 后的网页设置页（http://设备IP/）：固件 direct_api_config_page.h 里的同一份 HTML

设备"上网"由这台电脑代劳：对话时照 DirectApiProtocol 的格式真的去调你在设置页填的 API
（语音识别 → 大模型含工具调用 → 逐句 opus 合成），也可以切成不花钱的模拟回答。
路由器掉线、外网不通由模拟器页面的「网络环境」开关控制，出错时设备屏幕上的提示和固件一致。

设备存储（Wi-Fi、API 设置、长期记忆）存在旁边的 .sim-nvs.json：刷新页面、重启这个服务器都相当于
设备断电重启，设置还在；页面上的「恢复出厂」才清空。

用法：python serve.py          打开 http://127.0.0.1:8801/
      python serve.py --lan    同一局域网的手机也能打开设置页，设备屏幕上的地址和二维码换成电脑的局域网 IP
"""

import argparse
import json
import pathlib
import re
import socket
import sys
import threading
import time
import urllib.error
import urllib.request
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, quote, unquote, urlparse

HERE = pathlib.Path(__file__).resolve().parent
FIRMWARE = HERE.parent.parent / "firmware"
WIFI_ASSETS = FIRMWARE / "managed_components" / "78__esp-wifi-connect" / "assets"
CONFIG_PAGE_H = FIRMWARE / "main" / "protocols" / "direct_api_config_page.h"

SIM_PORT, AP_PORT, STA_PORT = 8801, 8802, 8803
AP_SSID = "Xiaozhi-1A2B"  # 固件：ssid_prefix "Xiaozhi" + MAC 末两字节
FAKE_IP = "192.168.1.88"

# 配网页扫到的周围 Wi-Fi（authmode 0 为开放网络）
SCAN = [
    {"ssid": "Home-5G", "rssi": -42, "authmode": 3},
    {"ssid": "Home-2.4G", "rssi": -55, "authmode": 3},
    {"ssid": "ChinaNet-8A3F", "rssi": -71, "authmode": 3},
    {"ssid": "邻居家的WiFi", "rssi": -80, "authmode": 4},
    {"ssid": "Starbucks", "rssi": -84, "authmode": 0},
]

# 和 direct_api_settings.h 一致
MAX_PROVIDERS, MAX_MODELS, MAX_PROVIDER_BYTES = 8, 30, 3800
ROLES = ("chat", "vision", "asr", "tts")
MEMORY_MAX_BYTES = 2000  # LongTermMemory::kMaxBytes


def empty_settings():
    return {"providers": [], "keys": {}, "roles": {r: {"p": "", "m": ""} for r in ROLES},
            "asr_lang": "", "tts_voice": "", "tts_voices": [], "tts_speak": True, "prompt": ""}


def voice_labels():
    # NomiDisplay：屏幕上的音色列表跟着合成供应商走，只显示短名；没有就是 OpenAI 的六个
    voices = nvs["directapi"]["tts_voices"] or ["alloy", "echo", "fable", "onyx", "nova", "shimmer"]
    return [re.split(r"[:/]", v)[-1] for v in voices]


lock = threading.Lock()
nvs = {"wifi": [], "directapi": empty_settings(), "memory": ""}
# 设备存储落到旁边的文件（已在 .gitignore），重启 serve.py 不丢，和真机断电后 NVS 还在一样；
# 里面有你填的 API Key，别外传。「恢复出厂」会清空它。
NVS_FILE = HERE / ".sim-nvs.json"


def load_nvs():
    try:
        data = json.loads(NVS_FILE.read_text(encoding="utf-8"))
        nvs.update(data)
        nvs["directapi"] = {**empty_settings(), **nvs["directapi"]}
    except (OSError, ValueError, KeyError, TypeError):
        pass


def save_nvs():
    NVS_FILE.write_text(json.dumps(nvs, ensure_ascii=False, indent=1), encoding="utf-8")
net = {"mode": "off"}  # 设备当前网络：off / ap（配网热点）/ sta（连上路由器）
events = []  # 网页上发生的事，模拟器轮询后交给设备代码
urls = {}


def push_event(kind, **data):
    with lock:
        events.append({"seq": len(events) + 1, "type": kind, **data})


def resolve(role):
    # DirectApiSettings::ResolveRole：用途 → 地址 / Key / 模型，没选或供应商不在了就是空的
    c = nvs["directapi"]
    r = c["roles"].get(role, {})
    p = next((p for p in c["providers"] if p["id"] == r.get("p")), None)
    if not p:
        return {"url": "", "key": "", "model": ""}
    return {"url": p["url"].rstrip("/"), "key": c["keys"].get(p["id"], ""), "model": r.get("m", "")}


def api_complete():
    # DirectApiConfig::IsComplete：对话、语音识别、语音合成都要选好模型
    return all(resolve(r)["url"] and resolve(r)["model"] for r in ("chat", "asr", "tts"))


# ---------- 网络环境和真实 API ----------
#
# 模拟设备上网时由这台电脑代它发请求。下面几个函数照着 direct_api_protocol.cc 写：请求格式、
# 出错时屏幕上的提示文字都一样。env 由模拟器页面的「网络环境」开关控制。

env = {"router": True, "internet": True, "api": "real"}  # api: real 真调 / mock 不花钱的假回答
history = []  # DirectApiProtocol::history_，设备内存里的对话历史，重启就没了
tools_broken = [False]
HISTORY_LIMIT = 12
HTTP_TIMEOUT = 30
DEFAULT_PROMPT = ("你是一个放在桌面上的小伙伴，用口语化的中文回答，每次不超过三句话，不要用 markdown。"
                  "回答开头必须带一个情绪标签，只能是 [happy]、[neutral]、[confused] 之一，"
                  "例如：[happy]好呀，我来帮你看看。")
# NomiDisplay::FacePrompt 的同一段说明（改了那边记得同步这里）
FACE_PROMPT = (
    "你有一张会动的脸：黑底白色线条，两只眼睛加一双小手。可以在情绪标签后面紧跟一段表情脚本 "
    "<face>JSON</face> 配合你说的话，不需要就不写，别每句都写。\n"
    "JSON 是 1～6 步的数组，每步字段都可选：dur 秒（0.3-4）；eyes：capsule 平常｜arc 笑眯眯｜ring 睁大的空心眼｜"
    "dash 平静｜dot 小眼｜closed 闭眼｜heart｜star｜x｜shades 墨镜｜spiral 转圈的晕；look [x,y] 视线（-1~1）；"
    "tilt 歪头（-1~1）；brow：up 挑眉｜down 皱眉；sweat：true 汗滴；blush：0-1 腮红；"
    "hands：none｜chin 托腮｜ear 手放耳边｜wave 挥手｜up 举双手｜cover 捂眼｜mic 拿麦克风｜cheeks 双手托腮；"
    "motion：nod 点头｜bounce 蹦｜shake 摇头｜sway 摇摆；"
    "prop 道具：{\"icon\":图标,\"anim\":\"bob|swing|spin|float|pop|orbit 绕着转\",\"at\":\"mouth|left|right|top\"}，图标有 "
    "note,music,heart,star,cup,mic,z,question,exclaim,lightning,leaf,sun,cloud,drop,sparkle,flower"
    "；没有合适的图标就用 \"strokes\" 画白色线条：坐标 0-100，[x1,y1,x2,y2,…] 是折线，[x,y,r] 是圆，"
    "r 为负是实心点，最多 12 笔。\n"
    "例：[happy]<face>[{\"eyes\":\"ring\",\"brow\":\"up\",\"dur\":0.8},{\"eyes\":\"arc\",\"hands\":\"up\","
    "\"motion\":\"bounce\",\"prop\":{\"icon\":\"star\",\"anim\":\"pop\",\"at\":\"top\"},\"dur\":2}]</face>太好啦！")


def extract_face(reply):
    """ExtractFace：从回复里取出 <face>…</face>，返回 (去掉后的回复, 脚本)；没结束标签按括号配对。"""
    start = reply.find("<face>")
    if start < 0:
        return reply, ""
    body = start + 6
    end = reply.find("</face>", body)
    if end >= 0:
        return reply[:start] + reply[end + 7:], reply[body:end]
    depth, in_str, i = 0, False, body
    while i < len(reply):
        c = reply[i]
        if in_str:
            if c == "\\":
                i += 1
            elif c == '"':
                in_str = False
        elif c == '"':
            in_str = True
        elif c in "[{":
            depth += 1
        elif c in "]}":
            depth -= 1
            if depth == 0:
                return reply[:start] + reply[i + 1:], reply[body:i + 1]
        i += 1
    return reply[:start], ""  # 写了一半：整段丢掉


TOOLS_HINT = ("\n\n你可以调用设备工具：调音量 / 亮度、查设备状态用对应工具；"
              "主人说了值得长期记住的信息（称呼、喜好、安排，或明确说\"记住\"）时，"
              "调用 self.memory.remember，一次一条简短事实。")
# 设备端 MCP 工具（模拟器只实现这几个，描述和参数是近似的）
TOOLS = [
    {"type": "function", "function": {"name": "self.get_device_status",
     "description": "Provides the real-time information of the device, including the current status of the audio speaker, screen, battery, network, etc.",
     "parameters": {"type": "object", "properties": {}}}},
    {"type": "function", "function": {"name": "self.audio_speaker.set_volume",
     "description": "Set the volume of the audio speaker. If the current volume is unknown, you must call `self.get_device_status` tool first and then call this tool.",
     "parameters": {"type": "object", "properties": {"volume": {"type": "integer", "minimum": 0, "maximum": 100}}, "required": ["volume"]}}},
    {"type": "function", "function": {"name": "self.screen.set_brightness", "description": "Set the brightness of the screen.",
     "parameters": {"type": "object", "properties": {"brightness": {"type": "integer", "minimum": 0, "maximum": 100}}, "required": ["brightness"]}}},
    {"type": "function", "function": {"name": "self.face.play",
     "description": "让你的脸表演一段动作。主人明确要你做表情、做鬼脸、跳舞、弹吉他之类时用；script 是表情脚本 JSON（步骤数组，格式和回复里的 <face> 一样）。",
     "parameters": {"type": "object", "properties": {"script": {"type": "string"}}, "required": ["script"]}}},
    {"type": "function", "function": {"name": "self.memory.remember",
     "description": "把主人希望长期记住的事存进设备的长期记忆。适合：主人告诉你名字/称呼、喜好、固定安排，或明确说\"记住\"。一次记一条简短的事实，闲聊内容不要记。",
     "parameters": {"type": "object", "properties": {"fact": {"type": "string", "maxLength": 200}}, "required": ["fact"]}}},
    {"type": "function", "function": {"name": "self.camera.look",
     "description": "看一眼摄像头拍到的画面并说出看到了什么。主人让你看东西、认东西、读画面上的文字、描述眼前的场景时调用；question 写上你想从画面里知道的问题。",
     "parameters": {"type": "object", "properties": {"question": {"type": "string"}}, "required": ["question"]}}},
]


class NetError(Exception):
    """连不上（对应固件里 http->Open 失败）。"""


def load_cfg():
    # DirectApiConfig::Load：视觉没单独选就用对话模型；留空的音色、语言、人设用默认值
    c = nvs["directapi"]
    llm, vision = resolve("chat"), resolve("vision")
    return {"llm": llm, "vision": vision if vision["url"] and vision["model"] else llm,
            "asr": resolve("asr"), "tts": resolve("tts"), "voice": c["tts_voice"] or "alloy",
            "lang": c["asr_lang"] or "zh", "prompt": c["prompt"] or DEFAULT_PROMPT,
            "speak": bool(c.get("tts_speak", True))}


def http_post(url, key, body, ctype):
    """返回 (状态码, 响应体)；设备没联网、外网不通、DNS / 连接失败、超时都算连不上。"""
    if net["mode"] != "sta" or not env["router"] or not env["internet"]:
        raise NetError("offline")
    req = urllib.request.Request(url, data=body, method="POST")
    req.add_header("Content-Type", ctype)
    if key:
        req.add_header("Authorization", "Bearer " + key)
    try:
        with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
    except (urllib.error.URLError, OSError, ValueError) as e:
        raise NetError(str(e)) from e


def post_json(url, key, obj):
    return http_post(url, key, json.dumps(obj, ensure_ascii=False).encode("utf-8"), "application/json")


def split_emotion(reply):
    # SplitEmotion：去掉开头的 [happy] 这类情绪标签（按字节算长度，和固件一致）
    s = reply.lstrip(" \n")
    if s.startswith("["):
        end = s.find("]")
        if end != -1 and len(s[:end].encode()) < 16:
            return s[1:end], s[end + 1:].lstrip(" \n")
    return "neutral", reply


def split_sentences(text):
    # SplitSentences：按句切开逐句合成，太短（不到 12 字节）的并到下一句
    out, cur = [], ""
    for ch in text:
        cur += ch
        if ch in "。！？；!?;\n" and len(cur.encode()) >= 12:
            out.append(cur)
            cur = ""
    if cur.strip(" \n"):
        out.append(cur)
    return out


def remember(fact):
    # LongTermMemory::Append：换行折成空格、完全相同的不重复，超过 2000 字节就拒绝
    fact = " ".join(fact.split())
    with lock:
        lines = [l for l in nvs["memory"].split("\n") if l]
        if fact in lines:
            return True
        text = "\n".join(lines + [fact])
        if len(text.encode()) > MEMORY_MAX_BYTES:
            return False
        nvs["memory"] = text
        save_nvs()
    return True


def call_tool(name, args, device, actions):
    try:
        a = json.loads(args or "{}")
    except ValueError:
        a = {}
    if name == "self.get_device_status":
        return json.dumps({"audio_speaker": {"volume": device.get("volume", 70)},
                           "screen": {"brightness": device.get("brightness", 80)},
                           "battery": {"level": 76, "charging": False}, "network": {"type": "wifi"}})
    if name == "self.audio_speaker.set_volume":
        actions.append({"type": "volume", "value": int(a.get("volume", 50))})
        return "true"
    if name == "self.screen.set_brightness":
        actions.append({"type": "brightness", "value": int(a.get("brightness", 80))})
        return "true"
    if name == "self.face.play":  # 真正的校验在设备（wasm）里，出错会写进串口日志
        actions.append({"type": "face", "script": str(a.get("script", ""))})
        return "正在表演"
    if name == "self.memory.remember":
        return "已记住" if remember(str(a.get("fact", ""))) else "长期记忆已满，请提示主人到网页设置页整理"
    if name == "self.camera.look":  # 模拟器没有摄像头，按固件"没拍到"的分支走
        return "已拍下画面，照片随后作为用户消息发给你"
    return f"Unknown tool: {name}"


def transcribe(cfg, wav, log):
    # Transcribe：不到 0.3 秒多半是误触；WAV 用 multipart 传给 /audio/transcriptions
    if len(wav) - 44 < 16000 * 2 // 3:
        return ""
    boundary = "----PocketCompanionBoundary"
    parts = [f'--{boundary}\r\nContent-Disposition: form-data; name="model"\r\n\r\n{cfg["asr"]["model"]}\r\n']
    if cfg["lang"]:
        parts.append(f'--{boundary}\r\nContent-Disposition: form-data; name="language"\r\n\r\n{cfg["lang"]}\r\n')
    parts.append(f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="speech.wav"\r\n'
                 "Content-Type: audio/wav\r\n\r\n")
    body = "".join(parts).encode() + wav + f"\r\n--{boundary}--\r\n".encode()
    try:
        status, resp = http_post(cfg["asr"]["url"] + "/audio/transcriptions", cfg["asr"]["key"], body,
                                 "multipart/form-data; boundary=" + boundary)
    except NetError:
        raise TurnError("语音识别连接失败")
    if status >= 400:
        log(f"E ASR failed: {status} {resp[:200].decode('utf-8', 'replace')}")
        raise TurnError(f"语音识别失败（HTTP {status}）")
    try:
        text = json.loads(resp).get("text", "")
    except ValueError:
        text = ""
    log(f"I ASR: {text}")
    return text


def chat(cfg, user_text, device, actions, log):
    history.append(["user", user_text])
    llm = cfg["llm"]

    def build(with_tools):
        system = cfg["prompt"]
        if nvs["memory"]:
            system += "\n\n[主人的长期记忆]（回答时自然运用，不要整段复述）\n" + nvs["memory"]
        system += "\n\n" + FACE_PROMPT
        if with_tools:
            system += TOOLS_HINT
        req = {"model": llm["model"],
               "messages": [{"role": "system", "content": system}] + [{"role": r, "content": c} for r, c in history]}
        if with_tools:
            req["tools"] = TOOLS
        return req

    with_tools = not tools_broken[0]
    req, reply, rounds = build(with_tools), "", 0
    while rounds < 4 and not reply:
        try:
            status, resp = post_json(llm["url"] + "/chat/completions", llm["key"], req)
        except NetError:
            history.pop()
            raise TurnError("大模型连接失败")
        if status >= 400:
            if with_tools and status < 500:
                log(f"W LLM rejected tools (HTTP {status}), retrying without them")
                tools_broken[0] = True
                with_tools = False
                req = build(False)
                continue
            log(f"E LLM failed: {status} {resp[:200].decode('utf-8', 'replace')}")
            history.pop()
            raise TurnError(f"大模型请求失败（HTTP {status}）")
        rounds += 1
        try:
            message = json.loads(resp)["choices"][0]["message"]
        except (ValueError, KeyError, IndexError, TypeError):
            break
        calls = message.get("tool_calls") or []
        if not calls:
            reply = message.get("content") or ""
            break
        log(f"I LLM wants {len(calls)} tool call(s)")
        req["messages"].append(message)
        looked = False
        for tc in calls:
            fn = tc.get("function") or {}
            if fn.get("name") == "self.camera.look":
                looked = True
            result = call_tool(fn.get("name", ""), fn.get("arguments"), device, actions)
            log(f"I Tool {fn.get('name')} -> {result[:120]}")
            req["messages"].append({"role": "tool", "tool_call_id": tc.get("id", ""), "content": result})
        if looked:
            # 模拟器没有摄像头：和固件"照片没编出来"分支一样，补一条用户消息说明
            req["messages"].append({"role": "user", "content": "（这次没有拍到画面，跟主人说一声吧）"})
    log(f"I LLM: {reply}")
    reply, face = extract_face(reply)  # 表情脚本不进历史，也不念出来
    if not reply:
        history.pop()
    else:
        history.append(["assistant", split_emotion(reply)[1]])
        del history[:-HISTORY_LIMIT]
    return reply, face


class TurnError(Exception):
    """一轮对话失败，消息就是固件 SetError 的文字，设备会显示成「错误」提示。"""


MOCK = ("今天适合出去走走吗？",
        '[happy]<face>[{"eyes":"arc","motion":"bounce","prop":{"icon":"sun","anim":"spin","at":"top"},"dur":2.5}]</face>'
        "当然适合！外面阳光正好，气温也舒服。记得带瓶水，傍晚可能会起风哦。")


def run_turn(wav, typed, device):
    """RunTurn 到合成之前：识别 → 大模型（含工具调用）→ 情绪 + 分句。"""
    lines, actions = [], []
    log = lines.append
    try:
        cfg = load_cfg()
        if env["api"] == "mock":
            if net["mode"] != "sta" or not env["router"] or not env["internet"]:
                raise TurnError("语音识别连接失败")
            text, reply = MOCK
            log("I （模拟 API）不发真实请求")
            reply, face = extract_face(reply)
        else:
            if typed:
                text = typed
                log("I （模拟器：用输入框的文字代替语音识别）")
            else:
                text = transcribe(cfg, wav, log)
            if not text:
                return {"stt": "", "log": lines}
            reply, face = chat(cfg, text, device, actions, log)
        if not reply and not face:
            raise TurnError("大模型没有返回内容")
        emotion, body = split_emotion(reply)
        return {"stt": text, "emotion": emotion, "face": face, "sentences": split_sentences(body),
                "actions": actions, "log": lines, "speak": cfg["speak"]}
    except TurnError as e:
        return {"error": str(e), "actions": actions, "log": lines}


tts_mp3 = [False]  # g_tts_mp3：opus 被拒过就改要 mp3，重启（/sim/boot）后重新试 opus


def sniff_speech(data):
    # SniffSpeech：按开头几个字节认格式
    if data[:4] == b"OggS":
        return "opus"
    if data[:4] == b"RIFF":
        return "wav"
    if data[:3] == b"ID3" or (len(data) >= 2 and data[0] == 0xFF and data[1] & 0xE0 == 0xE0):
        return "mp3"
    return None


def request_speech(cfg, text, log):
    # RequestSpeechWithFallback：先要 opus，被 4xx 拒了就换 mp3 再要一次
    def once(fmt):
        return post_json(cfg["tts"]["url"] + "/audio/speech", cfg["tts"]["key"],
                         {"model": cfg["tts"]["model"], "input": text, "voice": cfg["voice"], "response_format": fmt})
    status, resp = once("mp3" if tts_mp3[0] else "opus")
    if not tts_mp3[0] and 400 <= status < 500:
        log(f"W TTS rejected opus (HTTP {status}: {resp[:120].decode('utf-8', 'replace')}), switching to mp3")
        tts_mp3[0] = True
        status, resp = once("mp3")
    return status, resp


def speak(text):
    """Speak：返回 (音频字节, 错误文字, 串口日志列表)。模拟 API 时没有声音，前两个都是 None。"""
    lines = []
    if env["api"] == "mock":
        if net["mode"] != "sta" or not env["router"] or not env["internet"]:
            return None, "语音合成连接失败", lines
        return None, None, lines
    cfg = load_cfg()
    try:
        status, resp = request_speech(cfg, text, lines.append)
    except NetError:
        return None, "语音合成连接失败", lines
    if status >= 400:
        lines.append(f"E TTS failed: {status} {resp[:200].decode('utf-8', 'replace')}")
        return None, f"语音合成失败（HTTP {status}）", lines
    if not resp:
        return None, None, lines
    fmt = sniff_speech(resp)
    if not fmt:
        lines.append("E TTS response format unknown: " + resp[:4].hex(" "))
        return None, "语音合成返回的格式认不出（要 opus / mp3 / wav）", lines
    if fmt != "opus" and not getattr(speak, "noted", None) == fmt:
        speak.noted = fmt
        lines.append(f"I TTS {fmt}（设备上用乐鑫音频库自己解码）")
    return resp, None, lines


def self_test():
    # DirectApiProtocol::SelfTest
    if not api_complete():
        return "还没配置 API，请先在网页设置页填写"
    if env["api"] == "mock":
        return "（模拟 API）大模型正常 · 语音合成支持 opus"
    cfg = load_cfg()
    describe = lambda s: "连不上" if s == 0 else f"HTTP {s}"

    def status_of(url, key, obj):
        try:
            return post_json(url, key, obj)
        except NetError:
            return 0, b""

    start = time.time()
    s, _ = status_of(cfg["llm"]["url"] + "/chat/completions", cfg["llm"]["key"],
                     {"model": cfg["llm"]["model"], "max_tokens": 8, "messages": [{"role": "user", "content": "你好"}]})
    ms = int((time.time() - start) * 1000)
    report = f"大模型：正常（{ms} ms）" if 200 <= s < 300 else f"大模型：{describe(s)}，检查地址 / Key / 模型"
    try:
        s, body = request_speech(cfg, "测试", lambda line: None)
    except NetError:
        s, body = 0, b""
    if not 200 <= s < 300:
        report += f"\n语音合成：{describe(s)}，检查地址 / Key / 模型 / 音色"
    else:
        report += {"opus": "\n语音合成：正常（opus）", "mp3": "\n语音合成：正常（mp3，设备自己解码）",
                   "wav": "\n语音合成：正常（wav）"}.get(sniff_speech(body), "\n语音合成：返回的格式认不出，要 opus / mp3 / wav")
    return report


def config_page():
    text = CONFIG_PAGE_H.read_text(encoding="utf-8")
    m = re.search(r'kDirectApiConfigPage\[\]\s*=\s*R"HTML\((.*)\)HTML"', text, re.S)
    if not m:
        raise RuntimeError(f"{CONFIG_PAGE_H} 里找不到 R\"HTML( … )HTML\"")
    return m.group(1)


def lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))  # 不会真的发包，只为让系统选出局域网网卡
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


class Handler(SimpleHTTPRequestHandler):
    extensions_map = {**SimpleHTTPRequestHandler.extensions_map, ".wasm": "application/wasm"}
    quiet = False

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        self.send_header("Access-Control-Allow-Origin", "*")
        super().end_headers()

    def log_message(self, fmt, *args):
        if not self.quiet:
            sys.stdout.write(f"[{self.server.name}] {self.address_string()} {fmt % args}\n")

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    def send(self, code, data, ctype="application/json"):
        raw = data if isinstance(data, bytes) else (
            json.dumps(data, ensure_ascii=False) if not isinstance(data, str) else data).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype + ("; charset=utf-8" if "text" in ctype or "json" in ctype else ""))
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def redirect(self, location, code=302):
        self.send_response(code)
        self.send_header("Location", location)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def offline(self, why):
        # 设备不在这个网络上时，手机浏览器打不开它
        page = ("<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
                "<body style='font:15px system-ui;padding:24px;color:#555'>"
                f"<h3>无法访问此网站</h3><p>{why}</p>")
        self.send(503, page, "text/html")


class SimHandler(Handler):
    """模拟器页面和 /sim/* 接口。"""

    quiet = True  # 每帧都在轮询，不刷屏

    def __init__(self, *a, **kw):
        super().__init__(*a, directory=str(HERE), **kw)

    def do_GET(self):
        u = urlparse(self.path)
        if u.path == "/":
            return self.redirect("/sim.html")
        if u.path == "/sim/boot":
            history.clear()  # 对话历史在设备内存里，重启就没了
            tools_broken[0] = False
            tts_mp3[0] = False
            with lock:
                wifi = nvs["wifi"][0] if nvs["wifi"] else ""
            return self.send(200, {"wifi_ssid": wifi, "api_ok": api_complete(), "ap_ssid": AP_SSID,
                                   "env": env, "voices": voice_labels(), **urls})
        if u.path == "/sim/events":
            since = int(parse_qs(u.query).get("since", ["0"])[0])
            with lock:
                return self.send(200, {"seq": len(events), "events": events[since:]})
        return super().do_GET()

    def do_POST(self):
        u = urlparse(self.path)
        if u.path == "/sim/net":
            mode = json.loads(self.body() or b"{}").get("mode", "off")
            if mode != net["mode"]:
                print({"off": "设备网络断开", "ap": f"设备开启配网热点 {AP_SSID}",
                       "sta": "设备连上 Wi-Fi"}.get(mode, mode))
            net["mode"] = mode
            return self.send(200, {})
        if u.path == "/sim/reset":
            what = parse_qs(u.query).get("what", ["all"])[0]
            with lock:
                nvs["wifi"].clear()
                if what == "all":
                    nvs["directapi"] = empty_settings()
                    nvs["memory"] = ""
                save_nvs()
            print("清除 Wi-Fi" if what == "wifi" else "恢复出厂：设备存储已清空")
            return self.send(200, {})
        if u.path == "/sim/env":
            req = json.loads(self.body() or b"{}")
            for k in ("router", "internet", "api"):
                if k in req:
                    env[k] = req[k]
            print(f"网络环境：路由器{'在线' if env['router'] else '断开'}，外网{'通' if env['internet'] else '不通'}，"
                  f"API {'真实调用' if env['api'] == 'real' else '模拟'}")
            return self.send(200, env)
        if u.path == "/sim/selftest":
            return self.send(200, {"text": self_test()})
        if u.path == "/sim/turn":
            device = json.loads(unquote(self.headers.get("X-Sim-Device") or "{}"))
            typed = unquote(self.headers.get("X-Sim-Typed") or "")
            result = run_turn(self.body(), typed, device)
            for line in result.get("log", []):
                print("  " + line)
            if result.get("error"):
                print("  对话出错：" + result["error"])
            return self.send(200, result)
        if u.path == "/sim/speak":
            text = json.loads(self.body() or b"{}").get("text", "")
            audio, error, lines = speak(text)
            for line in lines:
                print("  " + line)
            if error:
                return self.send(200, {"error": error, "log": lines})
            if audio is None:
                self.send_response(204)
                self.end_headers()
                return
            # 浏览器自己能播 ogg / mp3 / wav；串口日志放在响应头里带回去
            ctype = {"opus": "audio/ogg", "mp3": "audio/mpeg", "wav": "audio/wav"}[sniff_speech(audio)]
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(audio)))
            self.send_header("X-Sim-Log", quote(json.dumps(lines, ensure_ascii=False)))
            self.end_headers()
            self.wfile.write(audio)
            return
        self.send(404, {"error": "not found"})


class ApHandler(Handler):
    """配网热点上的网页，接口和 esp-wifi-connect 的 wifi_configuration_ap.cc 一致。"""

    CAPTIVE = {"/hotspot-detect.html", "/mobile/status.php", "/check_network_status.txt", "/ncsi.txt",
               "/fwlink/", "/connectivity-check.html", "/success.txt", "/portal.html",
               "/library/test/success.html"}

    def asset(self, name):
        path = WIFI_ASSETS / name
        if not path.exists():
            return self.send(500, f"缺少 {path}，先编译一次固件（idf.py build）下载小智的依赖", "text/plain")
        self.send(200, path.read_bytes(), "text/html")

    def do_GET(self):
        if net["mode"] != "ap":
            return self.offline(f"设备没在配网模式，手机连不上热点 {AP_SSID}。")
        u = urlparse(self.path)
        q = parse_qs(u.query)
        if u.path == "/":
            return self.asset("wifi_configuration.html")
        if u.path == "/done.html":
            return self.asset("wifi_configuration_done.html")
        if u.path == "/scan":
            return self.send(200, {"support_5g": False, "aps": SCAN})
        if u.path == "/saved/list":
            with lock:
                return self.send(200, list(nvs["wifi"]))
        if u.path in ("/saved/set_default", "/saved/delete"):
            i = int(q.get("index", ["-1"])[0])
            with lock:
                if 0 <= i < len(nvs["wifi"]):
                    ssid = nvs["wifi"].pop(i)
                    if u.path == "/saved/set_default":
                        nvs["wifi"].insert(0, ssid)
                    save_nvs()
            return self.send(200, {})
        if u.path == "/advanced/config":
            return self.send(200, {"ota_url": "", "max_tx_power": 80, "remember_bssid": False,
                                   "sleep_mode": True, "show_ota_config": True, "show_sleep_config": True})
        if u.path in self.CAPTIVE or u.path.startswith("/generate_204"):
            return self.redirect("/?lang=zh-CN")
        self.send(404, "not found", "text/plain")

    def do_POST(self):
        if net["mode"] != "ap":
            return self.offline("设备没在配网模式。")
        u = urlparse(self.path)
        if u.path == "/submit":
            try:
                req = json.loads(self.body())
            except ValueError:
                return self.send(400, "Invalid JSON", "text/plain")
            ssid, password = req.get("ssid") or "", req.get("password") or ""
            if not ssid or len(ssid.encode()) >= 33:
                return self.send(200, {"success": False, "error": "Invalid SSID"})
            time.sleep(2)  # 设备会真的去连一下路由器
            ap = next((a for a in SCAN if a["ssid"] == ssid), None)
            if not env["router"]:
                print(f"配网：连接 {ssid} 失败（路由器断开）")
                return self.send(200, {"success": False, "error": "Failed to connect to the Access Point"})
            if ap and ap["authmode"] and len(password) < 8:
                print(f"配网：连接 {ssid} 失败（密码不对）")
                return self.send(200, {"success": False, "error": "Failed to connect to the Access Point"})
            with lock:
                if ssid in nvs["wifi"]:
                    nvs["wifi"].remove(ssid)
                nvs["wifi"].insert(0, ssid)
            save_nvs()
            print(f"配网：已连接并保存 {ssid}")
            push_event("wifi_saved", ssid=ssid)
            return self.send(200, {"success": True})
        if u.path == "/exit":
            self.send(200, {"success": True})
            push_event("ap_exit")
            return
        if u.path == "/advanced/submit":
            return self.send(200, {"success": True})
        self.send(404, "not found", "text/plain")


class StaHandler(Handler):
    """设备连上 Wi-Fi 后的网页设置页，接口和校验规则都照 direct_api_config_server.cc。"""

    def do_GET(self):
        if net["mode"] != "sta":
            return self.offline("设备还没连上 Wi-Fi，或者不在这个网络里。")
        u = urlparse(self.path)
        if u.path == "/":
            return self.send(200, config_page(), "text/html")
        if u.path == "/config.json":
            with lock:
                c = nvs["directapi"]
                roles = json.loads(json.dumps(c["roles"]))
                roles["asr"]["lang"] = c["asr_lang"]
                roles["tts"]["voice"] = c["tts_voice"]
                roles["tts"]["speak"] = bool(c.get("tts_speak", True))
                data = {"providers": [{**p, "has_key": bool(c["keys"].get(p["id"]))} for p in c["providers"]],
                        "roles": roles, "prompt": c["prompt"], "memory_md": nvs["memory"],
                        "limits": {"providers": MAX_PROVIDERS, "models": MAX_MODELS, "memory": MEMORY_MAX_BYTES}}
            return self.send(200, data)
        self.send(404, "not found", "text/plain")

    def do_POST(self):
        if net["mode"] != "sta":
            return self.offline("设备不在线。")
        path = urlparse(self.path).path
        try:
            req = json.loads(self.body() or b"{}")
        except ValueError:
            return self.send(200, {"ok": False, "error": "提交的内容不是 JSON"})
        if path == "/save":
            return self.save(req)
        if path == "/models":
            return self.models(req)
        self.send(404, "not found", "text/plain")

    def save(self, req):
        err = lambda msg: self.send(200, {"ok": False, "error": msg})
        c = nvs["directapi"]
        old = {p["id"]: p for p in c["providers"]}
        providers, new_keys, ids = [], {}, set()
        items = req.get("providers")
        if not isinstance(items, list) or len(items) > MAX_PROVIDERS:
            return err(f"供应商最多 {MAX_PROVIDERS} 个")
        for it in items:
            p = {"id": str(it.get("id", "")), "name": str(it.get("name", "")), "preset": str(it.get("preset", "")),
                 "url": str(it.get("url", "")).strip().rstrip("/ "),
                 "models": [m for m in it.get("models", []) if isinstance(m, str) and m and len(m.encode()) <= 120]}
            key = str(it.get("key") or "")
            if not re.fullmatch(r"p\d{1,3}", p["id"]) or p["id"] in ids:
                return err("供应商编号不对：" + p["id"])
            ids.add(p["id"])
            if not p["name"] or len(p["name"].encode()) > 48:
                return err("供应商名称要填，且不超过 16 个字")
            if not p["url"].startswith(("http://", "https://")):
                return err(f"「{p['name']}」的地址要以 http:// 或 https:// 开头")
            if len(p["models"]) > MAX_MODELS:
                return err(f"「{p['name']}」的模型最多 {MAX_MODELS} 个")
            o = old.get(p["id"])
            if o and o["url"] != p["url"] and not key and c["keys"].get(p["id"]):
                return err(f"「{p['name']}」的地址改了，请重新填写它的 Key（防止原来的 Key 被发到新地址）")
            if len(json.dumps({k: p[k] for k in ("name", "preset", "url", "models")}, ensure_ascii=False).encode()) > MAX_PROVIDER_BYTES:
                return err(f"「{p['name']}」的模型太多了，删掉几个再保存")
            if key:
                new_keys[p["id"]] = key
            providers.append(p)
        memory = str(req.get("memory_md", ""))
        if len(memory.encode()) > MEMORY_MAX_BYTES:
            return err(f"长期记忆超过 {MEMORY_MAX_BYTES} 字节，删掉一些再保存")
        roles_in = req.get("roles") or {}
        with lock:
            c["providers"] = providers
            c["keys"] = {i: k for i, k in c["keys"].items() if i in ids}
            c["keys"].update(new_keys)
            for r in ROLES:
                ri = roles_in.get(r) or {}
                pid, model = str(ri.get("p", "")), str(ri.get("m", ""))
                c["roles"][r] = {"p": pid, "m": model} if pid in ids else {"p": "", "m": ""}
            c["asr_lang"] = str((roles_in.get("asr") or {}).get("lang", ""))
            c["tts_voice"] = str((roles_in.get("tts") or {}).get("voice", ""))
            c["tts_voices"] = [v for v in (roles_in.get("tts") or {}).get("voices", []) if isinstance(v, str) and v][:20]
            if isinstance((roles_in.get("tts") or {}).get("speak"), bool):  # 旧页面不带就不动
                c["tts_speak"] = roles_in["tts"]["speak"]
            c["prompt"] = str(req.get("prompt", ""))
            nvs["memory"] = memory
            save_nvs()
        ok = api_complete()
        print(f"设置页：已保存 {len(providers)} 个供应商（{'可以对话了' if ok else '对话 / 语音识别 / 语音合成还没选全'}）")
        push_event("settings_saved", api_ok=ok, voices=voice_labels())
        self.send(200, {"ok": True})

    def models(self, req):
        # 设备代调 GET {地址}/models；没填新 Key 就用存的，但地址必须和存的一致
        url = str(req.get("url", "")).strip().rstrip("/ ")
        key = str(req.get("key") or "")
        if not url.startswith(("http://", "https://")):
            return self.send(200, {"ok": False, "error": "先填好地址"})
        c = nvs["directapi"]
        if not key:
            p = next((p for p in c["providers"] if p["id"] == req.get("id") and p["url"] == url), None)
            key = c["keys"].get(p["id"], "") if p else ""
        if not env["router"] or not env["internet"]:
            return self.send(200, {"ok": False, "error": "连不上 " + url})
        r = urllib.request.Request(url + "/models")
        if key:
            r.add_header("Authorization", "Bearer " + key)
        try:
            with urllib.request.urlopen(r, timeout=15) as resp:
                data = json.loads(resp.read())
        except urllib.error.HTTPError as e:
            msg = (f"Key 不对（HTTP {e.code}）" if e.code in (401, 403) else
                   "这家没有提供模型列表，请手动添加模型名" if e.code == 404 else f"获取失败（HTTP {e.code}）")
            return self.send(200, {"ok": False, "error": msg})
        except (urllib.error.URLError, OSError, ValueError):
            return self.send(200, {"ok": False, "error": "连不上 " + url})
        items = data.get("data", data) if isinstance(data, dict) else data
        ids = []
        for m in items if isinstance(items, list) else []:
            mid = m.get("id") if isinstance(m, dict) else None
            if isinstance(mid, str) and mid and mid not in ids:
                ids.append(mid)
        print(f"设置页：从 {url} 拉到 {len(ids)} 个模型")
        self.send(200, {"ok": True, "models": ids[:400]})


def serve(handler, host, port, name):
    httpd = ThreadingHTTPServer((host, port), handler)
    httpd.name = name
    threading.Thread(target=httpd.serve_forever, daemon=True).start()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--lan", action="store_true", help="监听局域网，设备屏幕上显示电脑的局域网地址")
    args = parser.parse_args()
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace", line_buffering=True)
    except AttributeError:
        pass

    host = "0.0.0.0" if args.lan else "127.0.0.1"
    ip = lan_ip() if args.lan else "127.0.0.1"
    # shown 是设备屏幕上 / 手机地址栏里显示的地址，real 是模拟器里手机实际打开的地址
    urls["ap_url"] = {"shown": "http://192.168.4.1", "real": f"http://{ip}:{AP_PORT}/?lang=zh-CN"}
    sta_real = f"http://{ip}:{STA_PORT}/"
    urls["settings_url"] = {"shown": sta_real if args.lan else f"http://{FAKE_IP}/", "real": sta_real}
    urls["device_ip"] = ip if args.lan else FAKE_IP

    load_nvs()
    if nvs["wifi"] or nvs["directapi"]["providers"]:
        print(f"读回上次的设备存储（{NVS_FILE.name}）：{len(nvs['wifi'])} 个 Wi-Fi，{len(nvs['directapi']['providers'])} 个供应商")
    if not (HERE / "device.wasm").exists():
        print("还没有 device.wasm，先运行 python build.py")
    config_page()  # 早点发现页面文件不对

    serve(SimHandler, host, SIM_PORT, "sim")
    serve(ApHandler, host, AP_PORT, "配网页")
    serve(StaHandler, host, STA_PORT, "设置页")
    print(f"整机模拟：http://127.0.0.1:{SIM_PORT}/")
    if args.lan:
        print(f"局域网里的手机可以直接打开设置页：{sta_real}（设备连上 Wi-Fi 之后）")
    print("Ctrl+C 退出")
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
