#!/usr/bin/env python3
"""Local models for docs_search.py: one llama.cpp server, started on demand.

    python scripts/docs/docs_llama.py setup [--chat]  download the pinned server and models
    python scripts/docs/docs_llama.py status          what is installed, and whether it runs
    python scripts/docs/docs_llama.py stop            stop the server

Everything lives outside the repository, in AUTANA_LLAMA_HOME (default
%LOCALAPPDATA%/autana/llama, or ~/.cache/autana/llama), shared by every
worktree. One llama-server runs in router mode on 127.0.0.1:AUTANA_LLAMA_PORT
(8765), loads a model on its first request and unloads it after ten idle
minutes, so nothing holds memory between questions. Every download is pinned to
a SHA-256 and checked before use.

Without a setup, search is lexical and nothing here runs: a missing model is a
weaker answer, never an error.
"""
import base64
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.error
import urllib.request
import zipfile
from array import array
from pathlib import Path

RELEASE = "b11188"
RELEASE_URL = "https://github.com/ggml-org/llama.cpp/releases/download/" + RELEASE + "/"
BUILDS = {
    ("Windows", "AMD64"): ("llama-b11188-bin-win-vulkan-x64.zip",
                           "e5f9d28aef5601668a769adbe1eecda6a23461246bc0e7be473ac9eb4295c9bc"),
    ("Linux", "x86_64"): ("llama-b11188-bin-ubuntu-x64.tar.gz",
                          "f6c6065c49090f76feca27207f6b9ea6f19e2f8a75c5fd8c75b9eaf3805980a2"),
    ("Darwin", "arm64"): ("llama-b11188-bin-macos-arm64.tar.gz",
                          "c4d6e517f01823913739c3c1e03d183aa52db4582b26a7680b6364b6e59fe7bf"),
    ("Darwin", "x86_64"): ("llama-b11188-bin-macos-x64.tar.gz",
                           "2fc90e0b064b8c3b5642a630f22b17ba52f0bd2937fab2bf8394319310819148"),
}
HF = "https://huggingface.co/"
MODELS = {
    "embed": {
        "file": "bge-small-en-v1.5-q8_0.gguf",
        "url": HF + "CompendiumLabs/bge-small-en-v1.5-gguf/resolve/main/bge-small-en-v1.5-q8_0.gguf",
        "sha256": "ec38e8da142596baa913124ae50550de284b6916bf59577ef2f0cb9660c2f514",
        "preset": {"embedding": "true", "pooling": "cls", "c": "2048", "np": "4",
                   "b": "512", "ub": "512"},
        "tokens": 500,
        "query_prefix": "Represent this sentence for searching relevant passages: ",
    },
    "chat": {
        "file": "Qwen3-4B-Instruct-2507-Q4_K_M.gguf",
        "url": HF + "unsloth/Qwen3-4B-Instruct-2507-GGUF/resolve/main/Qwen3-4B-Instruct-2507-Q4_K_M.gguf",
        "sha256": "3605803b982cb64aead44f6c1b2ae36e3acdb41d8e46c8a94c6533bc4c67e597",
        "preset": {"c": "8192", "np": "1", "jinja": "true"},
        "optional": True,
    },
}
IDLE_SECONDS = 600


def home():
    if os.environ.get("AUTANA_LLAMA_HOME"):
        return Path(os.environ["AUTANA_LLAMA_HOME"])
    base = os.environ.get("LOCALAPPDATA") or Path.home() / ".cache"
    return Path(base) / "autana" / "llama"


def port():
    return int(os.environ.get("AUTANA_LLAMA_PORT", "8765"))


def url(path):
    return f"http://127.0.0.1:{port()}{path}"


def server_binary():
    name = "llama-server.exe" if os.name == "nt" else "llama-server"
    found = sorted((home() / "bin" / RELEASE).rglob(name))
    return found[0] if found else None


def model_path(kind):
    return home() / "models" / MODELS[kind]["file"]


def installed(kind):
    return server_binary() is not None and model_path(kind).is_file()


