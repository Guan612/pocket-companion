"""做离线道路地图包（.nmap），给设备上的地图应用画周围的街道。

    python tools/map/make_map.py --name 成都 --bbox 30.62,104.02,30.70,104.12
    python tools/map/make_map.py --name 成都 --bbox 30.62,104.02,30.70,104.12 --upload 192.168.1.40

数据来自 OpenStreetMap（Overpass 接口，© OpenStreetMap contributors，ODbL），坐标 WGS-84，和手机 GPS 一致。
道路分四档：0 主干（motorway trunk primary 和匝道） 1 次干（secondary tertiary） 2 支路（residential service
等） 3 小路（人行道、台阶、自行车道、小径）。每条路还记着谁能走（步行 1、骑车 2、开车 4），设备上按选的
出行方式只画能走的；按 OSM 的默认（高速只给车、人行道只给人……），再看 foot / bicycle / motor_vehicle /
motorroad 这些标签改。
按 0.01° 见方（成都一带约 1 公里）切块，每块里的路裁到块的边上、去掉多余的点。

包的格式（小端）：
  "NMAP" u32 版本=1
  i32 起始纬度块号 lat0、i32 起始经度块号 lon0（都是 floor(度数 × 100)）、u16 行数、u16 列数
  每块一项（先按纬度从南往北、再按经度从西往东）：u32 在文件里的位置、u32 长度（0 = 这块没有路）
  每块：u16 路数；每条路 u8 档、u8 谁能走（步行 1、骑车 2、开车 4）、u16 点数，每个点 i16 x、i16 y =
  离这块西南角的经度、纬度差（单位 1e-5°）
放到卡上的 maps/ 文件夹（--upload 直接传到设备上）。
"""
import argparse
import json
import math
import os
import struct
import sys
import time
import urllib.parse
import urllib.request

MIRRORS = ['https://overpass-api.de/api/interpreter', 'https://overpass.private.coffee/api/interpreter',
           'https://maps.mail.ru/osm/tools/overpass/api/interpreter']
CLASS = {}
for k in ('motorway', 'trunk', 'primary', 'motorway_link', 'trunk_link', 'primary_link'):
    CLASS[k] = 0
for k in ('secondary', 'tertiary', 'secondary_link', 'tertiary_link'):
    CLASS[k] = 1
for k in ('unclassified', 'residential', 'living_street', 'road', 'service', 'track'):
    CLASS[k] = 2
for k in ('footway', 'pedestrian', 'path', 'steps', 'cycleway', 'bridleway'):
    CLASS[k] = 3
WALK, BIKE, CAR = 1, 2, 4
# 没标的时候谁能走（OSM 的一般默认）
DEFAULT_MODES = {'motorway': CAR, 'motorway_link': CAR, 'cycleway': BIKE, 'steps': WALK, 'footway': WALK,
                 'pedestrian': WALK, 'bridleway': WALK, 'path': WALK | BIKE}


def modes_of(tags):
    m = DEFAULT_MODES.get(tags['highway'], WALK | BIKE | CAR)
    yes = ('yes', 'designated', 'permissive')
    if tags.get('motorroad') == 'yes':
        m &= ~(WALK | BIKE)
    for key, bit in (('foot', WALK), ('bicycle', BIKE), ('motor_vehicle', CAR), ('motorcar', CAR)):
        v = tags.get(key)
        if v == 'no':
            m &= ~bit
        elif v in yes:
            m |= bit
    return m
TILE = 0.01    # 一块多大（度）
UNIT = 1e-5    # 点的单位（度），约 1 米
CHUNK = 0.04   # 一次问 Overpass 多大一片（度），太大会超时


def fetch(s, w, n, e):
    q = ('[out:json][timeout:120];way["highway"~"^(%s)$"](%.5f,%.5f,%.5f,%.5f);out tags geom;'
         % ('|'.join(CLASS), s, w, n, e))
    data = urllib.parse.urlencode({'data': q}).encode()
    for attempt in range(6):
        url = MIRRORS[attempt % len(MIRRORS)]
        try:
            req = urllib.request.Request(url, data=data, headers={'User-Agent': 'pocket-companion-make-map/1'})
            return json.load(urllib.request.urlopen(req, timeout=180))['elements']
        except Exception as ex:  # 忙、超时：换一个镜像再来
            print(f'  {url} 没成（{type(ex).__name__}），重试', file=sys.stderr)
            time.sleep(5 + attempt * 5)
    sys.exit('Overpass 一直不通，过会儿再试')


def clip(seg, x0, y0, x1, y1):
    """Liang–Barsky：线段裁到矩形里，裁不出东西返回 None"""
    (ax, ay), (bx, by) = seg
    dx, dy = bx - ax, by - ay
    t0, t1 = 0.0, 1.0
    for p, q in ((-dx, ax - x0), (dx, x1 - ax), (-dy, ay - y0), (dy, y1 - ay)):
        if p == 0:
            if q < 0:
                return None
        else:
            t = q / p
            if p < 0:
                t0 = max(t0, t)
            else:
                t1 = min(t1, t)
    if t0 > t1:
        return None
    return (ax + t0 * dx, ay + t0 * dy), (ax + t1 * dx, ay + t1 * dy)


