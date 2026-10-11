#!/usr/bin/env python3
"""session.json schema v6 -> v7：加入 L2 session 记忆的空骨架。

为什么迁移（而不是在读取时兼容）：

    v7 新增顶层 `session_memory`（事件表 + 条目表 + 消化水位）与两个 task 级水位
    （`memo_seen_seq`、`last_prefix_tokens`）。加载路径**严格只收当前版本**
    （见 edc_state_deserialize 的版本闸门），所以旧档必须先迁移 —— 否则新二进制
    会把它们改名成 `.broken` 隔离（护栏会拒绝覆写，字节不丢，但槽位要人工搬回）。

⚠️ 迁移**只补空结构**，不猜内容：

    * `session_memory` = 空表（`next_seq = 0` / 无事件 / 无条目）。
      这一层记忆在 v6 里根本不存在，所以"空"就是它当时的真值。
    * 每个 task 的 `memo_seen_seq = 0` —— 与空表一致（表头也是 0，两者等价：
      "看过 seq 0" == "看过全部"）。`last_prefix_tokens = 0`（v6 没有头部块）。

    于是迁移后**外部行为零变化**：没有事件 ⇒ 没有任何 task 会收到补丁。

用法：

    tools/migrate_session_v6_to_v7.py <path>...          # 干跑（默认），只报告
    tools/migrate_session_v6_to_v7.py <path>... --yes    # 真写，每个文件留 .v6.bak
"""

import argparse
import json
import shutil
import sys
from pathlib import Path

V6 = 6
V7 = 7

EMPTY_SESSION_MEMORY = {
    "next_seq": 0,
    "pruned_before": 0,
    "events": [],
    "entries": [],
}


def find_sessions(paths):
    out = []
    for p in paths:
        p = Path(p).expanduser()
        if p.is_dir():
            out.extend(sorted(p.rglob("session.json")))
        elif p.is_file():
            out.append(p)
        else:
            sys.stderr.write("跳过（不存在）: %s\n" % p)
    return out


def migrate_tree(root):
    """就地改写一棵 session.json。返回 (加了水位的 task 数)。"""
    ver = root.get("version")
    if ver == V7:
        return None
    if ver != V6:
        raise ValueError("不是 v6（version=%r），不动它" % ver)

    # 1) 顶层 session 记忆骨架（幂等：已有就先按 v6 语义清成空 —— v6 里它不存在）
    root["session_memory"] = json.loads(json.dumps(EMPTY_SESSION_MEMORY))

    # 2) task 级水位
    n = 0
    for t in root.get("tasks", []) or []:
        if not isinstance(t, dict):
            continue
        t["memo_seen_seq"] = 0
        t["last_prefix_tokens"] = 0
        n += 1

    root["version"] = V7
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--yes", action="store_true", help="真写（默认只干跑）")
    args = ap.parse_args()

    targets = find_sessions(args.paths)
    if not targets:
        sys.stderr.write("没找到 session.json\n")
        return 1

    changed = skipped = failed = 0
    for path in targets:
        try:
            root = json.loads(path.read_text())
            result = migrate_tree(root)
        except Exception as e:  # noqa: BLE001 — 迁移脚本, 报清楚就行
            sys.stderr.write("  ✗ %s: %s\n" % (path, e))
            failed += 1
            continue

        if result is None:
            print("  = %s: 已经是 v7，跳过" % path)
            skipped += 1
            continue

        print("  → %s: 补空的 session 记忆 + %d 个任务的水位，version 6 -> 7%s"
              % (path, result, "" if args.yes else "  [干跑]"))

        if not args.yes:
            changed += 1
            continue

        backup = path.with_suffix(".json.v6.bak")
        shutil.copy2(path, backup)
        path.write_text(json.dumps(root, ensure_ascii=False, indent=2) + "\n")

        # 自校验：读回来必须是 v7、空骨架齐全、每个 task 都有两个水位键
        back = json.loads(path.read_text())
        assert back["version"] == V7, "自校验失败: version != 7"
        sm = back.get("session_memory")
        assert isinstance(sm, dict), "自校验失败: 缺 session_memory"
        assert sm["next_seq"] == 0 and not sm["events"] and not sm["entries"], \
            "自校验失败: 骨架不是空的"
        for t in back.get("tasks", []) or []:
            if not isinstance(t, dict):
                continue
            assert "memo_seen_seq" in t and "last_prefix_tokens" in t, \
                "自校验失败: task 缺水印"
        print("     自校验通过，备份: %s" % backup.name)
        changed += 1

    print("\n改写 %d，跳过 %d，失败 %d%s"
          % (changed, skipped, failed, "（干跑，加 --yes 才真写）" if not args.yes else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
