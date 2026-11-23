#!/usr/bin/env python3
"""
Generates compact, deterministic Office & PDF embedded-object test fixtures
under tests/embedded_fixtures/ for C API verification (tests/c_api_embedded_test.c).

Run directly when fixtures need to be regenerated:
    python3 tests/gen_embedded_fixtures.py
"""

import io
import os
import shutil
import struct
import subprocess
import tempfile
import zipfile
import zlib

from PIL import Image
import docx
from pypdf import PdfReader, PdfWriter
from pypdf.generic import (
    ArrayObject,
    DecodedStreamObject,
    DictionaryObject,
    NameObject,
    NumberObject,
    TextStringObject,
)
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.pdfgen import canvas


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(SCRIPT_DIR, "embedded_fixtures")
CJK_FONT_PATH = "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf"
_FONT_REGISTERED = False


def ensure_cjk_font():
    global _FONT_REGISTERED
    if not _FONT_REGISTERED and os.path.exists(CJK_FONT_PATH):
        pdfmetrics.registerFont(TTFont("DroidCJK", CJK_FONT_PATH))
        _FONT_REGISTERED = True
    return "DroidCJK" if _FONT_REGISTERED else "Helvetica"


# ---------------------------------------------------------------------------
# Low-level builders: PNG, RTF, ZIP/OOXML/ODF, OLE2/CFB, BIFF8 XLS, PPT, PDF
# ---------------------------------------------------------------------------

def make_tiny_png() -> bytes:
    buf = io.BytesIO()
    img = Image.new("RGB", (8, 8), color=(32, 128, 224))
    img.save(buf, format="PNG")
    return buf.getvalue()


def make_rtf(text: str) -> bytes:
    """Builds an RTF payload encoding non-ASCII code points via RTF \\uN? Unicode escapes."""
    escaped = []
    for ch in text:
        code = ord(ch)
        if ch in ("\\", "{", "}"):
            escaped.append("\\" + ch)
        elif code < 128:
            escaped.append(ch)
        else:
            escaped.append(f"\\u{code}?")
    return ("{\\rtf1\\ansi\\uc1 " + "".join(escaped) + "}").encode("ascii")


def build_zip_bytes(entries, compress=True) -> bytes:
    buf = io.BytesIO()
    mode = zipfile.ZIP_DEFLATED if compress else zipfile.ZIP_STORED
    with zipfile.ZipFile(buf, "w", compression=mode) as zf:
        for name, data in entries:
            if isinstance(data, str):
                data = data.encode("utf-8")
            zi = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            zi.compress_type = mode
            zf.writestr(zi, data, compress_type=mode)
    return buf.getvalue()


def make_docx_bytes(main_text: str, extra_entries=None) -> bytes:
    doc = docx.Document()
    doc.add_paragraph(main_text)
    buf = io.BytesIO()
    doc.save(buf)
    if not extra_entries:
        return buf.getvalue()
    with zipfile.ZipFile(io.BytesIO(buf.getvalue()), "r") as zin:
        entries = [(info.filename, zin.read(info.filename)) for info in zin.infolist()]
    entries.extend(extra_entries)
    return build_zip_bytes(entries)


def make_xlsx_bytes(cell_text: str, extra_entries=None) -> bytes:
    entries = [
        ("[Content_Types].xml",
         '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
         '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
         '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
         '<Default Extension="xml" ContentType="application/xml"/>'
         '<Default Extension="bin" ContentType="application/vnd.openxmlformats-officedocument.oleObject"/>'
         '<Default Extension="png" ContentType="image/png"/>'
         '<Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>'
         '<Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>'
         '<Override PartName="/xl/sharedStrings.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml"/>'
         '</Types>'),
        ("_rels/.rels",
         '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
         '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
         '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/>'
         '</Relationships>'),
        ("xl/workbook.xml",
         '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
         '<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" '
         'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">'
         '<sheets><sheet name="Sheet1" sheetId="1" r:id="rId1"/></sheets></workbook>'),
        ("xl/_rels/workbook.xml.rels",
         '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
         '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
         '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/>'
         '<Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings" Target="sharedStrings.xml"/>'
         '</Relationships>'),
        ("xl/sharedStrings.xml",
         '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
         '<sst xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" count="1" uniqueCount="1">'
         f'<si><t>{cell_text}</t></si></sst>'),
        ("xl/worksheets/sheet1.xml",
         '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
         '<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">'
         '<sheetData><row r="1"><c r="A1" t="s"><v>0</v></c></row></sheetData></worksheet>'),
    ]
    if extra_entries:
        entries.extend(extra_entries)
    return build_zip_bytes(entries)


