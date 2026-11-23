#ifndef DOCTOTEXT_OSHARED_H
#define DOCTOTEXT_OSHARED_H

#include <iosfwd>
#include <stddef.h>
#include <string>

namespace doctotext
{
	class Metadata;
}
class ThreadSafeOLEStorage;

using namespace doctotext;

bool parse_oshared_summary_info(ThreadSafeOLEStorage& storage, std::ostream& log_stream, Metadata& meta);
bool parse_oshared_document_summary_info(ThreadSafeOLEStorage& storage, std::ostream& log_stream, int& slide_count);
bool get_codepage_from_document_summary_info(ThreadSafeOLEStorage& storage, std::ostream& log_stream, std::string& codepage);

bool is_encrypted_offcrypto_ole(ThreadSafeOLEStorage& storage);
bool is_encrypted_offcrypto_source(const std::string& file_name, const char* buffer, size_t buffer_size);
bool is_encrypted_doc_ole(ThreadSafeOLEStorage& storage);
bool is_encrypted_ppt_ole(ThreadSafeOLEStorage& storage);

enum OffCryptoResult
{
	OFFCRYPTO_NOT_ENCRYPTED = 0,
	OFFCRYPTO_DECRYPTED = 1,
	OFFCRYPTO_NEEDS_PASSWORD = 2
};

// Detects MS-OFFCRYPTO OLE containers (EncryptionInfo + EncryptedPackage) and attempts
// silent decryption using Office's built-in default password ("VelvetSweatshop").
OffCryptoResult try_decrypt_offcrypto_source(const std::string& file_name,
                                             const char* buffer,
                                             size_t buffer_size,
                                             std::string& out_decrypted_zip,
                                             std::ostream* log_stream = NULL);

#endif
