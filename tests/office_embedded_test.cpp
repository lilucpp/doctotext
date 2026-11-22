#include "../build/doctotext_c_api.h"
#include "../build/formatting_style.h"
#include "../build/plain_text_extractor.h"
#include "../src/pdf_parser.h"

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

	// Embedded OLE 3 (boundary bomb): fake uncompressed_size = 0x7FFFFFFF -> must safely reject
	std::string bomb_payload;
	push_u32_le(bomb_payload, 0x7FFFFFFFu);
	bomb_payload += "BAD_ZLIB_DATA";

	std::string ppt_stream;
	// Main slide text record: RT_TEXT_BYTES_ATOM (0x0FA8)
	push_ppt_rec(ppt_stream, 0x0000, 0x0FA8, "MAIN_PPT_SLIDE");
	// Uncompressed ExOleObjStg (0x0FC1, rec_inst=0)
	push_ppt_rec(ppt_stream, 0x0000, 0x0FC1, ole1_bytes);
	// Zlib-compressed ExOleObjStg (0x0FC1, rec_inst=1 -> ver_inst=0x0010)
	push_ppt_rec(ppt_stream, 0x0010, 0x0FC1, comp_payload);
	// Zlib bomb ExOleObjStg (0x0FC1, rec_inst=1)
	push_ppt_rec(ppt_stream, 0x0010, 0x0FC1, bomb_payload);
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
	    txt_off.find("PPT_ZLIB_EXOLE_OK") != std::string::npos)
	{
		std::cerr << "FAIL: Step6 PPT parse_embedded=false did not skip ExOleObjStg:\n" << txt_off << "\n";
		return 1;
	}

	std::cout << "PASS: Step 6 PPT 0x0FC1 ExOleObjStg (raw/zlib/bomb/switch)\n";
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
	std::cout << "office_embedded_test PASSED.\n";
	return 0;
}
