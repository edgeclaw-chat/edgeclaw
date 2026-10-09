#!/usr/bin/env python3
"""session.json schema v5 -> v6：折叠水位从**组级标量**改成**每个 turn 的前缀**。

为什么要迁移（而不是在读取时兼容）：

    v5 用 `group.folded_through_completion_seq` 记"这个组折到哪个提交序为止"，再配合
    每个 task 的 `turns_folded` 布尔，**推导**出整份 history 的前缀里有多少条是已折的。
    请求装配于是写成 `history[1 + 已折条数 ..]` —— 水位是个**组级标量**，只能落在
    "某条消息之前"，切不到一个 turn 的内部。

    v6 把水位搬到 turn 上（`turn.folded_prefix`）：折叠落地直接给被折的 turn 打前缀，
    组 history 只留**未折叠**的消息（落地即剪枝）。请求因此是平凡的 `history[1..]`，
    而"折到一半的 turn"也能表达 —— v5 的结构根本表达不了它。

⚠️ 迁移**只搬运已记录的事实**，不猜：

    * `turns_folded == true` 的 task：它的每个 turn 都整条进过记忆 -> `folded_prefix` = 该
      turn 的消息条数。
    * `turns_folded == false`（或缺键）：`folded_prefix` = 0。
    * `active_turn`（在途回合）：旧模型**从不**把它算进已折（`folded_msg_count` 只数
      committed turns）-> `folded_prefix` = 0。
    * 组级 `folded_through_completion_seq` 与 task 级 `turns_folded` 两个键一并删掉。

    自检会交叉验证两者：水位 W 覆盖的每个 task（0 < completion_seq <= W）都必须是
    `turns_folded == true`；不是就报出来（说明源文件本来就自相矛盾，迁移不改它）。

等价性自检（`--check` 默认开）：

    对每个组，**同一份数据**按旧模型与新模型各算一遍"下次请求里 system prompt 之后的
    那串消息"，要求逐条相同：
        旧 = 成员 committed turns 全量拼接 [已折条数 ..]  + active_turn 全量
        新 = 成员 turns 前缀之后拼接（跳过 sink==GROUP_MEMORY 的成员）+ active_turn 前缀之后
    用户组必须完全一致（这是"只换表示、不改行为"的证据）。系统组允许不同：新模型不再把
    折叠 prompt（GROUP_MEMORY 成员的 turn）混进组历史 —— 那是运行期就有的规则
    (edc_executor.c 的提交点)，旧 loader 没跟上，v6 借这次迁移对齐。

用法：

    tools/migrate_session_v5_to_v6.py <path>...          # 干跑（默认），只报告
    tools/migrate_session_v5_to_v6.py <path>... --yes    # 真写，每个文件留 .v5.bak

<path> 可以是 session.json，也可以是包含会话的工作区目录（递归找 session.json）。
幂等：已经是 v6 的文件跳过；自校验：写完立刻读回，重跑等价性检查。
"""
import argparse
import json
import shutil
import sys
from pathlib import Path

GROUP_MEMORY_SINK = "GROUP_MEMORY"


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


def turn_msgs(turn):
    msgs = turn.get("messages")
    return msgs if isinstance(msgs, list) else []


def turn_prefix(turn):
    v = turn.get("folded_prefix")
    return v if isinstance(v, int) and v >= 0 else 0


def ordered_members(group, tasks_by_id):
    """组内成员，按 completion_seq 稳定排序（与 C 侧 rebuild_history 的插入排序同序）。"""
    out = []
    for tid in group.get("member_task_ids", []) or []:
        t = tasks_by_id.get(tid)
        if t is not None:
            out.append(t)
    out.sort(key=lambda t: t.get("completion_seq") or 0)  # sorted() 稳定 == 插入排序
    return out


