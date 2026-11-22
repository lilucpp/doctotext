#include "../src/pdf_parser.h"
#include "../build/formatting_style.h"
#include "../build/plain_text_extractor.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

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

static int test_embedded_files_btree()
{
	const std::string rtf_doc = "{\\rtf1 Embedded RTF Child}";
	const std::string job_txt = "<<\n  /ASCII85EncodePages false\n>>";
	const std::string page_stream = "BT /F1 12 Tf 72 720 Td (Main Page Body) Tj ET";

	std::vector<std::string> objs;
	// 1: Catalog -> Pages (2) and Names (7)
	objs.push_back("<< /Type /Catalog /Pages 2 0 R /Names 7 0 R >>");
	// 2: Pages
	objs.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
	// 3: Page
	objs.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>");
	// 4: Font
	objs.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
	// 5: Page Content
	{
		std::ostringstream s;
		s << "<< /Length " << page_stream.size() << " >>\nstream\n" << page_stream << "\nendstream";
		objs.push_back(s.str());
	}
	// 6: Unused placeholder or second kid leaf - let's use 6 as Leaf A, 7 as Names, 8 as EmbeddedFiles Root, 9..12 as filespecs/streams
	// 6: Leaf A (RTF)
	objs.push_back("<< /Names [(child.rtf) 9 0 R] >>");
	// 7: Names dict
	objs.push_back("<< /EmbeddedFiles 8 0 R >>");
	// 8: EmbeddedFiles B-tree parent with /Kids [6 0 R 11 0 R]
	objs.push_back("<< /Kids [6 0 R 11 0 R] >>");
	// 9: Filespec A
	objs.push_back("<< /Type /Filespec /F (child.rtf) /UF (child.rtf) /EF << /F 10 0 R >> >>");
	// 10: Stream A
	{
		std::ostringstream s;
		s << "<< /Length " << rtf_doc.size() << " >>\nstream\n" << rtf_doc << "\nendstream";
		objs.push_back(s.str());
	}
	// 11: Leaf B (joboptions text)
	objs.push_back("<< /Names [(Preset.joboptions) 12 0 R] >>");
	// 12: Filespec B
	objs.push_back("<< /Type /Filespec /F (Preset.joboptions) /EF << /F 13 0 R >> >>");
	// 13: Stream B
	{
		std::ostringstream s;
		s << "<< /Length " << job_txt.size() << " >>\nstream\n" << job_txt << "\nendstream";
		objs.push_back(s.str());
	}

	std::string pdf_bytes = build_pdf_from_objects(objs, 1);

	doctotext::PlainTextExtractor extractor;
	std::string text;
	if (!extractor.processFile(pdf_bytes.data(), pdf_bytes.size(), text))
	{
		std::cerr << "FAIL: embedded files B-tree PDF failed to parse\n";
		return 1;
	}
	if (text.find("Main Page Body") == std::string::npos ||
	    text.find("Embedded RTF Child") == std::string::npos ||
	    text.find("/ASCII85EncodePages false") == std::string::npos)
	{
		std::cerr << "FAIL: missing expected body or embedded text:\n" << text << "\n";
		return 1;
	}

	PDFParser direct_pdf(pdf_bytes.data(), pdf_bytes.size());
	doctotext::FormattingStyle fmt;
	direct_pdf.plainText(fmt);
	std::vector<Attachment> atts;
	direct_pdf.getAttachments(atts);
	if (atts.size() != 2)
	{
		std::cerr << "FAIL: expected 2 PDF attachments, got " << atts.size() << "\n";
		return 1;
	}
	std::cout << "PASS: PDF /Names /EmbeddedFiles B-tree + text + getAttachments\n";
	return 0;
}