def sha256(file):
    digest = hashlib.sha256()
    with open(file, "rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def download(source, target, expected):
    if target.is_file() and sha256(target) == expected:
        return
    target.parent.mkdir(parents=True, exist_ok=True)
    partial = target.with_suffix(target.suffix + ".part")
    print(f"downloading {source}", file=sys.stderr)
    with urllib.request.urlopen(source) as response, open(partial, "wb") as out:
        shutil.copyfileobj(response, out, 1 << 20)
    actual = sha256(partial)
    if actual != expected:
        partial.unlink()
        sys.exit(f"docs_llama: {target.name} has SHA-256 {actual}, expected {expected}")
    partial.replace(target)


def setup(chat=False):
    build = BUILDS.get((platform.system(), platform.machine()))
    if not build:
        sys.exit(f"docs_llama: no pinned llama.cpp build for {platform.system()} "
                 f"{platform.machine()}; put llama-server under {home() / 'bin'}")
    if server_binary() is None:
        archive = home() / build[0]
        download(RELEASE_URL + build[0], archive, build[1])
        target = home() / "bin" / RELEASE
        if archive.suffix == ".zip":
            with zipfile.ZipFile(archive) as bundle:
                bundle.extractall(target)
        else:
            with tarfile.open(archive) as bundle:
                bundle.extractall(target, filter="data")
        for file in target.rglob("llama-server"):
            file.chmod(0o755)
        archive.unlink()
    for kind, model in MODELS.items():
        if model.get("optional") and not chat:
            continue
        download(model["url"], model_path(kind), model["sha256"])
    write_preset()
    print(f"installed in {home()}; embedding the documentation once")
    import docs_search
    index = docs_search.Index(docs_search.repo_root())
    index.semantic("warm up")
    print(f"{len(index.vectors)} passages embedded")


def write_preset():
    lines = ["version = 1", "", "[*]", "ngl = 99", ""]
    for kind, model in MODELS.items():
        if model_path(kind).is_file():
            lines.append(f"[{kind}]")
            lines.append(f"model = {model_path(kind).as_posix()}")
            lines += [f"{key} = {value}" for key, value in model["preset"].items()]
            lines.append("")
    preset = home() / "models.ini"
    preset.write_text("\n".join(lines), encoding="utf-8")
    return preset


def request(path, body=None, timeout=120):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(url(path), data, {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as response:
        return json.load(response)


def running():
    try:
        return request("/health", timeout=2).get("status") == "ok"
    except (OSError, ValueError):
        return False


def start():
    """The router, detached so it outlives this process; ten idle minutes unload its models."""
    if running():
        return True
    binary = server_binary()
    if binary is None:
        return False
    log = open(home() / "server.log", "ab")
    flags = {"creationflags": subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP} \
        if os.name == "nt" else {"start_new_session": True}
    subprocess.Popen([str(binary), "--models-preset", str(write_preset()), "--host", "127.0.0.1",
                      "--port", str(port()), "--sleep-idle-seconds", str(IDLE_SECONDS),
                      "--models-max", "3"],
                     stdout=log, stderr=log, stdin=subprocess.DEVNULL, **flags)
    deadline = time.time() + 30
    while time.time() < deadline:
        if running():
            return True
        time.sleep(0.2)
    return False


def stop():
    if not running():
        return False
    if os.name == "nt":
        subprocess.run(["taskkill", "/F", "/IM", "llama-server.exe"], capture_output=True)
    else:
        subprocess.run(["pkill", "-f", "llama-server.*--models-preset"], capture_output=True)
    return True


def fit(kind, text):
    """The text cut to what the model reads: a longer input is refused, not truncated."""
    limit = MODELS[kind]["tokens"]
    tokens = request(f"/tokenize?model={kind}", {"model": kind, "content": text})["tokens"]
    if len(tokens) <= limit:
        return text
    return request(f"/detokenize?model={kind}", {"model": kind, "tokens": tokens[:limit]})["content"]


def embed(texts, query=False):
    prefix = MODELS["embed"]["query_prefix"] if query else ""
    texts = [fit("embed", prefix + text) for text in texts]
    vectors = []
    for first in range(0, len(texts), 32):
        reply = request("/v1/embeddings", {"model": "embed", "input": texts[first:first + 32]},
                        timeout=600)
        vectors += [array("f", row["embedding"])
                    for row in sorted(reply["data"], key=lambda row: row["index"])]
    return vectors


def chat(messages, max_tokens=400, stream=None):
    """The reply to messages; with stream, each piece is also passed to it as it arrives."""
    body = {"model": "chat", "messages": messages, "max_tokens": max_tokens,
            "temperature": 0.2, "stream": stream is not None,
            "chat_template_kwargs": {"enable_thinking": False}}
    req = urllib.request.Request(url("/v1/chat/completions"), json.dumps(body).encode(),
                                 {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=600) as response:
        if stream is None:
            return json.load(response)["choices"][0]["message"]["content"]
        pieces = []
        for line in response:
            line = line.decode("utf-8").strip()
            if not line.startswith("data: ") or line == "data: [DONE]":
                continue
            piece = json.loads(line[6:])["choices"][0]["delta"].get("content") or ""
            pieces.append(piece)
            stream(piece)
        return "".join(pieces)


class VectorCache:
    """Vectors keyed by a hash of the model and the text, so an edit re-embeds only itself.

    One cache serves every worktree, so a vector this run did not ask for may be
    another branch's; unused ones are dropped only once they outnumber the used
    two to one.
    """

    def __init__(self, kind="embed"):
        self.model = MODELS[kind]["file"]
        self.file = home() / "cache" / (Path(self.model).stem + ".json")
        try:
            self.rows = json.loads(self.file.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            self.rows = {}
        self.used = set()
        self.dirty = False

    def key(self, text):
        return hashlib.sha1((self.model + "\0" + text).encode("utf-8")).hexdigest()

    def vectors(self, texts):
        keys = [self.key(text) for text in texts]
        self.used.update(keys)
        missing = sorted({k: t for k, t in zip(keys, texts) if k not in self.rows}.items())
        if missing:
            print(f"embedding {len(missing)} new sections", file=sys.stderr)
            for (k, _), vector in zip(missing, embed([t for _, t in missing])):
                self.rows[k] = base64.b64encode(vector.tobytes()).decode("ascii")
            self.dirty = True
        return [array("f", base64.b64decode(self.rows[k])) for k in keys]

    def save(self):
        if len(self.rows) > 3 * len(self.used):
            self.rows = {k: v for k, v in self.rows.items() if k in self.used}
            self.dirty = True
        if not self.dirty:
            return
        self.file.parent.mkdir(parents=True, exist_ok=True)
        partial = self.file.with_suffix(".part")
        partial.write_text(json.dumps(self.rows), encoding="utf-8")
        partial.replace(self.file)
        self.dirty = False


def status():
    print(f"home     {home()}")
    print(f"server   {server_binary() or 'not installed'}")
    for kind in MODELS:
        print(f"{kind:8} {'installed' if model_path(kind).is_file() else 'not installed'}"
              f"  {MODELS[kind]['file']}")
    print(f"running  {'yes, port ' + str(port()) if running() else 'no'}")


def main(argv):
    command = argv[0] if argv else "status"
    if command == "setup":
        setup(chat="--chat" in argv)
    elif command == "stop":
        print("stopped" if stop() else "not running")
    elif command == "status":
        status()
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
