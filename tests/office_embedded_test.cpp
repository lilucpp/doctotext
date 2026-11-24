#include "../build/doctotext_c_api.h"
#include "../build/formatting_style.h"
#include "../build/metadata.h"
#include "../build/plain_text_extractor.h"
#include "../src/doc_parser.h"
#include "../src/doctotext_unzip.h"
#include "../src/pdf_parser.h"
#include "../src/ppt_parser.h"
#include "../src/xls_parser.h"
#include "../src/xlsb_parser.h"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <zlib.h>

static void push_u16_le(std::string& dst, uint16_t v)
{
	dst.push_back((char)(v & 0xFF));
	dst.push_back((char)((v >> 8) & 0xFF));
}

static void push_u32_le(std::string& dst, uint32_t v)
{
	dst.push_back((char)(v & 0xFF));
	dst.push_back((char)((v >> 8) & 0xFF));
	dst.push_back((char)((v >> 16) & 0xFF));
	dst.push_back((char)((v >> 24) & 0xFF));
}

// Minimal uncompressed (STORE) ZIP archive builder in memory
static std::string build_zip_from_entries(const std::vector<std::pair<std::string, std::string> >& entries)
{
	std::string out;
	std::string central_dir;
	std::vector<uint32_t> local_offsets(entries.size(), 0);

	for (size_t i = 0; i < entries.size(); ++i)
	{
		const std::string& name = entries[i].first;
		const std::string& data = entries[i].second;
		local_offsets[i] = (uint32_t)out.size();
		uint32_t crc = (uint32_t)crc32(0L, (const Bytef*)data.data(), (uInt)data.size());

		// Local file header
		push_u32_le(out, 0x04034B50);
		push_u16_le(out, 20);
		push_u16_le(out, 0);
		push_u16_le(out, 0); // STORE
		push_u16_le(out, 0);
		push_u16_le(out, 0);
		push_u32_le(out, crc);
		push_u32_le(out, (uint32_t)data.size());
		push_u32_le(out, (uint32_t)data.size());
		push_u16_le(out, (uint16_t)name.size());
		push_u16_le(out, 0);
		out += name;
		out += data;

		// Central directory header
		push_u32_le(central_dir, 0x02014B50);
		push_u16_le(central_dir, 20);
		push_u16_le(central_dir, 20);
		push_u16_le(central_dir, 0);
		push_u16_le(central_dir, 0);
		push_u16_le(central_dir, 0);
		push_u16_le(central_dir, 0);
		push_u32_le(central_dir, crc);
		push_u32_le(central_dir, (uint32_t)data.size());
		push_u32_le(central_dir, (uint32_t)data.size());
		push_u16_le(central_dir, (uint16_t)name.size());
		push_u16_le(central_dir, 0);
		push_u16_le(central_dir, 0);
		push_u16_le(central_dir, 0);
		push_u16_le(central_dir, 0);
		push_u32_le(central_dir, 0);
		push_u32_le(central_dir, local_offsets[i]);
		central_dir += name;
	}

	uint32_t cd_offset = (uint32_t)out.size();
	out += central_dir;
	// End of central directory
	push_u32_le(out, 0x06054B50);
	push_u16_le(out, 0);
	push_u16_le(out, 0);
	push_u16_le(out, (uint16_t)entries.size());
	push_u16_le(out, (uint16_t)entries.size());
	push_u32_le(out, (uint32_t)central_dir.size());
	push_u32_le(out, cd_offset);
	push_u16_le(out, 0);
	return out;
}

struct OleSpecNode
{
	std::string name;
	bool is_storage;
	std::string data;
	std::vector<OleSpecNode> children;
};

static uint32_t assign_ole_tree(const OleSpecNode& node,
                                std::vector<OleSpecNode>& flat_nodes,
                                std::vector<uint32_t>& left_sib,
                                std::vector<uint32_t>& right_sib,
                                std::vector<uint32_t>& child_idx)
{
	uint32_t my_idx = (uint32_t)flat_nodes.size();
	flat_nodes.push_back(node);
	left_sib.push_back(0xFFFFFFFFu);
	right_sib.push_back(0xFFFFFFFFu);
	child_idx.push_back(0xFFFFFFFFu);

	if (node.is_storage && !node.children.empty())
	{
		std::vector<uint32_t> kids;
		for (size_t i = 0; i < node.children.size(); ++i)
			kids.push_back(assign_ole_tree(node.children[i], flat_nodes, left_sib, right_sib, child_idx));
		child_idx[my_idx] = kids[0];
		for (size_t i = 0; i + 1 < kids.size(); ++i)
			right_sib[kids[i]] = kids[i + 1];
	}
	return my_idx;
}

