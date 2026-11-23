/****************************************************************************
**
** DocToText - Converts DOC, XLS, XLSB, PPT, RTF, ODF (ODT, ODS, ODP),
**             OOXML (DOCX, XLSX, PPTX), iWork (PAGES, NUMBERS, KEYNOTE),
**             ODFXML (FODP, FODS, FODT), PDF, EML and HTML documents to plain text.
**             Extracts metadata and annotations.
**
** Copyright (c) 2006-2013, SILVERCODERS(R)
** http://silvercoders.com
**
** Project homepage: http://silvercoders.com/en/products/doctotext
**
** This program may be distributed and/or modified under the terms of the
** GNU General Public License version 2 as published by the Free Software
** Foundation and appearing in the file COPYING.GPL included in the
** packaging of this file.
**
** Please remember that any attempt to workaround the GNU General Public
** License using wrappers, pipes, client/server protocols, and so on
** is considered as license violation. If your program, published on license
** other than GNU General Public License version 2, calls some part of this
** code directly or indirectly, you have to buy commercial license.
** If you do not like our point of view, simply do not use the product.
**
** Licensees holding valid commercial license for this product
** may use this file in accordance with the license published by
** SILVERCODERS and appearing in the file COPYING.COM
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
**
*****************************************************************************/

#include "ppt_parser.h"

#include "embedded_object_extractor.h"
#include "formatting_style.h"
#include <algorithm>
#include <climits>
#include <cstring>
#include <iostream>
#include <memory>
#include <zlib.h>
#include <map>
#include <cmath>
#include "metadata.h"
#include "misc.h"
#include "oshared.h"
#include <set>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wv2/ustring.h>
#include "wv2/textconverter.h"
#include "wv2/utilities.h"
#include <vector>
#include <list>
#include <stack>
#include "thread_safe_ole_stream_reader.h"
#include "thread_safe_ole_storage.h"

using namespace wvWare;

enum RecordType
{
	RT_CSTRING = 0xFBA,
	RT_DOCUMENT = 0x03E8,
	RT_DRAWING = 0x040C,
	RT_END_DOCUMENT_ATOM = 0x03EA,
	RT_EX_OLE_OBJ_STG = 0x1011,
	RT_LIST = 0x07D0,
	RT_MAIN_MASTER = 0x03F8,
	RT_OE_PLACEHOLDER_ATOM = 0x0BC3,
	RT_SLIDE_BASE = 0x03EC, // Not in MS specification
	RT_SLIDE_LIST_WITH_TEXT = 0x0FF0,
	RT_SLIDE = 0x03EE,
	RT_TEXT_BYTES_ATOM = 0x0FA8,
	RT_TEXT_CHARS_ATOM = 0x0FA0,
	OFFICE_ART_CLIENT_DATA = 0xF011,
	OFFICE_ART_CLIENT_TEXTBOX = 0xF00D,
	OFFICE_ART_DG_CONTAINER = 0xF002,
	OFFICE_ART_SPGR_CONTAINER = 0xF003,
	OFFICE_ART_SP_CONTAINER = 0xF004
};

struct PPTParser::Implementation
{
	bool m_error;
	bool m_encrypted;
	std::string m_file_name;
	bool m_verbose_logging;
	std::ostream* m_log_stream;
	const char* m_buffer;
	size_t m_buffer_size;
	doctotext::ImageParserCallback m_image_callback;
	void* m_image_user_data;
	doctotext::FormattingStyle m_formatting_style;

	U16 getU16LE(const unsigned char* p)
	{
		return static_cast<U16>(p[0]) | (static_cast<U16>(p[1]) << 8);
	}

	U32 getU32LE(const unsigned char* p)
	{
		return static_cast<U32>(p[0]) |
		       (static_cast<U32>(p[1]) << 8) |
		       (static_cast<U32>(p[2]) << 16) |
		       (static_cast<U32>(p[3]) << 24);
	}

