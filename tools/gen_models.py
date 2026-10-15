#!/usr/bin/env python3
"""Fetch, test, and generate the checked-in AI-model catalog -- one command.

models.dev (https://models.dev, `sst/models.dev`) is a community-contributed open
database of AI model specs; this repo has no other maintainable source for the model
list. This script is the whole pipeline:

    1. FETCH   models.dev api.json + models.json
    2. TEST    every candidate provider's base URL, keyless (see endpoint_check)
    3. DROP    providers that fail the test (or that the curated list excludes)
    4. GENERATE models_catalog.json (deterministic; checked in, later embedded)

Run `tools/gen_models.py` to update the file; `--check` to fail CI when it is stale;
`--probe` to only print the per-provider test; `--smoke` to do a real keyed call
(needs the provider's env key, e.g. OPENAI_API_KEY).

Shape (models.dev's own split, so metadata is stored ONCE):
    { "source": ..., "generated_at": ...,
      "models":    { "<id>": {name, context, output, tool_call, reasoning,
                              attachment, modalities} },
      "providers": { "<pid>": {id, name, api, env, npm, doc, models: ["<id>", ...]} } }
Canonical model metadata comes from models.json; a model a provider exposes but the
canonical table lacks is filled from that provider's inline entry.

Candidates: DEFAULT_PROVIDERS (the set the product offers; edit to change it). The full
models.dev catalog is 205 providers / ~1.4 MB of mostly obscure aggregators, not worth
checking in. `--all` uses everyone, `--provider X` adds one. Several big providers
publish no `api` (they are SDK-native), so API_OVERRIDES supplies their OpenAI-compatible
base URL.
"""

import argparse
import datetime
import json
import sys
import urllib.error
import urllib.request

API_URL = "https://models.dev/api.json"
MODELS_URL = "https://models.dev/models.json"
DEFAULT_OUT = "models_catalog.json"

# The providers the product offers. Edit to change the wizard's list. The pipeline still
# tests each one and drops the ones that fail (see endpoint_check).
DEFAULT_PROVIDERS = [
    # SDK-native on models.dev (no `api`); base URL from API_OVERRIDES.
    "anthropic",
    "openai",
    "google",
    "groq",
    "mistral",
    "xai",
    "cohere",
    "perplexity",
    "cerebras",
    "deepinfra",
    "togetherai",
    "vercel",
    # OpenAI-compatible on models.dev (`api` is the real base URL).
    "deepseek",
    "openrouter",
    "fireworks-ai",
    "novita-ai",
    "siliconflow",
    "moonshotai",
    "zhipuai",
    "sambanova",
    "github-copilot",
    "huggingface",
    "baseten",
    "upstage",
    "ai21",
    "ollama-cloud",
    "lmstudio",
    "alibaba",
    # Omitted on purpose:
    #  - minimax     : models.dev gives an anthropic-protocol URL (…/anthropic/v1).
    #  - nvidia      : /v1/models is public, but /v1/chat/completions answers a plain
    #                  "404 page not found" (not OpenAI-shaped) => path unproven; its
    #                  model list also disagrees with the live one. Re-add after a keyed
    #                  `--smoke` proves it.
    #  - requesty    : models.dev's model ids share ZERO ids with its live /models list
    #                  => unusable catalog data.
]

# models.dev omits `api` for SDK-native providers (they ship an SDK-specific package);
# there is NO machine-readable source for their base URL, so this is a hand-verified
# table of each provider's documented OpenAI-compatible endpoint. `--probe` checks every
# one is reachable and OpenAI-shaped, so a typo or a dead host is caught, not guessed at.
# Edit an entry only against the vendor's own docs (then re-run --probe).
API_OVERRIDES = {
    "openai": "https://api.openai.com/v1",
    "anthropic": "https://api.anthropic.com/v1",
    "google": "https://generativelanguage.googleapis.com/v1beta/openai",
    "groq": "https://api.groq.com/openai/v1",
    "mistral": "https://api.mistral.ai/v1",
    "xai": "https://api.x.ai/v1",
    "cohere": "https://api.cohere.ai/compatibility/v1",
    "perplexity": "https://api.perplexity.ai",
    "cerebras": "https://api.cerebras.ai/v1",
    "deepinfra": "https://api.deepinfra.com/v1/openai",
    "togetherai": "https://api.together.xyz/v1",
    "vercel": "https://ai-gateway.vercel.sh/v1",
}