// Minimal 512-byte-sector OLE2 Compound File builder (mini_stream_cut_off = 0 so all streams use normal FAT sectors)
static std::string build_ole2_from_tree(const OleSpecNode& root_node)
{
	std::vector<OleSpecNode> flat;
	std::vector<uint32_t> left_sib, right_sib, child_idx;
	assign_ole_tree(root_node, flat, left_sib, right_sib, child_idx);

	// Sectors layout:
	// [0 .. num_fat-1] = FAT sectors
	// [num_fat .. num_fat+num_dir-1] = Directory sectors
	// [num_fat+num_dir ..] = Stream data sectors
	uint32_t num_dir_sectors = (uint32_t)((flat.size() + 3) / 4);
	std::vector<uint32_t> stream_start(flat.size(), 0xFFFFFFFEu);

	// Count total data sectors first
	uint32_t data_sectors = 0;
	for (size_t i = 0; i < flat.size(); ++i)
	{
		if (!flat[i].is_storage && !flat[i].data.empty())
			data_sectors += (uint32_t)((flat[i].data.size() + 511) / 512);
	}

	uint32_t num_fat_sectors = 1;
	while ((num_fat_sectors * 128u) < (num_fat_sectors + num_dir_sectors + data_sectors))
		++num_fat_sectors;

	uint32_t dir_start_sec = num_fat_sectors;
	uint32_t next_data_sec = dir_start_sec + num_dir_sectors;
	uint32_t total_sectors = next_data_sec + data_sectors;

	std::vector<uint32_t> fat(num_fat_sectors * 128u, 0xFFFFFFFFu);
	for (uint32_t s = 0; s < num_fat_sectors; ++s)
		fat[s] = 0xFFFFFFFDu; // FATSECT
	for (uint32_t d = 0; d < num_dir_sectors; ++d)
		fat[dir_start_sec + d] = (d + 1 < num_dir_sectors) ? (dir_start_sec + d + 1) : 0xFFFFFFFEu;

	std::string stream_blob(data_sectors * 512u, '\0');
	uint32_t cur_sec = next_data_sec;
	for (size_t i = 0; i < flat.size(); ++i)
	{
		if (flat[i].is_storage || flat[i].data.empty())
			continue;
		uint32_t nsec = (uint32_t)((flat[i].data.size() + 511) / 512);
		stream_start[i] = cur_sec;
		uint32_t rel_off = (cur_sec - next_data_sec) * 512u;
		std::memcpy(&stream_blob[rel_off], flat[i].data.data(), flat[i].data.size());
		for (uint32_t k = 0; k < nsec; ++k)
			fat[cur_sec + k] = (k + 1 < nsec) ? (cur_sec + k + 1) : 0xFFFFFFFEu;
		cur_sec += nsec;
	}

	// Build 512-byte CFB header
	std::string out(512, '\0');
	static const unsigned char kMagic[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
	std::memcpy(&out[0], kMagic, 8);
	// Minor/Major version, byte order, sector shift(9), mini shift(6)
	out[24] = 0x3E; out[25] = 0x00;
	out[26] = 0x03; out[27] = 0x00;
	out[28] = (char)0xFE; out[29] = (char)0xFF;
	out[30] = 0x09; out[31] = 0x00;
	out[32] = 0x06; out[33] = 0x00;
	// num_fat_sectors @ 44, first_dir @ 48, mini_cutoff = 0 @ 56, first_minifat = ENDOFCHAIN @ 60, first_difat = ENDOFCHAIN @ 68
	auto set_u32 = [&](size_t off, uint32_t v) {
		out[off + 0] = (char)(v & 0xFF);
		out[off + 1] = (char)((v >> 8) & 0xFF);
		out[off + 2] = (char)((v >> 16) & 0xFF);
		out[off + 3] = (char)((v >> 24) & 0xFF);
	};
	set_u32(44, num_fat_sectors);
	set_u32(48, dir_start_sec);
	set_u32(56, 0); // disable mini stream cutoff so all streams use normal sectors
	set_u32(60, 0xFFFFFFFEu);
	set_u32(64, 0);
	set_u32(68, 0xFFFFFFFEu);
	set_u32(72, 0);
	for (uint32_t i = 0; i < 109; ++i)
		set_u32(76 + i * 4, (i < num_fat_sectors) ? i : 0xFFFFFFFFu);

	// Append FAT sectors
	for (size_t i = 0; i < fat.size(); ++i)
		push_u32_le(out, fat[i]);

	// Append Directory sectors
	std::string dir_blob(num_dir_sectors * 512u, '\0');
	for (size_t i = 0; i < flat.size(); ++i)
	{
		size_t base = i * 128;
		size_t chars = std::min(flat[i].name.size(), (size_t)31);
		for (size_t c = 0; c < chars; ++c)
		{
			dir_blob[base + c * 2] = flat[i].name[c];
			dir_blob[base + c * 2 + 1] = 0;
		}
		uint16_t name_bytes = (uint16_t)((chars + 1) * 2);
		dir_blob[base + 64] = (char)(name_bytes & 0xFF);
		dir_blob[base + 65] = (char)((name_bytes >> 8) & 0xFF);
		dir_blob[base + 66] = (i == 0) ? 0x05 : (flat[i].is_storage ? 0x01 : 0x02);
		dir_blob[base + 67] = 0x01; // black node
		auto set_dir_u32 = [&](size_t off, uint32_t v) {
			dir_blob[base + off + 0] = (char)(v & 0xFF);
			dir_blob[base + off + 1] = (char)((v >> 8) & 0xFF);
			dir_blob[base + off + 2] = (char)((v >> 16) & 0xFF);
			dir_blob[base + off + 3] = (char)((v >> 24) & 0xFF);
		};
		set_dir_u32(68, left_sib[i]);
		set_dir_u32(72, right_sib[i]);
		set_dir_u32(76, child_idx[i]);
		set_dir_u32(116, stream_start[i]);
		set_dir_u32(120, flat[i].is_storage ? 0u : (uint32_t)flat[i].data.size());
	}
	out += dir_blob;
	out += stream_blob;
	(void)total_sectors;
	return out;
}

static std::string make_ole10_native_stream(const std::string& label,
                                            const std::string& org_path,
                                            const std::string& payload)
{
	std::string body;
	push_u16_le(body, 2); // flags1
	body += label; body.push_back('\0');
	body += org_path; body.push_back('\0');
	push_u16_le(body, 0); // flags2
	push_u16_le(body, 3); // unknown1
	std::string cmd = org_path + '\0';
	push_u32_le(body, (uint32_t)cmd.size());
	body += cmd;
	push_u32_le(body, (uint32_t)payload.size());
	body += payload;

	std::string out;
	push_u32_le(out, (uint32_t)body.size());
	out += body;
	return out;
}

static std::string build_pdf_from_objects(const std::vector<std::string>& bodies, int root_obj_num)
{
	std::string out = "%PDF-1.4\n";
	std::vector<size_t> offsets(bodies.size() + 1, 0);
	for (size_t i = 0; i < bodies.size(); ++i)
	{
		offsets[i + 1] = out.size();
		std::ostringstream oss;
		oss << (i + 1) << " 0 obj\n" << bodies[i] << "\nendobj\n";
		out += oss.str();
	}
	size_t xref_off = out.size();
	std::ostringstream xref;
	xref << "xref\n0 " << (bodies.size() + 1) << "\n0000000000 65535 f \n";
	for (size_t i = 1; i <= bodies.size(); ++i)
	{
		char line[32];
		std::snprintf(line, sizeof(line), "%010zu 00000 n \n", offsets[i]);
		xref << line;
	}
	xref << "trailer\n<< /Size " << (bodies.size() + 1) << " /Root " << root_obj_num << " 0 R >>\n"
	     << "startxref\n" << xref_off << "\n%%EOF\n";
	out += xref.str();
	return out;
}

static std::string escape_pdf_lit(const std::string& raw)
{
	std::string out;
	out.reserve(raw.size());
	for (size_t i = 0; i < raw.size(); ++i)
	{
		unsigned char c = (unsigned char)raw[i];
		if (c == '\\' || c == '(' || c == ')')
		{
			out.push_back('\\');
			out.push_back((char)c);
		}
		else if (c < 0x20 || c > 0x7E)
		{
			char oct[8];
			std::snprintf(oct, sizeof(oct), "\\%03o", (unsigned int)c);
			out += oct;
		}
		else
		{
			out.push_back((char)c);
		}
	}
	return out;
}

static std::string make_pdf_with_attachments(const std::string& page_text,
                                             const std::vector<std::pair<std::string, std::string> >& atts)
{
	std::vector<std::string> objs;
	// 1: Catalog
	objs.push_back("<< /Type /Catalog /Pages 2 0 R /Names 6 0 R >>");
	// 2: Pages
	objs.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
	// 3: Page
	objs.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>");
	// 4: Font
	objs.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
	// 5: Page Content
	{
		std::string st = "BT /F1 12 Tf 72 720 Td (" + escape_pdf_lit(page_text) + ") Tj ET";
		std::ostringstream s;
		s << "<< /Length " << st.size() << " >>\nstream\n" << st << "\nendstream";
		objs.push_back(s.str());
	}
	// 6: Names, 7: EmbeddedFiles
	objs.push_back("<< /EmbeddedFiles 7 0 R >>");
	std::ostringstream ef_names;
	ef_names << "<< /Names [";
	int next_id = 8;
	std::vector<std::string> extra_objs;
	for (size_t i = 0; i < atts.size(); ++i)
	{
		int fspec_id = next_id++;
		int stream_id = next_id++;
		ef_names << "(" << escape_pdf_lit(atts[i].first) << ") " << fspec_id << " 0 R ";
		std::ostringstream fspec;
		fspec << "<< /Type /Filespec /F (" << escape_pdf_lit(atts[i].first) << ") /EF << /F " << stream_id << " 0 R >> >>";
		extra_objs.push_back(fspec.str());
		std::ostringstream strm;
		strm << "<< /Length " << atts[i].second.size() << " >>\nstream\n" << atts[i].second << "\nendstream";
		extra_objs.push_back(strm.str());
	}
	ef_names << "] >>";
	objs.push_back(ef_names.str());
	for (size_t i = 0; i < extra_objs.size(); ++i)
		objs.push_back(extra_objs[i]);
	return build_pdf_from_objects(objs, 1);
}

static int test_step1_unified_switch_pdf()
{
	{
		doctotext::PlainTextExtractor extractor;
		std::string text_on;
		if (!extractor.processFile("testPDFEmbeddedDocx.pdf", text_on) ||
		    text_on.find("hyperlink test") == std::string::npos)
		{
			std::cerr << "FAIL: Step1 C++ default processFile(testPDFEmbeddedDocx.pdf)\n";
			return 1;
		}

		extractor.setParseEmbeddedFiles(false);
		extractor.setFormattingStyle(doctotext::FormattingStyle());
		std::string text_off;
		if (!extractor.processFile("testPDFEmbeddedDocx.pdf", text_off) ||
		    text_off.find("Main Document Text before DOCX attachment.") == std::string::npos ||
		    text_off.find("hyperlink test") != std::string::npos)
		{
			std::cerr << "FAIL: Step1 C++ disabled processFile(testPDFEmbeddedDocx.pdf)\n";
			return 1;
		}
		PDFParser direct_pdf("testPDFEmbeddedDocx.pdf");
		doctotext::FormattingStyle fmt_off;
		fmt_off.parse_embedded_files = false;
		std::string direct_off = direct_pdf.plainText(fmt_off);
		std::vector<doctotext::Attachment> atts_off;
		direct_pdf.getAttachments(atts_off);
		if (direct_off.find("hyperlink test") != std::string::npos || atts_off.empty())
		{
			std::cerr << "FAIL: Step1 PDFParser::getAttachments() should still collect attachments when parse_embedded=false\n";
			return 1;
		}
	}

	{
		doctotext::PlainTextExtractor extractor;
		std::string text_on;
		if (!extractor.processFile("testPDFFileEmbInAnnotation.pdf", text_on) ||
		    text_on.find("This is a Excel") == std::string::npos)
		{
			std::cerr << "FAIL: Step1 FileAttachment embedded xlsx expected when ON\n";
			return 1;
		}

		extractor.setParseEmbeddedFiles(false);
		std::string text_off;
		if (!extractor.processFile("testPDFFileEmbInAnnotation.pdf", text_off) ||
		    text_off.find("This is a pdf document") == std::string::npos ||
		    text_off.find("This is a Excel") != std::string::npos)
		{
			std::cerr << "FAIL: Step1 FileAttachment embedded xlsx expected skipped when OFF\n";
			return 1;
		}
	}

	{
		DocToTextExtractorParams* params_default = doctotext_create_extractor_params();
		doctotext_status status = DOCTOTEXT_STATUS_ERROR;
		DocToTextExtractedData* data_default = doctotext_process_file_ex(
			"testPDFEmbeddedDocx.pdf", params_default, &status, NULL);
		if (!data_default || status != DOCTOTEXT_STATUS_OK ||
		    std::strstr(doctotext_extracted_data_get_text(data_default), "hyperlink test") == NULL)
		{
			std::cerr << "FAIL: Step1 C API default ON failed\n";
			if (data_default) doctotext_free_extracted_data(data_default);
			doctotext_free_extractor_params(params_default);
			return 1;
		}
		doctotext_free_extracted_data(data_default);
		doctotext_free_extractor_params(params_default);

		DocToTextExtractorParams* params_off = doctotext_create_extractor_params();
		DocToTextFormattingStyle* fmt = doctotext_create_formatting_style();
		doctotext_extractor_params_set_parse_embedded_files(params_off, 0);
		doctotext_extractor_params_set_formatting_style(params_off, fmt);
		DocToTextExtractedData* data_off = doctotext_process_file_ex(
			"testPDFEmbeddedDocx.pdf", params_off, &status, NULL);
		if (!data_off || status != DOCTOTEXT_STATUS_OK)
		{
			std::cerr << "FAIL: Step1 C API OFF execution failed\n";
			if (data_off) doctotext_free_extracted_data(data_off);
			doctotext_free_formatting_style(fmt);
			doctotext_free_extractor_params(params_off);
			return 1;
		}
		const char* txt_off = doctotext_extracted_data_get_text(data_off);
		if (!txt_off ||
		    std::strstr(txt_off, "Main Document Text before DOCX attachment.") == NULL ||
		    std::strstr(txt_off, "hyperlink test") != NULL)
		{
			std::cerr << "FAIL: Step1 C API OFF failed to suppress embedded file\n";
			doctotext_free_extracted_data(data_off);
			doctotext_free_formatting_style(fmt);
			doctotext_free_extractor_params(params_off);
			return 1;
		}
		doctotext_free_extracted_data(data_off);
		doctotext_free_formatting_style(fmt);
		doctotext_free_extractor_params(params_off);
	}

	std::cout << "PASS: Step 1 unified parse_embedded_files switch (C++/C/PDF)\n";
	return 0;
}

static int test_step2_limits_isolation_and_threads()
{
	// 1. Depth limit (max_depth = 4): build 6-level nested PDF chain (L0 -> L1 -> L2 -> L3 -> L4 -> L5)
	std::string nested = make_pdf_with_attachments("LEVEL_5_SHOULD_STOP", std::vector<std::pair<std::string, std::string> >());
	for (int lv = 4; lv >= 1; --lv)
	{
		std::vector<std::pair<std::string, std::string> > atts;
		atts.push_back(std::make_pair("inner.pdf", nested));
		nested = make_pdf_with_attachments("LEVEL_" + std::to_string(lv) + "_MARKER", atts);
	}
	std::vector<std::pair<std::string, std::string> > root_atts;
	root_atts.push_back(std::make_pair("inner.pdf", nested));
	std::string root_pdf = make_pdf_with_attachments("LEVEL_0_ROOT", root_atts);

	{
		doctotext::PlainTextExtractor extractor;
		std::string text;
		if (!extractor.processFile(root_pdf.data(), root_pdf.size(), text))
		{
			std::cerr << "FAIL: Step2 nested depth PDF failed\n";
			return 1;
		}
		if (text.find("LEVEL_0_ROOT") == std::string::npos ||
		    text.find("LEVEL_1_MARKER") == std::string::npos ||
		    text.find("LEVEL_2_MARKER") == std::string::npos ||
		    text.find("LEVEL_3_MARKER") == std::string::npos ||
		    text.find("LEVEL_4_MARKER") == std::string::npos ||
		    text.find("LEVEL_5_SHOULD_STOP") != std::string::npos)
		{
			std::cerr << "FAIL: Step2 max_depth=4 guard mismatch, extracted:\n" << text << "\n";
			return 1;
		}
	}

	// 2. Count limit (max_count = 20): 25 RTF attachments in 1 PDF
	{
		std::vector<std::pair<std::string, std::string> > many;
		for (int i = 1; i <= 25; ++i)
		{
			char nm[32];
			std::snprintf(nm, sizeof(nm), "att_%02d.rtf", i);
			char body[64];
			std::snprintf(body, sizeof(body), "{\\rtf1 EMBED_ITEM_%02d}", i);
			many.push_back(std::make_pair(std::string(nm), std::string(body)));
		}
		std::string big_pdf = make_pdf_with_attachments("COUNT_LIMIT_ROOT", many);
		doctotext::PlainTextExtractor extractor;
		std::string text;
		if (!extractor.processFile(big_pdf.data(), big_pdf.size(), text))
		{
			std::cerr << "FAIL: Step2 count limit PDF failed\n";
			return 1;
		}
		if (text.find("EMBED_ITEM_01") == std::string::npos ||
		    text.find("EMBED_ITEM_20") == std::string::npos ||
		    text.find("EMBED_ITEM_21") != std::string::npos ||
		    text.find("EMBED_ITEM_25") != std::string::npos)
		{
			std::cerr << "FAIL: Step2 max_count=20 guard mismatch\n";
			return 1;
		}
	}

	// 3. Archive/image skip should not burn count budget + corrupt stream fault isolation
	{
		std::vector<std::pair<std::string, std::string> > mixed;
		for (int s = 0; s < 20; ++s)
		{
			char zname[32];
			std::snprintf(zname, sizeof(zname), "archive_%02d.zip", s);
			mixed.push_back(std::make_pair(std::string(zname), std::string("PK\x03\x04\x14\x00\x00\x00SECRET_IN_ZIP", 21)));
		}
		mixed.push_back(std::make_pair("icon.png", std::string("\x89PNG\r\n\x1A\nFAKEPNG", 15)));
		mixed.push_back(std::make_pair("broken.pdf", std::string("%PDF-1.4\nINVALID_GARBAGE_STREAM_NO_XREF", 39)));
		mixed.push_back(std::make_pair("valid.rtf", std::string("{\\rtf1 SURVIVED_VALID_CHILD}")));
		std::string mixed_pdf = make_pdf_with_attachments("ISOLATION_ROOT", mixed);

		doctotext::PlainTextExtractor extractor;
		std::string text;
		if (!extractor.processFile(mixed_pdf.data(), mixed_pdf.size(), text))
		{
			std::cerr << "FAIL: Step2 fault isolation PDF returned false\n";
			return 1;
		}
		if (text.find("ISOLATION_ROOT") == std::string::npos ||
		    text.find("SURVIVED_VALID_CHILD") == std::string::npos ||
		    text.find("SECRET_IN_ZIP") != std::string::npos)
		{
			std::cerr << "FAIL: Step2 archive skip / fault isolation mismatch, got:\n" << text << "\n";
			return 1;
		}
	}

	// 4. Multithreaded TLS isolation across threads
	{
		bool ok_flags[8];
		std::vector<std::thread> threads;
		for (int t = 0; t < 8; ++t)
		{
			ok_flags[t] = false;
			threads.emplace_back([&, t]() {
				doctotext::PlainTextExtractor ex;
				std::string txt;
				if (ex.processFile(root_pdf.data(), root_pdf.size(), txt) &&
				    txt.find("LEVEL_4_MARKER") != std::string::npos &&
				    txt.find("LEVEL_5_SHOULD_STOP") == std::string::npos)
				{
					ok_flags[t] = true;
				}
			});
		}
		for (size_t t = 0; t < threads.size(); ++t)
			threads[t].join();
		for (int t = 0; t < 8; ++t)
		{
			if (!ok_flags[t])
			{
				std::cerr << "FAIL: Step2 thread " << t << " failed TLS depth isolation\n";
				return 1;
			}
		}
	}

	std::cout << "PASS: Step 2 depth(4)/count(20) limits, archive skip, fault isolation & multithreading\n";
	return 0;
}

static int test_step3_ooxml_and_odf_embeddings()
{
	// 1. DOCX containing:
	//   - oleObject1.bin (OLE2 with CONTENTS=PDF + noise \x03EPRINT)
	//   - oleObject2.bin (OLE2 with \x01Ole10Native RTF + malformed oleObject3.bin)
	std::string inner_pdf = make_pdf_with_attachments("DOCX_EMBED_PDF_OK", std::vector<std::pair<std::string, std::string> >());
	OleSpecNode ole_pdf;
	ole_pdf.name = "Root Entry"; ole_pdf.is_storage = true;
	ole_pdf.children.push_back({"CONTENTS", false, inner_pdf, {}});
	ole_pdf.children.push_back({"\x03EPRINT", false, "EPRINT_NOISE_DO_NOT_EMIT", {}});

	OleSpecNode ole_native;
	ole_native.name = "Root Entry"; ole_native.is_storage = true;
	ole_native.children.push_back({"\x01Ole10Native", false,
		make_ole10_native_stream("note.rtf", "C:\\tmp\\note.rtf", "{\\rtf1 DOCX_NATIVE_RTF_OK}"), {}});

	// Malformed Ole10Native (overflow data_size=0xFFFFFFFF, must be skipped without crashing)
	std::string bad_native;
	push_u32_le(bad_native, 100);
	push_u16_le(bad_native, 2);
	bad_native += "bad.txt"; bad_native.push_back('\0');
	bad_native += "bad.txt"; bad_native.push_back('\0');
	push_u16_le(bad_native, 0);
	push_u16_le(bad_native, 3);
	push_u32_le(bad_native, 0);
	push_u32_le(bad_native, 0xFFFFFFFFu);
	OleSpecNode ole_bad;
	ole_bad.name = "Root Entry"; ole_bad.is_storage = true;
	ole_bad.children.push_back({"\x01Ole10Native", false, bad_native, {}});

	std::string docx_xml =
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
		"<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
		"<w:body><w:p><w:r><w:t>MAIN_DOCX_TEXT</w:t></w:r></w:p></w:body></w:document>";
	std::vector<std::pair<std::string, std::string> > docx_entries;
	docx_entries.push_back({"[Content_Types].xml", "<Types/>"});
	docx_entries.push_back({"word/document.xml", docx_xml});
	docx_entries.push_back({"word/embeddings/oleObject1.bin", build_ole2_from_tree(ole_pdf)});
	docx_entries.push_back({"word/embeddings/oleObject2.bin", build_ole2_from_tree(ole_native)});
	docx_entries.push_back({"word/embeddings/oleObject3.bin", build_ole2_from_tree(ole_bad)});
	std::string docx_buf = build_zip_from_entries(docx_entries);

	{
		doctotext::PlainTextExtractor ex;
		std::string txt;
		if (!ex.processFile(docx_buf.data(), docx_buf.size(), txt))
		{
			std::cerr << "FAIL: Step3 synthetic DOCX failed to parse\n";
			return 1;
		}
		if (txt.find("MAIN_DOCX_TEXT") == std::string::npos ||
		    txt.find("DOCX_EMBED_PDF_OK") == std::string::npos ||
		    txt.find("DOCX_NATIVE_RTF_OK") == std::string::npos ||
		    txt.find("EPRINT_NO_NOT_EMIT") != std::string::npos ||
		    txt.find("EPRINT_NOISE_DO_NOT_EMIT") != std::string::npos)
		{
			std::cerr << "FAIL: Step3 DOCX embedded extraction mismatch:\n" << txt << "\n";
			return 1;
		}
		ex.setParseEmbeddedFiles(false);
		std::string txt_off;
		if (!ex.processFile(docx_buf.data(), docx_buf.size(), txt_off) ||
		    txt_off.find("MAIN_DOCX_TEXT") == std::string::npos ||
		    txt_off.find("DOCX_EMBED_PDF_OK") != std::string::npos ||
		    txt_off.find("DOCX_NATIVE_RTF_OK") != std::string::npos)
		{
			std::cerr << "FAIL: Step3 DOCX parse_embedded=false did not skip embeddings\n";
			return 1;
		}
	}

	// 2. XLSX with OLE2 `Package` stream wrapping the synthetic DOCX above!
	{
		OleSpecNode ole_pkg;
		ole_pkg.name = "Root Entry"; ole_pkg.is_storage = true;
		ole_pkg.children.push_back({"Package", false, docx_buf, {}});

		std::vector<std::pair<std::string, std::string> > xlsx_entries;
		xlsx_entries.push_back({"xl/workbook.xml", "<workbook/>"});
		xlsx_entries.push_back({"xl/worksheets/sheet1.xml",
			"<worksheet><sheetData><row><c t=\"inlineStr\"><is><t>MAIN_XLSX_CELL</t></is></c></row></sheetData></worksheet>"});
		xlsx_entries.push_back({"xl/embeddings/oleObject1.bin", build_ole2_from_tree(ole_pkg)});
		std::string xlsx_buf = build_zip_from_entries(xlsx_entries);

		doctotext::PlainTextExtractor ex;
		std::string txt;
		if (!ex.processFile(xlsx_buf.data(), xlsx_buf.size(), txt) ||
		    txt.find("MAIN_XLSX_CELL") == std::string::npos ||
		    txt.find("MAIN_DOCX_TEXT") == std::string::npos ||
		    txt.find("DOCX_EMBED_PDF_OK") == std::string::npos)
		{
			std::cerr << "FAIL: Step3 XLSX->Package(DOCX)->PDF recursive extraction failed:\n" << txt << "\n";
			return 1;
		}
	}

	// 3. PPTX with direct embedded RTF + direct embedded PDF
	{
		std::vector<std::pair<std::string, std::string> > pptx_entries;
		pptx_entries.push_back({"ppt/presentation.xml", "<p:presentation/>"});
		pptx_entries.push_back({"ppt/slides/slide1.xml",
			"<p:sld><p:cSld><p:spTree><p:sp><p:txBody><a:p><a:r><a:t>MAIN_PPTX_SLIDE</a:t></a:r></a:p></p:txBody></p:sp></p:spTree></p:cSld></p:sld>"});
		pptx_entries.push_back({"ppt/embeddings/child.rtf", "{\\rtf1 PPTX_EMBED_RTF}"});
		std::string pptx_buf = build_zip_from_entries(pptx_entries);

		doctotext::PlainTextExtractor ex;
		std::string txt;
		if (!ex.processFile(pptx_buf.data(), pptx_buf.size(), txt) ||
		    txt.find("MAIN_PPTX_SLIDE") == std::string::npos ||
		    txt.find("PPTX_EMBED_RTF") == std::string::npos)
		{
			std::cerr << "FAIL: Step3 PPTX direct embedding failed:\n" << txt << "\n";
			return 1;
		}
	}

	// 4. ODT with <draw:object> (Object 1/content.xml) + binary OLE2 object ("Object 2") + ignored ObjectReplacements
	{
		std::string odt_content =
			"<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
			"<office:document-content xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
			"xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" "
			"xmlns:draw=\"urn:oasis:names:tc:opendocument:xmlns:drawing:1.0\" "
			"xmlns:xlink=\"http://www.w3.org/1999/xlink\">"
			"<office:body><office:text><text:p>MAIN_ODT_TEXT</text:p>"
			"<draw:frame><draw:object xlink:href=\"./Object 1\"/></draw:frame>"
			"</office:text></office:body></office:document-content>";
		std::string obj1_xml =
			"<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
			"<office:document-content xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
			"xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\">"
			"<office:body><office:text><text:p>ODT_OBJ1_XML</text:p></office:text></office:body></office:document-content>";

		std::vector<std::pair<std::string, std::string> > odt_entries;
		odt_entries.push_back({"mimetype", "application/vnd.oasis.opendocument.text"});
		odt_entries.push_back({"content.xml", odt_content});
		odt_entries.push_back({"Object 1/content.xml", obj1_xml});
		odt_entries.push_back({"Object 2", build_ole2_from_tree(ole_native)});
		odt_entries.push_back({"ObjectReplacements/Object 1", "PREVIEW_NOISE_SHOULD_SKIP"});
		std::string odt_buf = build_zip_from_entries(odt_entries);

		doctotext::PlainTextExtractor ex;
		std::string txt;
		if (!ex.processFile(odt_buf.data(), odt_buf.size(), txt) ||
		    txt.find("MAIN_ODT_TEXT") == std::string::npos ||
		    txt.find("ODT_OBJ1_XML") == std::string::npos ||
		    txt.find("DOCX_NATIVE_RTF_OK") == std::string::npos ||
		    txt.find("PREVIEW_NOISE_SHOULD_SKIP") != std::string::npos)
		{
			std::cerr << "FAIL: Step3 ODT embedded extraction mismatch:\n" << txt << "\n";
			return 1;
		}

		ex.setParseEmbeddedFiles(false);
		std::string txt_off;
		if (!ex.processFile(odt_buf.data(), odt_buf.size(), txt_off) ||
		    txt_off.find("MAIN_ODT_TEXT") == std::string::npos ||
		    txt_off.find("ODT_OBJ1_XML") != std::string::npos ||
		    txt_off.find("DOCX_NATIVE_RTF_OK") != std::string::npos)
		{
			std::cerr << "FAIL: Step3 ODT parse_embedded=false failed to skip ODF objects\n";
			return 1;
		}
	}

	std::cout << "PASS: Step 3 OOXML (DOCX/XLSX/PPTX) & ODF (ODT) embedded extraction\n";
	return 0;
}

static int test_step4_doc_objectpool()
{
	doctotext::PlainTextExtractor ex;
	std::string txt_on;
	if (!ex.processFile("embedded_spreadsheet.doc", txt_on))
	{
		std::cerr << "FAIL: Step4 DOC processFile(embedded_spreadsheet.doc) failed\n";
		return 1;
	}
	if (txt_on.find("First embedded spreadsheet:") == std::string::npos ||
	    txt_on.find("A1\tB1") == std::string::npos ||
	    txt_on.find("aę\tŚĆ") == std::string::npos)
	{
		std::cerr << "FAIL: Step4 DOC embedded spreadsheet missing when ON:\n" << txt_on << "\n";
		return 1;
	}

	ex.setParseEmbeddedFiles(false);
	std::string txt_off;
	if (!ex.processFile("embedded_spreadsheet.doc", txt_off))
	{
		std::cerr << "FAIL: Step4 DOC parse_embedded=false failed\n";
		return 1;
	}
	if (txt_off.find("First embedded spreadsheet:") == std::string::npos ||
	    txt_off.find("A1\tB1") != std::string::npos)
	{
		std::cerr << "FAIL: Step4 DOC parse_embedded=false did not skip embedded Workbook:\n" << txt_off << "\n";
		return 1;
	}

	std::cout << "PASS: Step 4 DOC ObjectPool embedded extraction & switch\n";
	return 0;
}

static std::string make_minimal_biff8_workbook(const std::string& cell_str)
{
	auto push_rec = [](std::string& dst, uint16_t type, const std::string& body) {
		push_u16_le(dst, type);
		push_u16_le(dst, (uint16_t)body.size());
		dst += body;
	};
	std::string wb;
	// BOF (0x0809): BIFF8 (0x0600), Worksheet (0x0010)
	std::string bof;
	push_u16_le(bof, 0x0600);
	push_u16_le(bof, 0x0010);
	for (int i = 0; i < 12; ++i) bof.push_back('\0');
	push_rec(wb, 0x0809, bof);

	// LABEL (0x0204): row(2) col(2) xf(2) len(2) flags(1=0) chars
	std::string lbl;
	push_u16_le(lbl, 0);
	push_u16_le(lbl, 0);
	push_u16_le(lbl, 0);
	push_u16_le(lbl, (uint16_t)cell_str.size());
	lbl.push_back('\0'); // 8-bit compressed ASCII
	lbl += cell_str;
	push_rec(wb, 0x0204, lbl);

	push_rec(wb, 0x000A, ""); // EOF
	return wb;
}

static int test_step5_xls_mbd_and_objectpool()
{
	std::string inner_pdf = make_pdf_with_attachments("XLS_MBD_PDF_OK", std::vector<std::pair<std::string, std::string> >());

	OleSpecNode mbd_pdf;
	mbd_pdf.name = "MBD00001234"; mbd_pdf.is_storage = true;
	mbd_pdf.children.push_back({"CONTENTS", false, inner_pdf, {}});

	OleSpecNode mbd_empty;
	mbd_empty.name = "MBD0000EMPTY"; mbd_empty.is_storage = true; // edge case: empty MBD storage

	std::string inner_ppt_stream;
	push_u16_le(inner_ppt_stream, 0x0000);
	push_u16_le(inner_ppt_stream, 0x0FA8); // RT_TEXT_BYTES_ATOM
	push_u32_le(inner_ppt_stream, 21);
	inner_ppt_stream += "XLS_MBD_DIRECT_PPT_OK";
	OleSpecNode mbd_ppt;
	mbd_ppt.name = "MBD0000PPT1"; mbd_ppt.is_storage = true;
	mbd_ppt.children.push_back({"PowerPoint Document", false, inner_ppt_stream, {}});

	OleSpecNode pool_item;
	pool_item.name = "_111"; pool_item.is_storage = true;
	pool_item.children.push_back({"\x01Ole10Native", false,
		make_ole10_native_stream("note.rtf", "note.rtf", "{\\rtf1 XLS_POOL_RTF_OK}"), {}});

	OleSpecNode pool_dir;
	pool_dir.name = "ObjectPool"; pool_dir.is_storage = true;
	pool_dir.children.push_back(pool_item);

	OleSpecNode root;
	root.name = "Root Entry"; root.is_storage = true;
	root.children.push_back({"Workbook", false, make_minimal_biff8_workbook("MAIN_XLS_CELL"), {}});
	root.children.push_back(mbd_pdf);
	root.children.push_back(mbd_empty);
	root.children.push_back(mbd_ppt);
	root.children.push_back(pool_dir);

	std::string xls_buf = build_ole2_from_tree(root);

	doctotext::PlainTextExtractor ex;
	std::string txt_on;
	if (!ex.processFile(xls_buf.data(), xls_buf.size(), txt_on) ||
	    txt_on.find("MAIN_XLS_CELL") == std::string::npos ||
	    txt_on.find("XLS_MBD_PDF_OK") == std::string::npos ||
	    txt_on.find("XLS_MBD_DIRECT_PPT_OK") == std::string::npos ||
	    txt_on.find("XLS_POOL_RTF_OK") == std::string::npos)
	{
		std::cerr << "FAIL: Step5 XLS MBD/ObjectPool extraction failed:\n" << txt_on << "\n";
		return 1;
	}

	ex.setParseEmbeddedFiles(false);
	std::string txt_off;
	if (!ex.processFile(xls_buf.data(), xls_buf.size(), txt_off) ||
	    txt_off.find("MAIN_XLS_CELL") == std::string::npos ||
	    txt_off.find("XLS_MBD_PDF_OK") != std::string::npos ||
	    txt_off.find("XLS_MBD_DIRECT_PPT_OK") != std::string::npos ||
	    txt_off.find("XLS_POOL_RTF_OK") != std::string::npos)
	{
		std::cerr << "FAIL: Step5 XLS parse_embedded=false did not skip embeddings:\n" << txt_off << "\n";
		return 1;
	}

	std::cout << "PASS: Step 5 XLS MBD*/ObjectPool embedded extraction & switch\n";
	return 0;
}

static int test_step6_ppt_exoleobjstg()
{
	auto push_ppt_rec = [](std::string& dst, uint16_t ver_inst, uint16_t rec_type, const std::string& body) {
		push_u16_le(dst, ver_inst);
		push_u16_le(dst, rec_type);
		push_u32_le(dst, (uint32_t)body.size());
		dst += body;
	};

	// Embedded OLE 1: uncompressed (rec_inst = 0 -> ver_inst = 0x0000) containing CONTENTS PDF
	std::string pdf_raw = make_pdf_with_attachments("PPT_RAW_EXOLE_OK", std::vector<std::pair<std::string, std::string> >());
	OleSpecNode ole1;
	ole1.name = "Root Entry"; ole1.is_storage = true;
	ole1.children.push_back({"CONTENTS", false, pdf_raw, {}});
	std::string ole1_bytes = build_ole2_from_tree(ole1);

	// Embedded OLE 2: zlib-compressed (rec_inst = 1 -> ver_inst = 0x0010) containing Ole10Native RTF
	OleSpecNode ole2;
	ole2.name = "Root Entry"; ole2.is_storage = true;
	ole2.children.push_back({"\x01Ole10Native", false,
		make_ole10_native_stream("slide_note.rtf", "slide_note.rtf", "{\\rtf1 PPT_ZLIB_EXOLE_OK}"), {}});
	std::string ole2_bytes = build_ole2_from_tree(ole2);

	uLongf comp_bound = compressBound((uLong)ole2_bytes.size());
	std::string comp_payload;
	push_u32_le(comp_payload, (uint32_t)ole2_bytes.size());
	std::vector<unsigned char> comp_buf(comp_bound);
	uLongf comp_len = comp_bound;
	if (compress2(comp_buf.data(), &comp_len,
	              reinterpret_cast<const Bytef*>(ole2_bytes.data()), (uLong)ole2_bytes.size(),
	              Z_DEFAULT_COMPRESSION) != Z_OK)
	{
		std::cerr << "FAIL: Step6 test compress2 failed\n";
		return 1;
	}
	comp_payload.append(reinterpret_cast<const char*>(comp_buf.data()), (size_t)comp_len);

	// Embedded OLE 2b: Z_SYNC_FLUSH-terminated zlib (no Z_FINISH block, ending in 00 00 FF FF as in MS-PPT)
	OleSpecNode ole2_sync;
	ole2_sync.name = "Root Entry"; ole2_sync.is_storage = true;
	ole2_sync.children.push_back({"\x01Ole10Native", false,
		make_ole10_native_stream("sync.rtf", "sync.rtf", "{\\rtf1 PPT_ZSYNC_EXOLE_OK}"), {}});
	std::string ole2_sync_bytes = build_ole2_from_tree(ole2_sync);
	std::string sync_payload;
	push_u32_le(sync_payload, (uint32_t)ole2_sync_bytes.size());
	{
		z_stream dz;
		std::memset(&dz, 0, sizeof(dz));
		if (deflateInit(&dz, Z_DEFAULT_COMPRESSION) != Z_OK)
		{
			std::cerr << "FAIL: Step6 test deflateInit failed\n";
			return 1;
		}
		std::vector<unsigned char> sbuf(ole2_sync_bytes.size() + 64);
		dz.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(ole2_sync_bytes.data()));
		dz.avail_in = (uInt)ole2_sync_bytes.size();
		dz.next_out = sbuf.data();
		dz.avail_out = (uInt)sbuf.size();
		int dret = deflate(&dz, Z_SYNC_FLUSH);
		deflateEnd(&dz);
		if (dret != Z_OK || dz.avail_in != 0 || dz.total_out < 4)
		{
			std::cerr << "FAIL: Step6 test deflate Z_SYNC_FLUSH failed\n";
			return 1;
		}
		sync_payload.append(reinterpret_cast<const char*>(sbuf.data()), (size_t)dz.total_out);
	}

	// Embedded OLE 3 (boundary bomb): fake uncompressed_size = 0x7FFFFFFF -> must safely reject
	std::string bomb_payload;
	push_u32_le(bomb_payload, 0x7FFFFFFFu);
	bomb_payload += "BAD_ZLIB_DATA";

	std::string ppt_stream;
	// Main slide text record: RT_TEXT_BYTES_ATOM (0x0FA8)
	push_ppt_rec(ppt_stream, 0x0000, 0x0FA8, "MAIN_PPT_SLIDE");
	// Uncompressed ExOleObjStg (0x1011, rec_inst=0)
	push_ppt_rec(ppt_stream, 0x0000, 0x1011, ole1_bytes);
	// Zlib-compressed ExOleObjStg (0x1011, rec_inst=1 -> ver_inst=0x0010)
	push_ppt_rec(ppt_stream, 0x0010, 0x1011, comp_payload);
	// Z_SYNC_FLUSH ExOleObjStg (0x1011, rec_inst=1 -> ver_inst=0x0010)
	push_ppt_rec(ppt_stream, 0x0010, 0x1011, sync_payload);
	// Zlib bomb ExOleObjStg (0x1011, rec_inst=1)
	push_ppt_rec(ppt_stream, 0x0010, 0x1011, bomb_payload);
	// Trailing slide text after ExOleObjStg (verifies stream cursor stayed aligned!)
	push_ppt_rec(ppt_stream, 0x0000, 0x0FA8, "AFTER_EXOLE_PPT_TEXT");

	OleSpecNode ppt_root;
	ppt_root.name = "Root Entry"; ppt_root.is_storage = true;
	ppt_root.children.push_back({"PowerPoint Document", false, ppt_stream, {}});
	std::string ppt_buf = build_ole2_from_tree(ppt_root);

	doctotext::PlainTextExtractor ex;
	std::string txt_on;
	if (!ex.processFile(ppt_buf.data(), ppt_buf.size(), txt_on) ||
	    txt_on.find("MAIN_PPT_SLIDE") == std::string::npos ||
	    txt_on.find("PPT_RAW_EXOLE_OK") == std::string::npos ||
	    txt_on.find("PPT_ZLIB_EXOLE_OK") == std::string::npos ||
	    txt_on.find("PPT_ZSYNC_EXOLE_OK") == std::string::npos ||
	    txt_on.find("AFTER_EXOLE_PPT_TEXT") == std::string::npos)
	{
		std::cerr << "FAIL: Step6 PPT ExOleObjStg extraction mismatch:\n" << txt_on << "\n";
		return 1;
	}

	ex.setParseEmbeddedFiles(false);
	std::string txt_off;
	if (!ex.processFile(ppt_buf.data(), ppt_buf.size(), txt_off) ||
	    txt_off.find("MAIN_PPT_SLIDE") == std::string::npos ||
	    txt_off.find("AFTER_EXOLE_PPT_TEXT") == std::string::npos ||
	    txt_off.find("PPT_RAW_EXOLE_OK") != std::string::npos ||
	    txt_off.find("PPT_ZLIB_EXOLE_OK") != std::string::npos ||
	    txt_off.find("PPT_ZSYNC_EXOLE_OK") != std::string::npos)
	{
		std::cerr << "FAIL: Step6 PPT parse_embedded=false did not skip ExOleObjStg:\n" << txt_off << "\n";
		return 1;
	}

	std::cout << "PASS: Step 6 PPT 0x1011 ExOleObjStg (raw/zlib/zsync/bomb/switch)\n";
	return 0;
}