static int test_xobject_font_scoping_and_glyphs()
{
	// Form XObject /I1 defines /T1_0 with 31=/uni25CF, 32=/unknownSkip, 33=/f_i, 34=/f_l, 35=/seven.oldstyle
	// Form XObject /I2 ALSO defines /T1_0 with standard WinAnsi (31 unset, 'A'=65) -> must NOT overwrite /I1's /T1_0!
	const std::string form1_stream = "BT /T1_0 12 Tf 72 720 Td (\\037!\"#$) Tj ET";
	const std::string form2_stream = "BT /T1_0 12 Tf 72 680 Td (FormTwoText) Tj ET";
	const std::string page_stream = "/I1 Do\n/I2 Do\n";

	// Character codes in form1_stream:
	// \037 = 31 -> /uni25CF -> U+25CF (●, UTF-8 \xE2\x97\x8F)
	// '!'  = 33 -> after 32 (/unknownGlyphName) advances slot -> 33=/f_i -> ﬁ (\xEF\xAC\x81)
	// '"'  = 34 -> /f_l -> ﬂ (\xEF\xAC\x82)
	// '#'  = 35 -> /seven.oldstyle -> 7
	// '$'  = 36 -> /u1F600 -> 😀 (\xF0\x9F\x98\x80)
	std::vector<std::string> objs;
	// 1: Catalog
	objs.push_back("<< /Type /Catalog /Pages 2 0 R >>");
	// 2: Pages
	objs.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
	// 3: Page
	objs.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /XObject << /I1 5 0 R /I2 6 0 R >> >> /Contents 4 0 R >>");
	// 4: Page Content
	{
		std::ostringstream s;
		s << "<< /Length " << page_stream.size() << " >>\nstream\n" << page_stream << "\nendstream";
		objs.push_back(s.str());
	}
	// 5: Form XObject /I1 (placed BEFORE /I2 in Resources map!)
	{
		std::ostringstream s;
		s << "<< /Type /XObject /Subtype /Form /BBox [0 0 612 792] "
		  << "/Resources << /Font << /T1_0 7 0 R >> >> /Length " << form1_stream.size()
		  << " >>\nstream\n" << form1_stream << "\nendstream";
		objs.push_back(s.str());
	}
	// 6: Form XObject /I2 (reuses /T1_0!)
	{
		std::ostringstream s;
		s << "<< /Type /XObject /Subtype /Form /BBox [0 0 612 792] "
		  << "/Resources << /Font << /T1_0 8 0 R >> >> /Length " << form2_stream.size()
		  << " >>\nstream\n" << form2_stream << "\nendstream";
		objs.push_back(s.str());
	}
	// 7: Font for /I1 /T1_0
	objs.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /CustomFont /Encoding << /Type /Encoding /BaseEncoding /WinAnsiEncoding /Differences [31 /uni25CF /unknownGlyphName /f_i /f_l /seven.oldstyle /u1F600] >> >>");
	// 8: Font for /I2 /T1_0
	objs.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");

	std::string pdf_bytes = build_pdf_from_objects(objs, 1);

	doctotext::PlainTextExtractor extractor;
	std::string text;
	if (!extractor.processFile(pdf_bytes.data(), pdf_bytes.size(), text))
	{
		std::cerr << "FAIL: Form XObject font scoping PDF failed to parse\n";
		return 1;
	}
	const std::string expected = "\xE2\x97\x8F\xEF\xAC\x81\xEF\xAC\x82" "7\xF0\x9F\x98\x80";
	if (text.find(expected) == std::string::npos || text.find("FormTwoText") == std::string::npos)
	{
		std::cerr << "FAIL: Form XObject font scoping or glyph decoding mismatch.\nGot: " << text << "\n";
		return 1;
	}
	std::cout << "PASS: Form XObject font scoping + /Differences (uniXXXX, uXXXXX, f_i, f_l, variant, unknown slot advance)\n";
	return 0;
}