def make_pptx_bytes(slide_text: str, extra_entries=None) -> bytes:
    entries = [
        ("[Content_Types].xml",
         '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
         '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
         '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
         '<Default Extension="xml" ContentType="application/xml"/>'
         '<Default Extension="bin" ContentType="application/vnd.openxmlformats-officedocument.oleObject"/>'
         '<Override PartName="/ppt/presentation.xml" ContentType="application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml"/>'
         '<Override PartName="/ppt/slides/slide1.xml" ContentType="application/vnd.openxmlformats-officedocument.presentationml.slide+xml"/>'
         '</Types>'),
        ("_rels/.rels",
         '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
         '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
         '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="ppt/presentation.xml"/>'
         '</Relationships>'),
        ("ppt/presentation.xml",
         '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
         '<p:presentation xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main" '
         'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">'
         '<p:sldIdLst><p:sldId id="256" r:id="rId1"/></p:sldIdLst></p:presentation>'),
        ("ppt/_rels/presentation.xml.rels",
         '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
         '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
         '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide" Target="slides/slide1.xml"/>'
         '</Relationships>'),
        ("ppt/slides/slide1.xml",
         '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
         '<p:sld xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main" '
         'xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main">'
         f'<p:cSld><p:spTree><p:sp><p:txBody><a:p><a:r><a:t>{slide_text}</a:t></a:r></a:p></p:txBody></p:sp></p:spTree></p:cSld></p:sld>'),
    ]
    if extra_entries:
        entries.extend(extra_entries)
    return build_zip_bytes(entries)


def make_odf_bytes(kind: str, main_text: str, obj1_text: str, obj2_ole_bytes: bytes, skip_marker: str) -> bytes:
    mime_map = {
        "odt": "application/vnd.oasis.opendocument.text",
        "ods": "application/vnd.oasis.opendocument.spreadsheet",
        "odp": "application/vnd.oasis.opendocument.presentation",
    }
    if kind == "odt":
        body_xml = (
            f'<office:text><text:p>{main_text}</text:p>'
            '<draw:frame><draw:object xlink:href="./Object 1"/></draw:frame>'
            '</office:text>'
        )
    elif kind == "ods":
        body_xml = (
            '<office:spreadsheet><table:table table:name="Sheet1">'
            f'<table:table-row><table:table-cell><text:p>{main_text}</text:p></table:table-cell></table:table-row>'
            '</table:table>'
            '<draw:frame><draw:object xlink:href="./Object 1"/></draw:frame>'
            '</office:spreadsheet>'
        )
    else:
        body_xml = (
            '<office:presentation><draw:page draw:name="page1">'
            f'<draw:frame><draw:text-box><text:p>{main_text}</text:p></draw:text-box></draw:frame>'
            '<draw:frame><draw:object xlink:href="./Object 1"/></draw:frame>'
            '</draw:page></office:presentation>'
        )

    content_xml = (
        '<?xml version="1.0" encoding="UTF-8"?>'
        '<office:document-content '
        'xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0" '
        'xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0" '
        'xmlns:table="urn:oasis:names:tc:opendocument:xmlns:table:1.0" '
        'xmlns:draw="urn:oasis:names:tc:opendocument:xmlns:drawing:1.0" '
        'xmlns:xlink="http://www.w3.org/1999/xlink">'
        f'<office:body>{body_xml}</office:body></office:document-content>'
    )
    obj1_xml = (
        '<?xml version="1.0" encoding="UTF-8"?>'
        '<office:document-content '
        'xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0" '
        'xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0">'
        f'<office:body><office:text><text:p>{obj1_text}</text:p></office:text></office:body>'
        '</office:document-content>'
    )
    entries = [
        ("mimetype", mime_map[kind]),
        ("content.xml", content_xml),
        ("Object 1/content.xml", obj1_xml),
        ("Object 2", obj2_ole_bytes),
        ("ObjectReplacements/Object 1", skip_marker.encode("utf-8")),
        ("Thumbnails/thumbnail.png", make_tiny_png()),
    ]
    return build_zip_bytes(entries)


def make_ole10_native_stream(label: str, org_path: str, payload: bytes) -> bytes:
    body = bytearray()
    body += struct.pack("<H", 2)
    body += label.encode("utf-8") + b"\x00"
    body += org_path.encode("utf-8") + b"\x00"
    body += struct.pack("<HH", 0, 3)
    cmd = org_path.encode("utf-8") + b"\x00"
    body += struct.pack("<I", len(cmd)) + cmd
    body += struct.pack("<I", len(payload)) + payload
    return struct.pack("<I", len(body)) + bytes(body)


