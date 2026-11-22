#!/usr/bin/env python3
"""
DocToText Side-by-Side Document Inspector Server.

Zero-dependency Python 3 HTTP server providing:
- Local directory & file browsing (+ browser folder/file upload fallback)
- Left-pane rich preview (native PDF plugin, LibreOffice -> PDF conversion,
  pdftoppm page rasterization, images, sandboxed HTML, plain text / EML)
- Right-pane DocToText extraction (CLI, --compact-whitespace, --meta,
  PP-OCRv6 C API extractor, and optional Apache Tika comparison)
"""

import argparse
import difflib
import hashlib
import json
import mimetypes
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent

LO_LOCK = threading.Lock()
PAGE_LOCK = threading.Lock()

DOCUMENT_EXTENSIONS = {
    ".pdf", ".doc", ".docx", ".docm", ".dotx",
    ".xls", ".xlsx", ".xlsm", ".xlsb",
    ".ppt", ".pptx", ".pptm",
    ".odt", ".ods", ".odp", ".odg", ".fodt", ".fods", ".fodp", ".fodg",
    ".rtf", ".html", ".htm", ".xml", ".eml", ".txt", ".md", ".csv", ".json",
    ".pages", ".numbers", ".key",
    ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp", ".svg", ".tif", ".tiff",
}

NATIVE_IMAGE_EXTS = {".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp", ".svg"}
OFFICE_CONVERTIBLE_EXTS = {
    ".doc", ".docx", ".docm", ".dotx",
    ".xls", ".xlsx", ".xlsm", ".xlsb",
    ".ppt", ".pptx", ".pptm",
    ".odt", ".ods", ".odp", ".odg", ".fodt", ".fods", ".fodp", ".fodg",
    ".rtf", ".csv", ".pages", ".numbers", ".key",
    ".tif", ".tiff",
}
HTML_EXTS = {".html", ".htm"}
TEXT_EXTS = {".txt", ".md", ".log", ".eml", ".xml", ".json", ".csv"}


def get_cache_root() -> Path:
    raw = os.environ.get("DOCTOTEXT_CACHE_DIR", "").strip()
    if raw:
        return Path(raw).expanduser().resolve()
    return Path(tempfile.gettempdir()) / "doctotext_preview_cache"


def cache_dirs():
    root = get_cache_root()
    return {
        "root": root,
        "pdf": root / "pdf",
        "pages": root / "pages",
        "uploads": root / "uploads",
        "lo_profile": root / "lo_profile",
    }


def ensure_cache_dirs():
    for d in cache_dirs().values():
        d.mkdir(parents=True, exist_ok=True)


def find_doctotext_bin():
    env_bin = os.environ.get("DOCTOTEXT_BIN", "").strip()
    if env_bin:
        p = Path(env_bin).expanduser().resolve()
        if p.is_file() and os.access(p, os.X_OK):
            return p
    for cand in (
        REPO_ROOT / "build" / "doctotext",
        REPO_ROOT / "src" / "doctotext",
    ):
        if cand.is_file() and os.access(cand, os.X_OK):
            return cand
    which_bin = shutil.which("doctotext")
    return Path(which_bin).resolve() if which_bin else None


def find_tika_jar():
    env_jar = os.environ.get("TIKA_JAR", "").strip()
    if env_jar:
        p = Path(env_jar).expanduser().resolve()
        if p.is_file():
            return p
    search_dirs = [
        REPO_ROOT,
        REPO_ROOT / "tools",
        SCRIPT_DIR,
        REPO_ROOT.parent / "test_dataset",
    ]
    for d in search_dirs:
        if not d.is_dir():
            continue
        exact = d / "tika-app.jar"
        if exact.is_file():
            return exact.resolve()
        matches = sorted(d.glob("tika-app*.jar"))
        if matches:
            return matches[-1].resolve()
    return None


