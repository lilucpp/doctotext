#ifndef DOCTOTEXT_EMBEDDED_OBJECT_EXTRACTOR_H
#define DOCTOTEXT_EMBEDDED_OBJECT_EXTRACTOR_H

#include "formatting_style.h"
#include "plain_text_extractor.h"
#include <cstddef>
#include <ostream>
#include <string>
#include <vector>

class DocToTextUnzip;
class ThreadSafeOLEStorage;

namespace doctotext
{
	static const int kMaxEmbedDepth = 4;
	static const int kMaxEmbedCount = 20;
	static const size_t kMaxEmbedBytes = 64ULL * 1024ULL * 1024ULL;

	struct EmbeddedExtractionContext
	{
		int depth;
		int max_depth;
		int extracted_count;
		int max_count;
		size_t max_bytes;
		FormattingStyle formatting;
		XmlParseMode xml_parse_mode;
		bool manage_xml_parser;
		bool verbose_logging;
		std::ostream* log_stream;
		ImageParserCallback image_callback;
		void* image_user_data;

		EmbeddedExtractionContext()
			: depth(0),
			  max_depth(kMaxEmbedDepth),
			  extracted_count(0),
			  max_count(kMaxEmbedCount),
			  max_bytes(kMaxEmbedBytes),
			  xml_parse_mode(PARSE_XML),
			  manage_xml_parser(true),
			  verbose_logging(false),
			  log_stream(NULL),
			  image_callback(NULL),
			  image_user_data(NULL)
		{
		}

		/// Returns active thread-local context if one is installed on this thread, else NULL.
		static EmbeddedExtractionContext* current();

		/// RAII scope: installs `root_ctx` on TLS if no active context exists on this thread;
		/// otherwise exposes the active TLS context (preserving shared depth, count, and visited set).
		class ScopedContext
		{
			private:
				EmbeddedExtractionContext* m_prev;
				EmbeddedExtractionContext* m_active;
				bool m_installed;
			public:
				explicit ScopedContext(EmbeddedExtractionContext& root_ctx);
				~ScopedContext();
				EmbeddedExtractionContext& context() { return *m_active; }
		};

		/// RAII depth guard for nested extraction recursion.
		class ScopedDepth
		{
			private:
				EmbeddedExtractionContext& m_ctx;
			public:
				explicit ScopedDepth(EmbeddedExtractionContext& ctx) : m_ctx(ctx) { ++m_ctx.depth; }
				~ScopedDepth() { --m_ctx.depth; }
		};
	};

	/// Checks if an arbitrary buffer looks like plain text (UTF-8 / UTF-16 BOM / ASCII) rather than binary noise.
	bool isTextLikeBuffer(const unsigned char* data, size_t size);

	/// Parses an OLE2 `\x01Ole10Native` wrapper stream into filename + raw payload bytes.
	bool parseOle10Native(const unsigned char* data,
	                      size_t size,
	                      size_t max_bytes,
	                      std::string& out_name,
	                      std::vector<unsigned char>& out_payload);

	/// Reads up to `max_bytes` from a named stream in the current directory of `storage`.
	bool readOLEStreamBytes(ThreadSafeOLEStorage& storage,
	                        const std::string& stream_name,
	                        size_t max_bytes,
	                        std::vector<unsigned char>& out_bytes);

	/// Extracts text from an in-memory embedded file buffer with depth/count/size limits and fault isolation.
	bool extractFromEmbeddedBuffer(const unsigned char* data,
	                               size_t size,
	                               const std::string& hint_name,
	                               EmbeddedExtractionContext& ctx,
	                               std::string& out_text);

	/// Unwraps an OLE2 storage/sub-storage (Office streams, Ole10Native, CONTENTS, Package) and extracts text.
	bool extractFromOLEStorage(ThreadSafeOLEStorage& storage,
	                           const std::string& hint_name,
	                           EmbeddedExtractionContext& ctx,
	                           std::string& out_text);

	/// Scans an EMF metafile buffer for EMR_COMMENT_MULTIFORMATS embedded PDFs and extracts text.
	bool extractEmfEmbeddedObjects(const unsigned char* data,
	                                 size_t size,
	                                 const std::string& hint_name,
	                                 EmbeddedExtractionContext& ctx,
	                                 std::string& out_text);

	/// Scans an OfficeArt DrawingGroup (MSODRAWINGGROUP) buffer for EMF BLIPs containing embedded PDFs.
	bool extractOfficeArtDrawingEmbeddings(const std::vector<unsigned char>& dgg_data,
	                                       EmbeddedExtractionContext& ctx,
	                                       std::string& out_text);

	/// Dispatches an arbitrary embedded container or file buffer (detecting OLE2 wrapper containers vs direct formats).
	bool extractFromEmbeddedContainerBuffer(const unsigned char* data,
	                                        size_t size,
	                                        const std::string& hint_name,
	                                        EmbeddedExtractionContext& ctx,
	                                        std::string& out_text);

	/// Scans ZIP archive entries matching OOXML embedding directories or ODF binary OLE objects.
	void extractZipEmbeddings(DocToTextUnzip& zipfile,
	                          bool is_ooxml,
	                          EmbeddedExtractionContext& ctx,
	                          std::string& out_text);

	/// Parses an OLE 1.0 object payload (e.g. from RTF `{\*\objdata ...}`) and extracts text.
	bool extractOle1ObjData(const unsigned char* data,
	                        size_t size,
	                        EmbeddedExtractionContext& ctx,
	                        std::string& out_text);
}

#endif