	U16 getU16LittleEndian(std::vector<unsigned char>::const_iterator buffer)
	{
		return (unsigned short int)(*buffer) | ((unsigned short int)(*(buffer + 1)) << 8);
	}

	U32 getU32LittleEndian(std::vector<unsigned char>::const_iterator buffer)
	{
		return (unsigned long)(*buffer) | ((unsigned long)(*(buffer +1 )) << 8L) | ((unsigned long)(*(buffer + 2)) << 16L) | ((unsigned long)(*(buffer + 3)) << 24L);
	}

	unsigned long clampRemaining(unsigned long rec_len, long end_pos, ThreadSafeOLEStreamReader& reader, bool warn_on_eof)
	{
		long cur = reader.tell();
		if (cur < 0 || cur >= end_pos)
			return 0;
		unsigned long avail = static_cast<unsigned long>(end_pos - cur);
		if (rec_len > avail)
		{
			if (warn_on_eof)
				*m_log_stream << "Warning: Read past EOF\n";
			return avail;
		}
		return rec_len;
	}

	void readTextCharsAtom(unsigned long rec_len, ThreadSafeOLEStreamReader& reader, std::string& text)
	{
		unsigned char buf[2];
		unsigned long clamped = clampRemaining(rec_len, static_cast<long>(reader.size()), reader, true);
		unsigned long text_len = clamped / 2;
		for (unsigned long i = 0; i < text_len; i++)
		{
			if (!reader.read(buf, 2))
				break;
			U32 u = getU16LE(buf);
			if (u == 0x0D || u == 0x0B)
				text += '\n';
			else
			{
				if (utf16_unichar_has_4_bytes(u) && ++i < text_len)
				{
					if (!reader.read(buf, 2))
						break;
					u = (u << 16) | getU16LE(buf);
				}
				text += unichar_to_utf8(u);
			}
		}
		if (clamped % 2 != 0)
			reader.seek(1, SEEK_CUR);
		text += '\n';
	}

	void readTextBytesAtom(unsigned long rec_len, ThreadSafeOLEStreamReader& reader, std::string& text)
	{
		unsigned char buf = 0;
		unsigned long text_len = clampRemaining(rec_len, static_cast<long>(reader.size()), reader, true);
		for (unsigned long i = 0; i < text_len; i++)
		{
			if (!reader.read(&buf, 1))
				break;
			U32 u = static_cast<U32>(buf);
			if (u == 0x0B || u == 0x0D)
				text += '\n';
			else
				text += unichar_to_utf8(u);
		}
		text += '\n';
	}

	bool clientDataHasPlaceholder(long cd_end, ThreadSafeOLEStreamReader& reader)
	{
		unsigned char hdr[8];
		while (reader.isValid() && reader.tell() + 8 <= cd_end)
		{
			if (!reader.read(hdr, 8))
				return false;
			int rec_type = getU16LE(hdr + 2);
			unsigned long rec_len = clampRemaining(getU32LE(hdr + 4), cd_end, reader, false);
			if (rec_type == RT_OE_PLACEHOLDER_ATOM)
				return true;
			reader.seek(rec_len, SEEK_CUR);
		}
		return false;
	}

	void parseMainMasterTextbox(long tb_end, ThreadSafeOLEStreamReader& reader, std::string& text)
	{
		unsigned char hdr[8];
		while (reader.isValid() && reader.tell() + 8 <= tb_end)
		{
			if (!reader.read(hdr, 8))
				break;
			int rec_type = getU16LE(hdr + 2);
			unsigned long rec_len = clampRemaining(getU32LE(hdr + 4), tb_end, reader, false);
			long rec_end = reader.tell() + static_cast<long>(rec_len);
			if (rec_type == RT_TEXT_CHARS_ATOM || rec_type == RT_CSTRING)
				readTextCharsAtom(rec_len, reader, text);
			else if (rec_type == RT_TEXT_BYTES_ATOM)
				readTextBytesAtom(rec_len, reader, text);
			if (reader.isValid())
				reader.seek(rec_end, SEEK_SET);
		}
		if (reader.isValid())
			reader.seek(tb_end, SEEK_SET);
	}