def build_ole2_from_tree(root_children) -> bytes:
    """
    Serializes a nested dict/list OLE2 tree (`[{"name": str, "is_storage": bool,
    "data": bytes, "children": [...]}]`) into a standard CFB v3 binary with
    mini_cutoff=0 so every stream resides cleanly in 512-byte FAT sectors.
    """
    flat = []

    def flatten(name, is_storage, data, children):
        idx = len(flat)
        flat.append({
            "name": name,
            "is_storage": is_storage,
            "data": data,
            "child": 0xFFFFFFFF,
            "left": 0xFFFFFFFF,
            "right": 0xFFFFFFFF,
        })
        child_indices = []
        for ch in children:
            child_indices.append(
                flatten(ch["name"], ch["is_storage"], ch.get("data", b""), ch.get("children", []))
            )
        if child_indices:
            flat[idx]["child"] = child_indices[0]
            for k in range(len(child_indices) - 1):
                flat[child_indices[k]]["right"] = child_indices[k + 1]
        return idx

    flatten("Root Entry", True, b"", root_children)

    data_sectors = sum((len(n["data"]) + 511) // 512 for n in flat if not n["is_storage"] and n["data"])
    num_dir_sectors = (len(flat) + 3) // 4
    num_fat_sectors = 1
    while True:
        total_sectors = num_fat_sectors + num_dir_sectors + data_sectors
        needed = (total_sectors + 127) // 128
        if needed <= num_fat_sectors:
            break
        num_fat_sectors = needed
    total_sectors = num_fat_sectors + num_dir_sectors + data_sectors

    fat = [0xFFFFFFFF] * (num_fat_sectors * 128)
    for s in range(num_fat_sectors):
        fat[s] = 0xFFFFFFFD  # FATSECT
    dir_start_sec = num_fat_sectors
    for s in range(num_dir_sectors):
        sec = dir_start_sec + s
        fat[sec] = (sec + 1) if (s + 1 < num_dir_sectors) else 0xFFFFFFFE

    next_data_sec = dir_start_sec + num_dir_sectors
    stream_start = [0xFFFFFFFE] * len(flat)
    stream_blob = bytearray(data_sectors * 512)
    cur_sec = next_data_sec
    for i, node in enumerate(flat):
        if node["is_storage"] or not node["data"]:
            continue
        raw = node["data"]
        nsec = (len(raw) + 511) // 512
        stream_start[i] = cur_sec
        rel_off = (cur_sec - next_data_sec) * 512
        stream_blob[rel_off:rel_off + len(raw)] = raw
        for k in range(nsec):
            fat[cur_sec + k] = (cur_sec + k + 1) if (k + 1 < nsec) else 0xFFFFFFFE
        cur_sec += nsec

    hdr = bytearray(512)
    hdr[0:8] = b"\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1"
    struct.pack_into("<HHHHHH", hdr, 24, 0x003E, 0x0003, 0xFFFE, 0x0009, 0x0006, 0)
    struct.pack_into("<IIIIII", hdr, 44, num_fat_sectors, dir_start_sec, 0, 0, 0xFFFFFFFE, 0)
    struct.pack_into("<II", hdr, 68, 0xFFFFFFFE, 0)
    for i in range(109):
        struct.pack_into("<I", hdr, 76 + i * 4, i if i < num_fat_sectors else 0xFFFFFFFF)

    fat_blob = struct.pack(f"<{len(fat)}I", *fat)
    dir_blob = bytearray(num_dir_sectors * 512)
    for i, node in enumerate(flat):
        base = i * 128
        name_u16 = node["name"][:31].encode("utf-16le") + b"\x00\x00"
        dir_blob[base:base + len(name_u16)] = name_u16
        struct.pack_into("<H", dir_blob, base + 64, len(name_u16))
        dir_blob[base + 66] = 0x05 if i == 0 else (0x01 if node["is_storage"] else 0x02)
        dir_blob[base + 67] = 0x01
        struct.pack_into("<III", dir_blob, base + 68, node["left"], node["right"], node["child"])
        struct.pack_into("<II", dir_blob, base + 116, stream_start[i], 0 if node["is_storage"] else len(node["data"]))

    return bytes(hdr) + fat_blob + bytes(dir_blob) + bytes(stream_blob)


def read_ole2_tree(blob: bytes):
    """Minimal CFB v3 reader (with MiniFAT support) returning root children tree."""
    sector_shift = struct.unpack_from("<H", blob, 30)[0]
    mini_shift = struct.unpack_from("<H", blob, 32)[0]
    sec_size = 1 << sector_shift
    mini_sec_size = 1 << mini_shift
    first_dir = struct.unpack_from("<I", blob, 48)[0]
    mini_cutoff = struct.unpack_from("<I", blob, 56)[0]
    first_minifat = struct.unpack_from("<I", blob, 60)[0]
    difat = list(struct.unpack_from("<109I", blob, 76))
    fat_secs = [s for s in difat if s != 0xFFFFFFFF]

    def read_sec(sec):
        off = 512 + sec * sec_size
        return blob[off:off + sec_size]

    fat = []
    for fs in fat_secs:
        fat.extend(struct.unpack_from(f"<{sec_size // 4}I", read_sec(fs)))

    def read_chain(start_sec, max_len=None):
        out = bytearray()
        cur = start_sec
        seen = set()
        while cur not in (0xFFFFFFFE, 0xFFFFFFFF) and cur not in seen:
            seen.add(cur)
            out.extend(read_sec(cur))
            cur = fat[cur]
        return bytes(out[:max_len] if max_len is not None else out)

    dir_raw = read_chain(first_dir)
    entries = []
    for off in range(0, len(dir_raw), 128):
        chunk = dir_raw[off:off + 128]
        nlen = struct.unpack_from("<H", chunk, 64)[0]
        obj_type = chunk[66]
        if obj_type == 0:
            entries.append(None)
            continue
        name = chunk[:max(0, nlen - 2)].decode("utf-16le", errors="replace")
        left, right, child = struct.unpack_from("<III", chunk, 68)
        start_sec, stream_size = struct.unpack_from("<II", chunk, 116)
        entries.append({
            "name": name,
            "type": obj_type,
            "left": left,
            "right": right,
            "child": child,
            "start": start_sec,
            "size": stream_size,
        })

    root_entry = entries[0]
    mini_stream = read_chain(root_entry["start"], root_entry["size"]) if root_entry["size"] > 0 else b""
    minifat = []
    if first_minifat not in (0xFFFFFFFE, 0xFFFFFFFF):
        mf_raw = read_chain(first_minifat)
        minifat = list(struct.unpack_from(f"<{len(mf_raw) // 4}I", mf_raw))

    def read_mini_chain(start_sec, max_len):
        out = bytearray()
        cur = start_sec
        seen = set()
        while cur not in (0xFFFFFFFE, 0xFFFFFFFF) and cur not in seen:
            seen.add(cur)
            off = cur * mini_sec_size
            out.extend(mini_stream[off:off + mini_sec_size])
            cur = minifat[cur]
        return bytes(out[:max_len])

    def collect_children(node_idx):
        res = []
        if node_idx == 0xFFFFFFFF or node_idx >= len(entries) or entries[node_idx] is None:
            return res
        e = entries[node_idx]
        res.extend(collect_children(e["left"]))
        if e["type"] == 1:
            res.append({
                "name": e["name"],
                "is_storage": True,
                "children": collect_children(e["child"]),
            })
        elif e["type"] == 2:
            if e["size"] == 0:
                data = b""
            elif e["size"] < mini_cutoff:
                data = read_mini_chain(e["start"], e["size"])
            else:
                data = read_chain(e["start"], e["size"])
            res.append({
                "name": e["name"],
                "is_storage": False,
                "data": data,
            })
        res.extend(collect_children(e["right"]))
        return res

    return collect_children(root_entry["child"])


def make_biff8_workbook(cell_text: str) -> bytes:
    def rec(rtype, payload):
        return struct.pack("<HH", rtype, len(payload)) + payload

    wb = bytearray()
    wb += rec(0x0809, struct.pack("<HHHH", 0x0600, 0x0005, 0, 0) + b"\x00" * 8)  # BOF
    wb += rec(0x0042, struct.pack("<H", 1200))  # CODEPAGE = UTF-16
    bs_name = b"Sheet1"
    globals_before_bs = bytes(wb)
    bs_len = 8 + len(bs_name)
    sheet_bof = rec(0x0809, struct.pack("<HHHH", 0x0600, 0x0010, 0, 0) + b"\x00" * 8)
    raw_txt = cell_text.encode("utf-16le")
    lbl = struct.pack("<HHHH", 0, 0, 0, len(cell_text)) + b"\x01" + raw_txt
    sheet_body = sheet_bof + rec(0x0204, lbl) + rec(0x000A, b"")
    globals_eof = rec(0x000A, b"")
    sheet_offset = len(globals_before_bs) + (4 + bs_len) + len(globals_eof)
    bs_payload = struct.pack("<IBBB", sheet_offset, 0, 0, len(bs_name)) + b"\x00" + bs_name
    return globals_before_bs + rec(0x0085, bs_payload) + globals_eof + sheet_body


def make_ppt_record(ver_inst: int, rec_type: int, body: bytes) -> bytes:
    return struct.pack("<HHI", ver_inst, rec_type, len(body)) + body


def make_ppt_text_record(text: str) -> bytes:
    """Uses RT_TEXT_CHARS_ATOM (0x0FA0, UTF-16LE) when non-ASCII, else RT_TEXT_BYTES_ATOM (0x0FA8)."""
    if all(ord(c) < 128 for c in text):
        return make_ppt_record(0x0000, 0x0FA8, text.encode("ascii"))
    return make_ppt_record(0x0000, 0x0FA0, text.encode("utf-16le"))


def make_pdf_bytes(main_text: str, attachments=None, file_annotations=None) -> bytes:
    """
    Creates a real PDF via reportlab + pypdf (switching cleanly between Helvetica
    for ASCII and DroidCJK subset for CJK/Unicode), adding optional /Names/EmbeddedFiles
    and page /FileAttachment annotations.
    """
    cjk_font = ensure_cjk_font()
    buf = io.BytesIO()
    c = canvas.Canvas(buf)
    t = c.beginText(72, 720)
    run = []
    cur_is_cjk = None
    for ch in main_text:
        is_cjk = ord(ch) >= 128
        if cur_is_cjk is not None and is_cjk != cur_is_cjk:
            t.setFont(cjk_font if cur_is_cjk else "Helvetica", 12)
            t.textOut("".join(run))
            run = []
        cur_is_cjk = is_cjk
        run.append(ch)
    if run:
        t.setFont(cjk_font if cur_is_cjk else "Helvetica", 12)
        t.textOut("".join(run))
    c.drawText(t)
    c.showPage()
    c.save()

    reader = PdfReader(io.BytesIO(buf.getvalue()))
    writer = PdfWriter()
    writer.append_pages_from_reader(reader)

    if attachments:
        for fname, fbytes in attachments:
            writer.add_attachment(fname, fbytes)

    if file_annotations:
        page = writer.pages[0]
        annots = page.get("/Annots")
        if annots is None:
            annots = ArrayObject()
            page[NameObject("/Annots")] = annots
        for fname, fbytes in file_annotations:
            ef_stream = DecodedStreamObject()
            ef_stream.set_data(fbytes)
            ef_stream[NameObject("/Type")] = NameObject("/EmbeddedFile")
            ef_stream[NameObject("/Length")] = NumberObject(len(fbytes))
            ef_ref = writer._add_object(ef_stream)

            fs_dict = DictionaryObject()
            fs_dict[NameObject("/Type")] = NameObject("/Filespec")
            fs_dict[NameObject("/F")] = TextStringObject(fname)
            fs_dict[NameObject("/UF")] = TextStringObject(fname)
            fs_dict[NameObject("/EF")] = DictionaryObject({NameObject("/F"): ef_ref})
            fs_ref = writer._add_object(fs_dict)

            annot_dict = DictionaryObject()
            annot_dict[NameObject("/Type")] = NameObject("/Annot")
            annot_dict[NameObject("/Subtype")] = NameObject("/FileAttachment")
            annot_dict[NameObject("/Rect")] = ArrayObject([NumberObject(72), NumberObject(680), NumberObject(96), NumberObject(704)])
            annot_dict[NameObject("/Contents")] = TextStringObject(f"Attached {fname}")
            annot_dict[NameObject("/FS")] = fs_ref
            annots.append(writer._add_object(annot_dict))

    out = io.BytesIO()
    writer.write(out)
    return out.getvalue()


def generate_base_doc_trees():
    """Uses LibreOffice once to convert minimal multi-encoding python-docx files into .doc CFB trees."""
    with tempfile.TemporaryDirectory() as tmp:
        src_docx = os.path.join(tmp, "host.docx")
        doc = docx.Document()
        doc.add_paragraph("DOC_HOST_MAIN_BODY_中文测试_Zażółć")
        doc.save(src_docx)
        subprocess.run(
            ["soffice", "--headless", "--convert-to", "doc", "--outdir", tmp, src_docx],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        with open(os.path.join(tmp, "host.doc"), "rb") as f:
            doc_bytes = f.read()
        host_tree = read_ole2_tree(doc_bytes)

        # Also convert a tiny embedded DOC so its WordDocument+table streams can sit in XLS MBD*
        emb_docx = os.path.join(tmp, "emb.docx")
        doc2 = docx.Document()
        doc2.add_paragraph("XLS_MBD_DIRECT_DOC_OK_内嵌中文文档")
        doc2.save(emb_docx)
        subprocess.run(
            ["soffice", "--headless", "--convert-to", "doc", "--outdir", tmp, emb_docx],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        with open(os.path.join(tmp, "emb.doc"), "rb") as f:
            emb_doc_bytes = f.read()
        emb_tree = read_ole2_tree(emb_doc_bytes)
        return host_tree, emb_tree


def write_fixture(name: str, data: bytes):
    path = os.path.join(OUT_DIR, name)
    with open(path, "wb") as f:
        f.write(data)
    print(f"Generated {name} ({len(data)} bytes)")


def main():
    if os.path.exists(OUT_DIR):
        shutil.rmtree(OUT_DIR)
    os.makedirs(OUT_DIR, exist_ok=True)

    tiny_png = make_tiny_png()
    doc_host_tree, doc_emb_tree = generate_base_doc_trees()

    # 1. host_docx_multi.docx (UTF-8, UTF-16LE, UTF-16BE, GB18030, PDF CJK, XLSX CJK, XLS CJK, PPT CJK, PNG)
    ole_utf8 = build_ole2_from_tree([
        {"name": "\x01Ole10Native", "is_storage": False,
         "data": make_ole10_native_stream(
             "note_utf8.txt", "C:\\tmp\\note_utf8.txt",
             "DOCX_OLE10_UTF8_中文_日本語_Тест".encode("utf-8"))}
    ])
    utf16le_payload = b"\xFF\xFE" + "DOCX_OLE10_UTF16LE_中文测试".encode("utf-16le")
    ole_utf16le = build_ole2_from_tree([
        {"name": "\x01Ole10Native", "is_storage": False,
         "data": make_ole10_native_stream("note_u16le.txt", "note_u16le.txt", utf16le_payload)}
    ])
    utf16be_payload = b"\xFE\xFF" + "DOCX_OLE10_UTF16BE_日本語".encode("utf-16be")
    ole_utf16be = build_ole2_from_tree([
        {"name": "\x01Ole10Native", "is_storage": False,
         "data": make_ole10_native_stream("note_u16be.txt", "note_u16be.txt", utf16be_payload)}
    ])
    gb18030_text = "DOCX_OLE10_GB18030_这是一段简体中文字符集自动识别测试内容用于覆盖非Unicode编码文件"
    ole_gb18030 = build_ole2_from_tree([
        {"name": "\x01Ole10Native", "is_storage": False,
         "data": make_ole10_native_stream("note_gbk.txt", "note_gbk.txt", gb18030_text.encode("gb18030"))}
    ])
    ole_contents_pdf = build_ole2_from_tree([
        {"name": "CONTENTS", "is_storage": False, "data": make_pdf_bytes("DOCX_CONTENTS_PDF_OK_中文PDF")}
    ])
    ole_package_xlsx = build_ole2_from_tree([
        {"name": "Package", "is_storage": False, "data": make_xlsx_bytes("DOCX_PACKAGE_XLSX_OK_嵌入表格中文")}
    ])
    ole_direct_xls = build_ole2_from_tree([
        {"name": "Workbook", "is_storage": False, "data": make_biff8_workbook("DOCX_DIRECT_XLS_OK_工作簿中文")}
    ])
    ole_direct_ppt = build_ole2_from_tree([
        {"name": "PowerPoint Document", "is_storage": False,
         "data": make_ppt_text_record("DOCX_DIRECT_PPT_OK_幻灯片中文")}
    ])
    write_fixture("host_docx_multi.docx", make_docx_bytes(
        "DOCX_HOST_MAIN_BODY_中文主文档_Zażółć",
        extra_entries=[
            ("word/embeddings/obj1_utf8.bin", ole_utf8),
            ("word/embeddings/obj2_utf16le.bin", ole_utf16le),
            ("word/embeddings/obj3_utf16be.bin", ole_utf16be),
            ("word/embeddings/obj4_gb18030.bin", ole_gb18030),
            ("word/embeddings/obj5_pdf.bin", ole_contents_pdf),
            ("word/embeddings/obj6_pkg.bin", ole_package_xlsx),
            ("word/embeddings/obj7_xls.bin", ole_direct_xls),
            ("word/embeddings/obj8_ppt.bin", ole_direct_ppt),
            ("word/embeddings/obj9_img.png", tiny_png),
        ],
    ))

    # 2. host_xlsx_multi.xlsx (Unicode + RTF Unicode + DOCX Japanese + CJK PDF)
    write_fixture("host_xlsx_multi.xlsx", make_xlsx_bytes(
        "XLSX_HOST_MAIN_CELL_主表格中文",
        extra_entries=[
            ("xl/embeddings/emb_rtf.bin", build_ole2_from_tree([
                {"name": "\x01Ole10Native", "is_storage": False,
                 "data": make_ole10_native_stream("x.rtf", "x.rtf",
                     make_rtf("XLSX_OLE10_RTF_OK_中文Unicode"))}
            ])),
            ("xl/embeddings/emb_docx.docx", make_docx_bytes("XLSX_DIRECT_DOCX_OK_内嵌文档日本語")),
            ("xl/embeddings/emb_pdf.bin", build_ole2_from_tree([
                {"name": "CONTENTS", "is_storage": False, "data": make_pdf_bytes("XLSX_CONTENTS_PDF_OK_中文")}
            ])),
        ],
    ))

    # 3. host_pptx_multi.pptx
    write_fixture("host_pptx_multi.pptx", make_pptx_bytes(
        "PPTX_HOST_MAIN_SLIDE_幻灯片中文_한국어",
        extra_entries=[
            ("ppt/embeddings/emb_xlsx.xlsx", make_xlsx_bytes("PPTX_DIRECT_XLSX_OK_嵌入表格中文")),
            ("ppt/embeddings/emb_rtf.bin", build_ole2_from_tree([
                {"name": "\x01Ole10Native", "is_storage": False,
                 "data": make_ole10_native_stream("p.rtf", "p.rtf", make_rtf("PPTX_OLE10_RTF_OK_中文RTF"))}
            ])),
        ],
    ))

    # 4. host_xlsb_multi.xlsb (reuse minimal tests/1.xlsb base + inject xl/embeddings/emb_pdf.pdf)
    with open(os.path.join(SCRIPT_DIR, "1.xlsb"), "rb") as f:
        xlsb_base = f.read()
    with zipfile.ZipFile(io.BytesIO(xlsb_base), "r") as zin:
        xlsb_entries = [(info.filename, zin.read(info.filename)) for info in zin.infolist()]
    xlsb_entries.append(("xl/embeddings/emb_pdf.pdf", make_pdf_bytes("XLSB_EMBED_PDF_OK_二进制表中文")))
    write_fixture("host_xlsb_multi.xlsb", build_zip_bytes(xlsb_entries))

    # 5-7. ODF: host_odt_multi.odt, host_ods_multi.ods, host_odp_multi.odp
    odf_ole_rtf = build_ole2_from_tree([
        {"name": "\x01Ole10Native", "is_storage": False,
         "data": make_ole10_native_stream("o.rtf", "o.rtf", make_rtf("ODF_OBJ2_OLE_RTF_OK_中文对象"))}
    ])
    write_fixture("host_odt_multi.odt", make_odf_bytes(
        "odt", "ODT_HOST_MAIN_BODY_中文主文档", "ODT_OBJ1_XML_OK_中文公式文本", odf_ole_rtf, "ODT_REPLACEMENT_MUST_SKIP"))
    write_fixture("host_ods_multi.ods", make_odf_bytes(
        "ods", "ODS_HOST_MAIN_CELL_中文表格", "ODS_OBJ1_XML_OK_中文图表", odf_ole_rtf, "ODS_REPLACEMENT_MUST_SKIP"))
    write_fixture("host_odp_multi.odp", make_odf_bytes(
        "odp", "ODP_HOST_MAIN_SLIDE_中文演示", "ODP_OBJ1_XML_OK_中文备注", odf_ole_rtf, "ODP_REPLACEMENT_MUST_SKIP"))

    # 8. host_doc_multi.doc (real LibreOffice .doc + ObjectPool children)
    doc_pool = {
        "name": "ObjectPool",
        "is_storage": True,
        "children": [
            {"name": "_1_xls", "is_storage": True, "children": [
                {"name": "Workbook", "is_storage": False, "data": make_biff8_workbook("DOC_POOL_XLS_OK_工作簿中文")}
            ]},
            {"name": "_2_ppt", "is_storage": True, "children": [
                {"name": "PowerPoint Document", "is_storage": False,
                 "data": make_ppt_text_record("DOC_POOL_PPT_OK_演示中文")}
            ]},
            {"name": "_3_pdf", "is_storage": True, "children": [
                {"name": "CONTENTS", "is_storage": False, "data": make_pdf_bytes("DOC_POOL_PDF_OK_嵌入PDF中文")}
            ]},
            {"name": "_4_rtf", "is_storage": True, "children": [
                {"name": "\x01Ole10Native", "is_storage": False,
                 "data": make_ole10_native_stream("d.rtf", "d.rtf", make_rtf("DOC_POOL_RTF_OK_中文RTF"))}
            ]},
            {"name": "_5_docx", "is_storage": True, "children": [
                {"name": "Package", "is_storage": False, "data": make_docx_bytes("DOC_POOL_DOCX_OK_中文DOCX")}
            ]},
        ],
    }
    doc_children = [ch for ch in doc_host_tree if ch["name"] != "ObjectPool"] + [doc_pool]
    write_fixture("host_doc_multi.doc", build_ole2_from_tree(doc_children))

    # 9. host_xls_multi.xls (BIFF8 Unicode + MBD* for PDF/DOCX/PPT/DOC + ObjectPool RTF + empty MBD)
    doc_emb_streams = [ch for ch in doc_emb_tree if not ch["is_storage"]]
    xls_children = [
        {"name": "Workbook", "is_storage": False, "data": make_biff8_workbook("XLS_HOST_MAIN_CELL_主表格中文")},
        {"name": "MBD00001", "is_storage": True, "children": [
            {"name": "CONTENTS", "is_storage": False, "data": make_pdf_bytes("XLS_MBD_PDF_OK_中文")}
        ]},
        {"name": "MBD00002", "is_storage": True, "children": [
            {"name": "Package", "is_storage": False, "data": make_docx_bytes("XLS_MBD_DOCX_OK_中文")}
        ]},
        {"name": "MBD00003", "is_storage": True, "children": [
            {"name": "PowerPoint Document", "is_storage": False,
             "data": make_ppt_text_record("XLS_MBD_PPT_OK_中文")}
        ]},
        {"name": "MBD00004", "is_storage": True, "children": doc_emb_streams},
        {"name": "MBD0000EMPTY", "is_storage": True, "children": []},
        {"name": "ObjectPool", "is_storage": True, "children": [
            {"name": "_1", "is_storage": True, "children": [
                {"name": "\x01Ole10Native", "is_storage": False,
                 "data": make_ole10_native_stream("x.rtf", "x.rtf", make_rtf("XLS_POOL_RTF_OK_中文"))}
            ]}
        ]},
    ]
    write_fixture("host_xls_multi.xls", build_ole2_from_tree(xls_children))

    # 10. host_ppt_multi.ppt (RT_TEXT_CHARS_ATOM + ExOleObjStg raw/zlib/bomb + trailing Unicode text)
    ppt_raw_ole = build_ole2_from_tree([
        {"name": "CONTENTS", "is_storage": False, "data": make_pdf_bytes("PPT_EXOLE_RAW_PDF_OK_中文")}
    ])
    ppt_zlib_ole = build_ole2_from_tree([
        {"name": "Package", "is_storage": False, "data": make_docx_bytes("PPT_EXOLE_ZLIB_DOCX_OK_中文")}
    ])
    ppt_zlib_payload = struct.pack("<I", len(ppt_zlib_ole)) + zlib.compress(ppt_zlib_ole)
    ppt_bomb_payload = struct.pack("<I", 0x7FFFFFFF) + b"BAD_ZLIB_BOMB"
    ppt_stream = (
        make_ppt_text_record("PPT_HOST_MAIN_SLIDE_幻灯片中文") +
        make_ppt_record(0x0000, 0x1011, ppt_raw_ole) +
        make_ppt_record(0x0010, 0x1011, ppt_zlib_payload) +
        make_ppt_record(0x0010, 0x1011, ppt_bomb_payload) +
        make_ppt_text_record("PPT_AFTER_EXOLE_OK_尾页中文")
    )
    write_fixture("host_ppt_multi.ppt", build_ole2_from_tree([
        {"name": "PowerPoint Document", "is_storage": False, "data": ppt_stream}
    ]))

    # 11. host_pdf_multi.pdf (reportlab CJK TTF + pypdf EmbeddedFiles UTF-16 names + FileAttachment + image)
    pdf_gbk_note = "PDF_EF_GB18030_这是一段简体中文PDF内嵌纯文本用于字符集检测验证".encode("gb18030")
    write_fixture("host_pdf_multi.pdf", make_pdf_bytes(
        "PDF_HOST_MAIN_TEXT_中文主文档",
        attachments=[
            ("附件_文档.docx", make_docx_bytes("PDF_EF_DOCX_OK_中文附件")),
            ("att_note.rtf", make_rtf("PDF_EF_RTF_OK_中文RTF")),
            ("att_gbk.txt", pdf_gbk_note),
            ("att_pic.png", tiny_png),
        ],
        file_annotations=[
            ("批注表格.xlsx", make_xlsx_bytes("PDF_ANNOT_XLSX_OK_中文批注")),
        ],
    ))

    # 12. boundary_depth_mixed.pdf: L0(PDF) -> L1(DOCX) -> L2(PPT) -> L3(XLS) -> L4(DOCX) -> L5(TXT stop)
    l4_docx = make_docx_bytes("DEPTH_L4_MARKER_第四层中文", extra_entries=[
        ("word/embeddings/l5.bin", build_ole2_from_tree([
            {"name": "\x01Ole10Native", "is_storage": False,
             "data": make_ole10_native_stream("l5.txt", "l5.txt", "DEPTH_L5_SHOULD_STOP_第五层".encode("utf-8"))}
        ]))
    ])
    l3_xls = build_ole2_from_tree([
        {"name": "Workbook", "is_storage": False, "data": make_biff8_workbook("DEPTH_L3_MARKER_第三层中文")},
        {"name": "MBD00004", "is_storage": True, "children": [
            {"name": "Package", "is_storage": False, "data": l4_docx}
        ]},
    ])
    l2_ppt_ole = build_ole2_from_tree([
        {"name": "Package", "is_storage": False, "data": l3_xls}
    ])
    l2_ppt = build_ole2_from_tree([
        {"name": "PowerPoint Document", "is_storage": False,
         "data": (
             make_ppt_text_record("DEPTH_L2_MARKER_第二层中文") +
             make_ppt_record(0x0000, 0x1011, l2_ppt_ole)
         )}
    ])
    l1_docx = make_docx_bytes("DEPTH_L1_MARKER_第一层中文", extra_entries=[
        ("word/embeddings/l2.ppt", l2_ppt),
    ])
    write_fixture("boundary_depth_mixed.pdf", make_pdf_bytes(
        "DEPTH_L0_MARKER_零层中文", attachments=[("l1.docx", l1_docx)]
    ))

    # 13. boundary_count_and_skip.pdf: 20 archives + 2 PNGs (no callback) + 25 RTFs (01..20 ok, 21..25 skip)
    dummy_zip = build_zip_bytes([("secret.txt", "SECRET_IN_ZIP")])
    count_atts = [(f"skip_{i:02d}.zip", dummy_zip) for i in range(1, 21)]
    count_atts.append(("skip_img1.png", tiny_png))
    count_atts.append(("skip_img2.png", tiny_png))
    for i in range(1, 26):
        count_atts.append((f"item_{i:02d}.rtf", make_rtf(f"COUNT_ITEM_{i:02d}_序号")))
    write_fixture("boundary_count_and_skip.pdf", make_pdf_bytes(
        "COUNT_HOST_MAIN_计数测试", attachments=count_atts
    ))

    # 14. boundary_zip_bomb.docx: 65 MiB zero-filled compressed stream (~65 KB on disk) + surviving RTF
    bomb_stream = b"\x00" * (65 * 1024 * 1024)
    write_fixture("boundary_zip_bomb.docx", make_docx_bytes(
        "ZIP_BOMB_HOST_OK_防炸弹中文",
        extra_entries=[
            ("word/embeddings/a_bomb_65mb.bin", bomb_stream),
            ("word/embeddings/z_after_bomb.rtf", make_rtf("SURVIVED_AFTER_BOMB_OK_存活中文")),
        ],
    ))

    # 15. boundary_fault_isolation.docx: corrupt PDF + corrupt OLE + skipped zip + surviving RTF
    write_fixture("boundary_fault_isolation.docx", make_docx_bytes(
        "FAULT_HOST_OK_隔离测试",
        extra_entries=[
            ("word/embeddings/a_corrupt.pdf", b"%PDF-1.4\nGARBAGE_NO_XREF"),
            ("word/embeddings/b_corrupt.bin", b"\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1" + b"\xFF" * 64),
            ("word/embeddings/c_skip.zip", dummy_zip),
            ("word/embeddings/d_valid.rtf", make_rtf("VALID_AFTER_CORRUPT_OK_中文恢复")),
        ],
    ))


if __name__ == "__main__":
    main()
