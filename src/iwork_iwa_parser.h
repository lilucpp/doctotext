#ifndef DOCTOTEXT_IWORK_IWA_PARSER_H
#define DOCTOTEXT_IWORK_IWA_PARSER_H

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

class DocToTextUnzip;

namespace doctotext
{
namespace iwa
{

struct IwaMessageSlice
{
	uint64_t archive_id;
	uint32_t message_type;
	size_t offset;
	size_t length;
};

/// Decompresses an Apple IWA stream ([1B chunk_type][3B LE length][raw Snappy]).
bool decompressIwaStream(const std::string& raw, std::string& out_decompressed, std::string* err_msg = NULL);

/// Indexes [varint header_len][ArchiveInfo][payload objects] slices within a decompressed IWA stream.
bool parseIwaObjectStream(const std::string& stream, std::vector<IwaMessageSlice>& out_slices);

/// Extracts lightweight text (TSWP bodies/notes and linked table strings) from a single decompressed IWA stream.
std::string extractTextFromIwaBufferForTest(const std::string& raw_iwa, bool* ok = NULL);

/// Returns true if an opened zip archive looks like an iWork '13+ (Index/*.iwa) or '18+ (*/Index.zip) package.
bool isModernIWorkArchive(DocToTextUnzip& unzip);

/// Extracts plain text from an iWork '13+ or '18+ archive using lightweight IWA/Protobuf wire decoding.
bool extractModernIWorkText(DocToTextUnzip& unzip,
                            bool verbose,
                            std::ostream& log_stream,
                            std::string& out_text,
                            bool& out_error);

} // namespace iwa
} // namespace doctotext

#endif