def find_ppocr_paths():
    env_bin = os.environ.get("PPOCR_BIN", "").strip()
    ppocr_bin = Path(env_bin).expanduser().resolve() if env_bin else (REPO_ROOT / "examples" / "example2_ppocr_extractor")

    env_models = os.environ.get("PPOCR_MODELS_DIR", "").strip()
    models_dir = None
    for cand in [
        Path(env_models).expanduser() if env_models else None,
        REPO_ROOT / "models" / "ppocr_v6",
        REPO_ROOT.parent / "soeasy" / "models" / "ppocr_v6",
    ]:
        if cand and cand.is_dir():
            models_dir = cand.resolve()
            break

    env_ort = os.environ.get("ONNXRUNTIME_LIB_DIR", "").strip()
    ort_dir = None
    for cand in [
        Path(env_ort).expanduser() if env_ort else None,
        REPO_ROOT / "third_party" / "onnxruntime" / "lib",
        REPO_ROOT.parent / "soeasy" / "third_party" / "onnxruntime" / "lib",
    ]:
        if cand and cand.is_dir():
            ort_dir = cand.resolve()
            break

    return ppocr_bin, models_dir, ort_dir


def detect_capabilities():
    doctotext_bin = find_doctotext_bin()
    ppocr_bin, ppocr_models_dir, ort_lib_dir = find_ppocr_paths()
    libreoffice_bin = shutil.which("libreoffice") or shutil.which("soffice")
    pdftoppm_bin = shutil.which("pdftoppm")
    pdfinfo_bin = shutil.which("pdfinfo")
    java_bin = shutil.which("java")
    tika_jar = find_tika_jar()

    initial_dir = os.environ.get("DOCTOTEXT_VIEWER_INITIAL_DIR", "").strip()
    presets = []
    seen_paths = set()

    def add_preset(label: str, path_obj: Path):
        if path_obj.is_dir():
            resolved = str(path_obj.resolve())
            if resolved not in seen_paths:
                seen_paths.add(resolved)
                presets.append({"label": label, "path": resolved})

    if initial_dir:
        init_p = Path(initial_dir).expanduser()
        if init_p.is_dir():
            add_preset(f"{init_p.name or init_p}/", init_p)

    add_preset("tests/", REPO_ROOT / "tests")
    add_preset("tests_img/", REPO_ROOT / "tests_img")
    add_preset("repo root", REPO_ROOT)

    if initial_dir and Path(initial_dir).expanduser().is_dir():
        default_dir = str(Path(initial_dir).expanduser().resolve())
    elif (REPO_ROOT / "tests").is_dir():
        default_dir = str((REPO_ROOT / "tests").resolve())
    else:
        default_dir = str(REPO_ROOT)

    return {
        "repo_root": str(REPO_ROOT),
        "initial_dir": default_dir,
        "cache_dir": str(get_cache_root()),
        "doctotext_available": bool(doctotext_bin),
        "doctotext_bin": str(doctotext_bin) if doctotext_bin else "",
        "ppocr_available": bool(
            ppocr_bin.is_file()
            and os.access(ppocr_bin, os.X_OK)
            and ppocr_models_dir is not None
        ),
        "ppocr_bin": str(ppocr_bin) if ppocr_bin.is_file() else "",
        "ppocr_models_dir": str(ppocr_models_dir) if ppocr_models_dir else "",
        "onnxruntime_lib_dir": str(ort_lib_dir) if ort_lib_dir else "",
        "libreoffice_available": bool(libreoffice_bin),
        "libreoffice_bin": libreoffice_bin or "",
        "pdftoppm_available": bool(pdftoppm_bin),
        "pdfinfo_available": bool(pdfinfo_bin),
        "tika_available": bool(java_bin and tika_jar),
        "tika_jar": str(tika_jar) if tika_jar else "",
        "presets": presets,
    }


def cache_key_for_file(file_path: Path) -> str:
    st = file_path.stat()
    raw = f"{file_path.resolve()}|{st.st_mtime_ns}|{st.st_size}".encode("utf-8", errors="replace")
    return hashlib.sha256(raw).hexdigest()[:24]


def resolve_input_file(raw_path: str) -> Path:
    if not raw_path:
        raise FileNotFoundError("Empty path")
    p = Path(raw_path).expanduser().resolve()
    if not p.is_file():
        raise FileNotFoundError(f"File not found: {raw_path}")
    return p


def resolve_browse_dir(raw_dir: str) -> Path:
    if not raw_dir:
        default_dir = REPO_ROOT / "tests"
        return default_dir if default_dir.is_dir() else REPO_ROOT
    p = Path(raw_dir).expanduser().resolve()
    if not p.is_dir():
        raise NotADirectoryError(f"Directory not found: {raw_dir}")
    return p


