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

#include "xls_parser.h"

#include "data_stream.h"
#include "embedded_object_extractor.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <map>
#include <cmath>
#include "misc.h"
#include "oshared.h"
#include <set>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utility>
#include <wv2/ustring.h>
#include "wv2/textconverter.h"
#include "wv2/utilities.h"
#include <vector>
#include <time.h>
#include "thread_safe_ole_stream_reader.h"
#include "thread_safe_ole_storage.h"

using namespace wvWare;

enum RecordType
{
	XLS_BOF = 0x809,
	XLS_CODEPAGE = 0x42,
	XLS_BLANK = 0x201,
	XLS_CONTINUE = 0x3C,
	XLS_DATE_1904 = 0x22,
	XLS_FILEPASS = 0x2F,
	XLS_FORMAT = 0x41E,
	XLS_FORMULA = 0x06,
	XLS_FORMULA_BIFF4 = 0x406,
	XLS_INTEGER_CELL = 0x202, // Not in MS specification
	XLS_LABEL_BIFF2 = 0x004,
	XLS_LABEL = 0x204,
	XLS_RSTRING = 0xD6,
	XLS_LABEL_SST = 0xFD,
	XLS_MULBLANK = 0xBE,
	XLS_MULRK = 0xBD,
	XLS_NUMBER = 0x203,
	XLS_RK = 0x27E,
	XLS_SST = 0xFC,
	XLS_STRING = 0x207,
	XLS_XF = 0xE0,
	XLS_XF_BIFF4 = 0x443,
	XLS_EOF = 0x0A
};

namespace
{
	struct BiffReader
	{
		virtual ~BiffReader() {}
		virtual bool readU16(U16& out) = 0;
		virtual bool read(U8* buf, size_t len) = 0;
		virtual size_t tell() const = 0;
		virtual size_t size() const = 0;
		virtual std::string getLastError() = 0;
	};

	class OleBiffReader : public BiffReader
	{
		ThreadSafeOLEStreamReader& m_reader;
	public:
		explicit OleBiffReader(ThreadSafeOLEStreamReader& r) : m_reader(r) {}
		bool readU16(U16& out) { return m_reader.readU16(out); }
		bool read(U8* buf, size_t len) { return m_reader.read(buf, len); }
		size_t tell() const
		{
			int pos = m_reader.tell();
			return pos >= 0 ? (size_t)pos : m_reader.size();
		}
		size_t size() const { return m_reader.size(); }
		std::string getLastError() { return m_reader.getLastError(); }
	};

	class RawBiffReader : public BiffReader
	{
		DataStream& m_stream;
		size_t m_size;
		std::string m_last_error;
	public:
		explicit RawBiffReader(DataStream& s) : m_stream(s), m_size(s.size()) {}
		bool readU16(U16& out)
		{
			U8 b[2];
			if (!m_stream.read(b, 1, 2))
			{
				m_last_error = "Unexpected end of BIFF stream";
				return false;
			}
			out = (U16)b[0] | ((U16)b[1] << 8);
			return true;
		}
		bool read(U8* buf, size_t len)
		{
			if (len == 0)
				return true;
			if (!m_stream.read(buf, 1, len))
			{
				m_last_error = "Unexpected end of BIFF stream";
				return false;
			}
			return true;
		}
		size_t tell() const { return m_stream.tell(); }
		size_t size() const { return m_size; }
		std::string getLastError() { return m_last_error; }
	};

	bool isBofRecordType(U16 rec_type)
	{
		return rec_type == 0x0009 || rec_type == 0x0209 || rec_type == 0x0409 || rec_type == XLS_BOF;
	}

	ThreadSafeOLEStreamReader* openWorkbookStream(ThreadSafeOLEStorage& storage)
	{
		ThreadSafeOLEStreamReader* reader = (ThreadSafeOLEStreamReader*)storage.createStreamReader("Workbook");
		if (reader != NULL)
			return reader;
		reader = (ThreadSafeOLEStreamReader*)storage.createStreamReader("Book");
		if (reader != NULL)
			return reader;
		std::vector<std::string> entries;
		if (!storage.getStreamsAndStoragesList(entries))
			return NULL;
		const char* candidates[2] = {"workbook", "book"};
		for (int pass = 0; pass < 2; ++pass)
		{
			for (size_t i = 0; i < entries.size(); ++i)
			{
				if (asciiEqualsIgnoreCase(entries[i], candidates[pass]))
				{
					reader = (ThreadSafeOLEStreamReader*)storage.createStreamReader(entries[i]);
					if (reader != NULL)
						return reader;
				}
			}
		}
		return NULL;
	}

	bool hasRawBiffBofHeader(DataStream& ds)
	{
		size_t total = ds.size();
		if (total < 8 || !ds.seek(0, SEEK_SET))
			return false;
		unsigned char hdr[8];
		if (!ds.read(hdr, 1, 8))
			return false;
		if (!ds.seek(0, SEEK_SET))
			return false;
		U16 rec_type = (U16)hdr[0] | ((U16)hdr[1] << 8);
		U16 rec_len = (U16)hdr[2] | ((U16)hdr[3] << 8);
		U16 biff_vers = (U16)hdr[4] | ((U16)hdr[5] << 8);
		U16 data_type = (U16)hdr[6] | ((U16)hdr[7] << 8);
		if (4 + (size_t)rec_len > total)
			return false;
		bool valid_data_type = (data_type == 0x0005 || data_type == 0x0006 || data_type == 0x0010 ||
		                        data_type == 0x0020 || data_type == 0x0040 || data_type == 0x0100);
		if (rec_type == 0x0009 && rec_len == 4 && valid_data_type)
			return true;
		if ((rec_type == 0x0209 || rec_type == 0x0409) && rec_len == 6 && valid_data_type)
			return true;
		if (rec_type == 0x0809 && (rec_len == 8 || rec_len == 16) && (biff_vers == 0x0500 || biff_vers == 0x0600) && valid_data_type)
			return true;
		return false;
	}
}