def sent_after_system_prompt(group, tasks_by_id, model):
    """system prompt 之后那串消息。model = "v5"（旧）或 "v6"（新）。"""
    msgs = []
    folded_total = 0
    for t in ordered_members(group, tasks_by_id):
        sink = t.get("sink")
        for turn in t.get("turns", []) or []:
            n = len(turn_msgs(turn))
            if model == "v5":
                folded_total += n if t.get("turns_folded") else 0
                msgs.extend(turn_msgs(turn))
            else:
                if sink == GROUP_MEMORY_SINK:
                    continue  # 折叠 prompt 不进历史（运行期规则）
                msgs.extend(turn_msgs(turn)[turn_prefix(turn):])
    if model == "v5":
        msgs = msgs[folded_total:]
        for t in ordered_members(group, tasks_by_id):
            at = t.get("active_turn")
            if isinstance(at, dict):
                msgs.extend(turn_msgs(at))
    else:
        for t in ordered_members(group, tasks_by_id):
            if t.get("sink") == GROUP_MEMORY_SINK:
                continue
            at = t.get("active_turn")
            if isinstance(at, dict):
                msgs.extend(turn_msgs(at)[turn_prefix(at):])
    return msgs


def model_slices(root, model):
    """按 model 规则算出每个组的"system prompt 之后那串消息"。"""
    tasks_by_id = {}
    for t in root.get("tasks", []) or []:
        if isinstance(t, dict) and t.get("task_id"):
            tasks_by_id[t["task_id"]] = t
    out, system = {}, set()
    for g in root.get("groups", []) or []:
        if not isinstance(g, dict):
            continue
        gid = g.get("group_id", "?")
        out[gid] = sent_after_system_prompt(g, tasks_by_id, model)
        if any(tasks_by_id.get(tid, {}).get("sink") == GROUP_MEMORY_SINK
               for tid in g.get("member_task_ids", []) or []):
            system.add(gid)
    return out, system


def compare_slices(before, after, system_groups):
    """before = 旧模型（v5 数据）切片, after = 新模型（v6 数据）切片。

    返回 (报告行, 用户组是否有不等)。判据是**同一份数据**两种表示算出的请求逐条相同。"""
    lines, mismatch = [], False
    for gid, old in before.items():
        new = after.get(gid, [])
        if old == new:
            lines.append("      = group %s: 请求序列一致（%d 条）" % (gid, len(new)))
        elif gid in system_groups:
            lines.append("      ~ group %s: 系统组少 %d 条折叠 prompt（旧 loader 把折叠 prompt 混进了"
                         "历史，v6 对齐运行期规则）" % (gid, len(old) - len(new)))
        else:
            mismatch = True
            lines.append("      ✗ group %s: 请求序列不一致（旧 %d 条 / 新 %d 条）" % (gid, len(old), len(new)))
    return lines, mismatch