# models.dev's `api` is the base URL of whichever SDK a provider uses (`npm` says which),
# NOT necessarily OpenAI-compatible: e.g. minimax is `@ai-sdk/anthropic` and its `api`
# points at `.../anthropic/v1`. So `api` is trusted only for these npm families; every
# other provider needs a verified API_OVERRIDES entry.
OPENAI_COMPAT_NPM = {
    "@ai-sdk/openai-compatible",
    "@openrouter/ai-sdk-provider",
}


def fetch(url):
    # models.dev answers 403 without a User-Agent (urllib sends none by default).
    req = urllib.request.Request(url, headers={"User-Agent": "edgeclaw-gen-models/1", "Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=60) as resp:
        return json.load(resp)


def openai_base_url(pid, p):
    """The provider's OpenAI-compatible base URL, or None if we have none verifiable."""
    if pid in API_OVERRIDES:
        return API_OVERRIDES[pid]
    if p.get("npm") in OPENAI_COMPAT_NPM:
        return p.get("api")
    return None


def keep_model(m):
    """Trim one models.dev model to the fields we consume."""
    out = {"name": m.get("name")}
    limit = m.get("limit") or {}
    if isinstance(limit, dict):
        out["context"] = limit.get("context")
        out["output"] = limit.get("output")
    for key in ("tool_call", "reasoning", "attachment"):
        if key in m:
            out[key] = bool(m[key])
    mod = m.get("modalities")
    if isinstance(mod, dict):
        out["modalities"] = {"input": mod.get("input") or [], "output": mod.get("output") or []}
    return out


def build_catalog(api, canonical, wanted):
    """Deterministic catalog: a shared `models` table + providers referencing ids."""
    providers = {}
    models = {}
    missing_api = []
    for pid in sorted(api.keys()):
        if wanted is not None and pid not in wanted:
            continue
        p = api[pid]
        url = openai_base_url(pid, p)
        if not url:
            missing_api.append(pid)
            continue
        ids = []
        for mid in sorted((p.get("models") or {}).keys()):
            ids.append(mid)
            if mid not in models:
                models[mid] = keep_model(canonical[mid] if mid in canonical else p["models"][mid])
        providers[pid] = {
            "id": p.get("id", pid),
            "name": p.get("name", pid),
            "api": url,
            "env": p.get("env") or [],
            "npm": p.get("npm"),
            "doc": p.get("doc"),
            "models": ids,
        }
    return providers, models, missing_api


def prune_models(providers, models):
    """Drop canonical models no kept provider references."""
    referenced = set()
    for p in providers.values():
        referenced.update(p["models"])
    return {mid: m for mid, m in models.items() if mid in referenced}


def render(providers, models, source, generated_at):
    # Fixed key order + sorted providers/models => stable diffs and a stable --check.
    doc = {"source": source, "generated_at": generated_at, "models": models, "providers": providers}
    return json.dumps(doc, indent=2, ensure_ascii=False) + "\n"


# ── endpoint testing (keyless) ───────────────────────────────────────────────

def _http(url, method="GET", body=None, headers=None, timeout=15, limit=300):
    """(status_or_None, first_bytes up to `limit`). None + bytes(msg) on transport error."""
    req = urllib.request.Request(url, method=method, data=body, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, resp.read(limit)
    except urllib.error.HTTPError as e:
        return e.code, e.read(limit)
    except Exception as e:  # DNS / TCP / TLS / timeout
        return None, str(e).encode()


def _is_local(base):
    return "127.0.0.1" in base or "localhost" in base or "0.0.0.0" in base


def _openai_shaped(body):
    """An OpenAI-compatible endpoint answers errors with {"error": {...}}."""
    if b'"error"' in body:
        return True
    try:
        return isinstance(json.loads(body).get("error"), (dict, str))
    except Exception:
        return False


# A no-key POST here: an OpenAI-shaped error proves host AND path are right.
_PROBE_BODY = b'{"model":"probe","messages":[{"role":"user","content":"ping"}],"max_tokens":1}'


def endpoint_check(pid, base, catalog_ids, timeout=15):
    """Classify a provider's base URL without a key. Returns (verdict, note):

      ok    -- reachable and an OpenAI-compatible endpoint (proven, or plausible when the
               gateway just answers a non-JSON body to an unauthenticated request)
      local -- a localhost server (cannot be checked from here); kept
      drop  -- unreachable, or /chat/completions is not there, or a PUBLIC /models list
               shares no id with our catalog (models.dev's data is unusable for it)

    Evidence used: GET {api}/models (a 200 whose body is {"data":[{"id":..}]} is strongest)
    and POST {api}/chat/completions (401/403 => right host+path, wants a key; a 404 with
    an OpenAI-shaped "model not found" body still means the path is right).
    """
    if _is_local(base):
        return "local", "local (not probed)"
    gst, gbody = _http(base + "/models", timeout=timeout, limit=2_000_000)
    if gst is None:
        return "drop", "UNREACHABLE"
    notes = []
    public_ids = None
    if gst == 200:
        try:
            public_ids = {m.get("id") for m in json.loads(gbody).get("data", []) if isinstance(m, dict)}
            notes.append(f"/models=200 openai-list n={len(public_ids)}")
        except Exception:
            notes.append("/models=200 (not an OpenAI list)")
    else:
        notes.append(f"/models={gst}" + (" openai-shaped" if _openai_shaped(gbody) else ""))

    pst, pbody = _http(base + "/chat/completions", method="POST", body=_PROBE_BODY,
                       headers={"Content-Type": "application/json"}, timeout=timeout, limit=2000)
    if pst in (400, 401, 403):
        notes.append(f"chat={pst} " + ("openai-shaped (path OK)" if _openai_shaped(pbody) else "(non-openai body)"))
    elif pst == 404 and _openai_shaped(pbody):
        notes.append("chat=404 model-not-found (path OK)")
    elif pst == 404:
        return "drop", "; ".join(notes + ["chat=404 (path wrong, non-openai body)"])
    elif pst is None:
        return "drop", "; ".join(notes + ["chat UNREACHABLE"])
    else:
        notes.append(f"chat={pst}")

    if public_ids is not None and catalog_ids:
        overlap = len(set(catalog_ids) & public_ids)
        notes.append(f"catalog={len(catalog_ids)} overlap={overlap}")
        if overlap == 0:
            return "drop", "; ".join(notes + ["model ids share nothing with the live list"])
    return "ok", "; ".join(notes)


def test_providers(providers, timeout=15):
    """Run endpoint_check on each provider; returns {pid: (verdict, note)}."""
    return {pid: endpoint_check(pid, providers[pid].get("api") or "", providers[pid].get("models") or [], timeout)
            for pid in sorted(providers.keys())}


def probe(providers, timeout=15):
    """Print the per-provider test table. Returns the ids that would be dropped."""
    results = test_providers(providers, timeout)
    drop = []
    for pid in sorted(results):
        verdict, note = results[pid]
        base = providers[pid].get("api") or ""
        print(f"  {pid:16} {base:52} [{verdict:5}] {note}")
        if verdict == "drop":
            drop.append(pid)
    return drop


def smoke(providers, timeout=30):
    """Real call with each provider's env key (if set). Returns the ids that FAILED."""
    import os
    failed = []
    for pid in sorted(providers.keys()):
        p = providers[pid]
        envs = p.get("env") or []
        key = next((os.environ[e] for e in envs if os.environ.get(e)), None)
        if not key:
            print(f"  {pid:16} no key (export one of: {', '.join(envs) or '-'})")
            continue
        base = (p.get("api") or "").rstrip("/")
        model = (p.get("models") or ["?"])[0]
        body = json.dumps({"model": model, "messages": [{"role": "user", "content": "ping"}],
                           "max_tokens": 1}).encode()
        st, resp = _http(base + "/chat/completions", method="POST", body=body,
                         headers={"Content-Type": "application/json", "Authorization": "Bearer " + key},
                         timeout=timeout)
        if st == 200:
            print(f"  {pid:16} OK  ({model})")
        elif st in (400, 404):
            print(f"  {pid:16} endpoint+auth OK, model '{model}' rejected ({st})")
        else:
            print(f"  {pid:16} FAIL ({st}): {resp.decode('utf-8', 'replace')[:80].strip()}")
            failed.append(pid)
    return failed


# ── main ─────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description="Fetch, test, and generate models_catalog.json")
    ap.add_argument("--api-url", default=API_URL, help=f"models.dev api.json (default {API_URL})")
    ap.add_argument("--models-url", default=MODELS_URL, help=f"models.dev models.json (default {MODELS_URL})")
    ap.add_argument("--from-api", help="read a local api.json instead of fetching")
    ap.add_argument("--from-models", help="read a local models.json instead of fetching")
    ap.add_argument("--out", default=DEFAULT_OUT, help=f"output path (default {DEFAULT_OUT})")
    ap.add_argument("--provider", action="append", help="add a provider id to the default list (repeatable)")
    ap.add_argument("--all", action="store_true", help="every provider models.dev knows (big)")
    ap.add_argument("--keep-all", action="store_true", help="do not drop providers that fail the endpoint test")
    ap.add_argument("--check", action="store_true", help="exit 1 if --out is stale (does not write)")
    ap.add_argument("--probe", action="store_true", help="only print the per-provider endpoint test")
    ap.add_argument("--smoke", action="store_true", help="real 1-token call using each provider's env key")
    args = ap.parse_args()

    api = json.load(open(args.from_api, encoding="utf-8")) if args.from_api else fetch(args.api_url)
    canonical = json.load(open(args.from_models, encoding="utf-8")) if args.from_models else fetch(args.models_url)

    if args.all:
        wanted = set(api.keys())
    else:
        wanted = set(DEFAULT_PROVIDERS) | set(args.provider or [])
        unknown = sorted(wanted - set(api.keys()))
        if unknown:
            print(f"warning: no such provider(s) on models.dev: {', '.join(unknown)}", file=sys.stderr)

    providers, models, missing_api = build_catalog(api, canonical, wanted)
    if missing_api:
        print(f"warning: no OpenAI base URL for {', '.join(missing_api)} (skipped)", file=sys.stderr)

    if args.smoke:
        bad = smoke(providers)
        if bad:
            print(f"SMOKE FAILED for: {', '.join(bad)}", file=sys.stderr)
            return 1
        return 0

    if args.probe:
        drop = probe(providers)
        if drop:
            print(f"would drop (endpoint test): {', '.join(drop)}", file=sys.stderr)
            return 1
        print(f"all {len(providers)} providers passed the endpoint test.")
        return 0

    # TEST + DROP (the update pipeline). --keep-all skips the drop but still reports.
    results = test_providers(providers)
    drop = [pid for pid in results if results[pid][0] == "drop"]
    if not args.keep_all:
        for pid in drop:
            print(f"drop {pid}: {results[pid][1]}", file=sys.stderr)
            del providers[pid]
        models = prune_models(providers, models)

    source = args.from_api or args.api_url

    if args.check:
        try:
            on_disk = json.load(open(args.out, encoding="utf-8"))
        except FileNotFoundError:
            on_disk = None
        fresh = render(providers, models, source, "")
        current = render(on_disk["providers"], on_disk["models"], on_disk.get("source", ""), "") if on_disk else None
        if current != fresh:
            have = len(on_disk["providers"]) if on_disk else 0
            print(f"{args.out} is STALE: on disk {have} providers, freshly generated {len(providers)}. "
                  f"Re-run tools/gen_models.py.", file=sys.stderr)
            return 1
        print(f"{args.out} is up to date ({len(providers)} providers, {len(models)} models).")
        return 0

    generated_at = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(render(providers, models, source, generated_at))
    tail = f" (dropped {len(drop)}: {', '.join(drop)})" if drop and not args.keep_all else ""
    print(f"wrote {args.out}: {len(providers)} providers, {len(models)} models{tail}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