	void parseMainMasterContainer(long end_pos, int depth, ThreadSafeOLEStreamReader& reader, std::string& text)
	{
		if (depth > 32)
		{
			if (reader.isValid())
				reader.seek(end_pos, SEEK_SET);
			return;
		}
		unsigned char hdr[8];
		while (reader.isValid() && reader.tell() + 8 <= end_pos)
		{
			if (!reader.read(hdr, 8))
				break;
			U16 ver_inst = getU16LE(hdr);
			int rec_type = getU16LE(hdr + 2);
			unsigned long rec_len = clampRemaining(getU32LE(hdr + 4), end_pos, reader, false);
			long child_end = reader.tell() + static_cast<long>(rec_len);

			if (rec_type == OFFICE_ART_SP_CONTAINER)
			{
				bool is_placeholder = false;
				std::vector<std::pair<long, long> > textboxes;
				while (reader.isValid() && reader.tell() + 8 <= child_end)
				{
					if (!reader.read(hdr, 8))
						break;
					int sub_type = getU16LE(hdr + 2);
					unsigned long sub_len = clampRemaining(getU32LE(hdr + 4), child_end, reader, false);
					long sub_start = reader.tell();
					long sub_end = sub_start + static_cast<long>(sub_len);
					if (sub_type == OFFICE_ART_CLIENT_DATA)
					{
						if (clientDataHasPlaceholder(sub_end, reader))
						{
							is_placeholder = true;
							break;
						}
					}
					else if (sub_type == OFFICE_ART_CLIENT_TEXTBOX)
					{
						textboxes.push_back(std::make_pair(sub_start, sub_end));
					}
					if (reader.isValid())
						reader.seek(sub_end, SEEK_SET);
				}
				if (!is_placeholder && reader.isValid())
				{
					for (size_t t = 0; t < textboxes.size() && reader.isValid(); ++t)
					{
						reader.seek(textboxes[t].first, SEEK_SET);
						if (reader.isValid())
							parseMainMasterTextbox(textboxes[t].second, reader, text);
					}
				}
				if (reader.isValid())
					reader.seek(child_end, SEEK_SET);
			}
			else if ((ver_inst & 0x0F) == 0x0F &&
			         (rec_type == RT_DRAWING || rec_type == OFFICE_ART_DG_CONTAINER || rec_type == OFFICE_ART_SPGR_CONTAINER))
			{
				parseMainMasterContainer(child_end, depth + 1, reader, text);
			}
			else
			{
				reader.seek(child_end, SEEK_SET);
			}
		}
		if (reader.isValid())
			reader.seek(end_pos, SEEK_SET);
	}

