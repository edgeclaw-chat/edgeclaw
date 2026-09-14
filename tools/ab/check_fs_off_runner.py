#!/usr/bin/env python3
"""断言: fs 工具的阻塞 I/O 不再冻住 runner。

判据是**能区分改动前后**的 —— 这是它存在的全部理由:
场景里 `read_file` 去读一个**无写端的 FIFO**(open 会阻塞), 随后发一条 `/whoami`。
那条命令能不能被答复, 就等于 runner 有没有被冻住。

实测对照:
    改动前(fs 是同步的)  屏幕上 "## Whoami" 出现 **0** 次   —— 整个应用冻住
    改动后(uv_queue_work) 出现 108 次(持续重绘)           —— 照常处理

另跑一个非尖刀场景 (`fsok`): `read_file` 读一个普通文件, 断言内容原样回来 ——
证明异步往返本身是对的, 而不只是"会堵住"。

用法:
    check_fs_off_runner.py --bin /tmp/ab/bin/k [--root /tmp/ab]

退出码:
    0 = 通过 (runner 没被冻住, 且 fs 往返正确)
    1 = runner 被冻住 (改动前的行为)
    2 = fs 往返不对 (工具消息/内容没回来) —— 这比 1 更严重, 说明跑是跑了但结果错
    3 = 场景没跑起来 (夹具/--pre 有问题): 结论无效, **不能**当成通过
"""
import argparse
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def run_fixture(binary, root, fixture, pre):
    out = Path(root) / "out" / ("check_" + fixture)
    r = subprocess.run(
        [sys.executable, str(HERE / "ab.py"), "run", "--bin", binary,
         "--label", "check_" + fixture, "--fixture", fixture,
         "--pre", pre],
        capture_output=True, text=True)
    sys.stderr.write(r.stdout + r.stderr)
    screen = out / "screen.bin"
    if not screen.exists():
        return None, out
    return screen.read_bytes(), out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--root", default="/tmp/ab")
    args = ap.parse_args()

    # ── 1. 尖刀: FIFO 上的 read_file 堵住时, runner 还活着吗 ─────────
    # 注意: fsok 夹具的 mkfifo 用不上, 无害; fifo 夹具才需要它。
    screen, out = run_fixture(args.bin, args.root, "fifo",
                              "mkfifo .edgeclaw/workspace/fifo_probe")
    if screen is None:
        print("无效: 场景没跑起来, 没有 screen.bin。")
        return 3

    whoami = screen.count(b"## Whoami")
    print("跑步者存活探针: 屏幕上 '## Whoami' 出现 %d 次" % whoami)
    if whoami == 0:
        print()
        print("失败: read_file 堵在 FIFO 上时, /whoami **没有被答复** —— "
              "runner 被 fs I/O 冻住了。")
        print("      (fs 挪出 runner 之前, 这就是预期结果。)")
        return 1

    # ── 2. 非尖刀: 异步往返本身对不对, 内容有没有原样回来 ───────────
    screen2, out2 = run_fixture(args.bin, args.root, "fsok",
                                "printf HELLO_AB_MARKER > .edgeclaw/workspace/probe.txt")
    if screen2 is None:
        print("无效: fsok 场景没跑起来。")
        return 3

    has_marker = b"HELLO_AB_MARKER" in screen2
    print("往返探针: read_file 的文件内容回来了吗 -> %s" % ("是" if has_marker else "否"))
    if not has_marker:
        print()
        print("失败: read_file 读了普通文件, 内容没有回到工具消息里 —— "
              "异步壳把结果弄丢了。")
        return 2

    print()
    print("通过: runner 未被 fs I/O 冻住, 且 fs 往返结果正确。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
