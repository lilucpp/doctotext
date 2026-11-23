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

namespace doctotext
{
namespace
{
	EmbeddedExtractionContext*& tlsContextSlot()
	{
		static thread_local EmbeddedExtractionContext* s_active = NULL;
		return s_active;
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
	bool extractEmfEmbeddedObjects(const unsigned char* data,
	                                 size_t size,
	                                 const std::string& hint_name,
	                                 EmbeddedExtractionContext& ctx,
	                                 std::string& out_text)
	{
		if (!data || size < 8)
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

}