	void parseRecord(int rec_type, unsigned long rec_len, U16 rec_inst, ThreadSafeOLEStreamReader& reader, std::string& text)
	{
		switch(rec_type)
		{
			case RT_CSTRING:
			case RT_TEXT_CHARS_ATOM: 
			{
				if (m_verbose_logging)
					*m_log_stream << "RT_TextCharsAtom or RT_CString\n";
				readTextCharsAtom(rec_len, reader, text);
				break;
			}
			case RT_DOCUMENT:
				if (m_verbose_logging)
					*m_log_stream << "RT_Document\n";
				break;
			case RT_DRAWING:
				if (m_verbose_logging)
					*m_log_stream << "RT_Drawing\n";
				break;
			case RT_END_DOCUMENT_ATOM:
			{
				if (m_verbose_logging)
					*m_log_stream << "RT_DocumentEnd\n";
				unsigned long len = clampRemaining(rec_len, static_cast<long>(reader.size()), reader, true);
				reader.seek(len, SEEK_CUR);
				break;
			}
			case RT_LIST:
				if (m_verbose_logging)
					*m_log_stream << "RT_List\n";
				break;
			case RT_MAIN_MASTER:
			{
				if (m_verbose_logging)
					*m_log_stream << "RT_MainMaster\n";
				unsigned long len = clampRemaining(rec_len, static_cast<long>(reader.size()), reader, true);
				parseMainMasterContainer(reader.tell() + static_cast<long>(len), 0, reader, text);
				break;
			}
			case RT_SLIDE:
				if (m_verbose_logging)
					*m_log_stream << "RT_Slide\n";
				break;
			case RT_SLIDE_BASE:
				break;
			case RT_SLIDE_LIST_WITH_TEXT:
				if (m_verbose_logging)
					*m_log_stream << "RT_SlideListWithText\n";
				break;
			case RT_TEXT_BYTES_ATOM:
			{
				if (m_verbose_logging)
					*m_log_stream << "RT_TextBytesAtom\n";
				readTextBytesAtom(rec_len, reader, text);
				break;
			}
			case RT_EX_OLE_OBJ_STG:
			{
				if (m_verbose_logging)
					*m_log_stream << "RT_ExOleObjStg\n";
				int cur_pos = reader.tell();
				size_t stream_size = reader.size();
				size_t avail = (cur_pos >= 0 && (size_t)cur_pos < stream_size) ? (stream_size - (size_t)cur_pos) : 0;
				unsigned long len = rec_len;
				if (len > avail)
				{
					*m_log_stream << "Warning: Read past EOF\n";
					len = (unsigned long)avail;
				}
				if (!m_formatting_style.parse_embedded_files || len == 0 || len > doctotext::kMaxEmbedBytes)
				{
					size_t skip = (size_t)len;
					while (skip > 0)
					{
						int step = (int)std::min(skip, (size_t)INT_MAX);
						if (!reader.seek(step, SEEK_CUR))
							break;
						skip -= (size_t)step;
					}
					break;
				}
				std::vector<unsigned char> raw(len);
				if (!reader.read(raw.data(), len))
					break;

				std::vector<unsigned char> ole_buf;
				if (rec_inst == 0)
				{
					ole_buf.swap(raw);
				}
				else if (rec_inst == 1 && len > 4 && (len - 4) <= (unsigned long)UINT_MAX)
				{
					U32 uncomp_len = getU32LittleEndian(raw.begin());
					const unsigned char* comp_src = raw.data() + 4;
					const size_t comp_len = (size_t)(len - 4);
					if (uncomp_len > 0 && uncomp_len <= doctotext::kMaxEmbedBytes)
					{
						struct ZStreamGuard
						{
							z_stream zs;
							bool active;
							ZStreamGuard() : active(false) { std::memset(&zs, 0, sizeof(zs)); }
							~ZStreamGuard() { if (active) inflateEnd(&zs); }
						} guard;

						if (inflateInit(&guard.zs) == Z_OK)
						{
							guard.active = true;
							const size_t init_cap = (uncomp_len < doctotext::kMaxEmbedBytes) ? ((size_t)uncomp_len + 1) : (size_t)uncomp_len;
							ole_buf.resize(init_cap);
							guard.zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(comp_src));
							guard.zs.avail_in = static_cast<uInt>(comp_len);
							const bool has_sync_trailer =
								(comp_len >= 4 && std::memcmp(comp_src + comp_len - 4, "\x00\x00\xFF\xFF", 4) == 0);
							bool ok = false;
							while (true)
							{
								if (guard.zs.total_out >= ole_buf.size())
								{
									if (ole_buf.size() >= doctotext::kMaxEmbedBytes)
									{
										unsigned char probe = 0;
										guard.zs.next_out = reinterpret_cast<Bytef*>(&probe);
										guard.zs.avail_out = 1;
										int probe_ret = inflate(&guard.zs, Z_SYNC_FLUSH);
										ok = (probe_ret == Z_STREAM_END && guard.zs.avail_out == 1) ||
										     ((probe_ret == Z_OK || probe_ret == Z_BUF_ERROR) &&
										      guard.zs.avail_in == 0 && guard.zs.avail_out == 1 &&
										      (has_sync_trailer || guard.zs.total_out == uncomp_len));
										break;
									}
									size_t next_sz = (ole_buf.size() > doctotext::kMaxEmbedBytes / 2)
									                     ? doctotext::kMaxEmbedBytes
									                     : (ole_buf.size() * 2);
									ole_buf.resize(next_sz);
								}
								guard.zs.next_out = reinterpret_cast<Bytef*>(ole_buf.data() + guard.zs.total_out);
								guard.zs.avail_out = static_cast<uInt>(ole_buf.size() - guard.zs.total_out);
								int ret = inflate(&guard.zs, Z_SYNC_FLUSH);
								if (ret == Z_STREAM_END)
								{
									ok = (guard.zs.total_out > 0);
									break;
								}
								// MS-PPT ExOleObjStgCompressedAtom flushes with Z_SYNC_FLUSH (00 00 FF FF) without Z_FINISH.
								if ((ret == Z_OK || ret == Z_BUF_ERROR) && guard.zs.avail_in == 0 && guard.zs.avail_out > 0 &&
								    guard.zs.total_out > 0 && (has_sync_trailer || guard.zs.total_out == uncomp_len))
								{
									ok = true;
									break;
								}
								if (ret != Z_OK || guard.zs.avail_out != 0)
								{
									break;
								}
							}
							if (ok && guard.zs.total_out <= ole_buf.size())
								ole_buf.resize((size_t)guard.zs.total_out);
							else
								ole_buf.clear();
						}
					}
				}
				if (!ole_buf.empty())
				{
					doctotext::EmbeddedExtractionContext fallback_ctx;
					fallback_ctx.formatting = m_formatting_style;
					fallback_ctx.verbose_logging = m_verbose_logging;
					fallback_ctx.log_stream = m_log_stream;
					fallback_ctx.image_callback = m_image_callback;
					fallback_ctx.image_user_data = m_image_user_data;
					doctotext::EmbeddedExtractionContext::ScopedContext tls_scope(fallback_ctx);
					doctotext::extractFromEmbeddedContainerBuffer(
						ole_buf.data(), ole_buf.size(), "embedded_ppt_ole", tls_scope.context(), text);
				}
				break;
			}
			case OFFICE_ART_CLIENT_TEXTBOX:
				if (m_verbose_logging)
					*m_log_stream << "OfficeArtClientTextbox\n";
				break;
			case OFFICE_ART_DG_CONTAINER:
				if (m_verbose_logging)
					*m_log_stream << "OfficeArtDgContainer\n";
				break;
			case OFFICE_ART_SPGR_CONTAINER:
				if (m_verbose_logging)
					*m_log_stream << "OfficeArtSpgrContainer\n";
				break;
			case OFFICE_ART_SP_CONTAINER:
				if (m_verbose_logging)
					*m_log_stream << "OfficeArtSpContainer\n";
				break;
			default:
				unsigned long len = rec_len;
				if (reader.tell() + len > reader.size())
				{
					*m_log_stream << "Warning: Read past EOF\n";
					len = reader.size() - reader.tell();
				}
				reader.seek(len, SEEK_CUR);
		}
		if (!reader.isValid())
		{
			*m_log_stream << reader.getLastError() + "\n";
			m_error = true;
		}
	}

	bool oleEof(ThreadSafeOLEStreamReader& reader)
	{
		return reader.tell() == reader.size();
	}

	void parseOldPPT(ThreadSafeOLEStorage& storage, ThreadSafeOLEStreamReader& reader, std::string& text)
	{
		std::vector<unsigned char> content(reader.size());
		if (!reader.read(&*content.begin(), reader.size()))	//this stream should only contain text
		{
			*m_log_stream << "Error reading Text_Content stream.\n";
			m_error = true;
			return;
		}
		text = std::string(content.begin(), content.end());
		std::string codepage;
		if (get_codepage_from_document_summary_info(storage, *m_log_stream, codepage))
		{
			TextConverter tc(codepage);
			text = ustring_to_string(tc.convert(text));
		}
		//new lines problem?
		for (size_t i = 0; i < text.length(); ++i)
			if (text[i] == 11)
				text[i] = 13;
	}

	void parsePPT(ThreadSafeOLEStreamReader& reader, std::string& text)
	{
		std::vector<unsigned char> rec(8);
		bool read_status = true;
		std::stack<long> container_ends;
		while (read_status)
		{
			int pos = reader.tell();
			read_status = reader.read(&*rec.begin(), 8);
			if (oleEof(reader))
			{
				parseRecord(RT_END_DOCUMENT_ATOM, 0, 0, reader, text);
				return;
			}
			if (rec.size() < 8)
				break;
			U16 rec_inst = getU16LittleEndian(rec.begin()) >> 4;
			int rec_type = getU16LittleEndian(rec.begin() + 2);
			U32 rec_len = getU32LittleEndian(rec.begin() + 4);
			if (m_verbose_logging)
			{
				while (!container_ends.empty() && pos+rec_len-1 > container_ends.top())
					container_ends.pop();
				std::string indend;
				for (int i = 0; i < container_ends.size(); i++)
					indend += "\t";
				*m_log_stream << indend << "record=0x" << std::hex << rec_type << ", begin=0x" << pos << ", end=0x" << pos + rec_len - 1 << "\n";
				container_ends.push(pos + rec_len - 1);
			}
			parseRecord(rec_type, rec_len, rec_inst, reader, text);
		}
	}
};

