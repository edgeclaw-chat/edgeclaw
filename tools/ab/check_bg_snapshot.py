#!/usr/bin/env python3
"""断言: 后台任务的状态变化必须经 `reason=state` 送达侧栏。

为什么这条要单独检查 —— 它是渲染层的性质, **比对产物看不见**:
侧栏的后台行在 500ms 轮询被删掉之后, 只剩事件推送这一条通路。产物 (session.json
/ chat.jsonl) 只能证明任务跑了, 证明不了"侧栏被告知了"。所以对它的回归只能在这里守。

背景 (为什么它现在是红的):
今天后台任务不是前台 active task, 推送闸门把它挡住了 —— 它的状态变化**只**以
`reason=poll` 出现。也就是说今天删掉轮询, 后台任务完成不会显示。本脚本存在的
意义就是让这件事在 C3 落地前是红的、落地后是绿的。

用法:
    check_bg_snapshot.py --bin /tmp/ab/bin/h [--root /tmp/ab]

退出码:
    0 = 通过 (后台变化经 reason=state 送达)
    1 = 断言失败 (没经过 state —— C3 之前这是**预期**的红)
    2 = 场景本身没跑起来 (没有 LLM 请求): 结论无效, **不能**当成通过
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True, help="被测二进制")
    ap.add_argument("--root", default="/tmp/ab", help="产物根目录 (默认 /tmp/ab)")
    ap.add_argument("--fixture", default="bg")
    args = ap.parse_args()

    out = Path(args.root) / "out" / "check_bg"
    trace = out / "snapshot_trace.txt"
    if trace.exists():
        trace.unlink()

    r = subprocess.run(
        [sys.executable, str(HERE / "ab.py"), "run",
         "--bin", args.bin, "--label", "check_bg", "--fixture", args.fixture,
         "--env", "EDGECLAW_SNAPSHOT_TRACE=%s" % trace],
        capture_output=True, text=True)
    sys.stderr.write(r.stdout + r.stderr)

    # 防空转闸: 没有 LLM 请求 = 场景没跑到该跑的地方, 结论无效。
    # (ab.py 自己用退出码 3 表达这件事; 这里再确认一次 trace 是否真的产生。)
    if r.returncode == 3 or not trace.exists():
        print("无效: 场景没有走通 LLM 回路 —— 这不是通过, 是没测到。")
        return 2

    pushes = []
    for line in trace.read_text().splitlines():
        if not line.startswith("push reason="):
            continue
        try:
            payload = json.loads(line.split(" ", 2)[2])
        except Exception:
            continue
        pushes.append((line.split()[1].split("=", 1)[1], payload))

    reasons = {}
    for why, _ in pushes:
        reasons[why] = reasons.get(why, 0) + 1
    print("推送原因统计: %s" % (reasons or "(无推送)"))

    by_state = [(why, p) for why, p in pushes if why == "state"]
    if not by_state:
        print()
        print("失败: %d 次推送里没有一次是 reason=state —— 后台任务的变化没有经"
              "事件推送送达侧栏。" % len(pushes))
        print("      今天这依赖 500ms 轮询兜底; 轮询一删, 后台任务完成就不会显示了。")
        print("      (C3 落地之前, 这条是**预期**的红。)")
        return 1

    # 送达到了: 把当时各 task 的状态打出来当证据。
    for why, p in by_state[-3:]:
        states = [(t.get("id"), t.get("state"), t.get("active")) for g in p.get("groups", [])
                  for t in g.get("tasks", [])]
        print("  reason=state 快照: %s" % states)
    print()
    print("通过: 后台任务的状态变化经 reason=state 送达了侧栏。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