struct XLSParser::Implementation
{
	bool m_error;
	bool m_encrypted;
	bool m_is_buffer;
	const char* m_buffer;
	size_t m_buffer_size;
	std::string m_codepage;
	std::string m_file_name;
	bool m_verbose_logging;
	std::ostream* m_log_stream;
	enum BiffVersion { BIFF2, BIFF3, BIFF4, BIFF5, BIFF8 };
	BiffVersion m_biff_version;
	struct XFRecord
	{
		short int num_format_id;
	};
	std::vector<XFRecord> m_xf_records;
	double m_date_shift;
	std::vector<std::string> m_shared_string_table;
	std::vector<unsigned char> m_shared_string_table_buf;
	std::vector<size_t> m_shared_string_table_record_sizes;
	int m_prev_rec_type;
	int m_last_string_formula_row;
	int m_last_string_formula_col;
	std::set<int> m_defined_num_format_ids;
	int m_last_row, m_last_col;
	doctotext::ImageParserCallback m_image_callback;
	void* m_image_user_data;

	U16 getU16LittleEndian(std::vector<unsigned char>::const_iterator buffer)
	{
		return (unsigned short int)(*buffer) | ((unsigned short int)(*(buffer + 1)) << 8);
	}
	
	S32 getS32LittleEndian(std::vector<unsigned char>::const_iterator buffer)
	{
		uint32_t v = (uint32_t)(*buffer) |
		             ((uint32_t)(*(buffer + 1)) << 8) |
		             ((uint32_t)(*(buffer + 2)) << 16) |
		             ((uint32_t)(*(buffer + 3)) << 24);
		return (S32)(int32_t)v;
	}  

	class StandardDateFormats : public std::map<int, std::string>
	{
		public:
			StandardDateFormats()
			{
				insert(value_type(0x0E, "%m-%d-%y"));
				insert(value_type(0x0F, "%d-%b-%y"));
				insert(value_type(0x10, "%d-%b"));
				insert(value_type(0x11, "%b-%d"));
				insert(value_type(0x12, "%l:%M %p"));
				insert(value_type(0x13, "%l:%M:%S %p"));
				insert(value_type(0x14, "%H:%M"));
				insert(value_type(0x15, "%H:%M:%S"));
				insert(value_type(0x16, "%m-%d-%y %H:%M"));
				insert(value_type(0x2d, "%M:%S"));
				insert(value_type(0x2e, "%H:%M:%S"));
				insert(value_type(0x2f, "%M:%S"));
				insert(value_type(0xa4, "%m.%d.%Y %l:%M:%S %p"));
			}
	};

	bool oleEof(BiffReader& reader)
	{
		return reader.tell() >= reader.size();
	}

	std::string getStandardDateFormat(int xf_index)
	{
		static StandardDateFormats formats;
		if (xf_index < 0 || (size_t)xf_index >= m_xf_records.size())
		{
			*m_log_stream << "Incorrect format code " << xf_index << ".\n";
			return "";
		}
		int num_format_id = m_xf_records[xf_index].num_format_id;
		if (m_defined_num_format_ids.count(num_format_id))
			return "";
		StandardDateFormats::iterator i = formats.find(num_format_id);
		if (i == formats.end())
			return "";
		else
			return i->second;
	}	

	std::string xlsDateToString(double xls_date, std::string date_fmt)
	{
		if (!std::isfinite(xls_date))
			return "";
		double seconds = std::rint((xls_date - m_date_shift) * 86400.0);
		double min_sec = (sizeof(time_t) >= 8) ? -62135596800.0 : -2147483648.0;
		double max_sec = (sizeof(time_t) >= 8) ? 253402300799.0 : 2147483647.0;
		if (!std::isfinite(seconds) || seconds < min_sec || seconds > max_sec)
			return "";
		time_t time = (time_t)seconds;
		struct tm tm_buf;
#ifdef _WIN32
		if (gmtime_s(&tm_buf, &time) != 0)
			return "";
#else
		if (!gmtime_r(&time, &tm_buf))
			return "";
#endif
		char buffer[128];
		if (strftime(buffer, sizeof(buffer) - 1, date_fmt.c_str(), &tm_buf) == 0)
			return "";
		return buffer;
	}

	std::string formatXLSNumber(double number, short int xf_index)
	{
		std::string date_fmt = getStandardDateFormat(xf_index);
		if (date_fmt != "")
			return xlsDateToString(number, date_fmt);
		else
		{
			char buffer[128];
			sprintf(buffer,"%.12g",number);
			return buffer;
		}
	}

	std::string parseXNum(std::vector<unsigned char>::const_iterator src,int xf_index)
	{
		union
		{
			unsigned char xls_num[8];
			double num;
		} xnum_conv;
		#ifdef WORDS_BIGENDIAN
			std::reverse_copy(src, src + 8, xnum_conv.xls_num)
		#else
			std::copy(src, src + 8, xnum_conv.xls_num);
		#endif
		return formatXLSNumber(xnum_conv.num, xf_index);
	}

	std::string parseRkRec(std::vector<unsigned char>::const_iterator src, short int xf_index)
	{
		double number;
		if ((*src) & 0x02)
			number = (double)(getS32LittleEndian(src) >> 2);
		else
		{
			union
			{
				unsigned char xls_num[8];
				double num;
			} rk_num_conv;
			std::fill(rk_num_conv.xls_num, rk_num_conv.xls_num + 8, '\0');
		#ifdef WORDS_BIGENDIAN
			std:reverse_copy(src, src + 4, dconv.xls_num);
			rk_num_conv.xls_num[0] &= 0xfc;
		#else
			std::copy(src, src + 4, rk_num_conv.xls_num + 4);
			rk_num_conv.xls_num[3] &= 0xfc;
		#endif
			number = rk_num_conv.num;
		}
		if ((*src) & 0x01)
			number *= 0.01;
		return formatXLSNumber(number, xf_index);
	}