PPTParser::PPTParser(const std::string& file_name)
{
	impl = new Implementation();
	impl->m_error = false;
	impl->m_encrypted = false;
	impl->m_file_name = file_name;
	impl->m_verbose_logging = false;
	impl->m_log_stream = &std::cerr;
	impl->m_buffer = NULL;
	impl->m_buffer_size = 0;
	impl->m_image_callback = NULL;
	impl->m_image_user_data = NULL;
}

PPTParser::PPTParser(const char *buffer, size_t size)
{
	impl = new Implementation();
	impl->m_error = false;
	impl->m_encrypted = false;
	impl->m_file_name = "Memory buffer";
	impl->m_verbose_logging = false;
	impl->m_log_stream = &std::cerr;
	impl->m_buffer = buffer;
	impl->m_buffer_size = size;
	impl->m_image_callback = NULL;
	impl->m_image_user_data = NULL;
}

PPTParser::~PPTParser()
{
	delete impl;
}

void PPTParser::setImageParserCallback(doctotext::ImageParserCallback callback, void* user_data)
{
	impl->m_image_callback = callback;
	impl->m_image_user_data = user_data;
}

void PPTParser::setVerboseLogging(bool verbose)
{
	impl->m_verbose_logging = verbose;
}

void PPTParser::setLogStream(std::ostream& log_stream)
{
	impl->m_log_stream = &log_stream;
}