static int test_step7_xlsb_velvet_rejection()
{
	XLSBParser xlsb("embedded_fixtures/velvet_standard.xlsx");
	if (xlsb.isXLSB() || xlsb.encrypted())
	{
		std::cerr << "FAIL: Step7 XLSBParser must reject VelvetSweatshop XLSX (isXLSB="
		          << xlsb.isXLSB() << ", encrypted=" << xlsb.encrypted() << ")\n";
		return 1;
	}
	XLSBParser xlsb_locked("embedded_fixtures/locked_agile_userpw.xlsx");
	if (xlsb_locked.isXLSB() || !xlsb_locked.encrypted() || xlsb_locked.error())
	{
		std::cerr << "FAIL: Step7 XLSBParser probe on encrypted file must not set error (isXLSB="
		          << xlsb_locked.isXLSB() << ", encrypted=" << xlsb_locked.encrypted()
		          << ", error=" << xlsb_locked.error() << ")\n";
		return 1;
	}
	doctotext::PlainTextExtractor ex;
	std::string text;
	if (!ex.processFile("embedded_fixtures/velvet_standard.xlsx", text) ||
	    text.find("VELVET_STD_XLSX_OK") == std::string::npos)
	{
		std::cerr << "FAIL: Step7 PlainTextExtractor failed on velvet_standard.xlsx: " << text << "\n";
		return 1;
	}
	std::cout << "PASS: Step 7 XLSBParser rejects VelvetSweatshop XLSX & keeps probe error=false on locked files\n";
	return 0;
}