static int test_multicolumn_and_compact_whitespace()
{
	// 1. Asymmetric 2-column cover + 3-column page + compact_whitespace normalization
	std::ostringstream p2col;
	p2col << "BT /F1 10 Tf ";
	for (int i = 0; i < 12; ++i)
	{
		int y = 720 - i * 32;
		p2col << "1 0 0 1 42 " << y << " Tm (LeftColItemWord" << i << "Details) Tj ";
	}
	for (int i = 0; i < 28; ++i)
	{
		int y = 720 - i * 20;
		p2col << "1 0 0 1 320 " << y << " Tm (RightColumnEntryLineNumber" << i << ") Tj ";
	}
	p2col << "ET";

	std::ostringstream p3col;
	p3col << "BT /F1 10 Tf ";
	const int xs[3] = {42, 220, 400};
	for (int c = 0; c < 3; ++c)
	{
		for (int i = 0; i < 28; ++i)
		{
			int y = 720 - i * 20;
			p3col << "1 0 0 1 " << xs[c] << " " << y << " Tm (Col" << (c + 1) << "IndexLineEntryNumber" << i << ") Tj ";
		}
	}
	// Justified spaced words at bottom
	p3col << "1 0 0 1 42 80 Tm (Alpha) Tj 1 0 0 1 82 80 Tm (Beta) Tj 1 0 0 1 250 80 Tm (TableCol) Tj ET";

	std::vector<std::string> objs;
	objs.push_back("<< /Type /Catalog /Pages 2 0 R >>");
	objs.push_back("<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>");
	objs.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> >> /Contents 6 0 R >>");
	objs.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> >> /Contents 7 0 R >>");
	objs.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
	{
		std::ostringstream s;
		std::string b = p2col.str();
		s << "<< /Length " << b.size() << " >>\nstream\n" << b << "\nendstream";
		objs.push_back(s.str());
	}
	{
		std::ostringstream s;
		std::string b = p3col.str();
		s << "<< /Length " << b.size() << " >>\nstream\n" << b << "\nendstream";
		objs.push_back(s.str());
	}

	std::string pdf_bytes = build_pdf_from_objects(objs, 1);

	doctotext::PlainTextExtractor extractor;
	doctotext::FormattingStyle fmt;
	fmt.compact_whitespace = true;
	extractor.setFormattingStyle(fmt);
	std::string text;
	if (!extractor.processFile(pdf_bytes.data(), pdf_bytes.size(), text))
	{
		std::cerr << "FAIL: multicolumn PDF failed to parse\n";
		return 1;
	}

	// Verify 2-col ordering (all left col precedes right col)
	size_t pos_left_last = text.find("LeftColItemWord11Details");
	size_t pos_right_first = text.find("RightColumnEntryLineNumber0");
	if (pos_left_last == std::string::npos || pos_right_first == std::string::npos || pos_left_last > pos_right_first)
	{
		std::cerr << "FAIL: asymmetric 2-col ordering failed\n";
		return 1;
	}

	// Verify 3-col ordering (col1 -> col2 -> col3)
	size_t p_c1 = text.find("Col1IndexLineEntryNumber27");
	size_t p_c2 = text.find("Col2IndexLineEntryNumber0");
	size_t p_c2_end = text.find("Col2IndexLineEntryNumber27");
	size_t p_c3 = text.find("Col3IndexLineEntryNumber0");
	if (p_c1 == std::string::npos || p_c2 == std::string::npos || p_c2_end == std::string::npos || p_c3 == std::string::npos ||
	    !(p_c1 < p_c2 && p_c2_end < p_c3))
	{
		std::cerr << "FAIL: 3-col reading order failed:\n" << text << "\n";
		return 1;
	}

	// Verify compact_whitespace collapses >=4 spaces to 2 and eliminates >=3 blank lines
	if (text.find("   ") != std::string::npos || text.find("\n\n\n") != std::string::npos)
	{
		std::cerr << "FAIL: compact_whitespace left >=3 spaces or >=3 newlines\n";
		return 1;
	}

	std::cout << "PASS: asymmetric 2-col + 3-col reading order + compact_whitespace\n";
	return 0;
}

int main()
{
	if (test_embedded_files_btree() != 0)
		return 1;
	if (test_xobject_font_scoping_and_glyphs() != 0)
		return 1;
	if (test_multicolumn_and_compact_whitespace() != 0)
		return 1;
	printf("pdf_embedded_and_glyph_test PASSED.\n");
	return 0;
}