def migrate_tree(root):
    """就地改写一棵 session.json。返回 (折叠的 task 数, 折叠的消息数, 交叉校验警告)。"""
    ver = root.get("version")
    if ver == 6:
        return None
    if ver != 5:
        raise ValueError("不是 v5（version=%r），不动它" % ver)

    tasks = [t for t in (root.get("tasks", []) or []) if isinstance(t, dict)]
    tasks_by_id = {t.get("task_id"): t for t in tasks if t.get("task_id")}

    # 1) task 级布尔 -> turn 级前缀
    n_tasks = n_msgs = 0
    for t in tasks:
        was_folded = bool(t.pop("turns_folded", False))
        if was_folded:
            n_tasks += 1
        for turn in t.get("turns", []) or []:
            if not isinstance(turn, dict):
                continue
            n = len(turn_msgs(turn))
            turn["folded_prefix"] = n if was_folded else 0
            if was_folded:
                n_msgs += n
        at = t.get("active_turn")
        if isinstance(at, dict):
            at["folded_prefix"] = 0  # 旧模型从不把在途回合算作已折

    # 2) 组级水位 -> 删掉；顺手交叉验证它与 turns_folded 是否自洽
    warns = []
    for g in root.get("groups", []) or []:
        if not isinstance(g, dict):
            continue
        watermark = g.pop("folded_through_completion_seq", 0) or 0
        for tid in g.get("member_task_ids", []) or []:
            t = tasks_by_id.get(tid)
            if not t:
                continue
            seq = t.get("completion_seq") or 0
            # 注意：读的是**迁移前**的布尔（已经在上面 pop 掉了）—— 用前缀反推
            folded_now = any(turn_prefix(turn) == len(turn_msgs(turn)) and len(turn_msgs(turn)) > 0
                             for turn in t.get("turns", []) or [])
            if 0 < seq <= watermark and not folded_now:
                warns.append("group %s 水位 %d 覆盖 task %s(seq=%d)，但它并没有被标已折叠"
                             % (g.get("group_id", "?"), watermark, tid, seq))

    root["version"] = 6
    return (n_tasks, n_msgs, warns)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--yes", action="store_true", help="真写（默认只干跑）")
    ap.add_argument("--no-check", action="store_true", help="跳过等价性自检")
    args = ap.parse_args()

    targets = find_sessions(args.paths)
    if not targets:
        sys.stderr.write("没找到 session.json\n")
        return 1

    changed = skipped = failed = 0
    for path in targets:
        try:
            root = json.loads(path.read_text())
            # 等价性基线必须在**迁移之前**取: 旧模型的公式要读 turns_folded 与组级水位,
            # 迁移会把它们删掉。
            before, system_groups = (model_slices(root, "v5") if not args.no_check else ({}, set()))
            result = migrate_tree(root)
        except Exception as e:  # noqa: BLE001 — 迁移脚本, 报清楚就行
            sys.stderr.write("  ✗ %s: %s\n" % (path, e))
            failed += 1
            continue

        if result is None:
            print("  = %s: 已经是 v6，跳过" % path)
            skipped += 1
            continue

        n_tasks, n_msgs, warns = result
        print("  → %s: %d 个任务的 turn 打上前缀（共 %d 条已折消息），version 5 -> 6%s"
              % (path, n_tasks, n_msgs, "" if args.yes else "  [干跑]"))
        for w in warns:
            print("      ! %s" % w)

        if not args.no_check:
            after, _ = model_slices(root, "v6")
            lines, mismatch = compare_slices(before, after, system_groups)
            for ln in lines:
                print(ln)
            if mismatch:
                sys.stderr.write("  ✗ %s: 等价性自检不通过，拒绝写入\n" % path)
                failed += 1
                continue

        if not args.yes:
            changed += 1
            continue

        backup = path.with_suffix(".json.v5.bak")
        shutil.copy2(path, backup)
        path.write_text(json.dumps(root, ensure_ascii=False, indent=2) + "\n")

        # 自校验：读回来必须是 v6、没有旧键、每个 turn 都有前缀，且等价性仍然成立
        back = json.loads(path.read_text())
        assert back["version"] == 6, "自校验失败: version != 6"
        for t in back.get("tasks", []) or []:
            if not isinstance(t, dict):
                continue
            assert "turns_folded" not in t, "自校验失败: 仍有 turns_folded"
            for turn in t.get("turns", []) or []:
                assert "folded_prefix" in turn, "自校验失败: turn 缺 folded_prefix"
            if isinstance(t.get("active_turn"), dict):
                assert "folded_prefix" in t["active_turn"], "自校验失败: active_turn 缺 folded_prefix"
        for g in back.get("groups", []) or []:
            if isinstance(g, dict):
                assert "folded_through_completion_seq" not in g, "自校验失败: 仍有组级水位"
        if not args.no_check:
            # 写回的文件用同一套 v6 规则再算一遍: 必须与内存里那份逐条相同 (验写入往返)
            back_after, _ = model_slices(back, "v6")
            _, mismatch = compare_slices(after, back_after, system_groups)
            assert not mismatch, "自校验失败: 写回后等价性不成立"
        print("     自校验通过，备份: %s" % backup.name)
        changed += 1

    print("\n改写 %d，跳过 %d，失败 %d%s"
          % (changed, skipped, failed, "（干跑，加 --yes 才真写）" if not args.yes else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