def convert_to_pdf(src_path: Path):
    """Convert Office/ODF/RTF/TIFF file to cached PDF via LibreOffice headless."""
    ext = src_path.suffix.lower()
    if ext == ".pdf":
        return src_path, None

    libreoffice_bin = shutil.which("libreoffice") or shutil.which("soffice")
    if not libreoffice_bin:
        return None, "LibreOffice is not installed on this machine."

    cdirs = cache_dirs()
    cdirs["pdf"].mkdir(parents=True, exist_ok=True)
    cdirs["lo_profile"].mkdir(parents=True, exist_ok=True)
    key = cache_key_for_file(src_path)
    target_pdf = cdirs["pdf"] / f"{key}.pdf"
    if target_pdf.is_file() and target_pdf.stat().st_size > 0:
        return target_pdf, None

    with LO_LOCK:
        if target_pdf.is_file() and target_pdf.stat().st_size > 0:
            return target_pdf, None

        tmp_out = cdirs["pdf"] / f"tmp_{key}_{threading.get_ident()}"
        tmp_out.mkdir(parents=True, exist_ok=True)
        try:
            cmd = [
                libreoffice_bin,
                f"-env:UserInstallation={cdirs['lo_profile'].resolve().as_uri()}",
                "--headless",
                "--norestore",
                "--nofirststartwizard",
                "--convert-to",
                "pdf",
                "--outdir",
                str(tmp_out),
                str(src_path),
            ]
            proc = subprocess.run(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=45,
                env={**os.environ, "HOME": str(cdirs["lo_profile"])},
            )
            generated = list(tmp_out.glob("*.pdf"))
            if proc.returncode != 0 or not generated:
                err_msg = (proc.stderr or proc.stdout or "").strip()
                if not err_msg:
                    err_msg = f"LibreOffice exited with code {proc.returncode}"
                return None, err_msg
            shutil.move(str(generated[0]), str(target_pdf))
            return target_pdf, None
        except subprocess.TimeoutExpired:
            return None, "LibreOffice PDF conversion timed out after 45s."
        except Exception as exc:
            return None, f"Conversion error: {exc}"
        finally:
            shutil.rmtree(tmp_out, ignore_errors=True)