def simplify(pts, tol):
    """Douglas–Peucker"""
    if len(pts) < 3:
        return pts
    (ax, ay), (bx, by) = pts[0], pts[-1]
    dx, dy = bx - ax, by - ay
    length = math.hypot(dx, dy) or 1e-12
    far, idx = -1, 0
    for i in range(1, len(pts) - 1):
        d = abs(dy * (pts[i][0] - ax) - dx * (pts[i][1] - ay)) / length
        if d > far:
            far, idx = d, i
    if far <= tol:
        return [pts[0], pts[-1]]
    return simplify(pts[:idx + 1], tol)[:-1] + simplify(pts[idx:], tol)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--name', required=True, help='包名（文件名），比如 成都')
    ap.add_argument('--bbox', required=True, help='南,西,北,东（度），比如 30.62,104.02,30.70,104.12')
    ap.add_argument('--out', default='build/maps', help='输出到哪个文件夹')
    ap.add_argument('--upload', help='设备的 IP：做好直接传到卡上的 maps/')
    args = ap.parse_args()
    s, w, n, e = (float(v) for v in args.bbox.split(','))
    if not (s < n and w < e) or (n - s) * (e - w) > 1:
        sys.exit('bbox 不对，或者太大（最多 1 平方度）')

    ways = {}
    rows_q, cols_q = math.ceil((n - s) / CHUNK - 1e-9), math.ceil((e - w) / CHUNK - 1e-9)  # 小数累加会多出一排空框
    for i in range(rows_q):
        for j in range(cols_q):
            box = (s + i * CHUNK, w + j * CHUNK, min(s + (i + 1) * CHUNK, n), min(w + (j + 1) * CHUNK, e))
            print('取 %.3f,%.3f – %.3f,%.3f' % box)
            for el in fetch(*box):
                if el.get('type') == 'way' and el.get('geometry'):
                    ways[el['id']] = el
    print(f'{len(ways)} 条路')

    lat0, lon0 = math.floor(s / TILE), math.floor(w / TILE)
    rows, cols = math.floor(n / TILE) - lat0 + 1, math.floor(e / TILE) - lon0 + 1
    tiles = [[] for _ in range(rows * cols)]
    for way in ways.values():
        cls = CLASS[way['tags']['highway']]
        modes = modes_of(way['tags'])
        if not modes:
            continue
        pts = [(g['lon'], g['lat']) for g in way['geometry']]
        # 这条路碰到的每一块都裁一段进去（相邻两块的段在边上接上）
        r0 = max(0, math.floor(min(p[1] for p in pts) / TILE) - lat0)
        r1 = min(rows - 1, math.floor(max(p[1] for p in pts) / TILE) - lat0)
        c0 = max(0, math.floor(min(p[0] for p in pts) / TILE) - lon0)
        c1 = min(cols - 1, math.floor(max(p[0] for p in pts) / TILE) - lon0)
        for r in range(r0, r1 + 1):
            for c in range(c0, c1 + 1):
                x0, y0 = (lon0 + c) * TILE, (lat0 + r) * TILE
                piece = []
                for i in range(1, len(pts)):
                    seg = clip((pts[i - 1], pts[i]), x0, y0, x0 + TILE, y0 + TILE)
                    if seg is None:
                        if len(piece) >= 2:
                            tiles[r * cols + c].append((cls, modes, piece))
                        piece = []
                        continue
                    if not piece or piece[-1] != seg[0]:
                        if len(piece) >= 2:
                            tiles[r * cols + c].append((cls, modes, piece))
                        piece = [seg[0]]
                    piece.append(seg[1])
                if len(piece) >= 2:
                    tiles[r * cols + c].append((cls, modes, piece))

    blobs = []
    total_pts = 0
    for i, roads in enumerate(tiles):
        r, c = divmod(i, cols)
        x0, y0 = (lon0 + c) * TILE, (lat0 + r) * TILE
        out = bytearray()
        count = 0
        for cls, modes, piece in sorted(roads, key=lambda t: -t[0]):  # 小路先画，主干压在上面
            q = [(round((x - x0) / UNIT), round((y - y0) / UNIT)) for x, y in piece]
            q = simplify(q, 1.5)
            q = [p for k, p in enumerate(q) if k == 0 or p != q[k - 1]]
            if len(q) < 2:
                continue
            out += struct.pack('<BBH', cls, modes, len(q)) + b''.join(struct.pack('<hh', x, y) for x, y in q)
            count += 1
            total_pts += len(q)
        blobs.append(struct.pack('<H', count) + out if count else b'')

    head = b'NMAP' + struct.pack('<IiiHH', 1, lat0, lon0, rows, cols)
    pos = len(head) + 8 * len(blobs)
    index = bytearray()
    for b in blobs:
        index += struct.pack('<II', pos if b else 0, len(b))
        pos += len(b)
    data = head + index + b''.join(blobs)
    os.makedirs(args.out, exist_ok=True)
    path = os.path.join(args.out, args.name + '.nmap')
    open(path, 'wb').write(data)
    print(f'{path}：{rows}×{cols} 块，{total_pts} 个点，{len(data) / 1024:.0f} KB')

    if args.upload:
        body = f'maps/{args.name}.nmap\n'.encode('utf-8') + data
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        req = urllib.request.Request(f'http://{args.upload}/sd/upload', data=body, method='POST')
        print('传到设备：', opener.open(req, timeout=900).read().decode('utf-8', 'replace'))


if __name__ == '__main__':
    main()