	std::string parseXLUnicodeString(std::vector<unsigned char>::const_iterator* src, std::vector<unsigned char>::const_iterator src_end, const std::vector<size_t>& record_sizes, size_t& record_index, size_t& record_pos)
	{
		if (record_index < record_sizes.size() && record_pos > record_sizes[record_index])
			*m_log_stream << "Warning: XLUnicodeString starts " << (record_pos - record_sizes[record_index]) << " bytes past record boundary.\n";
		while (record_index < record_sizes.size() && record_pos >= record_sizes[record_index])
		{
			record_pos -= record_sizes[record_index];
			record_index++;
		}
		if (record_index >= record_sizes.size())
		{
			*m_log_stream << "Unexpected end of buffer.\n";
			*src = src_end;
			return "";
		}
		/*
			This part is mostly based on OpenOffice/LibreOffice XLS format documentation and filter source code.
			record id   BIFF    ->  XF type     String type
			0x0004      2-7     ->  3 byte      8-bit length, byte string
			0x0004      8       ->  3 byte      16-bit length, unicode string
			0x0204      2-7     ->  2 byte      16-bit length, byte string
			0x0204      8       ->  2 byte      16-bit length, unicode string
		*/
		#warning TODO: Add support for record 0x0004 (in BIFF2).
		if (src_end - *src < 2)
		{
			*m_log_stream << "Unexpected end of buffer.\n";
			*src = src_end;
			return "";
		}
		int count = getU16LittleEndian(*src);
		*src += 2;
		record_pos += 2;
		int flags = 0;
		if (m_biff_version >= BIFF8)
		{
			if (src_end - *src < 1)
			{
				*m_log_stream << "Unexpected end of buffer.\n";
				*src = src_end;
				return "";
			}
			flags = **src;
			*src += 1;
			record_pos += 1;
		}
		int char_size = (flags & 0x01) ? 2 : 1;
		size_t after_text_block_len = 0;
		if (flags & 0x08) // rich text
		{
			if (m_verbose_logging)
				*m_log_stream << "Rich text flag enabled.\n";
			if (src_end - *src < 2)
			{
				*m_log_stream << "Unexpected end of buffer.\n";
				*src = src_end;
				return "";
			}
			after_text_block_len += 4 * (size_t)getU16LittleEndian(*src);
			*src += 2;
			record_pos += 2;
		}
		if (flags & 0x04) // asian
		{
			if (m_verbose_logging)
				*m_log_stream << "Asian flag enabled.\n";
			if (src_end - *src < 4)
			{
				*m_log_stream << "Unexpected end of buffer.\n";
				*src = src_end;
				return "";
			}
			S32 ext_len = getS32LittleEndian(*src);
			if (ext_len < 0)
			{
				*m_log_stream << "Unexpected end of buffer.\n";
				*src = src_end;
				return "";
			}
			after_text_block_len += (size_t)ext_len;
			*src += 4;
			record_pos += 4;
		}
		if (m_verbose_logging && after_text_block_len > 0)
			*m_log_stream << "Additional formatting blocks found, size " << after_text_block_len << " bytes.\n";
		std::string dest;
		std::vector<unsigned char>::const_iterator s = *src;
		int char_count = 0;
		for (int i = 0; i < count; )
		{
			if (s >= src_end || record_index >= record_sizes.size())
			{
				*m_log_stream << "Unexpected end of buffer.\n";
				*src = src_end;
				return dest;
			}
			if (record_pos > record_sizes[record_index])
				*m_log_stream << "Warning: record boundary crossed.\n";
			if (record_pos >= record_sizes[record_index])
			{
				if (m_verbose_logging)
					*m_log_stream << "Record boundary reached.\n";
				record_index++;
				record_pos = 0;
				while (record_index < record_sizes.size() && record_sizes[record_index] == 0)
					record_index++;
				// At the beginning of each CONTINUE record the option flags byte is repeated.
				// Only the character size flag will be set in this flags byte, the Rich-Text flag and the Far-East flag are set to zero.
				// In each CONTINUE record it is possible that the character size changes
				// from 8‑bit characters to 16‑bit characters and vice versa.
				if (record_index >= record_sizes.size() || s >= src_end)
				{
					*m_log_stream << "Unexpected end of buffer.\n";
					*src = src_end;
					return dest;
				}
				if ((*s) != 0 && (*s) != 1)
					*m_log_stream << "Incorrect XLUnicodeString flag.\n";
				char_size = ((*s) & 0x01) ? 2 : 1;
				s++;
				record_pos++;
				continue;
			}
			if (char_size == 2)
			{
				if (src_end - s < 2)
				{
					*m_log_stream << "Unexpected end of buffer.\n";
					*src = src_end;
					return dest;
				}
				unsigned int uc = getU16LittleEndian(s);
				#warning TODO: Find explanation (documentation) of NULL characters (OO skips them).
				if (uc != 0)
				{
					if (utf16_unichar_has_4_bytes(uc))
					{
						record_pos += 2;
						s += 2;
						if (src_end - s < 2)
						{
							*m_log_stream << "Unexpected end of buffer.\n";
							*src = src_end;
							return dest;
						}
						uc = (uc << 16) | getU16LittleEndian(s);
					}
					dest += unichar_to_utf8(uc);
					char_count++;
				}
			}
			else
			{
				if (s >= src_end)
				{
					*m_log_stream << "Unexpected end of buffer.\n";
					*src = src_end;
					return dest;
				}
				std::string c2(1, *s);
				if (m_codepage != "ASCII")
				{
					TextConverter tc(m_codepage);
					dest += ustring_to_string(tc.convert(c2));
				}
				else
					dest += c2;
				char_count++;
			}
			i++;
			s += char_size;
			record_pos += char_size;
		}
		size_t remaining = (s <= src_end) ? (size_t)(src_end - s) : 0;
		size_t skip = (after_text_block_len <= remaining) ? after_text_block_len : remaining;
		*src = s + skip;
		record_pos += skip;
		return dest;
	}