def get_pdf_page_count(pdf_path: Path) -> int:
    pdfinfo_bin = shutil.which("pdfinfo")
    if not pdfinfo_bin:
        return 0
    try:
        proc = subprocess.run(
            [pdfinfo_bin, str(pdf_path)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=10,
        )
        m = re.search(r"^Pages:\s*(\d+)", proc.stdout, re.MULTILINE)
        return int(m.group(1)) if m else 0
    except Exception:
        return 0


def render_pdf_page_png(pdf_path: Path, page_num: int, dpi: int = 150):
    pdftoppm_bin = shutil.which("pdftoppm")
    if not pdftoppm_bin:
        return None, "pdftoppm is not installed."
    dpi = max(72, min(dpi, 300))
    page_num = max(1, page_num)
    key = cache_key_for_file(pdf_path)
    out_dir = cache_dirs()["pages"] / f"{key}_{dpi}"
    out_dir.mkdir(parents=True, exist_ok=True)
    prefix = out_dir / "page"

    existing = sorted(out_dir.glob(f"page-*{page_num}.png"))
    for p in existing:
        if re.fullmatch(rf"page-0*{page_num}\.png", p.name):
            return p, None

    with PAGE_LOCK:
        for p in out_dir.glob(f"page-*{page_num}.png"):
            if re.fullmatch(rf"page-0*{page_num}\.png", p.name):
                return p, None
        try:
            proc = subprocess.run(
                [pdftoppm_bin, "-png", "-r", str(dpi), "-f", str(page_num), "-l", str(page_num), str(pdf_path), str(prefix)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=20,
            )
            if proc.returncode != 0:
                return None, (proc.stderr or proc.stdout or "pdftoppm failed").strip()
            for p in out_dir.glob(f"page-*{page_num}.png"):
                if re.fullmatch(rf"page-0*{page_num}\.png", p.name):
                    return p, None
            return None, f"Page {page_num} produced no image."
        except subprocess.TimeoutExpired:
            return None, "pdftoppm timed out."
        except Exception as exc:
            return None, str(exc)


def infer_status(exit_code: int, logs: str, text: str) -> str:
    combined = f"{logs}\n{text}".lower()
    if "encrypted" in combined or "requires a password" in combined or "password protected" in combined:
        return "ENCRYPTED"
    if exit_code == 0:
        return "OK"
    return "ERROR"


def parse_ppocr_output(stdout_text: str):
    start_marker = "============================== 提取结果内容 ==============================\n"
    end_marker = "\n========================================================================="
    s_idx = stdout_text.find(start_marker)
    if s_idx != -1:
        body_start = s_idx + len(start_marker)
        e_idx = stdout_text.find(end_marker, body_start)
        if e_idx != -1:
            extracted = stdout_text[body_start:e_idx]
            pre_logs = stdout_text[:s_idx].strip()
            post_logs = stdout_text[e_idx + len(end_marker):].strip()
            merged_logs = "\n".join(x for x in (pre_logs, post_logs) if x)
            return extracted, merged_logs
    return "", stdout_text.strip()


def run_extraction(file_path: Path, engine: str, parser_hint: str = "", verbose: bool = False):
    caps = detect_capabilities()
    t0 = time.perf_counter()

    env = os.environ.copy()
    ld_parts = []
    for cand in (
        REPO_ROOT / "build",
        REPO_ROOT / "src",
        Path(caps["doctotext_bin"]).parent if caps["doctotext_bin"] else None,
        Path(caps["onnxruntime_lib_dir"]) if caps["onnxruntime_lib_dir"] else None,
    ):
        if cand and cand.is_dir() and str(cand) not in ld_parts:
            ld_parts.append(str(cand))
    if env.get("LD_LIBRARY_PATH"):
        ld_parts.append(env["LD_LIBRARY_PATH"])
    env["LD_LIBRARY_PATH"] = ":".join(ld_parts)
    env.setdefault("TZ", "UTC")

    if engine in ("doctotext", "doctotext_compact", "doctotext_meta"):
        if not caps["doctotext_available"]:
            return {
                "engine": engine,
                "status": "ERROR",
                "exit_code": -1,
                "elapsed_ms": 0.0,
                "char_count": 0,
                "line_count": 0,
                "text": "",
                "logs": f"Binary not found: {caps['doctotext_bin']}",
            }
        cmd = [caps["doctotext_bin"]]
        if engine == "doctotext_compact":
            cmd.append("--compact-whitespace")
        elif engine == "doctotext_meta":
            cmd.append("--meta")
        if verbose:
            cmd.append("--verbose")
        allowed_hints = {
            "rtf", "odf", "ooxml", "xls", "xlsb", "iwork", "ppt", "doc",
            "html", "pdf", "eml", "odfxml", "txt", "image",
        }
        if parser_hint in allowed_hints:
            cmd.append(f"--{parser_hint}")
        cmd.append(str(file_path))

        try:
            proc = subprocess.run(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=60,
                env=env,
            )
            elapsed_ms = (time.perf_counter() - t0) * 1000.0
            text = proc.stdout.decode("utf-8", errors="replace")
            logs = proc.stderr.decode("utf-8", errors="replace").strip()
            status = infer_status(proc.returncode, logs, text)
            return {
                "engine": engine,
                "status": status,
                "exit_code": proc.returncode,
                "elapsed_ms": round(elapsed_ms, 2),
                "char_count": len(text),
                "line_count": len(text.splitlines()) if text else 0,
                "text": text,
                "logs": logs,
                "command": " ".join(cmd),
            }
        except subprocess.TimeoutExpired:
            return {
                "engine": engine,
                "status": "ERROR",
                "exit_code": -1,
                "elapsed_ms": 60000.0,
                "char_count": 0,
                "line_count": 0,
                "text": "",
                "logs": "doctotext execution timed out after 60s.",
            }

    if engine == "ppocr":
        if not caps["ppocr_available"]:
            return {
                "engine": engine,
                "status": "ERROR",
                "exit_code": -1,
                "elapsed_ms": 0.0,
                "char_count": 0,
                "line_count": 0,
                "text": "",
                "logs": "example2_ppocr_extractor or PP-OCRv6 models not available.",
            }
        cmd = [caps["ppocr_bin"], str(file_path), caps["ppocr_models_dir"]]
        try:
            proc = subprocess.run(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=90,
                env=env,
            )
            elapsed_ms = (time.perf_counter() - t0) * 1000.0
            stdout_str = proc.stdout.decode("utf-8", errors="replace")
            stderr_str = proc.stderr.decode("utf-8", errors="replace").strip()
            extracted, pre_logs = parse_ppocr_output(stdout_str)
            logs = "\n".join(x for x in (pre_logs, stderr_str) if x)
            status = infer_status(proc.returncode, logs, extracted)
            return {
                "engine": engine,
                "status": status,
                "exit_code": proc.returncode,
                "elapsed_ms": round(elapsed_ms, 2),
                "char_count": len(extracted),
                "line_count": len(extracted.splitlines()) if extracted else 0,
                "text": extracted,
                "logs": logs,
                "command": " ".join(cmd),
            }
        except subprocess.TimeoutExpired:
            return {
                "engine": engine,
                "status": "ERROR",
                "exit_code": -1,
                "elapsed_ms": 90000.0,
                "char_count": 0,
                "line_count": 0,
                "text": "",
                "logs": "example2_ppocr_extractor timed out after 90s.",
            }

    if engine == "tika":
        if not caps["tika_available"]:
            return {
                "engine": engine,
                "status": "ERROR",
                "exit_code": -1,
                "elapsed_ms": 0.0,
                "char_count": 0,
                "line_count": 0,
                "text": "",
                "logs": "Java or tika-app.jar is not available.",
            }
        cmd = ["java", "-jar", caps["tika_jar"], "-t", str(file_path)]
        try:
            proc = subprocess.run(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=60,
                env=env,
            )
            elapsed_ms = (time.perf_counter() - t0) * 1000.0
            text = proc.stdout.decode("utf-8", errors="replace")
            logs = proc.stderr.decode("utf-8", errors="replace").strip()
            status = infer_status(proc.returncode, logs, text)
            return {
                "engine": engine,
                "status": status,
                "exit_code": proc.returncode,
                "elapsed_ms": round(elapsed_ms, 2),
                "char_count": len(text),
                "line_count": len(text.splitlines()) if text else 0,
                "text": text,
                "logs": logs,
                "command": " ".join(cmd),
            }
        except subprocess.TimeoutExpired:
            return {
                "engine": engine,
                "status": "ERROR",
                "exit_code": -1,
                "elapsed_ms": 60000.0,
                "char_count": 0,
                "line_count": 0,
                "text": "",
                "logs": "Apache Tika timed out after 60s.",
            }

    return {
        "engine": engine,
        "status": "ERROR",
        "exit_code": -1,
        "elapsed_ms": 0.0,
        "char_count": 0,
        "line_count": 0,
        "text": "",
        "logs": f"Unknown extraction engine: {engine}",
    }


def classify_preview(file_path: Path):
    ext = file_path.suffix.lower()
    if ext == ".pdf":
        return "pdf"
    if ext in OFFICE_CONVERTIBLE_EXTS:
        return "office_pdf"
    if ext in NATIVE_IMAGE_EXTS:
        return "image"
    if ext in HTML_EXTS:
        return "html"
    if ext in TEXT_EXTS:
        return "text"
    return "unknown"


class InspectorHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, format, *args):
        # Quiet normal request chatter unless debugging
        pass

    def send_json(self, status_code: int, payload: dict):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status_code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def send_file_bytes(self, file_path: Path, content_type: str, inline: bool = True):
        data = file_path.read_bytes()
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        disp = "inline" if inline else "attachment"
        encoded_name = urllib.parse.quote(file_path.name)
        self.send_header("Content-Disposition", f"{disp}; filename*=UTF-8''{encoded_name}")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        qs = urllib.parse.parse_qs(parsed.query)

        if parsed.path in ("/", "/index.html"):
            index_file = SCRIPT_DIR / "index.html"
            if not index_file.is_file():
                self.send_error(HTTPStatus.NOT_FOUND, "index.html not found")
                return
            self.send_file_bytes(index_file, "text/html; charset=utf-8")
            return

        if parsed.path == "/api/capabilities":
            self.send_json(HTTPStatus.OK, detect_capabilities())
            return

        if parsed.path == "/api/browse":
            self.handle_browse(qs)
            return

        if parsed.path == "/api/file_info":
            self.handle_file_info(qs)
            return

        if parsed.path == "/api/preview":
            self.handle_preview(qs)
            return

        if parsed.path == "/api/pdf_info":
            self.handle_pdf_info(qs)
            return

        if parsed.path == "/api/pdf_page":
            self.handle_pdf_page(qs)
            return

        self.send_error(HTTPStatus.NOT_FOUND, "Not found")

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)

        if parsed.path == "/api/extract":
            self.handle_extract()
            return

        if parsed.path == "/api/diff":
            self.handle_diff()
            return

        if parsed.path == "/api/upload":
            self.handle_upload(urllib.parse.parse_qs(parsed.query))
            return

        self.send_error(HTTPStatus.NOT_FOUND, "Not found")

    def handle_browse(self, qs):
        raw_dir = qs.get("dir", [""])[0]
        show_all = qs.get("all", ["0"])[0] == "1"
        try:
            target = resolve_browse_dir(raw_dir)
        except Exception as exc:
            self.send_json(HTTPStatus.BAD_REQUEST, {"error": str(exc)})
            return

        dirs = []
        files = []
        try:
            with os.scandir(target) as it:
                for entry in it:
                    name = entry.name
                    if name.startswith("."):
                        continue
                    try:
                        is_dir = entry.is_dir(follow_symlinks=True)
                    except OSError:
                        continue
                    if is_dir:
                        dirs.append({
                            "name": name,
                            "path": str(Path(entry.path).resolve()),
                        })
                    else:
                        ext = Path(name).suffix.lower()
                        is_doc = ext in DOCUMENT_EXTENSIONS
                        if not show_all and not is_doc:
                            continue
                        try:
                            st = entry.stat()
                            size = st.st_size
                        except OSError:
                            size = 0
                        files.append({
                            "name": name,
                            "path": str(Path(entry.path).resolve()),
                            "ext": ext.lstrip(".") or "file",
                            "size": size,
                            "is_doc": is_doc,
                            "preview_kind": classify_preview(Path(entry.path)),
                        })
        except PermissionError as exc:
            self.send_json(HTTPStatus.FORBIDDEN, {"error": str(exc)})
            return

        dirs.sort(key=lambda d: d["name"].lower())
        files.sort(key=lambda f: f["name"].lower())
        parent = str(target.parent) if target.parent != target else None

        self.send_json(HTTPStatus.OK, {
            "dir": str(target),
            "parent": parent,
            "dirs": dirs,
            "files": files,
        })

    def handle_file_info(self, qs):
        raw_path = qs.get("path", [""])[0]
        try:
            file_path = resolve_input_file(raw_path)
        except Exception as exc:
            self.send_json(HTTPStatus.NOT_FOUND, {"error": str(exc)})
            return
        st = file_path.stat()
        kind = classify_preview(file_path)
        self.send_json(HTTPStatus.OK, {
            "name": file_path.name,
            "path": str(file_path),
            "ext": file_path.suffix.lower().lstrip("."),
            "size": st.st_size,
            "preview_kind": kind,
        })

    def handle_preview(self, qs):
        raw_path = qs.get("path", [""])[0]
        raw_mode = qs.get("raw", ["0"])[0] == "1"
        check_only = qs.get("check", ["0"])[0] == "1"
        try:
            file_path = resolve_input_file(raw_path)
        except Exception as exc:
            self.send_json(HTTPStatus.NOT_FOUND, {"error": str(exc)})
            return

        kind = classify_preview(file_path)
        if kind == "pdf":
            if check_only:
                self.send_json(HTTPStatus.OK, {"ok": True, "preview_kind": kind})
                return
            self.send_file_bytes(file_path, "application/pdf", inline=True)
            return

        if kind == "office_pdf" and not raw_mode:
            pdf_path, err = convert_to_pdf(file_path)
            if err or not pdf_path:
                self.send_json(HTTPStatus.UNPROCESSABLE_ENTITY, {
                    "error": err or "Could not convert file for preview",
                    "preview_kind": kind,
                })
                return
            if check_only:
                self.send_json(HTTPStatus.OK, {"ok": True, "preview_kind": kind})
                return
            self.send_file_bytes(pdf_path, "application/pdf", inline=True)
            return

        if check_only:
            self.send_json(HTTPStatus.OK, {"ok": True, "preview_kind": kind})
            return

        if kind == "image":
            mime, _ = mimetypes.guess_type(str(file_path))
            self.send_file_bytes(file_path, mime or "application/octet-stream", inline=True)
            return

        if kind == "html":
            data = file_path.read_bytes()
            text = data.decode("utf-8", errors="replace").encode("utf-8")
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(text)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(text)
            return

        # Plain text or unknown fallback (serve first 2MB safely as UTF-8 text)
        data = file_path.read_bytes()[: 2 * 1024 * 1024]
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            text = data.decode("latin-1", errors="replace")
        out = text.encode("utf-8")
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(out)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(out)

    def handle_pdf_info(self, qs):
        raw_path = qs.get("path", [""])[0]
        try:
            file_path = resolve_input_file(raw_path)
        except Exception as exc:
            self.send_json(HTTPStatus.NOT_FOUND, {"error": str(exc)})
            return

        pdf_path, err = convert_to_pdf(file_path)
        if err or not pdf_path:
            self.send_json(HTTPStatus.UNPROCESSABLE_ENTITY, {"error": err or "Cannot render pages"})
            return

        pages = get_pdf_page_count(pdf_path)
        self.send_json(HTTPStatus.OK, {
            "page_count": pages,
            "pdf_ready": True,
        })

    def handle_pdf_page(self, qs):
        raw_path = qs.get("path", [""])[0]
        page = int(qs.get("page", ["1"])[0] or "1")
        dpi = int(qs.get("dpi", ["150"])[0] or "150")
        try:
            file_path = resolve_input_file(raw_path)
        except Exception as exc:
            self.send_json(HTTPStatus.NOT_FOUND, {"error": str(exc)})
            return

        pdf_path, err = convert_to_pdf(file_path)
        if err or not pdf_path:
            self.send_json(HTTPStatus.UNPROCESSABLE_ENTITY, {"error": err or "Cannot convert to PDF"})
            return

        png_path, err = render_pdf_page_png(pdf_path, page, dpi)
        if err or not png_path:
            self.send_json(HTTPStatus.UNPROCESSABLE_ENTITY, {"error": err or "Cannot rasterize page"})
            return
        self.send_file_bytes(png_path, "image/png", inline=True)

    def read_json_body(self):
        length = int(self.headers.get("Content-Length", "0") or "0")
        raw = self.rfile.read(length)
        return json.loads(raw.decode("utf-8"))

    def handle_extract(self):
        try:
            payload = self.read_json_body()
            file_path = resolve_input_file(payload.get("path", ""))
        except Exception as exc:
            self.send_json(HTTPStatus.BAD_REQUEST, {"error": str(exc)})
            return

        engine = payload.get("engine", "doctotext")
        parser_hint = payload.get("parser_hint", "")
        verbose = bool(payload.get("verbose", False))

        result = run_extraction(file_path, engine=engine, parser_hint=parser_hint, verbose=verbose)
        self.send_json(HTTPStatus.OK, result)

    def handle_diff(self):
        try:
            payload = self.read_json_body()
        except Exception as exc:
            self.send_json(HTTPStatus.BAD_REQUEST, {"error": str(exc)})
            return
        a = (payload.get("left") or "").splitlines()
        b = (payload.get("right") or "").splitlines()
        left_label = payload.get("left_label", "DocToText")
        right_label = payload.get("right_label", "Tika")
        diff_lines = list(difflib.unified_diff(a, b, fromfile=left_label, tofile=right_label, lineterm=""))
        ratio = difflib.SequenceMatcher(None, "\n".join(a), "\n".join(b)).ratio()
        self.send_json(HTTPStatus.OK, {
            "similarity": round(ratio * 100.0, 1),
            "diff": "\n".join(diff_lines),
        })

    def handle_upload(self, qs):
        raw_name = qs.get("filename", ["uploaded_file"])[0]
        length = int(self.headers.get("Content-Length", "0") or "0")
        if length <= 0 or length > 256 * 1024 * 1024:
            self.send_json(HTTPStatus.BAD_REQUEST, {"error": "Invalid upload size (must be 1B - 256MB)"})
            return
        try:
            orig_name = os.path.basename(raw_name or "uploaded_file")
            safe_name = re.sub(r"[^A-Za-z0-9._\-\u4e00-\u9fff() ]+", "_", orig_name) or "uploaded_file"
            batch = hashlib.sha256(f"{time.time_ns()}|{safe_name}".encode("utf-8")).hexdigest()[:12]
            dest_dir = cache_dirs()["uploads"] / batch
            dest_dir.mkdir(parents=True, exist_ok=True)
            dest = dest_dir / safe_name
            remaining = length
            with open(dest, "wb") as out:
                while remaining > 0:
                    chunk = self.rfile.read(min(65536, remaining))
                    if not chunk:
                        break
                    out.write(chunk)
                    remaining -= len(chunk)
            self.send_json(HTTPStatus.OK, {
                "name": safe_name,
                "path": str(dest),
                "ext": dest.suffix.lower().lstrip("."),
                "size": dest.stat().st_size,
                "preview_kind": classify_preview(dest),
            })
        except Exception as exc:
            self.send_json(HTTPStatus.INTERNAL_SERVER_ERROR, {"error": str(exc)})