bool PPTParser::isPPT()
{
	impl->m_error = false;
	impl->m_encrypted = false;
	ThreadSafeOLEStorage* storage = NULL;
	if (impl->m_buffer)
		storage = new ThreadSafeOLEStorage(impl->m_buffer, impl->m_buffer_size);
	else
		storage = new ThreadSafeOLEStorage(impl->m_file_name);
	if (!storage->isValid())
	{
		delete storage;
		return false;
	}
	AbstractOLEStreamReader* reader = storage->createStreamReader("PowerPoint Document");
	if (reader == NULL)
	{
		delete storage;
		return false;
	}
	delete reader;
	delete storage;
	return true;
}

void PPTParser::getLinks(std::vector<Link>& links)
{
	#warning TODO: Implement this functionality.
}

static void parse_ppt_pictures(ThreadSafeOLEStorage& storage, doctotext::ImageParserCallback callback, void* user_data, std::ostream& log_stream, std::string& text)
{
	if (!callback)
		return;
	AbstractOLEStreamReader* reader = storage.createStreamReader("Pictures");
	if (!reader)
		return;

	size_t sz = reader->size();
	if (sz < 8)
	{
		delete reader;
		return;
	}

	std::vector<uint8_t> data(sz);
	if (!reader->read(data.data(), sz))
	{
		delete reader;
		return;
	}
	delete reader;

	int img_count = 0;
	for (size_t i = 0; i + 8 <= sz; )
	{
		uint16_t ver_inst = data[i] | (data[i+1] << 8);
		uint16_t rec_type = data[i+2] | (data[i+3] << 8);
		uint32_t rec_len = data[i+4] | (data[i+5] << 8) | (data[i+6] << 16) | (data[i+7] << 24);

		if (rec_len == 0 || i + 8 + rec_len > sz)
			break;

		// Escher Blip record types: 0xF018 to 0xF02A
		if (rec_type >= 0xF018 && rec_type <= 0xF02A)
		{
			size_t header_skip = 17;
			if (rec_len >= 33 + 4)
			{
				if (memcmp(&data[i + 8 + 33], "\x89PNG", 4) == 0 ||
				    (data[i + 8 + 33] == 0xFF && data[i + 8 + 34] == 0xD8) ||
				    memcmp(&data[i + 8 + 33], "BM", 2) == 0)
				{
					header_skip = 33;
				}
			}

			if (rec_len > header_skip)
			{
				size_t img_size = rec_len - header_skip;
				const uint8_t* img_bytes = &data[i + 8 + header_skip];

				std::string ext = "png";
				if (img_size >= 4 && memcmp(img_bytes, "\x89PNG", 4) == 0)
					ext = "png";
				else if (img_size >= 3 && img_bytes[0] == 0xFF && img_bytes[1] == 0xD8)
					ext = "jpg";
				else if (img_size >= 2 && memcmp(img_bytes, "BM", 2) == 0)
					ext = "bmp";
				else if (rec_type == 0xF01D || rec_type == 0xF01F || rec_type == 0xF029)
					ext = "jpg";

				img_count++;
				std::string img_name = "ppt/media/image" + std::to_string(img_count) + "." + ext;
				std::string parsed = callback(img_name, reinterpret_cast<const char*>(img_bytes), img_size, user_data);
				if (!parsed.empty())
				{
					if (!text.empty() && text[text.size() - 1] != '\n')
						text += "\n";
					text += parsed;
					if (text[text.size() - 1] != '\n')
						text += "\n";
				}
			}
		}
		i += 8 + rec_len;
	}
}