	void parseSharedStringTable(const std::vector<unsigned char>& sst_buf)
	{
		if (m_verbose_logging)
			*m_log_stream << "Parsing shared string table.\n";
		if (sst_buf.size() < 8)
			return;
		S32 sst_size_raw = getS32LittleEndian(sst_buf.begin() + 4);
		if (sst_size_raw <= 0)
			return;
		size_t sst_size = (size_t)sst_size_raw;
		std::vector<unsigned char>::const_iterator src = sst_buf.begin() + 8;
		size_t record_index = 0;
		size_t record_pos = 8;
		while (src < sst_buf.end() && m_shared_string_table.size() < sst_size)
			m_shared_string_table.push_back(parseXLUnicodeString(&src, sst_buf.end(), m_shared_string_table_record_sizes, record_index, record_pos));
	}	

	std::string cellText(int row, int col, const std::string& s)
	{
		std::string r;
		while (row > m_last_row)
		{
			r += "\n";
			++m_last_row;
			m_last_col = 0;
		}
		if (col > 0 && col <= m_last_col)
			r += "\t";
		while (col > m_last_col)
		{
			r += "\t";
			++m_last_col;
		}
		r += s;
		return r;
	}

	bool processRecord(int rec_type, const std::vector<unsigned char>& rec, std::string& text)
	{
		if (m_verbose_logging)
			*m_log_stream << std::hex << "record=0x" << rec_type << std::endl;
		if (rec_type != XLS_CONTINUE && m_prev_rec_type == XLS_SST)
			parseSharedStringTable(m_shared_string_table_buf);
		switch (rec_type)
		{
			case XLS_BLANK:
			{
				if (rec.size() < 4)
					break;
				int row = getU16LittleEndian(rec.begin());
				int col = getU16LittleEndian(rec.begin() + 2);
				text += cellText(row, col, "");
				break;
			}
			case 0x0009:
			case 0x0209:
			case 0x0409:
			case XLS_BOF:
			{
				m_last_row = 0;
				m_last_col = 0;
				#warning TODO: Check for stream type, ignore charts, or make it configurable
				#warning TODO: Mark beginning of sheet (configurable)
				break;
			}
			case XLS_CODEPAGE:
			{
				if (rec.size() == 2)
				{
					int codepage = getU16LittleEndian(rec.begin());
					if (codepage == 1200)
						break;
					else if (codepage == 367)
						m_codepage = "ASCII";
					else
						m_codepage = "cp" + int2string(codepage);
				}
				break;
			}
			case XLS_CONTINUE:
			{
				if (m_prev_rec_type != XLS_SST)
					return true; // do not change m_prev_rec_type
				m_shared_string_table_buf.reserve(m_shared_string_table_buf.size() + rec.size());
				m_shared_string_table_buf.insert(m_shared_string_table_buf.end(), rec.begin(), rec.begin() + rec.size());
				m_shared_string_table_record_sizes.push_back(rec.size());
				if (m_verbose_logging)
					*m_log_stream << "XLS_CONTINUE record for XLS_SST found. Index: " << m_shared_string_table_record_sizes.size() - 1 << ", size:" << rec.size() << ".\n";
				return true;
			}
			case XLS_DATE_1904:
				m_date_shift = 24107.0; 
				break;
			case XLS_EOF:
			{
				text += "\n";
				#warning TODO: Mark end of sheet (configurable)
				break;
			}
			case XLS_FILEPASS:
			{
				*m_log_stream << "XLS file is encrypted.\n";
				if (rec.size() >= 2)
				{
					U16 encryption_type = getU16LittleEndian(rec.begin());
					if (encryption_type == 0x0000)
						*m_log_stream << "XOR obfuscation encryption type detected.\n";
					else if (encryption_type == 0x0001)
					{
						*m_log_stream << "RC4 encryption type detected.\n";
						if (rec.size() >= 4)
						{
							U16 header_type = getU16LittleEndian(rec.begin() + 2);
							if (header_type == 0x0001)
								*m_log_stream << "RC4 encryption header found.\n";
							else if (header_type == 0x0002 || header_type == 0x0003)
								*m_log_stream << "RC4 CryptoAPI encryption header found.\n";
							else
								*m_log_stream << "Unknown RC4 encryption header.\n";
						}
					}
					else
						*m_log_stream << "Unknown encryption type.\n";
				}
				m_encrypted = true;
				m_error = true;
				return false;
			}
			case XLS_FORMAT:
			{
				if (rec.size() < 2)
					break;
				int num_format_id = getU16LittleEndian(rec.begin());
				m_defined_num_format_ids.insert(num_format_id);
				break;
			}
			case XLS_FORMULA:
			case XLS_FORMULA_BIFF4:
			{
				m_last_string_formula_row = -1;
				if (rec.size() < 14)
					break;
				int row = getU16LittleEndian(rec.begin());
				int col = getU16LittleEndian(rec.begin()+2);
				if (((unsigned char)rec[12] == 0xFF) && (unsigned char)rec[13] == 0xFF)
				{
					if (rec[6] == 0)
					{
						m_last_string_formula_row = row;
						m_last_string_formula_col = col;
					}
					else if (rec[6] == 1)
					{
						#warning TODO: check and test boolean formulas
						text += (rec[8] ? "TRUE" : "FALSE");
					}
					else if (rec[6] == 2)
						text += "ERROR";
				}
				else
				{
					int xf_index=getU16LittleEndian(rec.begin()+4);
					text += cellText(row, col, parseXNum(rec.begin() + 6,xf_index));
				}
				break;
			}
			case XLS_INTEGER_CELL:
			{
				if (rec.size() < 9)
					break;
				int row = getU16LittleEndian(rec.begin());
				int col = getU16LittleEndian(rec.begin()+2);
				text += cellText(row, col, int2string(getU16LittleEndian(rec.begin() + 7)));
				break;
			}
			case XLS_LABEL_BIFF2:
			{
				m_last_string_formula_row = -1;
				if (rec.size() < 8)
					break;
				int row = getU16LittleEndian(rec.begin());
				int col = getU16LittleEndian(rec.begin() + 2);
				size_t slen = rec[7];
				if (8 + slen > rec.size())
					slen = rec.size() - 8;
				std::string raw_s;
				if (slen > 0)
				{
					raw_s.assign((const char*)(&rec[8]), slen);
					if (m_codepage != "ASCII")
					{
						TextConverter tc(m_codepage);
						raw_s = ustring_to_string(tc.convert(raw_s));
					}
				}
				text += cellText(row, col, raw_s);
				break;
			}
			case XLS_RSTRING:
			case XLS_LABEL:
			{
				m_last_string_formula_row = -1;
				if (rec.size() < 8)
					break;
				int row = getU16LittleEndian(rec.begin()); 
				int col = getU16LittleEndian(rec.begin() + 2);
				std::vector<unsigned char>::const_iterator src=rec.begin() + 6;
				std::vector<size_t> sizes;
				sizes.push_back(rec.size() - 6);
				size_t record_index = 0;
				size_t record_pos = 0;
				text += cellText(row, col, parseXLUnicodeString(&src, rec.end(), sizes, record_index, record_pos));
				break;
			}
			case XLS_LABEL_SST:
			{
				m_last_string_formula_row = -1;
				if (rec.size() < 8)
					break;
				int row = getU16LittleEndian(rec.begin()); 
				int col = getU16LittleEndian(rec.begin() + 2);
				size_t sst_index = getU16LittleEndian(rec.begin() + 6);
				if (sst_index >= m_shared_string_table.size())
				{
					*m_log_stream << "Incorrect SST index.\n";
					return false;
				} else
					text += cellText(row, col, m_shared_string_table[sst_index]);
				break;
			}
			case XLS_MULBLANK:
			{
				if (rec.size() < 6)
					break;
				int row = getU16LittleEndian(rec.begin());
				int start_col = getU16LittleEndian(rec.begin() + 2);
				int end_col = getU16LittleEndian(rec.begin() + rec.size() - 2);
				if (end_col < start_col || (size_t)(end_col - start_col + 1) * 2 > rec.size() - 6)
					break;
				for (int c = start_col; c <= end_col; c++)
					text += cellText(row, c, "");
				break;
			}
			case XLS_MULRK:
			{
				m_last_string_formula_row = -1;
				if (rec.size() < 6)
					break;
				int row = getU16LittleEndian(rec.begin());
				int start_col = getU16LittleEndian(rec.begin() + 2);
				int end_col = getU16LittleEndian(rec.begin() + rec.size() - 2);
				if (end_col < start_col)
					break;
				for (size_t offset = 4, col = (size_t)start_col; (int)col <= end_col && offset + 6 <= rec.size() - 2; offset += 6, col++)
				{
					int xf_index = getU16LittleEndian(rec.begin() + offset);
					text += cellText(row, (int)col, parseRkRec(rec.begin() + offset + 2, xf_index));
				}
				break;
			}
			case XLS_NUMBER:
			case 0x03:
			case 0x103:
			case 0x303:
			{
				m_last_string_formula_row = -1;
				if (rec.size() < 14)
					break;
				int row = getU16LittleEndian(rec.begin());
				int col = getU16LittleEndian(rec.begin() + 2);
				text += cellText(row, col, parseXNum(rec.begin() + 6, getU16LittleEndian(rec.begin() + 4)));
				break;
			}
			case XLS_RK:
			{
				m_last_string_formula_row = -1;
				if (rec.size() < 10)
					break;
				int row = getU16LittleEndian(rec.begin());
				int col = getU16LittleEndian(rec.begin() + 2);
				int xf_index = getU16LittleEndian(rec.begin() + 4);
				text += cellText(row, col, parseRkRec(rec.begin() + 6, xf_index));
				break;
			}
			case XLS_SST:
			{
				m_shared_string_table_buf.clear();
				m_shared_string_table_record_sizes.clear();
				m_shared_string_table.clear();
				m_shared_string_table_buf.reserve(rec.size());
				m_shared_string_table_buf.insert(m_shared_string_table_buf.end(), rec.begin(), rec.begin() + rec.size());
				m_shared_string_table_record_sizes.push_back(rec.size());
				break;
			}
			case XLS_STRING:
			{
				std::vector<unsigned char>::const_iterator src = rec.begin();
				if (m_last_string_formula_row < 0) {
					*m_log_stream << "String record without preceeding string formula.\n";
					break;
				}
				std::vector<size_t> sizes;
				sizes.push_back(rec.size());
				size_t record_index = 0;
				size_t record_pos = 0;
				text += cellText(m_last_string_formula_row, m_last_string_formula_col, parseXLUnicodeString(&src, rec.end(), sizes, record_index, record_pos));
				break;
			}
			case XLS_XF:
			case 0x43:
			{
				if (rec.size() >= 4)
				{
					XFRecord xf_record;
					xf_record.num_format_id = getU16LittleEndian(rec.begin() + 2);
					m_xf_records.push_back(xf_record);
				}
				break;
			}
			case XLS_XF_BIFF4:
			{
				if (rec.size() >= 2)
				{
					XFRecord xf_record;
					xf_record.num_format_id = rec[1];
					m_xf_records.push_back(xf_record);
				}
				break;
			} 
		}
		m_prev_rec_type = rec_type;
		return true;
	}  

