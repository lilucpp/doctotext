#include "embedded_object_extractor.h"

#include "doc_parser.h"
#include "doctotext_unzip.h"
#include "misc.h"
#include "ppt_parser.h"
#include "thread_safe_ole_storage.h"
#include "thread_safe_ole_stream_reader.h"
#include "xls_parser.h"
#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <zlib.h>

namespace doctotext
{
namespace
{
	EmbeddedExtractionContext*& tlsContextSlot()
	{
		static thread_local EmbeddedExtractionContext* s_active = NULL;
		return s_active;
	}

	uint16_t readU16LE(const unsigned char* p)
	{
		return static_cast<uint16_t>(p[0]) |
		       (static_cast<uint16_t>(p[1]) << 8);
	}

	uint32_t readU32LE(const unsigned char* p)
	{
		return (uint32_t)p[0] |
		       ((uint32_t)p[1] << 8) |
		       ((uint32_t)p[2] << 16) |
		       ((uint32_t)p[3] << 24);
	}

	std::string baseFileName(const std::string& path)
	{
		size_t pos = path.find_last_of("\\/");
		if (pos == std::string::npos)
			return path;
		return path.substr(pos + 1);
	}

	void appendExtractedBlock(std::string& dst, const std::string& block)
	{
		if (block.empty())
			return;
		bool has_non_ws = false;
		for (size_t i = 0; i < block.size(); ++i)
		{
			unsigned char c = (unsigned char)block[i];
			if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
			{
				has_non_ws = true;
				break;
			}
		}
		if (!has_non_ws)
			return;
		if (!dst.empty() && dst[dst.size() - 1] != '\n')
			dst += '\n';
		dst += block;
		if (dst[dst.size() - 1] != '\n')
			dst += '\n';
	}

	bool hasStream(const std::vector<std::string>& entries, const char* target)
	{
		for (size_t i = 0; i < entries.size(); ++i)
		{
			if (entries[i] == target)
				return true;
		}
		return false;
	}

	bool hasActualStreamIgnoreCase(ThreadSafeOLEStorage& storage,
	                               const std::vector<std::string>& entries,
	                               const char* target)
	{
		for (size_t i = 0; i < entries.size(); ++i)
		{
			if (asciiEqualsIgnoreCase(entries[i], target))
			{
				AbstractOLEStreamReader* r = storage.createStreamReader(entries[i]);
				if (r != NULL)
				{
					delete r;
					return true;
				}
			}
		}
		return false;
	}