std::string PPTParser::plainText(const FormattingStyle& formatting)
{
	impl->m_error = false;
	impl->m_encrypted = false;
	impl->m_formatting_style = formatting;
	std::unique_ptr<ThreadSafeOLEStorage> storage;
	if (impl->m_buffer)
		storage.reset(new ThreadSafeOLEStorage(impl->m_buffer, impl->m_buffer_size));
	else
		storage.reset(new ThreadSafeOLEStorage(impl->m_file_name));
	if (!storage->isValid())
	{
		*impl->m_log_stream << "Error opening " << impl->m_file_name << " as OLE file.\n";
		impl->m_error = true;
		return "";
	}
	return plainText(*storage, formatting);
}

std::string PPTParser::plainText(ThreadSafeOLEStorage& storage, const FormattingStyle& formatting)
{
	impl->m_error = false;
	impl->m_encrypted = false;
	impl->m_formatting_style = formatting;
	if (!storage.isValid())
	{
		*impl->m_log_stream << "Error opening " << impl->m_file_name << " as OLE file.\n";
		impl->m_error = true;
		return "";
	}
	if (is_encrypted_ppt_ole(storage))
	{
		*impl->m_log_stream << "PPT file is encrypted and requires a password.\n";
		impl->m_encrypted = true;
		impl->m_error = true;
		return "";
	}
	std::string text;
	std::vector<std::string> dirs;
	if (storage.getStreamsAndStoragesList(dirs))
	{
		for (size_t i = 0; i < dirs.size(); ++i)
		{
			#warning TODO: Older PPT files (like Microsoft PowerPoint 4.0) have stream named "Text_Content". \
			I have only one file of this type and I cant find any documentation about it. I am not sure \
			if parsing those files is correct now... but for this one file that I have it works. It should be tested \
			more, one file is not enough. Stream "Text_Content" does not exist in newer versions of PowerPoint. \
			In older versions of PowerPoint (the file I have is an example) reading stream "PowerPoint Document" does not work.
			#warning TODO: Check if there is a better way to get PowerPoint version.
			if (dirs[i] == "Text_Content")
			{
				ThreadSafeOLEStreamReader* reader = (ThreadSafeOLEStreamReader*)storage.createStreamReader("Text_Content");
				if (reader != NULL)
				{
					impl->parseOldPPT(storage, *reader, text);
					delete reader;
				}
				else
				{
					*impl->m_log_stream << storage.getLastError() + "\n";
					impl->m_error = true;
				}
				return text;
			}
		}
	}
	ThreadSafeOLEStreamReader* reader = (ThreadSafeOLEStreamReader*)storage.createStreamReader("PowerPoint Document");
	if (reader == NULL)
	{
		*impl->m_log_stream << "Error opening " << impl->m_file_name << " as OLE file.\n";
		impl->m_error = true;
		return "";
	}
	doctotext::EmbeddedExtractionContext root_ctx;
	root_ctx.formatting = formatting;
	root_ctx.verbose_logging = impl->m_verbose_logging;
	root_ctx.log_stream = impl->m_log_stream;
	root_ctx.image_callback = impl->m_image_callback;
	root_ctx.image_user_data = impl->m_image_user_data;
	doctotext::EmbeddedExtractionContext::ScopedContext tls_scope(root_ctx);
	impl->parsePPT(*reader, text);
	delete reader;
	parse_ppt_pictures(storage, impl->m_image_callback, impl->m_image_user_data, *impl->m_log_stream, text);
	return text;
}

Metadata PPTParser::metaData()
{
	impl->m_error = false;
	impl->m_encrypted = false;
	Metadata meta;
	ThreadSafeOLEStorage* storage = NULL;
	if (impl->m_buffer)
		storage = new ThreadSafeOLEStorage(impl->m_buffer, impl->m_buffer_size);
	else
		storage = new ThreadSafeOLEStorage(impl->m_file_name);
	if (storage->isValid() && is_encrypted_ppt_ole(*storage))
	{
		impl->m_encrypted = true;
		impl->m_error = true;
		delete storage;
		return meta;
	}
	if (!parse_oshared_summary_info(*storage, *impl->m_log_stream, meta))
	{
		impl->m_error = true;
		delete storage;
		return meta;
	}
	// If page count not found use slide count as page count
	if (meta.pageCount() == -1)
	{
		int slide_count = 150;
		impl->m_error = !parse_oshared_document_summary_info(*storage, *impl->m_log_stream, slide_count);
		if (!impl->m_error)
			meta.setPageCount(slide_count);
	}
	delete storage;
	return meta;
}

bool PPTParser::error()
{
	return impl->m_error;
}

bool PPTParser::encrypted()
{
	return impl->m_encrypted;
}