	void extract_xls_drawing_images(const std::vector<uint8_t>& dgg_data, std::string& text)
	{
		if (!m_image_callback || dgg_data.size() < 8)
			return;

		int img_count = 0;
		for (size_t i = 0; i + 8 <= dgg_data.size(); )
		{
			uint16_t ver_inst = (uint16_t)dgg_data[i] | ((uint16_t)dgg_data[i+1] << 8);
			uint16_t rec_type = (uint16_t)dgg_data[i+2] | ((uint16_t)dgg_data[i+3] << 8);
			uint32_t rec_len = (uint32_t)dgg_data[i+4] | ((uint32_t)dgg_data[i+5] << 8) | ((uint32_t)dgg_data[i+6] << 16) | ((uint32_t)dgg_data[i+7] << 24);
			uint16_t ver = ver_inst & 0x0F;
			if (ver == 0x0F) // container
			{
				i += 8;
				continue;
			}
			if ((size_t)rec_len > dgg_data.size() - (i + 8))
				break;

			if (rec_type == 0xF007) // msofbtBSE
			{
				size_t fbse_end = i + 8 + (size_t)rec_len;
				if (rec_len >= 36)
				{
					size_t cb_name = dgg_data[i + 8 + 33];
					if (cb_name <= (size_t)rec_len - 36)
					{
						size_t blip_offset = i + 8 + 36 + cb_name;
						if (blip_offset + 8 <= fbse_end)
						{
							uint16_t b_rec_type = (uint16_t)dgg_data[blip_offset+2] | ((uint16_t)dgg_data[blip_offset+3] << 8);
							uint32_t b_rec_len = (uint32_t)dgg_data[blip_offset+4] | ((uint32_t)dgg_data[blip_offset+5] << 8) |
							                     ((uint32_t)dgg_data[blip_offset+6] << 16) | ((uint32_t)dgg_data[blip_offset+7] << 24);

							// Only extract raster bitmap BLIPs (PNG/JPEG/DIB/TIFF: 0xF01D..0xF02A); skip metafiles (EMF/WMF/PICT).
							if (b_rec_type >= 0xF01D && b_rec_type <= 0xF02A &&
							    (size_t)b_rec_len <= fbse_end - (blip_offset + 8))
							{
								size_t header_skip = 17;
								if (b_rec_len >= 33 + 4)
								{
									if (memcmp(&dgg_data[blip_offset + 8 + 33], "\x89PNG", 4) == 0 ||
									    (dgg_data[blip_offset + 8 + 33] == 0xFF && dgg_data[blip_offset + 8 + 34] == 0xD8) ||
									    memcmp(&dgg_data[blip_offset + 8 + 33], "BM", 2) == 0)
									{
										header_skip = 33;
									}
								}

								if (b_rec_len > header_skip)
								{
									size_t img_size = (size_t)b_rec_len - header_skip;
									const uint8_t* img_bytes = &dgg_data[blip_offset + 8 + header_skip];

									std::string ext = "png";
									if (img_size >= 4 && memcmp(img_bytes, "\x89PNG", 4) == 0)
										ext = "png";
									else if (img_size >= 3 && img_bytes[0] == 0xFF && img_bytes[1] == 0xD8)
										ext = "jpg";
									else if (img_size >= 2 && memcmp(img_bytes, "BM", 2) == 0)
										ext = "bmp";
									else if (b_rec_type == 0xF01D || b_rec_type == 0xF01F || b_rec_type == 0xF029)
										ext = "jpg";

									img_count++;
									std::string img_name = "xl/media/image" + std::to_string(img_count) + "." + ext;
									std::string parsed = m_image_callback(img_name, reinterpret_cast<const char*>(img_bytes), img_size, m_image_user_data);
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
						}
					}
				}
				i = fbse_end;
			}
			else
			{
				i += 8 + (size_t)rec_len;
			}
		}
	}

	void initEmbedContext(const FormattingStyle& formatting, doctotext::EmbeddedExtractionContext& root_ctx) const
	{
		root_ctx.formatting = formatting;
		root_ctx.verbose_logging = m_verbose_logging;
		root_ctx.log_stream = m_log_stream;
		root_ctx.image_callback = m_image_callback;
		root_ctx.image_user_data = m_image_user_data;
	}

	void parseXLS(BiffReader& reader, std::string& text, std::vector<uint8_t>* out_dgg_buf = NULL)
	{
		m_xf_records.clear();
		m_date_shift = 25569.0;
		m_shared_string_table.clear();
		m_shared_string_table_buf.clear();
		m_shared_string_table_record_sizes.clear();
		m_prev_rec_type = 0;
		m_last_string_formula_row = -1;
		m_last_string_formula_col = -1;
		m_defined_num_format_ids.clear();
		m_last_row = 0;
		m_last_col = 0;

		std::vector<unsigned char> rec;
		bool read_status = true;
		while (read_status)
		{
			if (oleEof(reader))
			{
				*m_log_stream << "No BOF record found.\n";
				return;
			}
			U16 rec_type, rec_len;
			if (!reader.readU16(rec_type) || !reader.readU16(rec_len))
			{
				*m_log_stream << reader.getLastError() << "\n";
				return;
			}
			if (isBofRecordType(rec_type))
			{
				U16 payload_read = 0;
				bool valid_bof_len = false;
				if (rec_type == 0x0009 && rec_len == 4)
					valid_bof_len = true;
				else if ((rec_type == 0x0209 || rec_type == 0x0409) && rec_len == 6)
					valid_bof_len = true;
				else if (rec_type == XLS_BOF && (rec_len == 8 || rec_len == 16))
					valid_bof_len = true;

				if (valid_bof_len)
				{
					switch (rec_type)
					{
						case XLS_BOF:
						{
							U16 biff_ver, data_type;
							if (!reader.readU16(biff_ver) || !reader.readU16(data_type))
							{
								*m_log_stream << reader.getLastError() << "\n";
								return;
							}
							payload_read = 4;
							//On microsoft site there is documentation only for "BIFF8". Documentation from OpenOffice is better:
							/*
								BIFF5:
								Offset	Size	Contents
								0		2		BIFF version (always 0500H for BIFF5). ...
								2		2		Type of the following data:	0005H = Workbook globals
																			0006H = Visual Basic module
																			0010H = Sheet or dialogue
																			0020H = Chart
																			0040H = Macro sheet
																			0100H = Workspace (BIFF5W only)
								4		2		Build identifier, must not be 0
								6		2		Build year
							*/
							/*
								BIFF8:
								Offset	Size	Contents
								0		2		BIFF version (always 0600H for BIFF8)
								2		2		Type of the following data: 0005H = Workbook globals
																			0006H = Visual Basic module
																			0010H = Sheet or dialogue
																			0020H = Chart
																			0040H = Macro sheet
																			0100H = Workspace (BIFF8W only)
								4		2		Build identifier, must not be 0
								6		2		Build year, must not be 0
								8		4		File history flags
								12		4		Lowest Excel version that can read all records in this file
							*/
							if (biff_ver == 0x600 && rec_len == 16)
							{
								if (m_verbose_logging)
									*m_log_stream << "Detected BIFF8 version\n";
								m_biff_version = BIFF8;
							}
							else
							{
								if (m_verbose_logging)
									*m_log_stream << "Detected BIFF5 version\n";
								m_biff_version = BIFF5;
							}
							break;
						}
						case 0x0209:
							if (m_verbose_logging)
								*m_log_stream << "Detected BIFF3 version\n";
							m_biff_version = BIFF3;
							break;
						case 0x0409:
							if (m_verbose_logging)
								*m_log_stream << "Detected BIFF4 version\n";
							m_biff_version = BIFF4;
							break;
						default:
							if (m_verbose_logging)
								*m_log_stream << "Detected BIFF2 version\n";
							m_biff_version = BIFF2;
							break;
					}
					U16 rem = (rec_len >= payload_read) ? (rec_len - payload_read) : 0;
					if (rem > 0)
					{
						rec.resize(rem);
						read_status = reader.read(&*rec.begin(), rem);
					}
					break;
				}
				else
				{
					*m_log_stream << "Invalid BOF record.\n";
					return;
				}
			}
			else
			{
				rec.resize(126);
				read_status = reader.read(&*rec.begin(), 126);
			}
		}
		if (oleEof(reader))
		{
			*m_log_stream << "No BOF record found.\n";
			return;
		}
		bool eof_rec_found = false;
		std::vector<uint8_t> dgg_buf;
		bool in_dgg = false;
		while (read_status)
		{
			if (oleEof(reader))
			{
				processRecord(XLS_EOF, std::vector<unsigned char>(), text);
				break;
			}
			U16 rec_type;
			if (!reader.readU16(rec_type))
			{
				*m_log_stream << reader.getLastError() << "\n";
				break;
			}
			if (oleEof(reader))
			{
				processRecord(XLS_EOF, std::vector<unsigned char>(), text);
				break;
			}
			U16 rec_len;
			if (!reader.readU16(rec_len))
			{
				*m_log_stream << reader.getLastError() << "\n";
				break;
			}
			if (rec_len > 0)
			{
				rec.resize(rec_len);
				read_status = reader.read(&*rec.begin(), rec_len);
				if (!read_status)
				{
					*m_log_stream << reader.getLastError() << "\n";
					break;
				}
			}
			else
				rec.clear();

			if (rec_type == 0x00EB) // MSODRAWINGGROUP
			{
				in_dgg = true;
				if (rec.size() <= doctotext::kMaxEmbedBytes - dgg_buf.size())
					dgg_buf.insert(dgg_buf.end(), rec.begin(), rec.end());
				else
					in_dgg = false;
			}
			else if (in_dgg && rec_type == 0x003C) // CONTINUE
			{
				if (rec.size() <= doctotext::kMaxEmbedBytes - dgg_buf.size())
					dgg_buf.insert(dgg_buf.end(), rec.begin(), rec.end());
				else
					in_dgg = false;
			}
			else if (in_dgg)
			{
				in_dgg = false;
			}

			if (eof_rec_found)
			{
				if (!isBofRecordType(rec_type))
					break;
			}
			if (!processRecord(rec_type, rec, text))
				break;
			if (rec_type == XLS_EOF)
				eof_rec_found = true;
			else
				eof_rec_found = false;	
		}
		if (!dgg_buf.empty() && m_image_callback)
		{
			extract_xls_drawing_images(dgg_buf, text);
		}
		if (out_dgg_buf)
		{
			*out_dgg_buf = std::move(dgg_buf);
		}
	}
};

XLSParser::XLSParser(const std::string& file_name)
{
	impl = new Implementation();
	impl->m_codepage = "cp1251";
	impl->m_error = false;
	impl->m_encrypted = false;
	impl->m_is_buffer = false;
	impl->m_file_name = file_name;
	impl->m_verbose_logging = false;
	impl->m_log_stream = &std::cerr;
	impl->m_buffer = NULL;
	impl->m_buffer_size = 0;
	impl->m_image_callback = NULL;
	impl->m_image_user_data = NULL;
}

XLSParser::XLSParser(const char *buffer, size_t size)
{
	impl = new Implementation();
	impl->m_codepage = "cp1251";
	impl->m_error = false;
	impl->m_encrypted = false;
	impl->m_is_buffer = true;
	impl->m_file_name = "Memory buffer";
	impl->m_verbose_logging = false;
	impl->m_log_stream = &std::cerr;
	impl->m_buffer = buffer;
	impl->m_buffer_size = size;
	impl->m_image_callback = NULL;
	impl->m_image_user_data = NULL;
}

XLSParser::~XLSParser()
{
	delete impl;
}

void XLSParser::setImageParserCallback(doctotext::ImageParserCallback callback, void* user_data)
{
	impl->m_image_callback = callback;
	impl->m_image_user_data = user_data;
}

void XLSParser::setVerboseLogging(bool verbose)
{
	impl->m_verbose_logging = verbose;
}

void XLSParser::setLogStream(std::ostream& log_stream)
{
	impl->m_log_stream = &log_stream;
}

bool XLSParser::isXLS()
{
	impl->m_error = false;
	impl->m_encrypted = false;
	if (impl->m_is_buffer && (!impl->m_buffer || impl->m_buffer_size == 0))
		return false;
	std::unique_ptr<ThreadSafeOLEStorage> storage(
		impl->m_is_buffer ? new ThreadSafeOLEStorage(impl->m_buffer, impl->m_buffer_size)
		                  : new ThreadSafeOLEStorage(impl->m_file_name));
	if (storage->isValid())
	{
		std::unique_ptr<ThreadSafeOLEStreamReader> reader(openWorkbookStream(*storage));
		return reader != NULL;
	}

	std::unique_ptr<DataStream> raw_stream(
		impl->m_is_buffer ? static_cast<DataStream*>(new BufferStream(impl->m_buffer, impl->m_buffer_size))
		                  : static_cast<DataStream*>(new FileStream(impl->m_file_name)));
	if (!raw_stream->open())
		return false;
	bool is_raw_biff = hasRawBiffBofHeader(*raw_stream);
	raw_stream->close();
	return is_raw_biff;
}

void XLSParser::getLinks(std::vector<Link>& links)
{
	#warning TODO: Implement this functionality.
}

std::string XLSParser::plainText(const FormattingStyle& formatting)
{
	impl->m_error = false;
	impl->m_encrypted = false;
	if (impl->m_is_buffer && (!impl->m_buffer || impl->m_buffer_size == 0))
	{
		*impl->m_log_stream << "Memory buffer is empty.\n";
		impl->m_error = true;
		return "";
	}
	std::unique_ptr<ThreadSafeOLEStorage> storage(
		impl->m_is_buffer ? new ThreadSafeOLEStorage(impl->m_buffer, impl->m_buffer_size)
		                  : new ThreadSafeOLEStorage(impl->m_file_name));
	if (!storage->isValid())
	{
		std::unique_ptr<DataStream> raw_stream(
			impl->m_is_buffer ? static_cast<DataStream*>(new BufferStream(impl->m_buffer, impl->m_buffer_size))
			                  : static_cast<DataStream*>(new FileStream(impl->m_file_name)));
		if (raw_stream->open() && hasRawBiffBofHeader(*raw_stream))
		{
			RawBiffReader biff_reader(*raw_stream);
			std::string text;
			std::vector<uint8_t> dgg_buf;
			impl->parseXLS(biff_reader, text, &dgg_buf);
			raw_stream->close();
			if (formatting.parse_embedded_files && !impl->m_error && !dgg_buf.empty())
			{
				doctotext::EmbeddedExtractionContext root_ctx;
				impl->initEmbedContext(formatting, root_ctx);
				doctotext::EmbeddedExtractionContext::ScopedContext tls_scope(root_ctx);
				doctotext::extractOfficeArtDrawingEmbeddings(dgg_buf, tls_scope.context(), text);
			}
			return text;
		}
		*impl->m_log_stream << "Error opening " << impl->m_file_name << " as OLE file.\n";
		impl->m_error = true;
		return "";
	}
	return plainText(*storage, formatting);
}

std::string XLSParser::plainText(ThreadSafeOLEStorage& storage, const FormattingStyle& formatting)
{
	impl->m_error = false;
	impl->m_encrypted = false;
	std::unique_ptr<ThreadSafeOLEStreamReader> reader(openWorkbookStream(storage));
	if (!reader)
	{
		*impl->m_log_stream << "Error opening " << impl->m_file_name << " as OLE file.\n";
		impl->m_error = true;
		return "";
	}
	std::string text;
	std::vector<uint8_t> dgg_buf;
	OleBiffReader biff_reader(*reader);
	impl->parseXLS(biff_reader, text, &dgg_buf);
	reader.reset();
	if (formatting.parse_embedded_files && !impl->m_error)
	{
		doctotext::EmbeddedExtractionContext root_ctx;
		impl->initEmbedContext(formatting, root_ctx);
		doctotext::EmbeddedExtractionContext::ScopedContext tls_scope(root_ctx);
		doctotext::EmbeddedExtractionContext& embed_ctx = tls_scope.context();
		if (!dgg_buf.empty())
			doctotext::extractOfficeArtDrawingEmbeddings(dgg_buf, embed_ctx, text);

		std::vector<std::string> entries;
		if (storage.getStreamsAndStoragesList(entries))
		{
			std::sort(entries.begin(), entries.end());
			for (size_t i = 0; i < entries.size(); ++i)
			{
				const std::string& name = entries[i];
				if (name == "ObjectPool")
				{
					if (storage.enterDirectory(name))
					{
						std::vector<std::string> pool_entries;
						if (storage.getStreamsAndStoragesList(pool_entries))
						{
							std::sort(pool_entries.begin(), pool_entries.end());
							for (size_t j = 0; j < pool_entries.size(); ++j)
							{
								if (storage.enterDirectory(pool_entries[j]))
								{
									doctotext::extractFromOLEStorage(storage, pool_entries[j], embed_ctx, text);
									storage.leaveDirectory();
								}
							}
						}
						storage.leaveDirectory();
					}
				}
				else if (name.size() >= 3 && name.compare(0, 3, "MBD") == 0)
				{
					if (storage.enterDirectory(name))
					{
						doctotext::extractFromOLEStorage(storage, name, embed_ctx, text);
						storage.leaveDirectory();
					}
				}
			}
		}
	}
	return text;
}

Metadata XLSParser::metaData()
{
	impl->m_error = false;
	impl->m_encrypted = false;
	Metadata meta;
	if (impl->m_is_buffer && (!impl->m_buffer || impl->m_buffer_size == 0))
		return meta;
	std::unique_ptr<ThreadSafeOLEStorage> storage(
		impl->m_is_buffer ? new ThreadSafeOLEStorage(impl->m_buffer, impl->m_buffer_size)
		                  : new ThreadSafeOLEStorage(impl->m_file_name));
	if (!storage->isValid())
		return meta;
	impl->m_error = !parse_oshared_summary_info(*storage, *impl->m_log_stream, meta);
	return meta;
}

bool XLSParser::error()
{
	return impl->m_error;
}

bool XLSParser::encrypted()
{
	return impl->m_encrypted;
}
