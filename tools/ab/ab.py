#!/usr/bin/env python3
"""A/B 对照驱动 —— 见 README.md。

    ab.py run      --bin BIN --label LBL --fixture smoke
    ab.py compare  --a DIR --b DIR
    ab.py selftest --bin BIN --fixture smoke

夹具从一份真实工作区**冻结**出来，两个二进制跑**同一份**数据（顺序执行，
HOME 路径固定，否则 config 里的沙箱路径会渗进 system prompt 破坏可比性）。
"""
import argparse
import fcntl
import json
import os
import pty
import re
import shutil
import signal
import struct
import subprocess
import sys
import termios
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
RUN_BASE = Path("/tmp/ab/run")
OUT_BASE = Path("/tmp/ab/out")
PORT = 8443
ROWS, COLS = 40, 100  # 固定终端尺寸：渲染确定性的前提

# edc_secret.c:25 / :193 "Priority 1: env var (scripts / Docker)"
# 设了它就跳过口令提示，且不需要 .key_hash → 夹具可全自动运行。
MASTER_KEY = "ab-fixture-master-key"

# 非确定性来源：写入时刻。会话状态、system prompt 里的 time 都是这个形状。
TS_RE = re.compile(r"\d{4}-\d{2}-\d{2}[ T]\d{2}:\d{2}:\d{2}")

BOOTSTRAP = ["AGENTS.md", "SOUL.md", "TOOLS.md", "USER.md", "HEARTBEAT.md"]


# 行号：往"会打日志的文件"里加一行，日志里的 `file.c:NNN` 就会位移 —— 那是
# 编辑的副作用，不是行为。不归一化的话，每次改代码 A/B 都会假红
# （实测：往 edc_https_client.c 里加 55 行 → cacert 那行 724 变 779）。
LINE_RE = re.compile(r"(\.(?:c|h|cpp|hpp):)\d+")


def mask_ts(s: str) -> str:
    return LINE_RE.sub(r"\1<LINE>", TS_RE.sub("<TS>", s))


# ---------------------------------------------------------------- fixture


def session_workdirs(ws: Path, sessions):
    """会话的工作目录在 `<session>/session.json` 的 `session.workdir`（绝对路径）。
    必须加进沙箱 rw_paths，否则工具会被拒（拒绝也要确定性地发生）。"""
    out = []
    for s in sessions:
        p = ws / s / "session.json"
        if not p.exists():
            continue
        try:
            d = json.loads(p.read_text())
        except Exception:
            continue
        wd = (d.get("session") or {}).get("workdir")
        if wd:
            out.append(wd)
    return out


def gen_config(ecl: Path, extra_rw=()):
    ws = ecl / "workspace"
    cfg = {
        "max_iter": -1,
        "memory_compress_threshold": 200,
        "group_fold_msg_threshold": 200,
        "tool_max_output_size": 10000,
        "sandbox": {
            "rw_paths": [str(ws) + "/", "/tmp", "/dev"] + list(extra_rw),
            "ro_paths": ["/usr", "/lib", "/lib64", "/bin", "/sbin", "/etc",
                         "/proc", "/sys", "/run"],
            "deny_paths": [],
            "enable": True,
        },
        "channel": {"type": "adapter"},
        "device_transport": "socket",
        "device_ip": "127.0.0.1",
        "device_port": 5000,
        "device_baud": 115200,
        "device_pty": "",
        "api_url": "https://127.0.0.1:%d/chat/completions" % PORT,
        "model": "fake",
        # 明文（无 EDGECLAW_ENC: 前缀）→ 不需要口令，可全自动
        "api_key": "sk-ab-fixture",
    }
    (ecl / "config.json").write_text(json.dumps(cfg, indent="\t"))


def freeze(home: Path, source: Path, sessions, refreeze=False):
    """把一份真实工作区冻结成夹具。大文件/备份不入夹具。

    **冻结一次、之后复用**：源工作区是活的（.task_seq 等高水位会变），
    每次重新冻结会让两次运行从不同的起点出发 —— 那样 A/B 的差异里就混进了
    夹具自身的漂移。`--refreeze` 才重建。"""
    if home.exists() and not refreeze:
        return
    if home.exists():
        shutil.rmtree(home)
    ecl = home / ".edgeclaw"
    ws = ecl / "workspace"
    ws.mkdir(parents=True)

    for name in BOOTSTRAP:
        if (source / name).exists():
            shutil.copy2(source / name, ws / name)
    for d in ["memory", "skills"]:
        if (source / d).is_dir():
            shutil.copytree(source / d, ws / d)
    for s in sessions:
        shutil.copytree(source / s, ws / s,
                        ignore=shutil.ignore_patterns("*.bak", "*.broken"))
    for f in [".session_seq", ".task_seq"]:
        if (source / f).exists():
            shutil.copy2(source / f, ws / f)

    idx = json.loads((source / "sessions.json").read_text())
    idx["sessions"] = [s for s in idx["sessions"] if s["id"] in sessions]
    idx["active_session_id"] = sessions[0]
    (ws / "sessions.json").write_text(json.dumps(idx, indent="\t"))
    gen_config(ecl, session_workdirs(ws, sessions))
    return ecl