static int test_step8_excel_edge_formats()
{
	doctotext::PlainTextExtractor ex;

	// 8.1 Case-insensitive OLE stream name ("WORKBOOK")
	{
		OleSpecNode root;
		root.name = "Root Entry";
		root.is_storage = true;
		root.children.push_back({"WORKBOOK", false, make_minimal_biff8_workbook("UPPERCASE_WORKBOOK_OK"), {}});
		std::string xls_buf = build_ole2_from_tree(root);
		std::string txt;
		if (!ex.processFile(xls_buf.data(), xls_buf.size(), txt) ||
		    txt.find("UPPERCASE_WORKBOOK_OK") == std::string::npos)
		{
			std::cerr << "FAIL: Step8 uppercase WORKBOOK stream failed: " << txt << "\n";
			return 1;
		}
	}

	// 8.2 Pre-OLE2 raw BIFF4 stream (0x0409 BOF len=6, 0x0204 LABEL, 0x000A EOF)
	{
		std::string biff4;
		// BOF BIFF4 (0x0409, len=6): ver=0x0000, type=0x0010 (worksheet), build=0
		push_u16_le(biff4, 0x0409);
		push_u16_le(biff4, 6);
		push_u16_le(biff4, 0x0000);
		push_u16_le(biff4, 0x0010);
		push_u16_le(biff4, 0x0000);
		// LABEL (0x0204): row=0, col=0, ixfe=0, cch(2)=11, ASCII="BIFF4_RAW_OK"
		const std::string label = "BIFF4_RAW_OK";
		push_u16_le(biff4, 0x0204);
		push_u16_le(biff4, (uint16_t)(8 + label.size()));
		push_u16_le(biff4, 0);
		push_u16_le(biff4, 0);
		push_u16_le(biff4, 0);
		push_u16_le(biff4, (uint16_t)label.size());
		biff4 += label;
		// EOF (0x000A, len=0)
		push_u16_le(biff4, 0x000A);
		push_u16_le(biff4, 0);

		std::string txt;
		if (!ex.processFile(biff4.data(), biff4.size(), txt) ||
		    txt.find("BIFF4_RAW_OK") == std::string::npos)
		{
			std::cerr << "FAIL: Step8 raw BIFF4 extraction failed: " << txt << "\n";
			return 1;
		}
	}

	// 8.3 XLSX SmartArt diagramData (empty sheetData + xl/diagrams/data1.xml via drawing rels)
	{
		std::vector<std::pair<std::string, std::string> > entries;
		entries.push_back({"xl/workbook.xml", "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"/>"});
		entries.push_back({"xl/worksheets/sheet1.xml",
			"<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
			"xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
			"<sheetData/><drawing r:id=\"rId1\"/></worksheet>"});
		entries.push_back({"xl/worksheets/_rels/sheet1.xml.rels",
			"<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
			"<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/drawing\" "
			"Target=\"../drawings/drawing1.xml\"/></Relationships>"});
		entries.push_back({"xl/drawings/drawing1.xml",
			"<xdr:wsDr xmlns:xdr=\"http://schemas.openxmlformats.org/drawingml/2006/spreadsheetDrawing\"/>"});
		entries.push_back({"xl/drawings/_rels/drawing1.xml.rels",
			"<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
			"<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/diagramData\" "
			"Target=\"../diagrams/data1.xml\"/></Relationships>"});
		entries.push_back({"xl/diagrams/data1.xml",
			"<dgm:dataModel xmlns:dgm=\"http://schemas.openxmlformats.org/drawingml/2006/diagram\" "
			"xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\">"
			"<dgm:ptLst><dgm:pt><dgm:t><a:p><a:r><a:t>SMARTART_DIAG_OK</a:t></a:r></a:p></dgm:t></dgm:pt></dgm:ptLst></dgm:dataModel>"});
		std::string xlsx_buf = build_zip_from_entries(entries);
		std::string txt;
		if (!ex.processFile(xlsx_buf.data(), xlsx_buf.size(), txt) ||
		    txt.find("SMARTART_DIAG_OK") == std::string::npos)
		{
			std::cerr << "FAIL: Step8 XLSX SmartArt diagramData failed: " << txt << "\n";
			return 1;
		}
	}

	// 8.4 Mac XLSX embedded PDF inside xl/media/image1.emf EMR_COMMENT_MULTIFORMATS
	{
		std::string pdf = make_pdf_with_attachments("MAC_EMF_EMBED_PDF_OK", std::vector<std::pair<std::string, std::string> >());
		// Pad PDF length to 4-byte boundary cleanly via EMF record size
		size_t pdf_pad = (4 - (pdf.size() % 4)) % 4;
		std::string emf;
		// Minimal EMR_HEADER (0x01, 108 bytes)
		push_u32_le(emf, 0x01);
		push_u32_le(emf, 108);
		emf.append(100, '\0');
		// EMR_COMMENT (0x46): 12 + 44 header bytes before pdf + pdf + pad
		uint32_t data_size = (uint32_t)(44 + pdf.size() + pdf_pad);
		uint32_t rec_size = 12 + data_size;
		push_u32_le(emf, 0x46);
		push_u32_le(emf, rec_size);
		push_u32_le(emf, data_size);
		emf += "GDIC";
		push_u32_le(emf, 0x40000004U); // EMR_COMMENT_MULTIFORMATS
		emf.append(16, '\0');           // OutputRect
		push_u32_le(emf, 1);            // CountFormats = 1
		emf += " FDP";                  // Signature ('PDF ' little-endian)
		push_u32_le(emf, 1);            // nVersion
		push_u32_le(emf, (uint32_t)pdf.size());
		push_u32_le(emf, 44);           // offData relative to rec+12
		emf += pdf;
		emf.append(pdf_pad, '\0');
		// EMR_EOF (0x0E, 20 bytes)
		push_u32_le(emf, 0x0E);
		push_u32_le(emf, 20);
		emf.append(12, '\0');

		std::vector<std::pair<std::string, std::string> > entries;
		entries.push_back({"xl/workbook.xml", "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"/>"});
		entries.push_back({"xl/worksheets/sheet1.xml", "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheetData/></worksheet>"});
		entries.push_back({"xl/media/image1.emf", emf});
		std::string xlsx_buf = build_zip_from_entries(entries);
		std::string txt;
		if (!ex.processFile(xlsx_buf.data(), xlsx_buf.size(), txt) ||
		    txt.find("MAC_EMF_EMBED_PDF_OK") == std::string::npos)
		{
			std::cerr << "FAIL: Step8 Mac XLSX EMF PDF extraction failed: " << txt << "\n";
			return 1;
		}

		// 8.4b parse_embedded_files=false must skip EMF embedded PDF
		doctotext::PlainTextExtractor ex_off;
		ex_off.setParseEmbeddedFiles(false);
		std::string txt_off;
		if (!ex_off.processFile(xlsx_buf.data(), xlsx_buf.size(), txt_off) ||
		    txt_off.find("MAC_EMF_EMBED_PDF_OK") != std::string::npos)
		{
			std::cerr << "FAIL: Step8 EMF PDF extracted despite parse_embedded_files=false: " << txt_off << "\n";
			return 1;
		}
	}

	// 8.5 BIFF4W multi-sheet raw stream (0x0409 BOF -> EOF -> 0x0409 BOF -> EOF)
	{
		std::string biff4w;
		auto append_sheet = [&](const std::string& label) {
			push_u16_le(biff4w, 0x0409); // BOF_BIFF_4
			push_u16_le(biff4w, 6);
			push_u16_le(biff4w, 0x0000);
			push_u16_le(biff4w, 0x0010);
			push_u16_le(biff4w, 0x0000);
			push_u16_le(biff4w, 0x0204); // XLS_LABEL
			push_u16_le(biff4w, (uint16_t)(8 + label.size()));
			push_u16_le(biff4w, 0);
			push_u16_le(biff4w, 0);
			push_u16_le(biff4w, 0);
			push_u16_le(biff4w, (uint16_t)label.size());
			biff4w += label;
			push_u16_le(biff4w, 0x000A); // EOF
			push_u16_le(biff4w, 0);
		};
		append_sheet("BIFF4W_SHEET1_OK");
		append_sheet("BIFF4W_SHEET2_OK");

		std::string txt;
		if (!ex.processFile(biff4w.data(), biff4w.size(), txt) ||
		    txt.find("BIFF4W_SHEET1_OK") == std::string::npos ||
		    txt.find("BIFF4W_SHEET2_OK") == std::string::npos)
		{
			std::cerr << "FAIL: Step8 BIFF4W multi-sheet extraction failed: " << txt << "\n";
			return 1;
		}
	}

	// 8.6 DOCX SmartArt fallback scan (word/diagrams/data1.xml without rels link)
	{
		std::vector<std::pair<std::string, std::string> > entries;
		entries.push_back({"word/document.xml",
			"<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
			"<w:body><w:p><w:r><w:t>DOCX_MAIN_OK</w:t></w:r></w:p></w:body></w:document>"});
		entries.push_back({"word/diagrams/data1.xml",
			"<dgm:dataModel xmlns:dgm=\"http://schemas.openxmlformats.org/drawingml/2006/diagram\" "
			"xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\">"
			"<dgm:ptLst><dgm:pt><dgm:t><a:p><a:r><a:t>DOCX_SMARTART_FALLBACK_OK</a:t></a:r></a:p></dgm:t></dgm:pt></dgm:ptLst></dgm:dataModel>"});
		std::string docx_buf = build_zip_from_entries(entries);
		std::string txt;
		if (!ex.processFile(docx_buf.data(), docx_buf.size(), txt) ||
		    txt.find("DOCX_MAIN_OK") == std::string::npos ||
		    txt.find("DOCX_SMARTART_FALLBACK_OK") == std::string::npos)
		{
			std::cerr << "FAIL: Step8 DOCX SmartArt fallback scan failed: " << txt << "\n";
			return 1;
		}
	}

	// 8.7 OLE embedded ObjectPool item containing a sub-storage (not stream) named "WORKBOOK" + \x01Ole10Native
	{
		OleSpecNode fake_workbook_dir;
		fake_workbook_dir.name = "WORKBOOK";
		fake_workbook_dir.is_storage = true;

		OleSpecNode pool_item;
		pool_item.name = "_222";
		pool_item.is_storage = true;
		pool_item.children.push_back(fake_workbook_dir);
		pool_item.children.push_back({"\x01Ole10Native", false,
			make_ole10_native_stream("note.rtf", "note.rtf", "{\\rtf1 WORKBOOK_DIR_FALLTHROUGH_OK}"), {}});

		OleSpecNode pool_dir;
		pool_dir.name = "ObjectPool";
		pool_dir.is_storage = true;
		pool_dir.children.push_back(pool_item);

		OleSpecNode root;
		root.name = "Root Entry";
		root.is_storage = true;
		root.children.push_back({"Workbook", false, make_minimal_biff8_workbook("PARENT_CELL"), {}});
		root.children.push_back(pool_dir);

		std::string xls_buf = build_ole2_from_tree(root);
		std::string txt;
		if (!ex.processFile(xls_buf.data(), xls_buf.size(), txt) ||
		    txt.find("WORKBOOK_DIR_FALLTHROUGH_OK") == std::string::npos)
		{
			std::cerr << "FAIL: Step8 storage named WORKBOOK blocked Ole10Native fallback: " << txt << "\n";
			return 1;
		}
	}

	// 8.8 Positive raw BIFF2 (0x0009 + 0x0004 XLS_LABEL_BIFF2) via on-disk FileStream, plus raw BIFF3 (0x0209)
	{
		std::string biff2;
		push_u16_le(biff2, 0x0009); push_u16_le(biff2, 4);
		push_u16_le(biff2, 0x0000); push_u16_le(biff2, 0x0010);
		const std::string lbl2 = "BIFF2_FILE_OK";
		push_u16_le(biff2, 0x0004);
		push_u16_le(biff2, (uint16_t)(8 + lbl2.size()));
		push_u16_le(biff2, 0);
		push_u16_le(biff2, 0);
		biff2.append(3, '\0');
		biff2.push_back((char)lbl2.size());
		biff2 += lbl2;
		push_u16_le(biff2, 0x000A); push_u16_le(biff2, 0);

		const char* tmp_path = "tmp_step8_biff2_test.xls";
		FILE* f = std::fopen(tmp_path, "wb");
		if (!f)
		{
			std::cerr << "FAIL: Step8 could not create temp file\n";
			return 1;
		}
		std::fwrite(biff2.data(), 1, biff2.size(), f);
		std::fclose(f);

		XLSParser disk_parser(tmp_path);
		bool is_xls = disk_parser.isXLS();
		std::string disk_txt = disk_parser.plainText(doctotext::FormattingStyle());
		std::remove(tmp_path);
		if (!is_xls || disk_parser.error() || disk_txt.find("BIFF2_FILE_OK") == std::string::npos)
		{
			std::cerr << "FAIL: Step8 raw BIFF2 FileStream extraction failed: " << disk_txt << "\n";
			return 1;
		}

		// BIFF3 (0x0209, len=6) + empty XLS_LABEL + XLS_DATE_1904 formatted XLS_NUMBER
		std::string biff3;
		push_u16_le(biff3, 0x0209); push_u16_le(biff3, 6);
		push_u16_le(biff3, 0x0000); push_u16_le(biff3, 0x0010); push_u16_le(biff3, 0x0000);
		// DATE_1904 (0x0022, len=2, flag=1) -> shift=24107.0
		push_u16_le(biff3, 0x0022); push_u16_le(biff3, 2); push_u16_le(biff3, 1);
		// XLS_XF_BIFF4 (0x0443, len=2, font=0, numFmt=0x0E %m-%d-%y)
		push_u16_le(biff3, 0x0443); push_u16_le(biff3, 2); biff3.push_back(0); biff3.push_back(0x0E);
		// Empty BIFF3 XLS_LABEL (cch=0, rec_len=8, no flags byte)
		push_u16_le(biff3, 0x0204); push_u16_le(biff3, 8);
		push_u16_le(biff3, 0); push_u16_le(biff3, 0); push_u16_le(biff3, 0); push_u16_le(biff3, 0);
		// Non-empty BIFF3 XLS_LABEL
		const std::string lbl3 = "BIFF3_RAW_OK";
		push_u16_le(biff3, 0x0204); push_u16_le(biff3, (uint16_t)(8 + lbl3.size()));
		push_u16_le(biff3, 0); push_u16_le(biff3, 1); push_u16_le(biff3, 0); push_u16_le(biff3, (uint16_t)lbl3.size());
		biff3 += lbl3;
		// XLS_NUMBER (0x0203) at row=1, col=0, xf=0, val=1462.0 (1904+1462 days = 1908-01-02)
		push_u16_le(biff3, 0x0203); push_u16_le(biff3, 14);
		push_u16_le(biff3, 1); push_u16_le(biff3, 0); push_u16_le(biff3, 0);
		double dval = 1462.0;
		unsigned char dbytes[8];
		std::memcpy(dbytes, &dval, 8);
		biff3.append((const char*)dbytes, 8);
		push_u16_le(biff3, 0x000A); push_u16_le(biff3, 0);

		std::string biff3_txt;
		if (!ex.processFile(biff3.data(), biff3.size(), biff3_txt) ||
		    biff3_txt.find("BIFF3_RAW_OK") == std::string::npos ||
		    biff3_txt.find("01-02-08") == std::string::npos)
		{
			std::cerr << "FAIL: Step8 raw BIFF3 + DATE_1904 extraction failed: " << biff3_txt << "\n";
			return 1;
		}
	}

	// 8.9 BIFF8 SST + CONTINUE (rich-text trailing block spanning across CONTINUE + empty CONTINUE + second string)
	{
		std::string sst_stream;
		// BOF BIFF8 (0x0809, len=16)
		push_u16_le(sst_stream, 0x0809); push_u16_le(sst_stream, 16);
		push_u16_le(sst_stream, 0x0600); push_u16_le(sst_stream, 0x0005);
		sst_stream.append(12, '\0');

		// SST record (0x00FC): cstTotal=2, cstUnique=2, str0=("AB", rich=1 (4 trailing bytes, 2 in SST, 2 in CONTINUE))
		std::string sst_rec;
		push_u32_le(sst_rec, 2); // cstTotal
		push_u32_le(sst_rec, 2); // cstUnique
		push_u16_le(sst_rec, 2); // cch = 2
		sst_rec.push_back(0x08); // flags: rich-text, 8-bit
		push_u16_le(sst_rec, 1); // cRun = 1 -> 4 trailing bytes
		sst_rec += "AB";         // 2 chars
		sst_rec.append(2, '\0'); // first 2 bytes of rich-text run inside SST
		push_u16_le(sst_stream, 0x00FC); push_u16_le(sst_stream, (uint16_t)sst_rec.size());
		sst_stream += sst_rec;

		// CONTINUE 1 (0x003C): empty 0-byte CONTINUE edge case
		push_u16_le(sst_stream, 0x003C); push_u16_le(sst_stream, 0);

		// CONTINUE 2 (0x003C): remaining 2 trailing rich-text bytes + str1 ("CD")
		std::string cont2;
		cont2.append(2, '\0');
		push_u16_le(cont2, 2);   // cch = 2
		cont2.push_back(0x00);   // 8-bit
		cont2 += "CD";
		push_u16_le(sst_stream, 0x003C); push_u16_le(sst_stream, (uint16_t)cont2.size());
		sst_stream += cont2;

		// Two XLS_LABEL_SST cells referencing index 0 and 1
		push_u16_le(sst_stream, 0x00FD); push_u16_le(sst_stream, 10);
		push_u16_le(sst_stream, 0); push_u16_le(sst_stream, 0); push_u16_le(sst_stream, 0); push_u32_le(sst_stream, 0);
		push_u16_le(sst_stream, 0x00FD); push_u16_le(sst_stream, 10);
		push_u16_le(sst_stream, 1); push_u16_le(sst_stream, 0); push_u16_le(sst_stream, 0); push_u32_le(sst_stream, 1);
		push_u16_le(sst_stream, 0x000A); push_u16_le(sst_stream, 0);

		OleSpecNode root;
		root.name = "Root Entry";
		root.is_storage = true;
		root.children.push_back({"Workbook", false, sst_stream, {}});
		std::string xls_buf = build_ole2_from_tree(root);
		std::string txt;
		if (!ex.processFile(xls_buf.data(), xls_buf.size(), txt) ||
		    txt.find("AB") == std::string::npos ||
		    txt.find("CD") == std::string::npos)
		{
			std::cerr << "FAIL: Step8 SST+CONTINUE spanning trailing block failed: " << txt << "\n";
			return 1;
		}
	}

	// 8.10 Mac XLS MSODRAWINGGROUP (0x00EB + 0x003C CONTINUE) zlib-compressed 0xF01A EMF BLIP PDF
	{
		std::string pdf = make_pdf_with_attachments("MAC_XLS_EMF_ZLIB_PDF_OK", std::vector<std::pair<std::string, std::string> >());
		size_t pdf_pad = (4 - (pdf.size() % 4)) % 4;
		std::string emf;
		push_u32_le(emf, 0x01); push_u32_le(emf, 108); emf.append(100, '\0');
		uint32_t data_size = (uint32_t)(44 + pdf.size() + pdf_pad);
		push_u32_le(emf, 0x46); push_u32_le(emf, 12 + data_size); push_u32_le(emf, data_size);
		emf += "GDIC";
		push_u32_le(emf, 0x40000004U);
		emf.append(16, '\0');
		push_u32_le(emf, 1);
		emf += " FDP";
		push_u32_le(emf, 1);
		push_u32_le(emf, (uint32_t)pdf.size());
		push_u32_le(emf, 44);
		emf += pdf;
		emf.append(pdf_pad, '\0');
		push_u32_le(emf, 0x0E); push_u32_le(emf, 20); emf.append(12, '\0');

		uLongf comp_bound = compressBound((uLong)emf.size());
		std::string comp(comp_bound, '\0');
		uLongf comp_len = comp_bound;
		if (compress2(reinterpret_cast<Bytef*>(&comp[0]), &comp_len,
		              reinterpret_cast<const Bytef*>(emf.data()), (uLong)emf.size(),
		              Z_DEFAULT_COMPRESSION) != Z_OK)
		{
			std::cerr << "FAIL: Step8 compress2 failed\n";
			return 1;
		}
		comp.resize((size_t)comp_len);

		// OfficeArtBlipEMF (0xF01A, inst=0x3D4 -> ver_inst=0x3D40)
		std::string blip;
		push_u16_le(blip, 0x3D40); push_u16_le(blip, 0xF01A);
		push_u32_le(blip, (uint32_t)(16 + 34 + comp.size()));
		blip.append(16, '\x11');                  // rgbUid1
		push_u32_le(blip, (uint32_t)emf.size());  // cbSize uncompressed
		blip.append(16, '\0');                    // rcBounds
		blip.append(8, '\0');                     // ptSize
		push_u32_le(blip, (uint32_t)comp.size()); // cbSave compressed
		blip.push_back(0x00);                     // fCompression = deflate
		blip.push_back((char)0xFE);               // fFilter

		blip += comp;

		// OfficeArtFBSE (0xF007, ver=2, inst=2 -> ver_inst=0x0022)
		std::string fbse;
		push_u16_le(fbse, 0x0022); push_u16_le(fbse, 0xF007);
		push_u32_le(fbse, (uint32_t)(36 + blip.size()));
		fbse.append(36, '\0');
		fbse += blip;

		// OfficeArtBStoreContainer (0xF001, ver=0xF, inst=1 -> ver_inst=0x001F)
		std::string bstore;
		push_u16_le(bstore, 0x001F); push_u16_le(bstore, 0xF001);
		push_u32_le(bstore, (uint32_t)fbse.size());
		bstore += fbse;

		// OfficeArtDggContainer (0xF000, ver=0xF -> ver_inst=0x000F)
		std::string dgg;
		push_u16_le(dgg, 0x000F); push_u16_le(dgg, 0xF000);
		push_u32_le(dgg, (uint32_t)bstore.size());
		dgg += bstore;

		// Build BIFF8 Workbook splitting dgg across 0x00EB + 0x003C CONTINUE
		size_t split = dgg.size() / 2;
		std::string wb;
		push_u16_le(wb, 0x0809); push_u16_le(wb, 16);
		push_u16_le(wb, 0x0600); push_u16_le(wb, 0x0005); wb.append(12, '\0');
		push_u16_le(wb, 0x00EB); push_u16_le(wb, (uint16_t)split);
		wb += dgg.substr(0, split);
		push_u16_le(wb, 0x003C); push_u16_le(wb, (uint16_t)(dgg.size() - split));
		wb += dgg.substr(split);
		push_u16_le(wb, 0x000A); push_u16_le(wb, 0);

		OleSpecNode root;
		root.name = "Root Entry"; root.is_storage = true;
		root.children.push_back({"Workbook", false, wb, {}});
		std::string xls_buf = build_ole2_from_tree(root);

		std::string txt;
		if (!ex.processFile(xls_buf.data(), xls_buf.size(), txt) ||
		    txt.find("MAC_XLS_EMF_ZLIB_PDF_OK") == std::string::npos)
		{
			std::cerr << "FAIL: Step8 Mac XLS MSODRAWINGGROUP EMF PDF failed: " << txt << "\n";
			return 1;
		}

		doctotext::PlainTextExtractor ex_off;
		ex_off.setParseEmbeddedFiles(false);
		std::string txt_off;
		if (!ex_off.processFile(xls_buf.data(), xls_buf.size(), txt_off) ||
		    txt_off.find("MAC_XLS_EMF_ZLIB_PDF_OK") != std::string::npos)
		{
			std::cerr << "FAIL: Step8 Mac XLS EMF PDF extracted despite parse_embedded_files=false: " << txt_off << "\n";
			return 1;
		}

		// With OCR image callback enabled, raster callback must ignore 0xF01A EMF BLIP (not emit fake PNG),
		// while EMF PDF extraction still works.
		int img_cb_calls = 0;
		auto img_cb = [](const std::string&, const char*, size_t, void* ud) -> std::string {
			++(*static_cast<int*>(ud));
			return "RASTER_CALLED";
		};
		doctotext::PlainTextExtractor ex_img;
		ex_img.setImageParserCallback(+img_cb, &img_cb_calls);
		std::string txt_img;
		if (!ex_img.processFile(xls_buf.data(), xls_buf.size(), txt_img) ||
		    img_cb_calls != 0 ||
		    txt_img.find("MAC_XLS_EMF_ZLIB_PDF_OK") == std::string::npos)
		{
			std::cerr << "FAIL: Step8 image callback wrongly invoked for 0xF01A EMF BLIP: calls=" << img_cb_calls << "\n";
			return 1;
		}

		// 8.10b Raw BIFF4 + uncompressed EMF (fCompression=0xFE) + 2 UIDs (inst=0x3D5) + cbName=4 + truncated outer container
		std::string pdf_raw = make_pdf_with_attachments("RAW_BIFF_UNCOMP_EMF_OK", std::vector<std::pair<std::string, std::string> >());
		size_t pdf_raw_pad = (4 - (pdf_raw.size() % 4)) % 4;
		std::string emf_raw;
		push_u32_le(emf_raw, 0x01); push_u32_le(emf_raw, 108); emf_raw.append(100, '\0');
		uint32_t ds_raw = (uint32_t)(44 + pdf_raw.size() + pdf_raw_pad);
		push_u32_le(emf_raw, 0x46); push_u32_le(emf_raw, 12 + ds_raw); push_u32_le(emf_raw, ds_raw);
		emf_raw += "GDIC"; push_u32_le(emf_raw, 0x40000004U); emf_raw.append(16, '\0');
		push_u32_le(emf_raw, 1); emf_raw += "PDF "; push_u32_le(emf_raw, 1);
		push_u32_le(emf_raw, (uint32_t)pdf_raw.size()); push_u32_le(emf_raw, 44);
		emf_raw += pdf_raw; emf_raw.append(pdf_raw_pad, '\0');
		push_u32_le(emf_raw, 0x0E); push_u32_le(emf_raw, 20); emf_raw.append(12, '\0');

		std::string blip2;
		push_u16_le(blip2, 0x3D50); push_u16_le(blip2, 0xF01A); // inst=0x3D5 (32B UIDs)
		push_u32_le(blip2, (uint32_t)(32 + 34 + emf_raw.size()));
		blip2.append(32, '\x33');
		push_u32_le(blip2, (uint32_t)emf_raw.size());
		blip2.append(24, '\0');
		push_u32_le(blip2, (uint32_t)emf_raw.size());
		blip2.push_back((char)0xFE); // uncompressed
		blip2.push_back((char)0xFE);
		blip2 += emf_raw;

		std::string fbse2;
		push_u16_le(fbse2, 0x0022); push_u16_le(fbse2, 0xF007);
		push_u32_le(fbse2, (uint32_t)(36 + 4 + blip2.size()));
		fbse2.append(33, '\0');
		fbse2.push_back(4); // cbName = 4
		fbse2.append(2, '\0');
		fbse2.append("a\0b\0", 4);
		fbse2 += blip2;

		// Outer container claims larger rec_len (truncated) to test container traversal resilience
		std::string dgg2;
		push_u16_le(dgg2, 0x000F); push_u16_le(dgg2, 0xF000);
		push_u32_le(dgg2, (uint32_t)(fbse2.size() + 64));
		dgg2 += fbse2;

		std::string raw_biff;
		push_u16_le(raw_biff, 0x0409); push_u16_le(raw_biff, 6);
		push_u16_le(raw_biff, 0); push_u16_le(raw_biff, 0x0010); push_u16_le(raw_biff, 0);
		push_u16_le(raw_biff, 0x00EB); push_u16_le(raw_biff, (uint16_t)dgg2.size());
		raw_biff += dgg2;
		push_u16_le(raw_biff, 0x000A); push_u16_le(raw_biff, 0);

		std::string txt_raw;
		if (!ex.processFile(raw_biff.data(), raw_biff.size(), txt_raw) ||
		    txt_raw.find("RAW_BIFF_UNCOMP_EMF_OK") == std::string::npos)
		{
			std::cerr << "FAIL: Step8 raw BIFF + 0x3D5 uncompressed EMF + cbName failed: " << txt_raw << "\n";
			return 1;
		}
	}

	std::cout << "PASS: Step 8 Excel edge formats (WORKBOOK caps, BIFF2/3/4/4W, SST/CONTINUE, SmartArt, EMF PDF)\n";
	return 0;
}

static int test_step9_xls_and_emf_malformed_guards()
{
	doctotext::PlainTextExtractor ex;

	// 9.1 Null/empty buffer guard on XLSParser
	{
		XLSParser null_xls(NULL, 0);
		if (null_xls.isXLS())
		{
			std::cerr << "FAIL: Step9 XLSParser(NULL, 0).isXLS() returned true\n";
			return 1;
		}
		std::string txt = null_xls.plainText(doctotext::FormattingStyle());
		if (!null_xls.error() || !txt.empty())
		{
			std::cerr << "FAIL: Step9 XLSParser(NULL, 0).plainText() expected error=true and empty output\n";
			return 1;
		}
	}

	// 9.2 Truncated / malformed BIFF records (must not crash or trigger UB/OOB under ASan/UBSan)
	{
		std::string bad_biff;
		// BOF_BIFF_4 (0x0409, len=6)
		push_u16_le(bad_biff, 0x0409); push_u16_le(bad_biff, 6);
		push_u16_le(bad_biff, 0); push_u16_le(bad_biff, 0x0010); push_u16_le(bad_biff, 0);

		// XLS_LABEL_BIFF2 (0x004) len=8 exact boundary (rec[7]=10, no rec[8])
		push_u16_le(bad_biff, 0x004); push_u16_le(bad_biff, 8);
		push_u16_le(bad_biff, 0); push_u16_le(bad_biff, 0);
		bad_biff.append(3, '\0'); bad_biff.push_back(10);

		// XLS_BLANK (0x201) len=2
		push_u16_le(bad_biff, 0x201); push_u16_le(bad_biff, 2); push_u16_le(bad_biff, 0);
		// XLS_FORMAT (0x41E) len=1
		push_u16_le(bad_biff, 0x41E); push_u16_le(bad_biff, 1); bad_biff.push_back(0);
		// XLS_LABEL_SST (0xFD) len=4
		push_u16_le(bad_biff, 0xFD); push_u16_le(bad_biff, 4); push_u16_le(bad_biff, 0); push_u16_le(bad_biff, 0);
		// XLS_MULBLANK (0xBE) len=4 & forged range len=6 (start=0, end=65535)
		push_u16_le(bad_biff, 0xBE); push_u16_le(bad_biff, 4); push_u16_le(bad_biff, 0); push_u16_le(bad_biff, 0);
		push_u16_le(bad_biff, 0xBE); push_u16_le(bad_biff, 6); push_u16_le(bad_biff, 0); push_u16_le(bad_biff, 0); push_u16_le(bad_biff, 0xFFFF);
		// XLS_MULRK (0xBD) len=4
		push_u16_le(bad_biff, 0xBD); push_u16_le(bad_biff, 4); push_u16_le(bad_biff, 0); push_u16_le(bad_biff, 0);
		// XLS_NUMBER (0x203) len=8
		push_u16_le(bad_biff, 0x203); push_u16_le(bad_biff, 8); bad_biff.append(8, '\0');
		// XLS_RK (0x27E) len=4
		push_u16_le(bad_biff, 0x27E); push_u16_le(bad_biff, 4); bad_biff.append(4, '\0');
		// XLS_XF_BIFF4 with date format 0x0E + NaN/huge XLS_NUMBER (tests xlsDateToString guard)
		push_u16_le(bad_biff, 0x443); push_u16_le(bad_biff, 2); bad_biff.push_back(0); bad_biff.push_back(0x0E);
		push_u16_le(bad_biff, 0x203); push_u16_le(bad_biff, 14);
		push_u16_le(bad_biff, 1); push_u16_le(bad_biff, 0); push_u16_le(bad_biff, 0);
		// IEEE 754 NaN (8 bytes)
		bad_biff.append("\x00\x00\x00\x00\x00\x00\xF8\x7F", 8);
		// XLS_SST (0xFC) truncated len=4 + XLS_EOF
		push_u16_le(bad_biff, 0xFC); push_u16_le(bad_biff, 4); bad_biff.append(4, '\0');
		push_u16_le(bad_biff, 0x000A); push_u16_le(bad_biff, 0);

		std::string txt;
		ex.processFile(bad_biff.data(), bad_biff.size(), txt);
	}

	// 9.3 Overlapping offData inside EMF EMR_COMMENT_MULTIFORMATS must be ignored safely
	{
		std::string pdf = make_pdf_with_attachments("SHOULD_BE_IGNORED_EMF", std::vector<std::pair<std::string, std::string> >());
		size_t pdf_pad = (4 - (pdf.size() % 4)) % 4;
		std::string emf;
		push_u32_le(emf, 0x01);
		push_u32_le(emf, 108);
		emf.append(100, '\0');
		uint32_t data_size = (uint32_t)(44 + pdf.size() + pdf_pad);
		uint32_t rec_size = 12 + data_size;
		push_u32_le(emf, 0x46);
		push_u32_le(emf, rec_size);
		push_u32_le(emf, data_size);
		emf += "GDIC";
		push_u32_le(emf, 0x40000004U);
		emf.append(16, '\0');
		push_u32_le(emf, 1);
		emf += " FDP";
		push_u32_le(emf, 1);
		push_u32_le(emf, (uint32_t)pdf.size());
		push_u32_le(emf, 16); // Invalid: < 28 + 1*16 = 44 (points into header/EmrFormat table)
		emf += pdf;
		emf.append(pdf_pad, '\0');
		push_u32_le(emf, 0x0E);
		push_u32_le(emf, 20);
		emf.append(12, '\0');

		std::vector<std::pair<std::string, std::string> > entries;
		entries.push_back({"xl/workbook.xml", "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"/>"});
		entries.push_back({"xl/worksheets/sheet1.xml", "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheetData/></worksheet>"});
		entries.push_back({"xl/media/image1.emf", emf});
		std::string xlsx_buf = build_zip_from_entries(entries);
		std::string txt;
		if (!ex.processFile(xlsx_buf.data(), xlsx_buf.size(), txt) ||
		    txt.find("SHOULD_BE_IGNORED_EMF") != std::string::npos)
		{
			std::cerr << "FAIL: Step9 invalid offData EMF payload was unexpectedly accepted: " << txt << "\n";
			return 1;
		}
	}

	// 9.4 Oversized cbSize & corrupt deflate stream in XLS MSODRAWINGGROUP EMF BLIP must fail safely
	{
		auto make_bad_emf_xls = [&](uint32_t claimed_cb_size, const std::string& comp_bytes) -> std::string {
			std::string blip;
			push_u16_le(blip, 0x3D40); push_u16_le(blip, 0xF01A);
			push_u32_le(blip, (uint32_t)(16 + 34 + comp_bytes.size()));
			blip.append(16, '\x22');
			push_u32_le(blip, claimed_cb_size);
			blip.append(24, '\0');
			push_u32_le(blip, (uint32_t)comp_bytes.size());
			blip.push_back(0x00); // deflate
			blip.push_back((char)0xFE);
			blip += comp_bytes;

			std::string fbse;
			push_u16_le(fbse, 0x0022); push_u16_le(fbse, 0xF007);
			push_u32_le(fbse, (uint32_t)(36 + blip.size()));
			fbse.append(36, '\0');
			fbse += blip;

			std::string dgg;
			push_u16_le(dgg, 0x000F); push_u16_le(dgg, 0xF000);
			push_u32_le(dgg, (uint32_t)fbse.size());
			dgg += fbse;

			std::string wb;
			push_u16_le(wb, 0x0809); push_u16_le(wb, 16);
			push_u16_le(wb, 0x0600); push_u16_le(wb, 0x0005); wb.append(12, '\0');
			push_u16_le(wb, 0x00EB); push_u16_le(wb, (uint16_t)dgg.size());
			wb += dgg;
			push_u16_le(wb, 0x000A); push_u16_le(wb, 0);

			OleSpecNode root;
			root.name = "Root Entry"; root.is_storage = true;
			root.children.push_back({"Workbook", false, wb, {}});
			return build_ole2_from_tree(root);
		};

		// 9.4a Forged huge cbSize (> kMaxEmbedBytes)
		std::string xls_huge = make_bad_emf_xls(0xFFFFFFFFU, std::string("\x78\x9c\x03\x00\x00\x00\x00\x01", 8));
		std::string txt1;
		if (!ex.processFile(xls_huge.data(), xls_huge.size(), txt1) ||
		    txt1.find_first_not_of(" \t\r\n") != std::string::npos)
		{
			std::cerr << "FAIL: Step9 forged cbSize EMF XLS unexpected output: " << txt1 << "\n";
			return 1;
		}

		// 9.4b Valid cbSize=64 with corrupt deflate bytes (exercises inflateInit + inflate failure + ZlibStreamGuard)
		std::string xls_corrupt = make_bad_emf_xls(64U, std::string("\x78\x9c\xff\xff\x00\x00\xde\xad", 8));
		std::string txt2;
		if (!ex.processFile(xls_corrupt.data(), xls_corrupt.size(), txt2) ||
		    txt2.find_first_not_of(" \t\r\n") != std::string::npos)
		{
			std::cerr << "FAIL: Step9 corrupt zlib EMF XLS unexpected output: " << txt2 << "\n";
			return 1;
		}

		// 9.4c Declared cbSize=8 smaller than actual compressed stream (128B zeroes) must stop at cbSize and reject
		std::string big_zeros(128, '\0');
		uLongf zlen = compressBound((uLong)big_zeros.size());
		std::string zdata(zlen, '\0');
		if (compress2(reinterpret_cast<Bytef*>(&zdata[0]), &zlen,
		              reinterpret_cast<const Bytef*>(big_zeros.data()), (uLong)big_zeros.size(),
		              Z_DEFAULT_COMPRESSION) != Z_OK)
		{
			std::cerr << "FAIL: Step9c compress2 failed\n";
			return 1;
		}
		zdata.resize((size_t)zlen);
		std::string xls_mismatch = make_bad_emf_xls(8U, zdata);
		std::string txt3;
		if (!ex.processFile(xls_mismatch.data(), xls_mismatch.size(), txt3) ||
		    txt3.find_first_not_of(" \t\r\n") != std::string::npos)
		{
			std::cerr << "FAIL: Step9 cbSize<actual EMF XLS unexpected output: " << txt3 << "\n";
			return 1;
		}
	}

	std::cout << "PASS: Step 9 malformed XLS/BIFF and EMF boundary guards\n";
	return 0;
}

static int test_step10_rtf_cp65001_and_unmatched_braces()
{
	doctotext::PlainTextExtractor ex;
	const std::string rtf_utf8 =
		"{\\rtf1\\ansi\\ansicpg65001\\deff0"
		"{\\fonttbl{\\f0\\fcharset0 Arial;}{\\f1\\fcharset204 Times;}}"
		"\\f0 HEX:\\'e4\\'b8\\'ad\\'e6\\'96\\'87_\\'f0\\'9f\\'98\\'80 "
		"{\\f1 \\'ef\\f0 }"
		"RAW:\xE4\xB8\xAD\xE6\x96\x87_\xF0\x9F\x98\x80"
		"\\par ExtraClosingOk\\'e4}}}";
	std::string out;
	if (!ex.processFile(rtf_utf8.data(), rtf_utf8.size(), out) ||
	    out.find("HEX:\xE4\xB8\xAD\xE6\x96\x87_\xF0\x9F\x98\x80") == std::string::npos ||
	    out.find("RAW:\xE4\xB8\xAD\xE6\x96\x87_\xF0\x9F\x98\x80") == std::string::npos ||
	    out.find("ExtraClosingOk") == std::string::npos)
	{
		std::cerr << "FAIL: Step10 RTF ansicpg65001 / unmatched braces extraction mismatch: " << out << "\n";
		return 1;
	}
	std::cout << "PASS: Step 10 RTF ansicpg65001 UTF-8 (hex/raw/4-byte) and extra closing braces\n";
	return 0;
}

static int test_step11_odf_negative_and_modern_iwork_guards()
{
	// 1. Corrupt non-ZIP ODT must fail cleanly without falling back to TXT
	doctotext::PlainTextExtractor ex;
	std::string out;
	if (ex.processFile("testODTnotaZipFile.odt", out))
	{
		std::cerr << "FAIL: Step11 testODTnotaZipFile.odt unexpectedly succeeded: " << out << "\n";
		return 1;
	}
	const std::string fake_odt = "This is not a zip file!\n";
	if (ex.processFile(doctotext::PlainTextExtractor::PARSER_ODF_OOXML, true, fake_odt.data(), fake_odt.size(), out))
	{
		std::cerr << "FAIL: Step11 fake ODT buffer unexpectedly succeeded\n";
		return 1;
	}

	DocToTextExtractorParams* params = doctotext_create_extractor_params();
	doctotext_extractor_params_set_log_file(params, "/dev/null");
	doctotext_status st = DOCTOTEXT_STATUS_OK;
	DocToTextException* exc = NULL;
	DocToTextExtractedData* bad_data = doctotext_process_file_ex("testODTnotaZipFile.odt", params, &st, &exc);
	if (bad_data != NULL || st != DOCTOTEXT_STATUS_ERROR)
	{
		std::cerr << "FAIL: Step11 C API did not report DOCTOTEXT_STATUS_ERROR on testODTnotaZipFile.odt\n";
		if (bad_data) doctotext_free_extracted_data(bad_data);
		if (exc) doctotext_free_exception(exc);
		doctotext_free_extractor_params(params);
		return 1;
	}
	if (exc)
		doctotext_free_exception(exc);

	// 2. Malformed IWA / corrupt nested Index.zip boundary guards must fail safely without crashing
	const std::vector<std::string> bad_iwas = {
		std::string("\x00\x05\x00", 3),                      // truncated 3B chunk header
		std::string("\x00\xFF\x00\x00" "abc", 7),            // length exceeds input
		std::string("\x01\x01\x00\x00\x00", 5),              // unsupported chunk type
		std::string("\x00\x04\x00\x00\x05\xFF\xFF\xFF", 8)   // corrupt Snappy block
	};
	for (size_t i = 0; i < bad_iwas.size(); ++i)
	{
		std::vector<std::pair<std::string, std::string> > entries;
		entries.push_back(std::make_pair(std::string("Index/Document.iwa"), bad_iwas[i]));
		std::string zip_buf = build_zip_from_entries(entries);
		std::string tmp_out;
		if (ex.processFile(doctotext::PlainTextExtractor::PARSER_IWORK, false, zip_buf.data(), zip_buf.size(), tmp_out))
		{
			std::cerr << "FAIL: Step11 malformed IWA case " << i << " unexpectedly succeeded\n";
			doctotext_free_extractor_params(params);
			return 1;
		}
	}
	{
		std::vector<std::pair<std::string, std::string> > entries;
		entries.push_back(std::make_pair(std::string("Presentation.key/Index.zip"), std::string("not_a_zip")));
		std::string zip_buf = build_zip_from_entries(entries);
		std::string tmp_out;
		if (ex.processFile(doctotext::PlainTextExtractor::PARSER_IWORK, false, zip_buf.data(), zip_buf.size(), tmp_out))
		{
			std::cerr << "FAIL: Step11 corrupt nested Index.zip unexpectedly succeeded\n";
			doctotext_free_extractor_params(params);
			return 1;
		}
	}

	// 3. Subsequent valid ODP & modern iWork '13/'18 files must still parse cleanly via C++ and C APIs
	if (!ex.processFile("testMasterFooter.odp", out) || out.find("Master footer is here") == std::string::npos)
	{
		std::cerr << "FAIL: Step11 testMasterFooter.odp missing master footer: " << out << "\n";
		doctotext_free_extractor_params(params);
		return 1;
	}
	if (!ex.processFile("testKeynote2018.key", out) || out.find("Libreoffice 6.2") == std::string::npos ||
	    out.find("Teks Judul") != std::string::npos)
	{
		std::cerr << "FAIL: Step11 testKeynote2018.key extraction mismatch: " << out << "\n";
		doctotext_free_extractor_params(params);
		return 1;
	}
	if (!ex.processFile("testPages2013.pages", out) ||
	    out.find("Sample pages document") == std::string::npos ||
	    out.find("Column one\nColumn two\nColumn three") == std::string::npos)
	{
		std::cerr << "FAIL: Step11 testPages2013.pages extraction mismatch: " << out << "\n";
		doctotext_free_extractor_params(params);
		return 1;
	}
	exc = NULL;
	st = DOCTOTEXT_STATUS_ERROR;
	DocToTextExtractedData* ok_data = doctotext_process_file_ex("testNumbers2013.numbers", params, &st, &exc);
	if (!ok_data || st != DOCTOTEXT_STATUS_OK ||
	    std::strstr(doctotext_extracted_data_get_text(ok_data), "Checking Account: 300545668") == NULL)
	{
		std::cerr << "FAIL: Step11 C API testNumbers2013.numbers failed\n";
		if (ok_data) doctotext_free_extracted_data(ok_data);
		if (exc) doctotext_free_exception(exc);
		doctotext_free_extractor_params(params);
		return 1;
	}
	doctotext_free_extracted_data(ok_data);
	doctotext_free_extractor_params(params);

	std::cout << "PASS: Step 11 negative ODT rejection, malformed IWA guards, ODP master footer, and iWork 13/18\n";
	return 0;
}

static std::string to_hex_str(const std::string& raw)
{
	static const char kHex[] = "0123456789abcdef";
	std::string out;
	out.reserve(raw.size() * 2);
	for (size_t i = 0; i < raw.size(); ++i)
	{
		unsigned char b = static_cast<unsigned char>(raw[i]);
		out.push_back(kHex[(b >> 4) & 0xF]);
		out.push_back(kHex[b & 0xF]);
	}
	return out;
}

static void push_u32_le_str(std::string& dst, uint32_t v)
{
	dst.push_back(static_cast<char>(v & 0xFF));
	dst.push_back(static_cast<char>((v >> 8) & 0xFF));
	dst.push_back(static_cast<char>((v >> 16) & 0xFF));
	dst.push_back(static_cast<char>((v >> 24) & 0xFF));
}

static std::string make_ole1_objdata_prefix(uint32_t format_id,
                                            const std::string& class_name,
                                            const std::string& payload)
{
	std::string raw;
	push_u32_le_str(raw, 0x00000501u); // version 1.5
	push_u32_le_str(raw, format_id);
	std::string cls_nul = class_name + '\0';
	push_u32_le_str(raw, static_cast<uint32_t>(cls_nul.size()));
	raw += cls_nul;
	push_u32_le_str(raw, 0); // topicLen
	push_u32_le_str(raw, 0); // itemLen
	push_u32_le_str(raw, static_cast<uint32_t>(payload.size()));
	raw += payload;
	return raw;
}

static int test_step12_rtf_objdata_charts_numbers()
{
	doctotext::PlainTextExtractor ex;
	std::string out;

	// 1. RTF mid-group \* must not swallow following text, while group-start {\*\...} still skips
	const std::string rtf_star =
		"{\\rtf1\\ansi\\deff0"
		"{\\fonttbl{\\f0 Times;}}"
		"\\pard{\\loch\\f0\\*\\cs7\\cf0 The quick brown fox jumps over the lazy dog}"
		"{\\*\\ignoredDest SHOULD_NOT_APPEAR}\\par}";
	if (!ex.processFile(rtf_star.data(), rtf_star.size(), out) ||
	    out.find("The quick brown fox jumps over the lazy dog") == std::string::npos ||
	    out.find("SHOULD_NOT_APPEAR") != std::string::npos)
	{
		std::cerr << "FAIL: Step12 RTF mid-group \\* handling mismatch: " << out << "\n";
		return 1;
	}

	// 2. RTF {\*\objdata ...} with Package + OLE2 + link (FormatID=1 skip) + parse_embedded_files toggle
	std::string pkg_payload;
	const std::string inner_txt = "RTF_PKG_UTF8_\xE6\x99\xAE\xE6\x9E\x97\xE6\x96\xAF\xE9\xA1\xBF_OK";
	pkg_payload.push_back('\x02'); pkg_payload.push_back('\x00');
	pkg_payload += std::string("note.txt") + '\0';
	pkg_payload += '\0'; // iconPath
	pkg_payload.push_back('\x00'); pkg_payload.push_back('\x00'); // iconIndex
	pkg_payload.push_back('\x03'); pkg_payload.push_back('\x00'); // type = 3 embedded
	push_u32_le_str(pkg_payload, 8);
	pkg_payload += std::string("note.txt") + '\0';
	push_u32_le_str(pkg_payload, static_cast<uint32_t>(inner_txt.size()));
	pkg_payload += inner_txt;

	OleSpecNode ole_xls_root;
	ole_xls_root.name = "Root Entry";
	ole_xls_root.is_storage = true;
	ole_xls_root.children.push_back({"Workbook", false, make_minimal_biff8_workbook("RTF_OLE2_XLS_OK"), {}});
	std::string ole_xls_buf = build_ole2_from_tree(ole_xls_root);

	std::string link_raw = make_ole1_objdata_prefix(1u, "Word.Document.8", "LINKED_SKIP_TEXT");
	std::string pkg_raw = make_ole1_objdata_prefix(2u, "Package", pkg_payload);
	std::string ole_raw = make_ole1_objdata_prefix(2u, "Excel.Sheet.8", ole_xls_buf);

	// 2b. Package with UTF-16LE surrogate-pair Unicode path and \binN bytes containing braces
	std::string pkg_u16_payload;
	const std::string inner_u16_txt = "RTF_PKG_U16_SURROGATE_OK";
	pkg_u16_payload.push_back('\x02'); pkg_u16_payload.push_back('\x00');
	pkg_u16_payload += std::string("fallback.dat") + '\0';
	pkg_u16_payload += '\0';
	pkg_u16_payload.push_back('\x00'); pkg_u16_payload.push_back('\x00');
	pkg_u16_payload.push_back('\x03'); pkg_u16_payload.push_back('\x00');
	push_u32_le_str(pkg_u16_payload, 1);
	pkg_u16_payload += '\0';
	push_u32_le_str(pkg_u16_payload, static_cast<uint32_t>(inner_u16_txt.size()));
	pkg_u16_payload += inner_u16_txt;
	// Unicode filename: "C:\\sub\\" + U+1F4C4 (D83D DCC4) + ".txt" = 13 UTF-16LE code units
	push_u32_le_str(pkg_u16_payload, 13u);
	const uint16_t u16_name[] = {
		'C', ':', '\\', 's', 'u', 'b', '\\', 0xD83Du, 0xDCC4u, '.', 't', 'x', 't'
	};
	for (size_t i = 0; i < sizeof(u16_name) / sizeof(u16_name[0]); ++i)
	{
		pkg_u16_payload.push_back(static_cast<char>(u16_name[i] & 0xFF));
		pkg_u16_payload.push_back(static_cast<char>((u16_name[i] >> 8) & 0xFF));
	}
	std::string pkg_u16_raw = make_ole1_objdata_prefix(2u, "Package", pkg_u16_payload);

	std::string rtf_obj =
		"{\\rtf1\\ansi\\deff0"
		"{\\b\\bin3 }{}}AFTER_BIN_OK "
		"{\\*\\dest\\bin2 {}}AFTER_SKIP_OK "
		"{\\object\\objautlink{\\*\\objdata " + to_hex_str(link_raw) + "}}"
		"{\\object\\objemb{\\*\\objdata " + to_hex_str(pkg_raw) + "}}"
		"{\\object\\objemb{\\*\\objdata " + to_hex_str(pkg_u16_raw) + "}}"
		"{\\object\\objemb{\\*\\objdata " + to_hex_str(ole_raw) + "}}"
		"\\par}";
	if (!ex.processFile(rtf_obj.data(), rtf_obj.size(), out) ||
	    out.find("AFTER_BIN_OK") == std::string::npos ||
	    out.find("AFTER_SKIP_OK") == std::string::npos ||
	    out.find("RTF_PKG_UTF8_\xE6\x99\xAE\xE6\x9E\x97\xE6\x96\xAF\xE9\xA1\xBF_OK") == std::string::npos ||
	    out.find("RTF_PKG_U16_SURROGATE_OK") == std::string::npos ||
	    out.find("RTF_OLE2_XLS_OK") == std::string::npos ||
	    out.find("LINKED_SKIP_TEXT") != std::string::npos)
	{
		std::cerr << "FAIL: Step12 RTF objdata extraction mismatch: " << out << "\n";
		return 1;
	}

	ex.setParseEmbeddedFiles(false);
	std::string out_off;
	if (!ex.processFile(rtf_obj.data(), rtf_obj.size(), out_off) ||
	    out_off.find("RTF_PKG_UTF8") != std::string::npos ||
	    out_off.find("RTF_OLE2_XLS_OK") != std::string::npos)
	{
		std::cerr << "FAIL: Step12 RTF parse_embedded=false did not skip objdata: " << out_off << "\n";
		ex.setParseEmbeddedFiles(true);
		return 1;
	}
	ex.setParseEmbeddedFiles(true);

	// 3. OOXML chart XML extraction in DOCX and PPTX (incl. mc:Fallback skip & literal series boundaries)
	{
		const std::string chart_xml =
			"<c:chartSpace xmlns:c=\"http://schemas.openxmlformats.org/drawingml/2006/chart\" "
			"xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
			"xmlns:mc=\"http://schemas.openxmlformats.org/markup-compatibility/2006\">"
			"<c:chart><c:title><c:tx><c:rich><a:p><a:r><a:t>CHART_TITLE_OK</a:t></a:r></a:p></c:rich></c:tx></c:title>"
			"<mc:AlternateContent>"
			"<mc:Choice Requires=\"c14\"><c:tx><c:rich><a:p><a:r><a:t>ALT_CHOICE_OK</a:t></a:r></a:p></c:rich></c:tx></mc:Choice>"
			"<mc:Fallback><c:tx><c:rich><a:p><a:r><a:t>ALT_FALLBACK_SKIP</a:t></a:r></a:p></c:rich></c:tx></mc:Fallback>"
			"</mc:AlternateContent>"
			"<c:plotArea><c:pieChart><c:ser>"
			"<c:tx><c:strRef><c:f>Sheet1!$B$1</c:f><c:strCache><c:pt idx=\"0\"><c:v>SERIES_LABEL_OK</c:v></c:pt></c:strCache></c:strRef></c:tx>"
			"<c:cat><c:strRef><c:f>Sheet1!$A$2:$A$3</c:f><c:strCache><c:pt idx=\"0\"><c:v>January</c:v></c:pt><c:pt idx=\"1\"><c:v>February</c:v></c:pt></c:strCache></c:strRef></c:cat>"
			"</c:ser><c:ser><c:tx><c:v>SERIES_LIT_1</c:v></c:tx></c:ser><c:ser><c:tx><c:v>SERIES_LIT_2</c:v></c:tx></c:ser>"
			"</c:pieChart></c:plotArea></c:chart></c:chartSpace>";

		std::vector<std::pair<std::string, std::string> > docx_entries = {
			{"word/document.xml", "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:body><w:p/></w:body></w:document>"},
			{"word/charts/chart1.xml", chart_xml}
		};
		std::string docx_buf = build_zip_from_entries(docx_entries);
		if (!ex.processFile(docx_buf.data(), docx_buf.size(), out) ||
		    out.find("CHART_TITLE_OK") == std::string::npos ||
		    out.find("ALT_CHOICE_OK") == std::string::npos ||
		    out.find("ALT_FALLBACK_SKIP") != std::string::npos ||
		    out.find("SERIES_LABEL_OK") == std::string::npos ||
		    out.find("SERIES_LIT_1\nSERIES_LIT_2") == std::string::npos ||
		    out.find("January\tFebruary") == std::string::npos ||
		    out.find("Sheet1!$A$2:$A$3") != std::string::npos)
		{
			std::cerr << "FAIL: Step12 DOCX chart extraction mismatch: " << out << "\n";
			return 1;
		}

		std::vector<std::pair<std::string, std::string> > pptx_entries = {
			{"ppt/presentation.xml",
			 "<p:presentation xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\" "
			 "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
			 "<p:sldIdLst><p:sldId id=\"256\" r:id=\"rId1\"/></p:sldIdLst></p:presentation>"},
			{"ppt/_rels/presentation.xml.rels",
			 "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
			 "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide\" Target=\"slides/slide1.xml\"/>"
			 "</Relationships>"},
			{"ppt/slides/slide1.xml", "<p:sld xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"><p:cSld><p:spTree/></p:cSld></p:sld>"},
			{"ppt/charts/chart1.xml", chart_xml}
		};
		std::string pptx_buf = build_zip_from_entries(pptx_entries);
		if (!ex.processFile(pptx_buf.data(), pptx_buf.size(), out) ||
		    out.find("CHART_TITLE_OK") == std::string::npos ||
		    out.find("January\tFebruary") == std::string::npos)
		{
			std::cerr << "FAIL: Step12 PPTX chart extraction mismatch: " << out << "\n";
			return 1;
		}
	}

	// 4. Legacy Numbers custom table name (even when sf:name-is-visible="false")
	{
		const std::string idx_xml =
			"<?xml version=\"1.0\"?>"
			"<ls:document xmlns:ls=\"http://developer.apple.com/namespaces/ls\" "
			"xmlns:sf=\"http://developer.apple.com/namespaces/sf\" "
			"xmlns:sfa=\"http://developer.apple.com/namespaces/sfa\">"
			"<ls:workspace-array><ls:workspace ls:workspace-name=\"Sheet 1\">"
			"<ls:page-info><sf:tabular-info>"
			"<sf:tabular-model sf:name=\"This is the main table\" sf:name-is-visible=\"false\"/>"
			"</sf:tabular-info></ls:page-info>"
			"</ls:workspace></ls:workspace-array></ls:document>";
		std::vector<std::pair<std::string, std::string> > num_entries = {
			{"index.xml", idx_xml}
		};
		std::string num_buf = build_zip_from_entries(num_entries);
		if (!ex.processFile(num_buf.data(), num_buf.size(), out) ||
		    out.find("This is the main table") == std::string::npos)
		{
			std::cerr << "FAIL: Step12 Numbers custom table name extraction mismatch: " << out << "\n";
			return 1;
		}
	}

	// 5. Adversarial regression guards (Step 13):
	//    - DocToTextUnzip closed/unopened encryption probes
	//    - IWorkParser metadata encryption propagation & modern ZIP-encrypted IWA
	//    - RTF parseNumber 75-digit overflow guard, odd/ignored-group objdata, OLE2 CONTENTS PDF, utf8_to_ustring trailing/invalid UTF-8
	//    - XLSX chart1.xml + strLit/numLit row boundaries
	{
		// 5a. DocToTextUnzip unopened & closed safety
		DocToTextUnzip empty_unzip;
		if (empty_unzip.isEncryptedEntry("index.xml") || empty_unzip.hasEncryptedEntries())
		{
			std::cerr << "FAIL: Step13 unopened DocToTextUnzip returned encrypted\n";
			return 1;
		}
		std::vector<std::pair<std::string, std::string> > plain_zip_entries = {{"a.txt", "hello"}};
		std::string plain_zip = build_zip_from_entries(plain_zip_entries);
		DocToTextUnzip closed_unzip(plain_zip.data(), plain_zip.size());
		if (!closed_unzip.open())
		{
			std::cerr << "FAIL: Step13 closed_unzip.open failed\n";
			return 1;
		}
		closed_unzip.close();
		if (closed_unzip.isEncryptedEntry("a.txt") || closed_unzip.hasEncryptedEntries())
		{
			std::cerr << "FAIL: Step13 closed DocToTextUnzip returned encrypted\n";
			return 1;
		}

		// 5b. PlainTextExtractor::extractMetadata on encrypted .pages
		doctotext::Metadata pages_meta;
		doctotext::PlainTextExtractor meta_ex;
		bool meta_ok = meta_ex.extractMetadata("encrypted/testPagesPwdProtected.pages", pages_meta);
		if (meta_ok || !meta_ex.isEncrypted())
		{
			std::cerr << "FAIL: Step13 extractMetadata on encrypted .pages did not set isEncrypted\n";
			return 1;
		}

		// 5c. Modern iWork ZIP with Index/Document.iwa marked encrypted (PKZIP bit 0 in local & central headers)
		std::vector<std::pair<std::string, std::string> > mod_entries = {
			{"Metadata/BuildVersionHistory.plist", "<plist/>"},
			{"Index/Document.iwa", "ENCRYPTED_IWA_BYTES"}
		};
		std::string mod_iwork = build_zip_from_entries(mod_entries);
		// Patch general purpose bit flag (bit 0) on Index/Document.iwa in local & central directory headers
		for (size_t pos = 0; pos + 30 <= mod_iwork.size(); ++pos)
		{
			if (std::memcmp(&mod_iwork[pos], "PK\x03\x04", 4) == 0)
			{
				uint16_t nlen = static_cast<uint16_t>(static_cast<unsigned char>(mod_iwork[pos + 26])) |
				                (static_cast<uint16_t>(static_cast<unsigned char>(mod_iwork[pos + 27])) << 8);
				if (pos + 30 + nlen <= mod_iwork.size() &&
				    mod_iwork.compare(pos + 30, nlen, "Index/Document.iwa") == 0)
				{
					mod_iwork[pos + 6] |= 0x01;
				}
			}
			else if (pos + 46 <= mod_iwork.size() && std::memcmp(&mod_iwork[pos], "PK\x01\x02", 4) == 0)
			{
				uint16_t nlen = static_cast<uint16_t>(static_cast<unsigned char>(mod_iwork[pos + 28])) |
				                (static_cast<uint16_t>(static_cast<unsigned char>(mod_iwork[pos + 29])) << 8);
				if (pos + 46 + nlen <= mod_iwork.size() &&
				    mod_iwork.compare(pos + 46, nlen, "Index/Document.iwa") == 0)
				{
					mod_iwork[pos + 8] |= 0x01;
				}
			}
		}
		doctotext::PlainTextExtractor mod_ex;
		std::string mod_out;
		if (mod_ex.processFile(mod_iwork.data(), mod_iwork.size(), mod_out) || !mod_ex.isEncrypted())
		{
			std::cerr << "FAIL: Step13 modern encrypted IWA did not report isEncrypted\n";
			return 1;
		}

		// 5d. Overlong/invalid 4-byte UTF-8 and trailing multi-byte UTF-8 inside RTF Package
		std::string tail_u8_pkg;
		const std::string tail_u8_txt = "TAIL_UTF8_\xE6\x99\xAE\xE6\x9E\x97\xE6\x96\x87";
		tail_u8_pkg.push_back('\x02'); tail_u8_pkg.push_back('\x00');
		tail_u8_pkg += std::string("tail.txt") + '\0';
		tail_u8_pkg += '\0';
		tail_u8_pkg.push_back('\x00'); tail_u8_pkg.push_back('\x00');
		tail_u8_pkg.push_back('\x03'); tail_u8_pkg.push_back('\x00');
		push_u32_le_str(tail_u8_pkg, 8);
		tail_u8_pkg += std::string("tail.txt") + '\0';
		push_u32_le_str(tail_u8_pkg, static_cast<uint32_t>(tail_u8_txt.size()));
		tail_u8_pkg += tail_u8_txt;
		std::string tail_u8_raw = make_ole1_objdata_prefix(2u, "Package", tail_u8_pkg);

		std::string bad_u8_pkg;
		const std::string bad_u8_txt = "PREFIX_A\xF0\x80\x80\x80SUFFIX_Z";
		bad_u8_pkg.push_back('\x02'); bad_u8_pkg.push_back('\x00');
		bad_u8_pkg += std::string("bad.txt") + '\0';
		bad_u8_pkg += '\0';
		bad_u8_pkg.push_back('\x00'); bad_u8_pkg.push_back('\x00');
		bad_u8_pkg.push_back('\x03'); bad_u8_pkg.push_back('\x00');
		push_u32_le_str(bad_u8_pkg, 7);
		bad_u8_pkg += std::string("bad.txt") + '\0';
		push_u32_le_str(bad_u8_pkg, static_cast<uint32_t>(bad_u8_txt.size()));
		bad_u8_pkg += bad_u8_txt;
		std::string bad_u8_raw = make_ole1_objdata_prefix(2u, "Package", bad_u8_pkg);

		std::string bad_u8_rtf =
			"{\\rtf1\\ansi\\deff0"
			"{\\object\\objemb{\\*\\objdata " + to_hex_str(tail_u8_raw) + "}}"
			"{\\object\\objemb{\\*\\objdata " + to_hex_str(bad_u8_raw) + "}}"
			"\\par}";
		std::string bad_u8_out;
		if (!ex.processFile(bad_u8_rtf.data(), bad_u8_rtf.size(), bad_u8_out) ||
		    bad_u8_out.find("TAIL_UTF8_\xE6\x99\xAE\xE6\x9E\x97\xE6\x96\x87") == std::string::npos ||
		    bad_u8_out.find("PREFIX_A") == std::string::npos ||
		    bad_u8_out.find("SUFFIX_Z") == std::string::npos)
		{
			std::cerr << "FAIL: Step13 UTF-8 RTF Package guard failed: " << bad_u8_out << "\n";
			return 1;
		}

		// 5e. RTF 75-digit numeric param, odd hex, nested inside outer skipped group, and OLE2 CONTENTS PDF
		OleSpecNode ole_pdf_root;
		ole_pdf_root.name = "Root Entry";
		ole_pdf_root.is_storage = true;
		ole_pdf_root.children.push_back({"CONTENTS", false, make_pdf_with_attachments("RTF_OLE2_PDF_OK", std::vector<std::pair<std::string, std::string> >()), {}});
		std::string ole_pdf_buf = build_ole2_from_tree(ole_pdf_root);
		std::string ole_pdf_raw = make_ole1_objdata_prefix(2u, "AcroExch.Document", ole_pdf_buf);
		std::string outer_skip_raw = make_ole1_objdata_prefix(2u, "Package", pkg_payload);
		std::string overlong_bin = "\\bin" + std::string(75, '9') + " ";
		std::string adv_rtf =
			"{\\rtf1\\ansi\\deff0"
			"{\\b" + overlong_bin + "SAFE_AFTER_LONG_ARG }"
			"{\\*\\themedata{\\*\\objdata " + to_hex_str(outer_skip_raw) + "}}"
			"{\\object\\objemb{\\*\\objdata 010}}"
			"{\\object\\objemb{\\*\\objdata " + to_hex_str(ole_pdf_raw) + "}}"
			"\\par}";
		if (!ex.processFile(adv_rtf.data(), adv_rtf.size(), out) ||
		    out.find("SAFE_AFTER_LONG_ARG") == std::string::npos ||
		    out.find("RTF_OLE2_PDF_OK") == std::string::npos ||
		    out.find("RTF_PKG_UTF8") != std::string::npos)
		{
			std::cerr << "FAIL: Step13 adversarial RTF mismatch: " << out << "\n";
			return 1;
		}

		// 5f. XLSX chart with strLit and numLit row splitting
		const std::string xlsx_chart =
			"<c:chartSpace xmlns:c=\"http://schemas.openxmlformats.org/drawingml/2006/chart\">"
			"<c:chart><c:plotArea><c:barChart><c:ser>"
			"<c:cat><c:strLit><c:ptCount val=\"2\"/><c:pt idx=\"0\"><c:v>LIT_A</c:v></c:pt><c:pt idx=\"1\"><c:v>LIT_B</c:v></c:pt></c:strLit></c:cat>"
			"<c:val><c:numLit><c:ptCount val=\"1\"/><c:pt idx=\"0\"><c:v>99</c:v></c:pt></c:numLit></c:val>"
			"</c:ser></c:barChart></c:plotArea></c:chart></c:chartSpace>";
		std::vector<std::pair<std::string, std::string> > xlsx_entries = {
			{"xl/workbook.xml", "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheets><sheet name=\"S1\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>"},
			{"xl/worksheets/sheet1.xml", "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheetData/></worksheet>"},
			{"xl/charts/chart1.xml", xlsx_chart}
		};
		std::string xlsx_buf = build_zip_from_entries(xlsx_entries);
		if (!ex.processFile(xlsx_buf.data(), xlsx_buf.size(), out) ||
		    out.find("LIT_A\tLIT_B\n99") == std::string::npos)
		{
			std::cerr << "FAIL: Step13 XLSX chart/strLit/numLit mismatch: " << out << "\n";
			return 1;
		}

		// 5g. BIFF8 XLS TXO (0x1B6) textbox extraction (8-bit compressed & UTF-16LE, ignoring cbRuns CONTINUE)
		auto push_rec_biff = [](std::string& dst, uint16_t type, const std::string& body) {
			push_u16_le(dst, type);
			push_u16_le(dst, (uint16_t)body.size());
			dst += body;
		};
		std::string wb;
		std::string bof;
		push_u16_le(bof, 0x0600); push_u16_le(bof, 0x0010);
		for (int z = 0; z < 12; ++z) bof.push_back('\0');
		push_rec_biff(wb, 0x0809, bof);
		std::string txo1(18, '\0');
		const std::string t1_ascii = "XLS_TXO_ASCII_OK";
		txo1[10] = static_cast<char>(t1_ascii.size() & 0xFF);
		txo1[12] = 8; // cbRuns
		push_rec_biff(wb, 0x01B6, txo1);
		push_rec_biff(wb, 0x003C, std::string(1, '\0') + t1_ascii);
		push_rec_biff(wb, 0x003C, std::string(8, '\x41')); // formatting runs MUST NOT leak into text

		std::string txo2(18, '\0');
		txo2[10] = 3; // 3 UTF-16LE chars
		txo2[12] = 8;
		push_rec_biff(wb, 0x01B6, txo2);
		std::string u16_cont(1, '\x01');
		push_u16_le(u16_cont, 0x6771u); // 東
		push_u16_le(u16_cont, 0x4EACu); // 京
		push_u16_le(u16_cont, 0x0021u); // !
		push_rec_biff(wb, 0x003C, u16_cont);
		push_rec_biff(wb, 0x003C, std::string(8, '\x42'));
		push_rec_biff(wb, 0x000A, "");

		OleSpecNode txo_root;
		txo_root.name = "Root Entry";
		txo_root.is_storage = true;
		txo_root.children.push_back({"Workbook", false, wb, {}});
		std::string txo_xls = build_ole2_from_tree(txo_root);
		if (!ex.processFile(txo_xls.data(), txo_xls.size(), out) ||
		    out.find("XLS_TXO_ASCII_OK") == std::string::npos ||
		    out.find("\xE6\x9D\xB1\xE4\xBA\xAC!") == std::string::npos ||
		    out.find("AAAAAAAA") != std::string::npos ||
		    out.find("BBBBBBBB") != std::string::npos)
		{
			std::cerr << "FAIL: Step13 XLS TXO textbox mismatch: " << out << "\n";
			return 1;
		}

		// 5h. DOC EQ phonetic field parsing edge cases
		if (doctotext::extract_doc_eq_field_text("\\o\\ad(\\s\\up 10(\xE3\x81\xA8\xE3\x81\x86\xE3\x81\x8D\xE3\x82\x87\xE3\x81\x86),\xE6\x9D\xB1\xE4\xBA\xAC)") != "\xE6\x9D\xB1\xE4\xBA\xAC" ||
		    doctotext::extract_doc_eq_field_text("\\o\\ad(\\s\\up 10(r1),A)\\o\\ad(\\s\\up 10(r2),B)") != "AB" ||
		    doctotext::extract_doc_eq_field_text("prefix \\o(\\s\\up 10(ruby),base) suffix") != "prefix base suffix" ||
		    doctotext::extract_doc_eq_field_text("\\over(a,b)") != "\\over(a,b)" ||
		    doctotext::extract_doc_eq_field_text("\\o(\\s\\up 10(a),x\\)y\\,z)") != "x)y,z" ||
		    doctotext::extract_doc_eq_field_text("\\o(a,\\o(\\s\\up 10(ruby),b))") != "ab" ||
		    doctotext::extract_doc_eq_field_text("\\o(a,\\f(1,2))") != "a1 2" ||
		    doctotext::extract_doc_eq_field_text("\\f(\\s\\up 10(a),b)") != "a b")
		{
			std::cerr << "FAIL: Step13 DOC EQ field extraction mismatch\n";
			return 1;
		}

		// 5i. BIFF8 XLS TXO UTF-16LE split surrogate, Mac CR newline, Latin-1 (under CODEPAGE 1200), & ANSI DBCS (CODEPAGE 936)
		std::string wb2;
		push_rec_biff(wb2, 0x0809, bof);
		std::string cp1200;
		push_u16_le(cp1200, 1200);
		push_rec_biff(wb2, 0x0042, cp1200);
		// Split U+1F600 (D83D DE00) across two CONTINUEs + Mac '\r' newline -> \n + Latin-1 0xE9 ('é')
		std::string txo_u16(18, '\0');
		txo_u16[10] = 5; // D83D, DE00, 'A', '\r', 'B' (5 UTF-16 code units)
		push_rec_biff(wb2, 0x01B6, txo_u16);
		std::string c1(1, '\x01');
		push_u16_le(c1, 0xD83Du);
		push_rec_biff(wb2, 0x003C, c1);
		std::string c2(1, '\x01');
		push_u16_le(c2, 0xDE00u);
		push_u16_le(c2, 0x0041u);
		push_u16_le(c2, 0x000Du);
		push_u16_le(c2, 0x0042u);
		push_rec_biff(wb2, 0x003C, c2);

		// Latin-1 compressed 8-bit under CODEPAGE 1200: "caf\xE9" + Mac '\r' + "end"
		std::string txo_latin1(18, '\0');
		txo_latin1[10] = 8;
		push_rec_biff(wb2, 0x01B6, txo_latin1);
		push_rec_biff(wb2, 0x003C, std::string(1, '\0') + "caf\xE9\rend");

		// Explicit DBCS CODEPAGE 936 ("中文") compressed 8-bit TXO (also left un-EOF'd to test post-loop flushTxoText)
		std::string cp936;
		push_u16_le(cp936, 936);
		push_rec_biff(wb2, 0x0042, cp936);
		std::string txo_gbk(18, '\0');
		txo_gbk[10] = 4;
		push_rec_biff(wb2, 0x01B6, txo_gbk);
		push_rec_biff(wb2, 0x003C, std::string(1, '\0') + "\xD6\xD0\xCE\xC4");

		OleSpecNode txo_root2;
		txo_root2.name = "Root Entry";
		txo_root2.is_storage = true;
		txo_root2.children.push_back({"Workbook", false, wb2, {}});
		std::string txo_xls2 = build_ole2_from_tree(txo_root2);
		std::string out2;
		if (!ex.processFile(txo_xls2.data(), txo_xls2.size(), out2) ||
		    out2.find("\xF0\x9F\x98\x80" "A\nB") == std::string::npos ||
		    out2.find("caf\xC3\xA9\nend") == std::string::npos ||
		    out2.find("\xE4\xB8\xAD\xE6\x96\x87") == std::string::npos)
		{
			std::cerr << "FAIL: Step13 XLS TXO edge cases mismatch: " << out2 << "\n";
			return 1;
		}

		// 5j. PPTParser / ThreadSafeOLEStreamReader exact-EOF sector boundary (4096 B) vs truncated FAT
		std::string ppt_stream;
		push_u16_le(ppt_stream, 0x0000);
		push_u16_le(ppt_stream, 0x03EA); // RT_END_DOCUMENT_ATOM
		push_u32_le(ppt_stream, 4088);
		ppt_stream.append(4088, '\0');
		OleSpecNode ppt_root;
		ppt_root.name = "Root Entry";
		ppt_root.is_storage = true;
		ppt_root.children.push_back({"PowerPoint Document", false, ppt_stream, {}});
		std::string empty_ppt_ole = build_ole2_from_tree(ppt_root);
		PPTParser ppt_parser(empty_ppt_ole.data(), empty_ppt_ole.size());
		FormattingStyle ppt_fmt;
		std::string ppt_text = ppt_parser.plainText(ppt_fmt);
		if (!ppt_parser.isPPT() || ppt_parser.error() || !ppt_text.empty())
		{
			std::cerr << "FAIL: Step13 PPT sector-boundary EOF failed: error=" << ppt_parser.error() << "\n";
			return 1;
		}

		// 5k. OOXML DOCX alternate main document path via _rels/.rels (/word/document2.xml)
		std::vector<std::pair<std::string, std::string> > alt_docx_entries = {
			{"[Content_Types].xml", "<?xml version=\"1.0\" encoding=\"utf-8\"?><Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\"><Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/><Default Extension=\"xml\" ContentType=\"application/xml\"/><Override PartName=\"/word/document2.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/></Types>"},
			{"_rels/.rels", "<?xml version=\"1.0\" encoding=\"utf-8\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"/word/external_decoy.xml\" TargetMode=\"External\" Id=\"rId0\"/><Relationship Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"/word/document2.xml\" Id=\"rId1\"/></Relationships>"},
			{"word/_rels/document2.xml.rels", "<?xml version=\"1.0\" encoding=\"utf-8\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"/>"},
			{"word/external_decoy.xml", "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:body><w:p><w:r><w:t>EXT_DECOY_SKIP</w:t></w:r></w:p></w:body></w:document>"},
			{"word/document2.xml", "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:body><w:p><w:r><w:t>ALT_DOC_PATH_OK</w:t></w:r></w:p></w:body></w:document>"}
		};
		std::string alt_docx_buf = build_zip_from_entries(alt_docx_entries);
		std::string alt_out;
		if (!ex.processFile(alt_docx_buf.data(), alt_docx_buf.size(), alt_out) ||
		    alt_out.find("ALT_DOC_PATH_OK") == std::string::npos ||
		    alt_out.find("EXT_DECOY_SKIP") != std::string::npos)
		{
			std::cerr << "FAIL: Step13 DOCX alternate document path mismatch: " << alt_out << "\n";
			return 1;
		}

		// 5l. OOXML PPTX/XLSX alternate main document paths (ppt/presentation2.xml & xl/workbook2.xml)
		std::vector<std::pair<std::string, std::string> > alt_pptx_entries = {
			{"_rels/.rels", "<?xml version=\"1.0\" encoding=\"utf-8\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"/ppt/presentation2.xml\" Id=\"rId1\"/></Relationships>"},
			{"ppt/presentation2.xml", "<p:presentation xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"/>"},
			{"ppt/_rels/presentation2.xml.rels", "<?xml version=\"1.0\" encoding=\"utf-8\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide\" Target=\"slides/custom_slide.xml\" Id=\"rId1\"/></Relationships>"},
			{"ppt/slides/custom_slide.xml", "<p:sld xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\" xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\"><p:cSld><p:spTree><p:sp><p:txBody><a:p><a:r><a:t>ALT_PPTX_SLIDE_OK</a:t></a:r></a:p></p:txBody></p:sp></p:spTree></p:cSld></p:sld>"}
		};
		std::string alt_pptx_buf = build_zip_from_entries(alt_pptx_entries);
		std::string alt_pptx_out;
		if (!ex.processFile(alt_pptx_buf.data(), alt_pptx_buf.size(), alt_pptx_out) ||
		    alt_pptx_out.find("ALT_PPTX_SLIDE_OK") == std::string::npos)
		{
			std::cerr << "FAIL: Step13 PPTX alternate presentation path mismatch: " << alt_pptx_out << "\n";
			return 1;
		}
		Metadata alt_pptx_meta;
		if (!ex.extractMetadata(alt_pptx_buf.data(), alt_pptx_buf.size(), alt_pptx_meta) ||
		    alt_pptx_meta.pageCount() != 1)
		{
			std::cerr << "FAIL: Step13 PPTX alternate pageCount mismatch: " << alt_pptx_meta.pageCount() << "\n";
			return 1;
		}

		std::vector<std::pair<std::string, std::string> > alt_xlsx_entries = {
			{"_rels/.rels", "<?xml version=\"1.0\" encoding=\"utf-8\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"/xl/workbook2.xml\" Id=\"rId1\"/></Relationships>"},
			{"xl/workbook2.xml", "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"/>"},
			{"xl/_rels/workbook2.xml.rels", "<?xml version=\"1.0\" encoding=\"utf-8\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings\" Target=\"/xl/custom_sharedStrings.xml\" Id=\"rId1\"/><Relationship Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/custom_sheet.xml\" Id=\"rId2\"/></Relationships>"},
			{"xl/custom_sharedStrings.xml", "<sst xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><si><t>ALT_XLSX_CUSTOM_RELS_OK</t></si></sst>"},
			{"xl/worksheets/custom_sheet.xml", "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheetData><row r=\"1\"><c r=\"A1\" t=\"s\"><v>0</v></c></row></sheetData></worksheet>"}
		};
		std::string alt_xlsx_buf = build_zip_from_entries(alt_xlsx_entries);
		std::string alt_xlsx_out;
		if (!ex.processFile(alt_xlsx_buf.data(), alt_xlsx_buf.size(), alt_xlsx_out) ||
		    alt_xlsx_out.find("ALT_XLSX_CUSTOM_RELS_OK") == std::string::npos)
		{
			std::cerr << "FAIL: Step13 XLSX alternate workbook path mismatch: " << alt_xlsx_out << "\n";
			return 1;
		}
	}

	{
		// 13b. Headless HTML fragment disguised as OOXML/DOC (e.g. <p>&nbsp;...</p> without <html>)
		const std::string bare_p = "<p>\xe3\x80\x80\xe3\x80\x80&nbsp;&nbsp;\xe7\xbb\xbf\xe8\x89\xb2\xe5\x86\x9c\xe6\x88\xbf</p><p>Line2</p>";
		std::string bare_p_out;
		if (!ex.processFile(doctotext::PlainTextExtractor::PARSER_ODF_OOXML, true, bare_p.data(), bare_p.size(), bare_p_out) ||
		    bare_p_out.find("\xe7\xbb\xbf\xe8\x89\xb2\xe5\x86\x9c\xe6\x88\xbf") == std::string::npos ||
		    bare_p_out.find("&nbsp;") != std::string::npos ||
		    bare_p_out.find("Line2") == std::string::npos)
		{
			std::cerr << "FAIL: Step13b headless <p> OOXML fallback failed: " << bare_p_out << "\n";
			return 1;
		}

		const std::string bom_div = "\xEF\xBB\xBF \r\n<!-- lead comment --><div class=\"main\"><table><tr><td>BomDivOk&nbsp;1</td></tr></table></div>";
		std::string bom_div_out;
		if (!ex.processFile(doctotext::PlainTextExtractor::PARSER_DOC, true, bom_div.data(), bom_div.size(), bom_div_out) ||
		    bom_div_out.find("BomDivOk 1") == std::string::npos)
		{
			std::cerr << "FAIL: Step13b BOM+comment <div> DOC fallback failed: " << bom_div_out << "\n";
			return 1;
		}

		const std::string mixed_doctype = "<!DoCtYpE   HtMl><p>Doctype&nbsp;Ok</p>";
		std::string mixed_doctype_out;
		if (!ex.processFile(doctotext::PlainTextExtractor::PARSER_ODF_OOXML, true, mixed_doctype.data(), mixed_doctype.size(), mixed_doctype_out) ||
		    mixed_doctype_out.find("Doctype Ok") == std::string::npos)
		{
			std::cerr << "FAIL: Step13b mixed-case DOCTYPE fallback failed: " << mixed_doctype_out << "\n";
			return 1;
		}

		const std::string large_p = "<p>" + std::string(70000, 'A') + "TailToken</p>";
		std::string large_p_out;
		if (!ex.processFile(doctotext::PlainTextExtractor::PARSER_ODF_OOXML, true, large_p.data(), large_p.size(), large_p_out) ||
		    large_p_out.find("TailToken") == std::string::npos)
		{
			std::cerr << "FAIL: Step13b >64KB single-<p> tail closing check failed\n";
			return 1;
		}

		// 65539-byte boundary straddle: "</p>" starts at byte 65535 and ends at 65538
		const std::string straddle_close_p = "<p>" + std::string(65521, 'A') + "StraddleEnd</p>";
		std::string straddle_close_out;
		if (straddle_close_p.size() != 65539 ||
		    !ex.processFile(doctotext::PlainTextExtractor::PARSER_ODF_OOXML, true, straddle_close_p.data(), straddle_close_p.size(), straddle_close_out) ||
		    straddle_close_out.find("StraddleEnd") == std::string::npos)
		{
			std::cerr << "FAIL: Step13b 65539-byte </p> boundary straddle failed\n";
			return 1;
		}

		// 65537-byte boundary straddle: "/>" of "<br/>" splits across bytes 65535..65536
		const std::string straddle_self_close = "<p>SelfCloseEdge" + std::string(65516, 'B') + "<br/>";
		std::string straddle_self_out;
		if (straddle_self_close.size() != 65537 ||
		    !ex.processFile(doctotext::PlainTextExtractor::PARSER_ODF_OOXML, true, straddle_self_close.data(), straddle_self_close.size(), straddle_self_out) ||
		    straddle_self_out.find("SelfCloseEdge") == std::string::npos)
		{
			std::cerr << "FAIL: Step13b 65537-byte /> boundary straddle failed\n";
			return 1;
		}

		// Non-UTF-8 (GB18030/GBK) headless <p> fragment without <body> or <meta charset>
		const std::string gbk_p =
			"<p>\xd7\xa1\xb7\xbf\xb3\xc7\xcf\xe7\xbd\xa8\xc9\xe8\xb2\xbf\xb9\xd8\xd3\xda\xbf\xaa\xd5\xb9\xc2\xcc\xc9\xab\xc5\xa9\xb7\xbf\xbd\xa8\xc9\xe8\xb5\xc4\xcd\xa8\xd6\xaa\xa3\xac\xb4\xd9\xbd\xf8\xc5\xa9\xb4\xe5\xc2\xcc\xc9\xab\xbd\xa8\xd6\xfe\xb7\xa2\xd5\xb9\xa1\xa3&nbsp;</p>";
		std::string gbk_out;
		if (!ex.processFile(doctotext::PlainTextExtractor::PARSER_ODF_OOXML, true, gbk_p.data(), gbk_p.size(), gbk_out) ||
		    gbk_out.find("\xe4\xbd\x8f\xe6\x88\xbf\xe5\x9f\x8e\xe4\xb9\xa1\xe5\xbb\xba\xe8\xae\xbe\xe9\x83\xa8") == std::string::npos ||
		    gbk_out.find("&nbsp;") != std::string::npos)
		{
			std::cerr << "FAIL: Step13b headless GBK <p> charset detection failed: " << gbk_out << "\n";
			return 1;
		}

		// Valid XHTML with <?xml ...?> prolog must still match PARSER_HTML
		const std::string valid_xhtml = "<?xml version=\"1.0\" encoding=\"utf-8\"?><html xmlns=\"http://www.w3.org/1999/xhtml\"><p>XhtmlOk</p></html>";
		std::string xhtml_out;
		if (!ex.processFile(valid_xhtml.data(), valid_xhtml.size(), xhtml_out) ||
		    xhtml_out.find("XhtmlOk") == std::string::npos)
		{
			std::cerr << "FAIL: Step13b valid XHTML failed: " << xhtml_out << "\n";
			return 1;
		}

		// Negative guards for HTMLParser::isHTML()
		const std::vector<std::string> non_html_cases = {
			"<?xml version=\"1.0\"?><p>XmlNotHtml</p>",
			"<?xml version=\"1.0\"?><!-- <html> --><office:document>NotHtml</office:document>",
			"<?xml version=\"1.0\"?><doc><![CDATA[<html>]]></doc>",
			"<article><title>DocBook</title></article>",
			"<section><title>DocBook</title></section>",
			"<person>XmlTagNotHtml</person>",
			std::string("<p>Lucene\0\0Binary</p>", 21),
			"<p>Unclosed fragment without closing slash",
			"<p>ratio </ 5",
			"<pre>cmd 2>&1 /dev/null />",
			"<p>only comment<!-- </p> -->"
		};
		for (size_t i = 0; i < non_html_cases.size(); ++i)
		{
			doctotext::PlainTextExtractor::ParserType detected = doctotext::PlainTextExtractor::PARSER_AUTO;
			if (ex.parserTypeByFileContent(non_html_cases[i].data(), non_html_cases[i].size(), detected) &&
			    detected == doctotext::PlainTextExtractor::PARSER_HTML)
			{
				std::cerr << "FAIL: Step13b non-HTML negative case " << i << " misclassified as PARSER_HTML\n";
				return 1;
			}
		}
	}

	std::cout << "PASS: Step 12 & 13 RTF/OOXML/iWork/XLS-TXO/DOC-EQ/PPT-EOF regression checks\n";
	return 0;
}

int main()
{
	if (test_step1_unified_switch_pdf() != 0)
		return 1;
	if (test_step2_limits_isolation_and_threads() != 0)
		return 1;
	if (test_step3_ooxml_and_odf_embeddings() != 0)
		return 1;
	if (test_step4_doc_objectpool() != 0)
		return 1;
	if (test_step5_xls_mbd_and_objectpool() != 0)
		return 1;
	if (test_step6_ppt_exoleobjstg() != 0)
		return 1;
	if (test_step7_xlsb_velvet_rejection() != 0)
		return 1;
	if (test_step8_excel_edge_formats() != 0)
		return 1;
	if (test_step9_xls_and_emf_malformed_guards() != 0)
		return 1;
	if (test_step10_rtf_cp65001_and_unmatched_braces() != 0)
		return 1;
	if (test_step11_odf_negative_and_modern_iwork_guards() != 0)
		return 1;
	if (test_step12_rtf_objdata_charts_numbers() != 0)
		return 1;
	std::cout << "office_embedded_test PASSED.\n";
	return 0;
}