def main():
    parser = argparse.ArgumentParser(description="DocToText Split-Pane Document Inspector")
    parser.add_argument("--host", default="127.0.0.1", help="Bind host (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=8765, help="Bind port (default: 8765)")
    parser.add_argument("--dir", default="", help="Optional initial folder shown in sidebar")
    parser.add_argument("--doctotext-bin", default="", help="Path to doctotext executable (or DOCTOTEXT_BIN)")
    parser.add_argument("--tika-jar", default="", help="Path to tika-app.jar (or TIKA_JAR)")
    parser.add_argument("--ppocr-bin", default="", help="Path to example2_ppocr_extractor (or PPOCR_BIN)")
    parser.add_argument("--ppocr-models", default="", help="Path to ppocr_v6 models dir (or PPOCR_MODELS_DIR)")
    parser.add_argument("--onnxruntime-lib", default="", help="Path to onnxruntime lib dir (or ONNXRUNTIME_LIB_DIR)")
    parser.add_argument("--cache-dir", default="", help="Preview cache directory (or DOCTOTEXT_CACHE_DIR)")
    args = parser.parse_args()

    if args.dir:
        os.environ["DOCTOTEXT_VIEWER_INITIAL_DIR"] = args.dir
    if args.doctotext_bin:
        os.environ["DOCTOTEXT_BIN"] = args.doctotext_bin
    if args.tika_jar:
        os.environ["TIKA_JAR"] = args.tika_jar
    if args.ppocr_bin:
        os.environ["PPOCR_BIN"] = args.ppocr_bin
    if args.ppocr_models:
        os.environ["PPOCR_MODELS_DIR"] = args.ppocr_models
    if args.onnxruntime_lib:
        os.environ["ONNXRUNTIME_LIB_DIR"] = args.onnxruntime_lib
    if args.cache_dir:
        os.environ["DOCTOTEXT_CACHE_DIR"] = args.cache_dir

    ensure_cache_dirs()
    caps = detect_capabilities()

    server = ThreadingHTTPServer((args.host, args.port), InspectorHandler)
    actual_port = server.server_address[1]
    url = f"http://{args.host}:{actual_port}"
    print(f"DocToText Inspector running at: {url}", flush=True)
    print(f"  initial dir : {caps['initial_dir']}", flush=True)
    print(f"  cache dir   : {caps['cache_dir']}", flush=True)
    print(f"  doctotext   : {caps['doctotext_bin'] or 'NOT FOUND (required)'}", flush=True)
    print(f"  libreoffice : {caps['libreoffice_bin'] or 'not installed (optional, Office preview disabled)'}", flush=True)
    print(f"  pdftoppm    : {'available' if caps['pdftoppm_available'] else 'not installed (optional, raster mode disabled)'}", flush=True)
    print(f"  tika        : {caps['tika_jar'] or 'not configured (optional, pass --tika-jar or TIKA_JAR)'}", flush=True)
    print(f"  ppocr       : {caps['ppocr_models_dir'] if caps['ppocr_available'] else 'not configured (optional)'}", flush=True)
    print("Press Ctrl+C to stop.", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down.", flush=True)
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