# ------------------------------------------------------------------ pty


class Screen(threading.Thread):
    """把 pty 的输出灌进文件（不解析，只留证）。"""

    def __init__(self, master, path):
        super().__init__(daemon=True)
        self.master, self.path, self.done = master, path, False

    def run(self):
        with open(self.path, "wb") as f:
            while not self.done:
                try:
                    data = os.read(self.master, 65536)
                except OSError:
                    break
                if not data:
                    break
                f.write(data)
                f.flush()


def run_script(master, script: Path):
    for raw in script.read_text().splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        op, _, arg = line.partition(" ")
        if op == "wait":
            time.sleep(int(arg) / 1000.0)
        elif op == "send":
            data = arg.encode().decode("unicode_escape").encode("latin-1")
            os.write(master, data)
        elif op == "resize":
            r, _, c = arg.partition("x")
            fcntl.ioctl(master, termios.TIOCSWINSZ,
                        struct.pack("HHHH", int(r), int(c), 0, 0))
            os.killpg(os.getpgid(PID[0]), signal.SIGWINCH)
        else:
            raise SystemExit("ab.py: 未知指令 %r" % op)


PID = [0]


def _preexec():
    os.setsid()
    fcntl.ioctl(0, termios.TIOCSCTTY, 0)  # 让 pty 成为控制终端


def run_binary(binary: Path, home: Path, script: Path, out: Path, fake: Path,
               responses: Path, extra_env=None):
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env.update(HOME=str(home), TERM="xterm-256color", COLORTERM="truecolor",
               LANG="C.UTF-8", LC_ALL="C.UTF-8",
               EDGECLAW_MASTER_KEY=MASTER_KEY)
    env.pop("EDGECLAW_ENC", None)
    if extra_env:
        env.update(extra_env)

    llm = subprocess.Popen(
        [sys.executable, str(HERE / "fake_llm.py"), "--port", str(PORT),
         "--responses", str(responses),
         "--request-log", str(out / "llm_requests.jsonl")],
        stderr=subprocess.PIPE)
    time.sleep(1.2)  # 等握手
    if llm.poll() is not None:
        sys.exit("ab.py: fake_llm 起不来\n" + llm.stderr.read().decode())

    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLS, 0, 0))
    p = subprocess.Popen([str(binary)], stdin=slave, stdout=slave,
                         stderr=slave, env=env, cwd=str(home),
                         preexec_fn=_preexec, close_fds=True)
    os.close(slave)
    PID[0] = p.pid
    screen = Screen(master, out / "screen.bin")
    screen.start()
    try:
        run_script(master, script)
        time.sleep(1.5)  # settle
        os.killpg(os.getpgid(p.pid), signal.SIGINT)  # 优雅退出（会落盘）
        try:
            p.wait(timeout=20)
        except subprocess.TimeoutExpired:
            os.killpg(os.getpgid(p.pid), signal.SIGKILL)
            p.wait(timeout=10)
            sys.stderr.write("ab.py: 警告 —— 进程未在 20s 内优雅退出，已强杀\n")
    finally:
        time.sleep(0.3)
        screen.done = True
        try:
            os.close(master)
        except OSError:
            pass
        llm.terminate()
        llm.wait(timeout=5)

    collect(home, out)

    # 防"空转通过"：夹具备了响应却一个 LLM 请求都没发生 → 回路没被覆盖，
    # 此时 compare 一致是没有意义的。
    req_log = out / "llm_requests.jsonl"
    nreq = sum(1 for _ in open(req_log)) if req_log.exists() else 0
    if nreq == 0:
        sys.stderr.write("ab.py: 错误 —— 没有任何 LLM 请求，LLM 回路未被覆盖；"
                         "compare 的结论无效\n")
        return 3
    return p.returncode


# ------------------------------------------------------------- artifacts


def collect(home: Path, out: Path):
    art = out / "artifacts"
    if art.exists():
        shutil.rmtree(art)
    ws = home / ".edgeclaw" / "workspace"
    art.mkdir(parents=True)
    for p in sorted(ws.rglob("*")):
        if p.is_file():
            rel = p.relative_to(ws)
            dst = art / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(p, dst)
    logdir = home / ".edgeclaw" / "log"
    if logdir.is_dir():
        shutil.copytree(logdir, art / "_log", dirs_exist_ok=True)


# ------------------------------------------------------------- compare