	bool readCStr(const unsigned char* data, size_t size, size_t& pos, std::string& out_str)
	{
		if (pos >= size)
			return false;
		const void* nul = std::memchr(data + pos, '\0', size - pos);
		if (!nul)
			return false;
		size_t end = (const unsigned char*)nul - data;
		out_str.assign((const char*)(data + pos), end - pos);
		pos = end + 1;
		return true;
	}
}

EmbeddedExtractionContext* EmbeddedExtractionContext::current()
{
	return tlsContextSlot();
}

EmbeddedExtractionContext::ScopedContext::ScopedContext(EmbeddedExtractionContext& root_ctx)
	: m_prev(tlsContextSlot()), m_active(NULL), m_installed(false)
{
	if (m_prev != NULL)
	{
		m_active = m_prev;
	}
	else
	{
		tlsContextSlot() = &root_ctx;
		m_active = &root_ctx;
		m_installed = true;
	}
}

EmbeddedExtractionContext::ScopedContext::~ScopedContext()
{
	if (m_installed)
		tlsContextSlot() = m_prev;
}

bool isTextLikeBuffer(const unsigned char* data, size_t size)
{
	if (!data || size == 0)
		return false;
	if (size >= 2 &&
	    ((data[0] == 0xFF && data[1] == 0xFE) || (data[0] == 0xFE && data[1] == 0xFF)))
		return true;
	size_t sample_len = std::min(size, (size_t)4096);
	size_t control_count = 0;
	for (size_t i = 0; i < sample_len; ++i)
	{
		unsigned char c = data[i];
		if (c == 0)
			return false;
		if (c < 0x20 && c != '\t' && c != '\r' && c != '\n' && c != '\f')
			++control_count;
	}
	return control_count * 20 <= sample_len;
}

bool parseOle10Native(const unsigned char* data,
                      size_t size,
                      size_t max_bytes,
                      std::string& out_name,
                      std::vector<unsigned char>& out_payload)
{
	out_name.clear();
	out_payload.clear();
	// Minimum Ole10Native layout: 4B totalSize + 2B flags1 + 2 NUL strings + 4B flags2/unknown + 4B cmdLen + 4B dataSize
	if (!data || size < 20 || size > max_bytes)
		return false;

	uint32_t total_size = readU32LE(data);
	if (total_size < 12)
		return false;

	size_t pos = 6; // skip 4B total_size + 2B flags1
	std::string label;
	std::string file_path;
	if (!readCStr(data, size, pos, label))
		return false;
	if (!readCStr(data, size, pos, file_path))
		return false;

	// 2B flags2 + 2B unknown1 + 4B command_len
	if (pos + 8 > size)
		return false;
	pos += 4;
	uint32_t cmd_len = readU32LE(data + pos);
	pos += 4;
	if (cmd_len > size - pos)
		return false;
	pos += cmd_len;

	if (pos + 4 > size)
		return false;
	uint32_t data_size = readU32LE(data + pos);
	pos += 4;
	if (data_size == 0 || data_size > max_bytes || data_size > size - pos)
		return false;

	out_payload.assign(data + pos, data + pos + data_size);
	std::string clean_label = baseFileName(label);
	std::string clean_path = baseFileName(file_path);
	out_name = !clean_label.empty() ? clean_label : clean_path;
	return true;
}

bool readOLEStreamBytes(ThreadSafeOLEStorage& storage,
                        const std::string& stream_name,
                        size_t max_bytes,
                        std::vector<unsigned char>& out_bytes)
{
	out_bytes.clear();
	AbstractOLEStreamReader* reader = storage.createStreamReader(stream_name);
	if (!reader)
		return false;
	size_t sz = reader->size();
	if (sz == 0 || sz > max_bytes)
	{
		delete reader;
		return false;
	}
	try
	{
		out_bytes.resize(sz);
		bool ok = reader->read(out_bytes.data(), sz);
		delete reader;
		if (!ok)
		{
			out_bytes.clear();
			return false;
		}
		return true;
	}
	catch (...)
	{
		delete reader;
		out_bytes.clear();
		return false;
	}
}

bool extractFromEmbeddedBuffer(const unsigned char* data,
                               size_t size,
                               const std::string& hint_name,
                               EmbeddedExtractionContext& ctx,
                               std::string& out_text)
{
	if (!ctx.formatting.parse_embedded_files)
		return false;
	if (!data || size == 0)
		return false;
	if (size > ctx.max_bytes)
	{
		if (ctx.verbose_logging && ctx.log_stream)
			*ctx.log_stream << "Skipping embedded object (" << hint_name << "): exceeds max_bytes limit.\n";
		return false;
	}
	if (ctx.depth >= ctx.max_depth)
	{
		if (ctx.verbose_logging && ctx.log_stream)
			*ctx.log_stream << "Skipping embedded object (" << hint_name << "): max recursion depth reached.\n";
		return false;
	}
	if (ctx.extracted_count >= ctx.max_count)
	{
		if (ctx.verbose_logging && ctx.log_stream)
			*ctx.log_stream << "Skipping embedded object (" << hint_name << "): max embedded count reached.\n";
		return false;
	}

	EmbeddedExtractionContext::ScopedContext tls_scope(ctx);
	EmbeddedExtractionContext::ScopedDepth depth_guard(tls_scope.context());

	try
	{
		PlainTextExtractor nested_extractor;
		nested_extractor.setFormattingStyle(ctx.formatting);
		nested_extractor.setXmlParseMode(ctx.xml_parse_mode);
		nested_extractor.setManageXmlParser(ctx.manage_xml_parser);
		nested_extractor.setVerboseLogging(ctx.verbose_logging);
		if (ctx.log_stream)
			nested_extractor.setLogStream(*ctx.log_stream);
		if (ctx.image_callback)
			nested_extractor.setImageParserCallback(ctx.image_callback, ctx.image_user_data);

		PlainTextExtractor::ParserType ext_type = PlainTextExtractor::PARSER_AUTO;
		if (!hint_name.empty())
			ext_type = nested_extractor.parserTypeByFileExtension(hint_name);

		PlainTextExtractor::ParserType content_type = PlainTextExtractor::PARSER_AUTO;
		bool sniffed = nested_extractor.parserTypeByFileContent(
			reinterpret_cast<const char*>(data), size, content_type);

		// Skip archives (.zip / .7z / .rar) when not disguised as Office OOXML/ODF
		if (ext_type == PlainTextExtractor::PARSER_ARCHIVE ||
		    (sniffed && content_type == PlainTextExtractor::PARSER_ARCHIVE &&
		     ext_type != PlainTextExtractor::PARSER_ODF_OOXML &&
		     ext_type != PlainTextExtractor::PARSER_XLSB &&
		     ext_type != PlainTextExtractor::PARSER_IWORK))
		{
			if (ctx.verbose_logging && ctx.log_stream)
				*ctx.log_stream << "Skipping archive embedded object (" << hint_name << ").\n";
			return false;
		}

		// Handle standalone embedded image via callback
		if (ext_type == PlainTextExtractor::PARSER_IMAGE ||
		    (sniffed && content_type == PlainTextExtractor::PARSER_IMAGE))
		{
			if (!ctx.image_callback)
				return false;
			++ctx.extracted_count;
			std::string img_text = ctx.image_callback(
				hint_name.empty() ? "embedded_image" : hint_name,
				reinterpret_cast<const char*>(data),
				size,
				ctx.image_user_data);
			appendExtractedBlock(out_text, img_text);
			return !img_text.empty();
		}

		bool has_binary_doc_magic =
			(size >= 5 && std::memcmp(data, "%PDF-", 5) == 0) ||
			(size >= 8 && std::memcmp(data, "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8) == 0) ||
			(size >= 4 && std::memcmp(data, "PK\x03\x04", 4) == 0);
		bool can_try_ext = (ext_type != PlainTextExtractor::PARSER_AUTO &&
		                    ext_type != PlainTextExtractor::PARSER_TXT &&
		                    ext_type != PlainTextExtractor::PARSER_ARCHIVE &&
		                    ext_type != PlainTextExtractor::PARSER_IMAGE);
		bool can_try_sniff = (sniffed &&
		                      content_type != PlainTextExtractor::PARSER_ARCHIVE &&
		                      content_type != PlainTextExtractor::PARSER_IMAGE);
		bool can_try_txt = !has_binary_doc_magic && !can_try_sniff && isTextLikeBuffer(data, size);
		if (!can_try_ext && !can_try_sniff && !can_try_txt)
			return false;

		++ctx.extracted_count;

		std::string nested_text;
		bool extracted = false;
		if (can_try_ext)
		{
			extracted = nested_extractor.processFile(
				ext_type, false,
				reinterpret_cast<const char*>(data), size, nested_text);
		}
		if (!extracted && can_try_sniff)
		{
			extracted = nested_extractor.processFile(
				content_type, false,
				reinterpret_cast<const char*>(data), size, nested_text);
		}
		if (!extracted && can_try_txt)
		{
			extracted = nested_extractor.processFile(
				PlainTextExtractor::PARSER_TXT, false,
				reinterpret_cast<const char*>(data), size, nested_text);
		}
		if (extracted)
		{
			appendExtractedBlock(out_text, nested_text);
			return !nested_text.empty();
		}
	}
	catch (...)
	{
		if (ctx.verbose_logging && ctx.log_stream)
			*ctx.log_stream << "Error extracting embedded object (" << hint_name << ").\n";
	}
	return false;
}

namespace
{
	template <typename ParserT>
	bool extractFromOLEStorageWithParser(ThreadSafeOLEStorage& storage,
	                                     EmbeddedExtractionContext& ctx,
	                                     std::string& out_text)
	{
		++ctx.extracted_count;
		EmbeddedExtractionContext::ScopedContext tls_scope(ctx);
		EmbeddedExtractionContext::ScopedDepth depth_guard(tls_scope.context());
		try
		{
			ParserT parser("");
			parser.setVerboseLogging(ctx.verbose_logging);
			if (ctx.log_stream)
				parser.setLogStream(*ctx.log_stream);
			if (ctx.image_callback)
				parser.setImageParserCallback(ctx.image_callback, ctx.image_user_data);
			std::string parsed_text = parser.plainText(storage, ctx.formatting);
			if (!parser.error())
			{
				appendExtractedBlock(out_text, parsed_text);
				return !parsed_text.empty();
			}
		}
		catch (...)
		{
		}
		return false;
	}
}

bool extractFromOLEStorage(ThreadSafeOLEStorage& storage,
                           const std::string& hint_name,
                           EmbeddedExtractionContext& ctx,
                           std::string& out_text)
{
	if (!ctx.formatting.parse_embedded_files)
		return false;
	if (!storage.isValid())
		return false;
	if (ctx.depth >= ctx.max_depth || ctx.extracted_count >= ctx.max_count)
		return false;

	std::vector<std::string> entries;
	if (!storage.getStreamsAndStoragesList(entries))
		return false;

	// 1. Direct MS Excel / Word / PowerPoint sub-storage (e.g. inside DOC ObjectPool or XLS MBD)
	if (hasActualStreamIgnoreCase(storage, entries, "Workbook") ||
	    hasActualStreamIgnoreCase(storage, entries, "Book"))
		return extractFromOLEStorageWithParser<XLSParser>(storage, ctx, out_text);
	if (hasStream(entries, "WordDocument"))
		return extractFromOLEStorageWithParser<DOCParser>(storage, ctx, out_text);
	if (hasStream(entries, "PowerPoint Document") || hasStream(entries, "Text_Content"))
		return extractFromOLEStorageWithParser<PPTParser>(storage, ctx, out_text);

	// 2. \x01Ole10Native wrapped payload
	if (hasStream(entries, "\x01Ole10Native"))
	{
		std::vector<unsigned char> raw_stream;
		if (readOLEStreamBytes(storage, "\x01Ole10Native", ctx.max_bytes, raw_stream))
		{
			std::string native_name;
			std::vector<unsigned char> payload;
			if (parseOle10Native(raw_stream.data(), raw_stream.size(), ctx.max_bytes, native_name, payload))
			{
				return extractFromEmbeddedBuffer(
					payload.data(),
					payload.size(),
					!native_name.empty() ? native_name : hint_name,
					ctx,
					out_text);
			}
		}
	}

	// 3. CONTENTS stream (direct embedded PDF, RTF, etc.)
	if (hasStream(entries, "CONTENTS"))
	{
		std::vector<unsigned char> payload;
		if (readOLEStreamBytes(storage, "CONTENTS", ctx.max_bytes, payload))
		{
			return extractFromEmbeddedBuffer(payload.data(), payload.size(), hint_name, ctx, out_text);
		}
	}

	// 4. Package stream (OOXML .docx/.xlsx/.pptx wrapped inside OLE)
	if (hasStream(entries, "Package"))
	{
		std::vector<unsigned char> payload;
		if (readOLEStreamBytes(storage, "Package", ctx.max_bytes, payload))
		{
			return extractFromEmbeddedBuffer(payload.data(), payload.size(), hint_name, ctx, out_text);
		}
	}

	return false;
}

namespace
{
	bool startsWith(const std::string& s, const char* prefix)
	{
		size_t plen = std::strlen(prefix);
		return s.size() >= plen && s.compare(0, plen, prefix) == 0;
	}

	bool endsWithCaseInsensitive(const std::string& s, const char* suffix)
	{
		size_t slen = s.size();
		size_t flen = std::strlen(suffix);
		if (slen < flen)
			return false;
		for (size_t i = 0; i < flen; ++i)
		{
			char a = s[slen - flen + i];
			char b = suffix[i];
			if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
			if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
			if (a != b)
				return false;
		}
		return true;
	}

	bool isOdfEmbeddedBinaryCandidate(const std::string& path)
	{
		if (path.empty() || path[path.size() - 1] == '/')
			return false;
		if (startsWith(path, "ObjectReplacements/") ||
		    path.find("/ObjectReplacements/") != std::string::npos ||
		    startsWith(path, "Thumbnails/") ||
		    path.find("/Thumbnails/") != std::string::npos ||
		    startsWith(path, "Pictures/") ||
		    path.find("/Pictures/") != std::string::npos ||
		    startsWith(path, "META-INF/") ||
		    startsWith(path, "Configurations2/"))
			return false;
		if (path == "mimetype" ||
		    endsWithCaseInsensitive(path, ".xml") ||
		    endsWithCaseInsensitive(path, ".rdf") ||
		    endsWithCaseInsensitive(path, ".svg"))
			return false;
		// ODF binary OLE objects live in "Object ..." or "Objects/..." or non-XML binary files
		return startsWith(path, "Object") || path.find("/Object") != std::string::npos;
	}

	bool hasStructuredContainerOrDocMagic(const std::string& data)
	{
		if (data.size() >= 8 &&
		    std::memcmp(data.data(), "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8) == 0)
			return true;
		if (data.size() >= 5 && std::memcmp(data.data(), "%PDF-", 5) == 0)
			return true;
		if (data.size() >= 4 && std::memcmp(data.data(), "PK\x03\x04", 4) == 0)
			return true;
		if (data.size() >= 5 && std::memcmp(data.data(), "{\\rtf", 5) == 0)
			return true;
		return false;
	}
}

bool extractEmfEmbeddedObjects(const unsigned char* data,
                                 size_t size,
                                 const std::string& hint_name,
                                 EmbeddedExtractionContext& ctx,
                                 std::string& out_text)
{
	if (!ctx.formatting.parse_embedded_files || !data || size < 8)
		return false;
	if (ctx.depth >= ctx.max_depth || ctx.extracted_count >= ctx.max_count)
		return false;
	static const uint32_t kEmrEof = 0x0E;
	static const uint32_t kEmrComment = 0x46;
	static const uint32_t kEmrCommentMultiFormats = 0x40000004U;
	bool any = false;
	size_t off = 0;
	while (off + 8 <= size && ctx.extracted_count < ctx.max_count)
	{
		uint32_t rec_type = readU32LE(data + off);
		uint32_t rec_size = readU32LE(data + off + 4);
		if (rec_size < 8 || (rec_size & 3U) != 0 || rec_size > size - off)
			break;
		if (rec_type == kEmrEof)
			break;
		if (rec_type == kEmrComment && rec_size >= 40)
		{
			const unsigned char* rec = data + off;
			uint32_t data_size = readU32LE(rec + 8);
			if (data_size >= 28 && data_size <= rec_size - 12 &&
			    std::memcmp(rec + 12, "GDIC", 4) == 0 &&
			    readU32LE(rec + 16) == kEmrCommentMultiFormats)
			{
				uint32_t count_formats = readU32LE(rec + 36);
				uint32_t max_formats = (rec_size - 40) / 16U;
				if (count_formats > max_formats)
					count_formats = max_formats;
				for (uint32_t k = 0; k < count_formats && ctx.extracted_count < ctx.max_count; ++k)
				{
					size_t fmt_off = 40 + (size_t)k * 16;
					if (std::memcmp(rec + fmt_off, " FDP", 4) == 0 ||
					    std::memcmp(rec + fmt_off, "PDF ", 4) == 0)
					{
						uint32_t cb_data = readU32LE(rec + fmt_off + 8);
						uint32_t off_data = readU32LE(rec + fmt_off + 12);
						// [MS-EMF] 2.3.3.4: offData is relative to start of EmrCommentMultiFormats payload (rec + 12, where "GDIC" begins).
						size_t min_off_data = 28 + (size_t)count_formats * 16;
						if (off_data >= min_off_data && off_data <= rec_size - 12 &&
						    cb_data > 0 && cb_data <= ctx.max_bytes &&
						    cb_data <= (rec_size - 12 - off_data))
						{
							const unsigned char* pdf_ptr = rec + 12 + off_data;
							if (extractFromEmbeddedBuffer(pdf_ptr, cb_data, hint_name + ".pdf", ctx, out_text))
								any = true;
						}
					}
				}
			}
		}
		off += rec_size;
	}
	return any;
}

bool extractOfficeArtDrawingEmbeddings(const std::vector<unsigned char>& dgg_data,
                                       EmbeddedExtractionContext& ctx,
                                       std::string& out_text)
{
	if (!ctx.formatting.parse_embedded_files || dgg_data.size() < 8)
		return false;
	if (ctx.depth >= ctx.max_depth || ctx.extracted_count >= ctx.max_count)
		return false;

	struct ZlibStreamGuard
	{
		z_stream zs;
		bool active;
		ZlibStreamGuard() : active(false) { std::memset(&zs, 0, sizeof(zs)); }
		~ZlibStreamGuard() { if (active) inflateEnd(&zs); }
	};

	bool any = false;
	int emf_index = 0;
	size_t total_emf_bytes = 0;
	for (size_t i = 0;
	     i + 8 <= dgg_data.size() &&
	     ctx.extracted_count < ctx.max_count &&
	     emf_index < ctx.max_count &&
	     total_emf_bytes < ctx.max_bytes; )
	{
		uint16_t ver_inst = (uint16_t)dgg_data[i] | ((uint16_t)dgg_data[i + 1] << 8);
		uint16_t rec_type = (uint16_t)dgg_data[i + 2] | ((uint16_t)dgg_data[i + 3] << 8);
		uint32_t rec_len = readU32LE(&dgg_data[i + 4]);
		uint16_t ver = ver_inst & 0x0F;

		if (ver == 0x0F)
		{
			i += 8;
			continue;
		}
		if ((size_t)rec_len > dgg_data.size() - (i + 8))
			break;

		if (rec_type == 0xF007) // OfficeArtFBSE
		{
			size_t fbse_end = i + 8 + (size_t)rec_len;
			if (rec_len >= 36)
			{
				size_t cb_name = dgg_data[i + 8 + 33];
				if (cb_name <= (size_t)rec_len - 36)
				{
					size_t blip_off = i + 8 + 36 + cb_name;
					if (blip_off + 8 <= fbse_end)
					{
						uint16_t b_ver_inst = (uint16_t)dgg_data[blip_off] | ((uint16_t)dgg_data[blip_off + 1] << 8);
						uint16_t b_rec_type = (uint16_t)dgg_data[blip_off + 2] | ((uint16_t)dgg_data[blip_off + 3] << 8);
						uint32_t b_rec_len = readU32LE(&dgg_data[blip_off + 4]);
						uint16_t b_inst = b_ver_inst >> 4;
						// [MS-ODRAW] 2.2.24: OfficeArtBlipEMF (0xF01A), recInstance 0x3D4 (1 UID) or 0x3D5 (2 UIDs)
						if (b_rec_type == 0xF01A &&
						    (b_inst == 0x3D4 || b_inst == 0x3D5) &&
						    (size_t)b_rec_len <= fbse_end - (blip_off + 8))
						{
							size_t uid_bytes = (b_inst == 0x3D5) ? 32 : 16;
							if ((size_t)b_rec_len > uid_bytes + 34)
							{
								const unsigned char* mh = &dgg_data[blip_off + 8 + uid_bytes];
								uint32_t cb_size = readU32LE(mh);
								uint32_t cb_save = readU32LE(mh + 28);
								uint8_t f_compression = mh[32];
								size_t payload_off = blip_off + 8 + uid_bytes + 34;
								size_t avail = (size_t)b_rec_len - (uid_bytes + 34);
								size_t comp_len = (cb_save > 0 && (size_t)cb_save <= avail) ? (size_t)cb_save : avail;
								if (comp_len > 0)
								{
									size_t remaining_budget = ctx.max_bytes - total_emf_bytes;
									if (f_compression == 0xFE) // msocompressionNone
									{
										if (comp_len <= remaining_budget)
										{
											++emf_index;
											total_emf_bytes += comp_len;
											std::string hint = "xls/drawing_emf_" + std::to_string(emf_index);
											if (extractEmfEmbeddedObjects(&dgg_data[payload_off], comp_len, hint, ctx, out_text))
												any = true;
										}
									}
									else if (f_compression == 0x00 && cb_size > 0 && (size_t)cb_size <= remaining_budget) // zlib deflate
									{
										if (comp_len <= (size_t)UINT_MAX)
										{
											++emf_index;
											size_t max_uncomp = std::min<size_t>((size_t)cb_size, remaining_budget);
											ZlibStreamGuard zguard;
											zguard.zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(&dgg_data[payload_off]));
											zguard.zs.avail_in = (uInt)comp_len;
											if (inflateInit(&zguard.zs) == Z_OK)
											{
												zguard.active = true;
												std::vector<unsigned char> emf_buf;
												emf_buf.reserve(std::min<size_t>(max_uncomp, 256U * 1024U));
												unsigned char chunk[32768];
												bool ok = false;
												for (;;)
												{
													zguard.zs.next_out = chunk;
													zguard.zs.avail_out = sizeof(chunk);
													int zret = inflate(&zguard.zs, Z_NO_FLUSH);
													size_t produced = sizeof(chunk) - zguard.zs.avail_out;
													if (produced > max_uncomp - emf_buf.size())
														break;
													if (produced > 0)
														emf_buf.insert(emf_buf.end(), chunk, chunk + produced);
													if (zret == Z_STREAM_END)
													{
														ok = true;
														break;
													}
													if (zret != Z_OK || (zguard.zs.avail_in == 0 && produced == 0))
														break;
												}
												total_emf_bytes += emf_buf.size();
												if (ok && !emf_buf.empty())
												{
													std::string hint = "xls/drawing_emf_" + std::to_string(emf_index);
													if (extractEmfEmbeddedObjects(emf_buf.data(), emf_buf.size(), hint, ctx, out_text))
														any = true;
												}
											}
										}
									}
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
	return any;
}

bool extractFromEmbeddedContainerBuffer(const unsigned char* data,
                                        size_t size,
                                        const std::string& hint_name,
                                        EmbeddedExtractionContext& ctx,
                                        std::string& out_text)
{
	if (!ctx.formatting.parse_embedded_files || !data || size == 0 || size > ctx.max_bytes)
		return false;
	if (ctx.depth >= ctx.max_depth || ctx.extracted_count >= ctx.max_count)
		return false;

	static const unsigned char kOle2Magic[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
	if (size >= 8 && std::memcmp(data, kOle2Magic, 8) == 0)
	{
		ThreadSafeOLEStorage storage(reinterpret_cast<const char*>(data), size);
		if (storage.isValid())
		{
			std::vector<std::string> entries;
			if (storage.getStreamsAndStoragesList(entries) &&
			    (hasStream(entries, "\x01Ole10Native") ||
			     hasStream(entries, "CONTENTS") ||
			     hasStream(entries, "Package")))
			{
				return extractFromOLEStorage(storage, hint_name, ctx, out_text);
			}
		}
	}
	return extractFromEmbeddedBuffer(data, size, hint_name, ctx, out_text);
}

void extractZipEmbeddings(DocToTextUnzip& zipfile,
                          bool is_ooxml,
                          EmbeddedExtractionContext& ctx,
                          std::string& out_text)
{
	if (!ctx.formatting.parse_embedded_files)
		return;
	if (ctx.depth >= ctx.max_depth || ctx.extracted_count >= ctx.max_count)
		return;

	std::vector<std::string> files;
	if (!zipfile.getFileList(files))
		return;
	std::sort(files.begin(), files.end());

	for (size_t i = 0; i < files.size(); ++i)
	{
		if (ctx.extracted_count >= ctx.max_count)
			break;
		const std::string& path = files[i];
		if (path.empty() || path[path.size() - 1] == '/')
			continue;

		bool match = false;
		bool is_emf_media = false;
		if (is_ooxml)
		{
			match = startsWith(path, "word/embeddings/") ||
			        startsWith(path, "xl/embeddings/") ||
			        startsWith(path, "ppt/embeddings/");
			if (!match && endsWithCaseInsensitive(path, ".emf") &&
			    (startsWith(path, "word/media/") ||
			     startsWith(path, "xl/media/") ||
			     startsWith(path, "ppt/media/")))
			{
				is_emf_media = true;
			}
		}
		else
		{
			match = isOdfEmbeddedBinaryCandidate(path);
		}
		if (!match && !is_emf_media)
			continue;

		unsigned long entry_size = 0;
		if (zipfile.getFileSize(path, entry_size) && entry_size > ctx.max_bytes)
		{
			if (ctx.verbose_logging && ctx.log_stream)
				*ctx.log_stream << "Skipping oversized zip embedding (" << path << "): " << entry_size << " bytes.\n";
			continue;
		}

		std::string payload;
		int read_limit = (ctx.max_bytes < (size_t)INT_MAX) ? (int)(ctx.max_bytes + 1) : 0;
		if (!zipfile.read(path, &payload, read_limit) || payload.empty())
			continue;
		if (payload.size() > ctx.max_bytes)
		{
			if (ctx.verbose_logging && ctx.log_stream)
				*ctx.log_stream << "Skipping oversized zip embedding (" << path << "): exceeds max_bytes.\n";
			continue;
		}
		if (is_emf_media)
		{
			extractEmfEmbeddedObjects(
				reinterpret_cast<const unsigned char*>(payload.data()),
				payload.size(),
				path,
				ctx,
				out_text);
			continue;
		}
		if (!is_ooxml && !hasStructuredContainerOrDocMagic(payload))
			continue;

		extractFromEmbeddedContainerBuffer(
			reinterpret_cast<const unsigned char*>(payload.data()),
			payload.size(),
			path,
			ctx,
			out_text);
	}
}

namespace
{
	bool readLenPrefixedAnsi(const unsigned char* data, size_t size, size_t& pos, std::string& out)
	{
		if (size - pos < 4)
			return false;
		uint32_t len = readU32LE(data + pos);
		pos += 4;
		if (len > size - pos)
			return false;
		out.assign(reinterpret_cast<const char*>(data + pos), len);
		while (!out.empty() && out[out.size() - 1] == '\0')
			out.erase(out.size() - 1);
		pos += len;
		return true;
	}

	void appendUtf8CodePoint(uint32_t cp, std::string& out)
	{
		if (cp <= 0x7Fu)
		{
			out.push_back(static_cast<char>(cp));
		}
		else if (cp <= 0x7FFu)
		{
			out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
			out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
		}
		else if (cp >= 0xD800u && cp <= 0xDFFFu)
		{
			// Skip unpaired UTF-16 surrogates
		}
		else if (cp <= 0xFFFFu)
		{
			out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
			out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
			out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
		}
		else if (cp <= 0x10FFFFu)
		{
			out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
			out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
			out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
			out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
		}
	}

	void decodeUtf16LeName(const unsigned char* p, uint32_t code_units, std::string& out)
	{
		out.clear();
		for (uint32_t i = 0; i < code_units; ++i)
		{
			uint16_t ch = readU16LE(p + static_cast<size_t>(i) * 2);
			if (ch == 0)
				break;
			if (ch >= 0xD800u && ch <= 0xDBFFu && i + 1 < code_units)
			{
				uint16_t lo = readU16LE(p + static_cast<size_t>(i + 1) * 2);
				if (lo >= 0xDC00u && lo <= 0xDFFFu)
				{
					uint32_t cp = 0x10000u +
					              ((static_cast<uint32_t>(ch - 0xD800u) << 10) |
					               static_cast<uint32_t>(lo - 0xDC00u));
					appendUtf8CodePoint(cp, out);
					++i;
					continue;
				}
			}
			appendUtf8CodePoint(ch, out);
		}
	}

	bool parseOle1Package(const unsigned char* pkg,
	                      size_t pkg_size,
	                      size_t max_bytes,
	                      std::string& out_name,
	                      const unsigned char*& out_payload,
	                      size_t& out_payload_size)
	{
		out_name.clear();
		out_payload = NULL;
		out_payload_size = 0;
		if (!pkg || pkg_size < 16 || pkg_size > max_bytes)
			return false;
		size_t pos = 2; // 2B type
		std::string display_name;
		std::string icon_path;
		std::string ansi_file_path;
		if (!readCStr(pkg, pkg_size, pos, display_name))
			return false;
		if (!readCStr(pkg, pkg_size, pos, icon_path))
			return false;
		if (pkg_size - pos < 8)
			return false;
		pos += 2; // iconIndex
		uint16_t pkg_type = readU16LE(pkg + pos);
		pos += 2;
		if (pkg_type != 3) // 1 = link, 3 = embedded object
			return false;
		pos += 4; // filePathLen
		if (!readCStr(pkg, pkg_size, pos, ansi_file_path))
			return false;
		if (pkg_size - pos < 4)
			return false;
		uint32_t data_size = readU32LE(pkg + pos);
		pos += 4;
		if (data_size == 0 || data_size > max_bytes || data_size > pkg_size - pos)
			return false;
		out_payload = pkg + pos;
		out_payload_size = data_size;
		pos += data_size;

		std::string unicode_path;
		if (pkg_size - pos >= 4)
		{
			uint32_t unicode_chars = readU32LE(pkg + pos);
			pos += 4;
			if (unicode_chars > 0 && unicode_chars <= 4096 &&
			    static_cast<size_t>(unicode_chars) * 2 <= pkg_size - pos)
			{
				decodeUtf16LeName(pkg + pos, unicode_chars, unicode_path);
			}
		}
		std::string chosen = !unicode_path.empty()
			? unicode_path
			: (!display_name.empty() ? display_name : ansi_file_path);
		out_name = baseFileName(chosen);
		return true;
	}
}

bool extractOle1ObjData(const unsigned char* data,
                        size_t size,
                        EmbeddedExtractionContext& ctx,
                        std::string& out_text)
{
	out_text.clear();
	if (!ctx.formatting.parse_embedded_files || !data || size < 20 || size > ctx.max_bytes)
		return false;
	if (ctx.depth >= ctx.max_depth || ctx.extracted_count >= ctx.max_count)
		return false;

	size_t pos = 4; // OLE 1.0 version
	uint32_t format_id = readU32LE(data + pos);
	pos += 4;
	if (format_id != 2) // 2 = embedded, 1 = linked
		return false;

	std::string class_name;
	std::string topic_name;
	std::string item_name;
	if (!readLenPrefixedAnsi(data, size, pos, class_name))
		return false;
	if (!readLenPrefixedAnsi(data, size, pos, topic_name))
		return false;
	if (!readLenPrefixedAnsi(data, size, pos, item_name))
		return false;

	if (size - pos < 4)
		return false;
	uint32_t data_size = readU32LE(data + pos);
	pos += 4;
	if (data_size == 0 || data_size > ctx.max_bytes || data_size > size - pos)
		return false;

	const unsigned char* payload = data + pos;
	if (data_size >= 8 && std::memcmp(payload, "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8) == 0)
	{
		ThreadSafeOLEStorage storage(reinterpret_cast<const char*>(payload), data_size);
		if (storage.isValid())
			return extractFromOLEStorage(storage, class_name, ctx, out_text);
		return false;
	}
	if (asciiEqualsIgnoreCase(class_name, "Package"))
	{
		std::string hint_name;
		const unsigned char* pkg_payload = NULL;
		size_t pkg_payload_size = 0;
		if (!parseOle1Package(payload, data_size, ctx.max_bytes, hint_name, pkg_payload, pkg_payload_size))
			return false;
		return extractFromEmbeddedBuffer(pkg_payload, pkg_payload_size, hint_name, ctx, out_text);
	}
	return extractFromEmbeddedBuffer(payload, data_size, class_name, ctx, out_text);
}

}
