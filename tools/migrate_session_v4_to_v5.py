#!/usr/bin/env python3
"""session.json schema v4 -> v5：把压扁的 `state` 拆成 `step` + `life`。

为什么要迁移（而不是在读取时兼容）：

    v4 的 `state` 把两件**正交**的事挤成了一个常量 —— "在哪一步"和"活着吗"。
    旧代码在**读入时**用 normalize_interrupted_state 把五个相位一起塌成 SUSPENDED，
    相位信息就此永久丢失，resume 只能靠残留物（工具账本 / active_turn）顺序探测把
    相位猜回来。v5 分开存，读入时只收敛 life，step 原样保留。

⚠️ 旧文件里**已经丢掉**的东西找不回来：一个 v4 的 SUSPENDED 任务，磁盘上根本没写它
   当时在哪一步。迁移只能给一个中性值（READY）。这是 **v4 的缺陷，不是迁移的缺陷** ——
   迁移之后不再发生。

映射（与 C 侧 legacy_to_step_life 逐条一致）：

    IDLE           -> step READY        life ALIVE
    WAITING_DEPS   -> step AWAIT_DEPS   life ALIVE
    WAITING_INPUT  -> step AWAIT_INPUT  life ALIVE
    LLM_PROCESSING -> step AWAIT_EXEC   life ALIVE
    TOOL_CALLING   -> step AWAIT_TOOLS  life ALIVE
    SUSPENDED      -> step READY        life SUSPENDED   (step 未知, 取中性值)
    COMPLETED      -> step DONE         life COMPLETED
    CANCELLED      -> step DONE         life CANCELLED
    ERROR          -> step DONE         life ERROR

用法：

    tools/migrate_session_v4_to_v5.py <path>...          # 干跑（默认），只报告
    tools/migrate_session_v4_to_v5.py <path>... --yes    # 真写，每个文件留 .v4.bak

<path> 可以是 session.json，也可以是包含会话的工作区目录（递归找 session.json）。
幂等：已经是 v5 的文件跳过；自校验：写完立刻读回验一遍。
"""
import argparse
import json
import shutil
import sys
from pathlib import Path

STEP_OF = {
    "IDLE": ("READY", "ALIVE"),
    "WAITING_DEPS": ("AWAIT_DEPS", "ALIVE"),
    "WAITING_INPUT": ("AWAIT_INPUT", "ALIVE"),
    "LLM_PROCESSING": ("AWAIT_EXEC", "ALIVE"),
    "TOOL_CALLING": ("AWAIT_TOOLS", "ALIVE"),
    "SUSPENDED": ("READY", "SUSPENDED"),
    "COMPLETED": ("DONE", "COMPLETED"),
    "CANCELLED": ("DONE", "CANCELLED"),
    "ERROR": ("DONE", "ERROR"),
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
    """把一棵 session.json 从 v4 改写成 v5，返回改写的任务数。就地修改 root。"""
    ver = root.get("version")
    if ver == 5:
        return None  # 已经迁过
    if ver != 4:
        raise ValueError("不是 v4（version=%r），不动它" % ver)

    n = 0
    for task in root.get("tasks", []) or []:
        if not isinstance(task, dict):
            continue
        state = task.pop("state", None)
        if state is None:
            continue
        if state not in STEP_OF:
            raise ValueError("未知 state: %r" % state)
        step, life = STEP_OF[state]
        task["step"] = step
        task["life"] = life
        n += 1

    root["version"] = 5
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
            raw = path.read_text()
            root = json.loads(raw)
            n = migrate_tree(root)
        except Exception as e:  # noqa: BLE001 — 迁移脚本, 报清楚就行
            sys.stderr.write("  ✗ %s: %s\n" % (path, e))
            failed += 1
            continue

        if n is None:
            print("  = %s: 已经是 v5，跳过" % path)
            skipped += 1
            continue

        print("  → %s: %d 个任务 state -> step+life，version 4 -> 5%s"
              % (path, n, "" if args.yes else "  [干跑]"))
        if not args.yes:
            changed += 1
            continue

        backup = path.with_suffix(".json.v4.bak")
        shutil.copy2(path, backup)
        path.write_text(json.dumps(root, ensure_ascii=False, indent=2) + "\n")

        # 自校验：读回来必须真是 v5，且每个任务都有 step/life
        back = json.loads(path.read_text())
        assert back["version"] == 5, "自校验失败: version != 5"
        for t in back.get("tasks", []) or []:
            if isinstance(t, dict) and "state" in t:
                raise AssertionError("自校验失败: 仍有 state 键")
            if isinstance(t, dict) and ("step" not in t or "life" not in t):
                raise AssertionError("自校验失败: 缺 step/life")
        print("     自校验通过，备份: %s" % backup.name)
        changed += 1

    print("\n改写 %d，跳过 %d，失败 %d%s"
          % (changed, skipped, failed, "（干跑，加 --yes 才真写）" if not args.yes else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