def canon(path: Path):
    """归一化：掩掉时刻；JSON 再规范化（键序无关）。"""
    raw = path.read_bytes()
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        return raw  # 二进制（screen.bin）逐字节
    if path.suffix == ".json":
        try:
            obj = json.loads(text)
        except Exception:
            return mask_ts(text).encode()
        return json.dumps(_mask_obj(obj), sort_keys=True,
                          ensure_ascii=False, indent=1).encode()
    return mask_ts(text).encode()


def _mask_obj(o):
    if isinstance(o, str):
        return mask_ts(o)
    if isinstance(o, list):
        return [_mask_obj(x) for x in o]
    if isinstance(o, dict):
        return {k: _mask_obj(v) for k, v in o.items()}
    return o


def compare(a: Path, b: Path) -> int:
    # 帧不比：TUI 有动画，且 ftxui 只重绘 diff（README §为什么第一版不比终端帧）
    ignore = {"screen.bin"}

    def files(root):
        return sorted(p.relative_to(root) for p in root.rglob("*")
                      if p.is_file() and p.name not in ignore)

    fa, fb = set(files(a)), set(files(b))
    ok = True
    for rel in sorted(fa | fb):
        if rel not in fa:
            print("  + 只在 B: %s" % rel); ok = False; continue
        if rel not in fb:
            print("  - 只在 A: %s" % rel); ok = False; continue
        ca, cb = canon(a / rel), canon(b / rel)
        if ca != cb:
            ok = False
            print("  ! 不同: %s  (A %d B %d 字节)"
                  % (rel, len(ca), len(cb)))
            if rel.suffix in (".json", ".jsonl") or rel.suffix == "":
                import difflib
                la = ca.decode("utf-8", "replace").splitlines()
                lb = cb.decode("utf-8", "replace").splitlines()
                for d in list(difflib.unified_diff(la, lb, "A", "B", lineterm=""))[:40]:
                    print("      " + d)
    print("compare: %s" % ("一致" if ok else "有差异"))
    return 0 if ok else 1


# ------------------------------------------------------------------ main


def cmd_run(args):
    sess = args.sessions.split(",")
    src = Path(args.source).expanduser()
    pristine = RUN_BASE / args.fixture / "pristine"  # 冻结的**原始**夹具
    home = RUN_BASE / args.fixture / "home"          # 每次运行的工作副本
    out = Path(args.out) if args.out else OUT_BASE / args.label

    freeze(pristine, src, sess, refreeze=args.refreeze)
    if home.exists():
        shutil.rmtree(home)
    shutil.copytree(pristine, home, symlinks=True)

    # 启动应用**之前**的布置（在 home 里跑）。用途: 造那些 copytree 带不进来的
    # 东西 —— 比如一个 FIFO（就地重建的夹具里放不住）。
    if args.pre:
        import subprocess
        r = subprocess.run(args.pre, shell=True, cwd=home, capture_output=True, text=True)
        if r.returncode != 0:
            sys.stderr.write("--pre 失败(%s): %s\n" % (r.returncode, r.stderr))
            return 1

    rc = run_binary(Path(args.bin), home, HERE / "scripts" / (args.fixture + ".txt"),
                    out, home, HERE / "responses" / (args.fixture + ".json"),
                    extra_env=dict(kv.split("=", 1) for kv in (args.env or [])))
    print("run %s: 退出码 %s，产物在 %s" % (args.label, rc, out))
    return 0


def cmd_compare(args):
    return compare(Path(args.a), Path(args.b))


def cmd_selftest(args):
    args.sessions = args.sessions
    for lbl in ("self1", "self2"):
        args.label, args.out = lbl, str(OUT_BASE / lbl)
        cmd_run(args)
    print("selftest: 同一二进制两次运行的对照")
    return compare(OUT_BASE / "self1", OUT_BASE / "self2")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p):
        p.add_argument("--bin", required=True)
        p.add_argument("--fixture", default="smoke")
        p.add_argument("--source", default="~/.edgeclaw/workspace")
        p.add_argument("--sessions", default="s8")
        p.add_argument("--refreeze", action="store_true",
                       help="重建冻结夹具（默认复用第一次冻结的结果）")
        p.add_argument("--env", action="append", metavar="K=V",
                       help="额外传一传环境变量给**被测二进制**（可重复）")

    p = sub.add_parser("run"); common(p)
    p.add_argument("--pre", default=None,
                   help="启动应用之前在 home 里跑的 shell（布置夹具带不进来的东西，如 FIFO）")
    p.add_argument("--label", required=True)
    p.add_argument("--out", default=None)
    p.set_defaults(func=cmd_run)

    p = sub.add_parser("compare")
    p.add_argument("--a", required=True)
    p.add_argument("--b", required=True)
    p.set_defaults(func=cmd_compare)

    p = sub.add_parser("selftest"); common(p)
    p.set_defaults(func=cmd_selftest)

    args = ap.parse_args()
    sys.exit(args.func(args))


if __name__ == "__main__":
    main()
