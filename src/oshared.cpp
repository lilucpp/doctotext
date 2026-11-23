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

#include "oshared.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <memory>
#include <stdio.h>
#include <string.h>
#include <vector>
#include "metadata.h"
#include "poppler_decrypt.h"
#include "wv2/utilities.h"
#include <time.h>
#include "thread_safe_ole_stream_reader.h"
#include "thread_safe_ole_storage.h"
#include "xml_stream.h"

using namespace wvWare;

static bool read_vt_string(ThreadSafeOLEStreamReader* reader, std::ostream& log_stream, std::string& s)
{
	U16 string_type;
	if (!reader->readU16(string_type) || string_type != 0x1E)
	{
		log_stream << "Incorrect string type.\n";
		return false;
	}
	reader->seek(2, SEEK_CUR); //padding
	U32 len;
	reader->readU32(len);
	s = "";
	for (int j = 0; j < len - 1; j++)
	{
		S8 ch;
		reader->readS8(ch);
		if (ch == '\0')
			break;
		s += ch;
	}
	if (!reader->isValid())
	{
		log_stream << reader->getLastError() << "\n";
		return false;
	}
	return true;
}

static bool read_vt_i4(ThreadSafeOLEStreamReader* reader, std::ostream& log_stream, S32& i)
{
	U16 string_type;
	if (!reader->readU16(string_type) || string_type != 0x0003)
	{
		log_stream << "Incorrect value type.\n";
		return false;
	}
	reader->seek(2, SEEK_CUR); //padding
	reader->readS32(i);
	if (!reader->isValid())
	{
		log_stream << reader->getLastError() << "\n";
		return false;
	}
	return true;
}

static bool read_vt_i2(ThreadSafeOLEStreamReader* reader, std::ostream& log_stream, S16& i)
{
	U16 string_type;
	if (!reader->readU16(string_type) || string_type != 0x0002)
	{
		log_stream << "Incorrect value type.\n";
		return false;
	}
	reader->seek(2, SEEK_CUR); //padding
	reader->readS16(i);
	if (!reader->isValid())
	{
		log_stream << reader->getLastError() << "\n";
		return false;
	}
	return true;
}

static bool read_vt_filetime(ThreadSafeOLEStreamReader* reader, std::ostream& log_stream, tm& time)
{
	U16 type;
	if (!reader->readU16(type) || type != 0x0040)
	{
		log_stream << "Incorrect variable type.\n";
		return false;
	}
	reader->seek(2, SEEK_CUR); //padding
	U32 file_time_low, file_time_high;
	reader->readU32(file_time_low);
	reader->readU32(file_time_high);
	if (!reader->isValid())
	{
		log_stream << reader->getLastError() << "\n";
		return false;
	}
	if (file_time_low == 0 && file_time_high == 0)
	{
		// Sometimes field exists but date is zero.
		// Last modification time saved by LibreOffice 3.5 when document is created is an example.
		time = (tm){0};
		return true;
	}
	unsigned long long int file_time = ((unsigned long long int)file_time_high << 32) | (unsigned long long int)file_time_low;
	time_t t = (time_t)(file_time / 10000000 - 11644473600LL);
	tm* res = gmtime(&t);
	if (res == NULL)
	{
		log_stream << "Incorrect time value.\n";
		return false;
	}
	time = *res;
	return true;
}

bool parse_oshared_summary_info(ThreadSafeOLEStorage& storage, std::ostream& log_stream, Metadata& meta)
{
	log_stream << "Extracting metadata.\n";
	if (!storage.isValid())
	{
		log_stream << "Error opening " << storage.name() << " as OLE file.\n";
		return false;
	}
	ThreadSafeOLEStreamReader* reader = (ThreadSafeOLEStreamReader*)storage.createStreamReader("\005SummaryInformation");
	if (reader == NULL)
	{
		log_stream << "Error opening SummaryInformation stream.\n";
		return false;
	}
	size_t field_set_stream_start = reader->tell();
	U16 byte_order;
	if (!reader->readU16(byte_order) || byte_order != 0xFFFE)
	{
		delete reader;
		log_stream << "Incorrect ByteOrder value.\n";
		return false;
	}
	U16 version;
	if (!reader->readU16(version) || version != 0x00)
	{
		delete reader;
		log_stream << "Incorrect Version value.\n";
		return false;
	}
	reader->seek(4, SEEK_CUR); //system indentifier
	for (int i = 0; i < 4; i++)
	{
		U32 clsid_part;
		if (!reader->readU32(clsid_part) || clsid_part != 0x00)
		{
			delete reader;
			log_stream << "Incorrect CLSID value.\n";
			return false;
		}
	}
	U32 num_property_sets;
	if (!reader->readU32(num_property_sets) || (num_property_sets != 0x01 && num_property_sets != 0x02))
	{
		delete reader;
		log_stream << "Incorrect number of property sets.\n";
		return false;
	}
	reader->seek(16, SEEK_CUR);	// fmtid0_part
	U32 offset;
	reader->readU32(offset);
	int property_set_pos = field_set_stream_start + offset;
	reader->seek(property_set_pos, SEEK_SET);
	U32 size, num_props = 0;
	reader->readU32(size);
	reader->readU32(num_props);
	for (int i = 0; i < num_props; i++)
	{
		U32 prop_id, offset;
		reader->readU32(prop_id);
		reader->readU32(offset);
		int p = reader->tell();
		switch (prop_id)
		{
			case 0x00000004:
			{
				reader->seek(property_set_pos + offset, SEEK_SET);
				std::string author;
				if (read_vt_string(reader, log_stream, author))
					meta.setAuthor(author);
				break;
			}
			case 0x00000008:
			{
				reader->seek(property_set_pos + offset, SEEK_SET);
				std::string last_modified_by;
				if (read_vt_string(reader, log_stream, last_modified_by))
					meta.setLastModifiedBy(last_modified_by);
				break;
			}
			case 0x0000000C:
			{
				reader->seek(property_set_pos + offset, SEEK_SET);
				tm creation_date;
				if (read_vt_filetime(reader, log_stream, creation_date))
					meta.setCreationDate(creation_date);
				break;
			}
			case 0x0000000D:
			{
				reader->seek(property_set_pos + offset, SEEK_SET);
				tm last_modification_date;
				if (read_vt_filetime(reader, log_stream, last_modification_date))
					meta.setLastModificationDate(last_modification_date);
				break;
			}
			case 0x0000000E:
			{
				reader->seek(property_set_pos + offset, SEEK_SET);
				S32 page_count;
				if (read_vt_i4(reader, log_stream, page_count))
					meta.setPageCount(page_count);
				break;
			}
			case 0x0000000F:
			{
				reader->seek(property_set_pos + offset, SEEK_SET);
				S32 word_count;
				if (read_vt_i4(reader, log_stream, word_count))
					meta.setWordCount(word_count);
				break;
			}
		}
		reader->seek(p, SEEK_SET);
		if (!reader->isValid())
		{
			log_stream << reader->getLastError() << "\n";
			delete reader;
			return false;
		}
	}
	if (!reader->isValid())
	{
		log_stream << reader->getLastError() << "\n";
		delete reader;
		return false;
	}
	delete reader;
	return true;
}

static ThreadSafeOLEStreamReader* open_oshared_document_summary_info(ThreadSafeOLEStorage& storage, std::ostream& log_stream, size_t& field_set_stream_start)
{
	ThreadSafeOLEStreamReader* reader = (ThreadSafeOLEStreamReader*)storage.createStreamReader("\005DocumentSummaryInformation");
	if (reader == NULL)
	{
		log_stream << "Error opening DocumentSummaryInformation stream.\n";
		return NULL;
	}
	field_set_stream_start = reader->tell();
	U16 byte_order;
	if (!reader->readU16(byte_order) || byte_order != 0xFFFE)
	{
		log_stream << "Incorrect ByteOrder value.\n";
		delete reader;
		return NULL;
	}
	U16 version;
	if (!reader->readU16(version) || version != 0x00)
	{
		log_stream << "Incorrect Version value.\n";
		delete reader;
		return NULL;
	}
	reader->seek(4, SEEK_CUR);	// system indentifier
	for (int i = 0; i < 4; i++)
	{
		U32 clsid_part;;
		if (!reader->readU32(clsid_part) || clsid_part != 0x00)
		{
			log_stream << "Incorrect CLSID value.\n";
			delete reader;
			return NULL;
		}
	}
	U32 num_property_sets;
	if (!reader->readU32(num_property_sets) || (num_property_sets != 0x01 && num_property_sets != 0x02))
	{
		log_stream << "Incorrect number of property sets.\n";
		delete reader;
		return NULL;
	}
	reader->seek(16, SEEK_CUR);	//fmtid0_part
	if (!reader->isValid())
	{
		log_stream << reader->getLastError() << "\n";
		delete reader;
		return NULL;
	}
	return reader;
}

bool get_codepage_from_document_summary_info(ThreadSafeOLEStorage& storage, std::ostream& log_stream, std::string& codepage)
{
	size_t field_set_stream_start;
	if (!storage.isValid())
	{
		log_stream << "Error opening " << storage.name() << " as OLE file.\n";
		return false;
	}
	ThreadSafeOLEStreamReader* reader = open_oshared_document_summary_info(storage, log_stream, field_set_stream_start);
	if (reader == NULL)
		return false;
	U32 offset;
	reader->readU32(offset);
	int property_set_pos = field_set_stream_start + offset;
	reader->seek(property_set_pos, SEEK_SET);
	U32 size, num_props = 0;
	reader->readU32(size);
	reader->readU32(num_props);
	for (int i = 0; i < num_props; i++)
	{
		U32 prop_id, offset;
		reader->readU32(prop_id);
		reader->readU32(offset);
		int p = reader->tell();
		switch (prop_id)
		{
			case 0x00000001:
				reader->seek(property_set_pos + offset, SEEK_SET);
				S16 icodepage;
				if (!read_vt_i2(reader, log_stream, icodepage))
				{
					delete reader;
					return false;
				}
				codepage = "cp" + int2string(icodepage);
				delete reader;
				return true;
		}
		reader->seek(p, SEEK_SET);
		if (!reader->isValid())
		{
			log_stream << reader->getLastError() << "\n";
			delete reader;
			return false;
		}
	}
	if (!reader->isValid())
	{
		log_stream << reader->getLastError() << "\n";
		delete reader;
		return false;
	}
	delete reader;
	return false;
}

bool parse_oshared_document_summary_info(ThreadSafeOLEStorage& storage, std::ostream& log_stream, int& slide_count)
{
	log_stream << "Extracting additional metadata.\n";
	size_t field_set_stream_start;
	if (!storage.isValid())
	{
		log_stream << "Error opening " << storage.name() << " as OLE file.\n";
		return false;
	}
	ThreadSafeOLEStreamReader* reader = open_oshared_document_summary_info(storage, log_stream, field_set_stream_start);
	if (reader == NULL)
		return false;
	U32 offset;
	reader->readU32(offset);
	int property_set_pos = field_set_stream_start + offset;
	reader->seek(property_set_pos, SEEK_SET);
	U32 size, num_props = 0;
	reader->readU32(size);
	reader->readU32(num_props);
	bool slide_count_found = false;
	for (int i = 0; i < num_props; i++)
	{
		U32 prop_id, offset;
		reader->readU32(prop_id);
		reader->readU32(offset);
		int p = reader->tell();
		switch (prop_id)
		{
			case 0x00000007:
				reader->seek(property_set_pos + offset, SEEK_SET);
				if (!read_vt_i4(reader, log_stream, slide_count))
				{
					delete reader;
					return false;
				}
				slide_count_found = true;
				break;
		}
		reader->seek(p, SEEK_SET);
		if (!reader->isValid())
		{
			log_stream << reader->getLastError() << "\n";
			delete reader;
			return false;
		}
	}
	if (!slide_count_found)
		slide_count = -1;
	if (!reader->isValid())
	{
		log_stream << reader->getLastError() << "\n";
		delete reader;
		return false;
	}
	delete reader;
	return true;
}

static bool ole_stream_exists(ThreadSafeOLEStorage& storage, const std::string& name)
{
	AbstractOLEStreamReader* reader = storage.createStreamReader(name);
	if (!reader)
		return false;
	delete reader;
	return true;
}

static bool has_ole2_header(const std::string& file_name, const char* buffer, size_t buffer_size)
{
	static const unsigned char ole_magic[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
	if (buffer)
	{
		if (buffer_size < 8)
			return false;
		return memcmp(buffer, ole_magic, 8) == 0;
	}
	FILE* f = fopen(file_name.c_str(), "rb");
	if (!f)
		return false;
	unsigned char hdr[8];
	size_t n = fread(hdr, 1, 8, f);
	fclose(f);
	return n == 8 && memcmp(hdr, ole_magic, 8) == 0;
}

bool is_encrypted_offcrypto_ole(ThreadSafeOLEStorage& storage)
{
	if (!storage.isValid())
		return false;
	return ole_stream_exists(storage, "EncryptionInfo") && ole_stream_exists(storage, "EncryptedPackage");
}

bool is_encrypted_offcrypto_source(const std::string& file_name, const char* buffer, size_t buffer_size)
{
	if (!has_ole2_header(file_name, buffer, buffer_size))
		return false;
	ThreadSafeOLEStorage* storage = buffer
		? new ThreadSafeOLEStorage(buffer, buffer_size)
		: new ThreadSafeOLEStorage(file_name);
	bool encrypted = is_encrypted_offcrypto_ole(*storage);
	delete storage;
	return encrypted;
}

bool is_encrypted_doc_ole(ThreadSafeOLEStorage& storage)
{
	if (!storage.isValid())
		return false;
	AbstractOLEStreamReader* reader = storage.createStreamReader("WordDocument");
	if (!reader)
		return false;
	unsigned char fib_hdr[12] = {0};
	bool encrypted = false;
	if (reader->read(fib_hdr, 12))
		encrypted = ((fib_hdr[11] & 0x01) != 0); // FibBase.fEncrypted (offset 0x0A, bit 8)
	delete reader;
	return encrypted;
}

bool is_encrypted_ppt_ole(ThreadSafeOLEStorage& storage)
{
	if (!storage.isValid())
		return false;
	if (ole_stream_exists(storage, "EncryptedSummary"))
		return true;
	AbstractOLEStreamReader* reader = storage.createStreamReader("Current User");
	if (!reader)
		return false;
	unsigned char cu_hdr[16] = {0};
	bool encrypted = false;
	if (reader->read(cu_hdr, 16))
	{
		// CurrentUserAtom.headerToken == 0xF3D1C4DF indicates encrypted PPT
		encrypted = (cu_hdr[12] == 0xDF && cu_hdr[13] == 0xC4 && cu_hdr[14] == 0xD1 && cu_hdr[15] == 0xF3);
	}
	delete reader;
	return encrypted;
}

namespace
{
	const size_t kMaxEncryptionInfoBytes = 1024 * 1024;
	const size_t kMaxEncryptedPackageBytes = 128ULL * 1024ULL * 1024ULL;

	U32 readU32LE(const unsigned char* p)
	{
		return static_cast<U32>(p[0]) |
		       (static_cast<U32>(p[1]) << 8) |
		       (static_cast<U32>(p[2]) << 16) |
		       (static_cast<U32>(p[3]) << 24);
	}

	U16 readU16LE(const unsigned char* p)
	{
		return static_cast<U16>(p[0]) | (static_cast<U16>(p[1]) << 8);
	}

	uint64_t readU64LE(const unsigned char* p)
	{
		return static_cast<uint64_t>(readU32LE(p)) |
		       (static_cast<uint64_t>(readU32LE(p + 4)) << 32);
	}

	void writeU32LE(uint32_t v, unsigned char out[4])
	{
		out[0] = static_cast<unsigned char>(v & 0xFF);
		out[1] = static_cast<unsigned char>((v >> 8) & 0xFF);
		out[2] = static_cast<unsigned char>((v >> 16) & 0xFF);
		out[3] = static_cast<unsigned char>((v >> 24) & 0xFF);
	}

	bool readOleStreamBounded(ThreadSafeOLEStorage& storage,
	                          const std::string& name,
	                          size_t max_bytes,
	                          std::string& out)
	{
		out.clear();
		AbstractOLEStreamReader* reader = storage.createStreamReader(name);
		if (!reader)
			return false;
		size_t sz = reader->size();
		if (sz == 0 || sz > max_bytes)
		{
			delete reader;
			return false;
		}
		out.resize(sz);
		bool ok = reader->read(reinterpret_cast<uint8_t*>(&out[0]), sz);
		delete reader;
		if (!ok)
			out.clear();
		return ok;
	}

	size_t asciiToUtf16LE(const char* s, unsigned char* out, size_t max_bytes)
	{
		if (!s || !out)
			return 0;
		size_t pos = 0;
		for (size_t i = 0; s[i] != '\0' && pos + 2 <= max_bytes; ++i)
		{
			out[pos++] = static_cast<unsigned char>(s[i]);
			out[pos++] = 0;
		}
		return pos;
	}

	enum HashKind
	{
		HASH_UNKNOWN = 0,
		HASH_SHA1 = 20,
		HASH_SHA256 = 32,
		HASH_SHA384 = 48,
		HASH_SHA512 = 64
	};

	void computeHash(HashKind kind, const unsigned char* data, size_t len, unsigned char* out)
	{
		switch (kind)
		{
			case HASH_SHA1:
				poppler_sha1(data, len, out);
				break;
			case HASH_SHA256:
				poppler_sha256(data, len, out);
				break;
			case HASH_SHA384:
				poppler_sha384(data, len, out);
				break;
			case HASH_SHA512:
				poppler_sha512(data, len, out);
				break;
			default:
				break;
		}
	}

	struct StandardHeaderInfo
	{
		int aes_bits;
		const unsigned char* salt;
		const unsigned char* enc_verifier;
		const unsigned char* enc_verifier_hash;
	};

	bool parseStandardHeader(const unsigned char* data, size_t size, StandardHeaderInfo& info)
	{
		if (size < 44)
			return false;
		const U32 header_size = readU32LE(data + 8);
		if (header_size < 32 || 12ULL + header_size > size)
			return false;

		const U32 flags = readU32LE(data + 12);
		U32 alg_id = readU32LE(data + 20);
		U32 alg_hash = readU32LE(data + 24);
		const U32 key_bits = readU32LE(data + 28);

		if (alg_id == 0 && (flags & 0x20) && (flags & 0x10))
			alg_id = (key_bits == 256) ? 0x6610U : 0x660EU;
		if (alg_hash == 0 && (flags & 0x20))
			alg_hash = 0x8004U;

		if ((alg_id != 0x660EU && alg_id != 0x6610U) || alg_hash != 0x8004U)
			return false;
		info.aes_bits = (alg_id == 0x6610U) ? 256 : 128;
		if (key_bits != static_cast<U32>(info.aes_bits))
			return false;

		const size_t ver_off = 12 + static_cast<size_t>(header_size);
		if (ver_off + 40 + 32 > size)
			return false;
		const U32 salt_size = readU32LE(data + ver_off);
		if (salt_size != 16 || ver_off + 4 + salt_size + 16 + 4 + 32 > size)
			return false;

		info.salt = data + ver_off + 4;
		info.enc_verifier = info.salt + 16;
		const U32 verifier_hash_size = readU32LE(info.enc_verifier + 16);
		if (verifier_hash_size != 20)
			return false;
		info.enc_verifier_hash = info.enc_verifier + 20;
		return true;
	}

	bool verifyStandardPassword(const StandardHeaderInfo& info,
	                            const char* password,
	                            unsigned char out_key[32])
	{
		unsigned char buf[16 + 128];
		memcpy(buf, info.salt, 16);
		const size_t pwd_len = asciiToUtf16LE(password, buf + 16, 128);

		unsigned char h[20];
		poppler_sha1(buf, 16 + pwd_len, h);

		unsigned char iter_buf[24];
		for (uint32_t i = 0; i < 50000; ++i)
		{
			writeU32LE(i, iter_buf);
			memcpy(iter_buf + 4, h, 20);
			poppler_sha1(iter_buf, 24, h);
		}

		unsigned char final_in[24];
		memcpy(final_in, h, 20);
		writeU32LE(0, final_in + 20);
		unsigned char h_final[20];
		poppler_sha1(final_in, 24, h_final);

		unsigned char x1_in[64];
		unsigned char x2_in[64];
		memset(x1_in, 0x36, 64);
		memset(x2_in, 0x5C, 64);
		for (int i = 0; i < 20; ++i)
		{
			x1_in[i] ^= h_final[i];
			x2_in[i] ^= h_final[i];
		}
		unsigned char x_key[40];
		poppler_sha1(x1_in, 64, x_key);
		poppler_sha1(x2_in, 64, x_key + 20);

		unsigned char verifier[16];
		unsigned char verifier_hash[32];
		if (!poppler_aes_ecb_decrypt(x_key, info.aes_bits, info.enc_verifier, 16, verifier) ||
		    !poppler_aes_ecb_decrypt(x_key, info.aes_bits, info.enc_verifier_hash, 32, verifier_hash))
		{
			return false;
		}
		unsigned char actual_hash[20];
		poppler_sha1(verifier, 16, actual_hash);
		if (memcmp(actual_hash, verifier_hash, 20) != 0)
			return false;

		memcpy(out_key, x_key, static_cast<size_t>(info.aes_bits / 8));
		return true;
	}

	bool decryptStandardPackageInPlace(ThreadSafeOLEStorage& storage,
	                                   int aes_bits,
	                                   const unsigned char* key,
	                                   std::string& out_zip)
	{
		if (!readOleStreamBounded(storage, "EncryptedPackage", kMaxEncryptedPackageBytes + 8, out_zip) ||
		    out_zip.size() < 24)
		{
			out_zip.clear();
			return false;
		}
		const uint64_t pkg_size = readU64LE(reinterpret_cast<const unsigned char*>(out_zip.data()));
		const size_t enc_len = out_zip.size() - 8;
		if (pkg_size < 4 || pkg_size > kMaxEncryptedPackageBytes || pkg_size > enc_len || (enc_len % 16) != 0)
		{
			out_zip.clear();
			return false;
		}
		// Forward block-by-block decryption with out == in - 8 consumes each 16-byte ciphertext block
		// before overwriting earlier bytes, eliminating a second multi-megabyte buffer allocation.
		unsigned char* base = reinterpret_cast<unsigned char*>(&out_zip[0]);
		if (!poppler_aes_ecb_decrypt(key, aes_bits, base + 8, enc_len, base))
		{
			out_zip.clear();
			return false;
		}
		out_zip.resize(static_cast<size_t>(pkg_size));
		if (out_zip.size() < 4 || out_zip[0] != 'P' || out_zip[1] != 'K')
		{
			out_zip.clear();
			return false;
		}
		return true;
	}

	bool base64Decode(const std::string& src, std::vector<unsigned char>& out)
	{
		static const int kDec[256] = {
			-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
			-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
			-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
			52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
			-1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
			15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
			-1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
			41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
			-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
			-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
			-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
			-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
			-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
			-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
			-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
			-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
		};
		out.clear();
		uint32_t val = 0;
		int valb = -8;
		bool seen_pad = false;
		int pad_count = 0;
		for (size_t i = 0; i < src.size(); ++i)
		{
			unsigned char c = static_cast<unsigned char>(src[i]);
			if (c == ' ' || c == '\n' || c == '\r' || c == '\t')
				continue;
			if (c == '=')
			{
				seen_pad = true;
				if (++pad_count > 2)
					return false;
				continue;
			}
			if (seen_pad)
				return false;
			int d = kDec[c];
			if (d == -1)
				return false;
			val = (val << 6) + static_cast<uint32_t>(d);
			valb += 6;
			if (valb >= 0)
			{
				out.push_back(static_cast<unsigned char>((val >> valb) & 0xFF));
				valb -= 8;
			}
		}
		if (valb == -2 || out.empty())
			return false;
		return true;
	}

	bool parseStrictU32(const std::string& s, uint32_t& out)
	{
		if (s.empty())
			return false;
		for (size_t i = 0; i < s.size(); ++i)
		{
			if (s[i] < '0' || s[i] > '9')
				return false;
		}
		errno = 0;
		char* end = NULL;
		unsigned long v = strtoul(s.c_str(), &end, 10);
		if (errno != 0 || end != s.c_str() + s.size() || v > 0xFFFFFFFFUL)
			return false;
		out = static_cast<uint32_t>(v);
		return true;
	}

	HashKind parseHashAlgorithm(const std::string& name)
	{
		if (name == "SHA1") return HASH_SHA1;
		if (name == "SHA256") return HASH_SHA256;
		if (name == "SHA384") return HASH_SHA384;
		if (name == "SHA512") return HASH_SHA512;
		return HASH_UNKNOWN;
	}

	std::string getXmlAttr(xmlNodePtr node, const char* attr)
	{
		xmlChar* v = xmlGetProp(node, reinterpret_cast<const xmlChar*>(attr));
		if (!v)
			return "";
		std::string s(reinterpret_cast<const char*>(v));
		xmlFree(v);
		return s;
	}

	void collectLocalNameNodes(xmlNodePtr root, const char* target, std::vector<xmlNodePtr>& out)
	{
		for (xmlNodePtr cur = root; cur != NULL; cur = cur->next)
		{
			if (cur->type == XML_ELEMENT_NODE && xmlStrcmp(cur->name, reinterpret_cast<const xmlChar*>(target)) == 0)
				out.push_back(cur);
			collectLocalNameNodes(cur->children, target, out);
		}
	}

	void deriveAgileBlockKey(HashKind hash_kind,
	                         const unsigned char* h_n,
	                         const unsigned char block_key[8],
	                         size_t key_bytes,
	                         unsigned char* out_key)
	{
		const size_t hash_len = static_cast<size_t>(hash_kind);
		unsigned char in[64 + 8];
		memcpy(in, h_n, hash_len);
		memcpy(in + hash_len, block_key, 8);
		unsigned char digest[64];
		computeHash(hash_kind, in, hash_len + 8, digest);
		memset(out_key, 0x36, key_bytes);
		memcpy(out_key, digest, std::min(hash_len, key_bytes));
	}

	void deriveAgileIV(HashKind hash_kind,
	                   const std::vector<unsigned char>& salt,
	                   const unsigned char* block_key,
	                   size_t block_key_len,
	                   unsigned char out_iv[16])
	{
		if (!block_key || block_key_len == 0)
		{
			memset(out_iv, 0x36, 16);
			memcpy(out_iv, salt.data(), std::min(salt.size(), size_t(16)));
			return;
		}
		const size_t hash_len = static_cast<size_t>(hash_kind);
		unsigned char in[64 + 8];
		const size_t salt_len = std::min(salt.size(), size_t(64));
		const size_t bk_len = std::min(block_key_len, size_t(8));
		memcpy(in, salt.data(), salt_len);
		memcpy(in + salt_len, block_key, bk_len);
		unsigned char digest[64];
		computeHash(hash_kind, in, salt_len + bk_len, digest);
		memset(out_iv, 0x36, 16);
		memcpy(out_iv, digest, std::min(hash_len, size_t(16)));
	}

	struct AgileEncryptedKeyParams
	{
		uint32_t spin_count;
		int key_bits;
		size_t salt_size;
		HashKind hash_kind;
		std::vector<unsigned char> salt;
		std::vector<unsigned char> enc_ver_input;
		std::vector<unsigned char> enc_ver_value;
		std::vector<unsigned char> enc_key_value;
	};

	struct AgileParams
	{
		int kd_key_bits;
		HashKind kd_hash;
		std::vector<unsigned char> kd_salt;
		std::vector<AgileEncryptedKeyParams> keys;
	};

	bool parseAgileEncryptedKeyNode(xmlNodePtr ek, int kd_key_bits, AgileEncryptedKeyParams& out)
	{
		uint32_t block_sz = 0, key_bits = 0, hash_sz = 0, declared_salt_sz = 0, spins = 0;
		if (getXmlAttr(ek, "cipherAlgorithm") != "AES" ||
		    getXmlAttr(ek, "cipherChaining") != "ChainingModeCBC" ||
		    !parseStrictU32(getXmlAttr(ek, "blockSize"), block_sz) || block_sz != 16 ||
		    !parseStrictU32(getXmlAttr(ek, "keyBits"), key_bits) ||
		    (key_bits != 128 && key_bits != 256))
		{
			return false;
		}
		out.key_bits = static_cast<int>(key_bits);
		out.hash_kind = parseHashAlgorithm(getXmlAttr(ek, "hashAlgorithm"));
		if (out.hash_kind == HASH_UNKNOWN ||
		    !parseStrictU32(getXmlAttr(ek, "hashSize"), hash_sz) ||
		    hash_sz != static_cast<uint32_t>(out.hash_kind) ||
		    !parseStrictU32(getXmlAttr(ek, "saltSize"), declared_salt_sz) ||
		    declared_salt_sz < 1 || declared_salt_sz > 64 ||
		    !parseStrictU32(getXmlAttr(ek, "spinCount"), spins) ||
		    spins > 1000000UL)
		{
			return false;
		}
		out.spin_count = spins;
		out.salt_size = static_cast<size_t>(declared_salt_sz);

		if (!base64Decode(getXmlAttr(ek, "saltValue"), out.salt) ||
		    !base64Decode(getXmlAttr(ek, "encryptedVerifierHashInput"), out.enc_ver_input) ||
		    !base64Decode(getXmlAttr(ek, "encryptedVerifierHashValue"), out.enc_ver_value) ||
		    !base64Decode(getXmlAttr(ek, "encryptedKeyValue"), out.enc_key_value))
		{
			return false;
		}
		if (out.salt.size() != out.salt_size ||
		    out.enc_ver_input.size() < 16 || out.enc_ver_input.size() > 64 || (out.enc_ver_input.size() % 16) != 0 ||
		    out.enc_ver_value.size() < static_cast<size_t>(out.hash_kind) || out.enc_ver_value.size() > 64 ||
		    (out.enc_ver_value.size() % 16) != 0 ||
		    out.enc_key_value.size() < static_cast<size_t>(kd_key_bits / 8) || out.enc_key_value.size() > 64 ||
		    (out.enc_key_value.size() % 16) != 0)
		{
			return false;
		}
		return true;
	}

	bool parseAgileParams(const unsigned char* enc_info, size_t enc_info_len, AgileParams& params)
	{
		if (enc_info_len <= 8)
			return false;
		XmlStream::ensureXmlParserInitialized(true);
		struct XmlDocGuard
		{
			xmlDocPtr doc;
			explicit XmlDocGuard(xmlDocPtr d) : doc(d) {}
			~XmlDocGuard() { if (doc) xmlFreeDoc(doc); }
		} doc_guard(xmlReadMemory(reinterpret_cast<const char*>(enc_info + 8),
		                           static_cast<int>(enc_info_len - 8),
		                           "EncryptionInfo.xml", NULL,
		                           XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING));
		if (!doc_guard.doc)
			return false;
		xmlNodePtr root = xmlDocGetRootElement(doc_guard.doc);
		std::vector<xmlNodePtr> kd_nodes;
		std::vector<xmlNodePtr> ek_nodes;
		collectLocalNameNodes(root, "keyData", kd_nodes);
		collectLocalNameNodes(root, "encryptedKey", ek_nodes);
		if (kd_nodes.empty() || ek_nodes.empty())
			return false;

		xmlNodePtr kd = kd_nodes[0];
		uint32_t kd_block_sz = 0, kd_key_bits = 0, kd_hash_sz = 0, kd_salt_sz = 0;
		if (getXmlAttr(kd, "cipherAlgorithm") != "AES" ||
		    getXmlAttr(kd, "cipherChaining") != "ChainingModeCBC" ||
		    !parseStrictU32(getXmlAttr(kd, "blockSize"), kd_block_sz) || kd_block_sz != 16 ||
		    !parseStrictU32(getXmlAttr(kd, "keyBits"), kd_key_bits) ||
		    (kd_key_bits != 128 && kd_key_bits != 256))
		{
			return false;
		}
		params.kd_key_bits = static_cast<int>(kd_key_bits);
		params.kd_hash = parseHashAlgorithm(getXmlAttr(kd, "hashAlgorithm"));
		if (params.kd_hash == HASH_UNKNOWN ||
		    !parseStrictU32(getXmlAttr(kd, "hashSize"), kd_hash_sz) ||
		    kd_hash_sz != static_cast<uint32_t>(params.kd_hash) ||
		    !parseStrictU32(getXmlAttr(kd, "saltSize"), kd_salt_sz) ||
		    kd_salt_sz < 1 || kd_salt_sz > 64 ||
		    !base64Decode(getXmlAttr(kd, "saltValue"), params.kd_salt) ||
		    params.kd_salt.size() != static_cast<size_t>(kd_salt_sz))
		{
			return false;
		}

		for (size_t i = 0; i < ek_nodes.size(); ++i)
		{
			AgileEncryptedKeyParams ek;
			if (parseAgileEncryptedKeyNode(ek_nodes[i], params.kd_key_bits, ek))
				params.keys.push_back(ek);
		}
		return !params.keys.empty();
	}

	bool verifyAgilePassword(const AgileParams& params,
	                         const char* password,
	                         unsigned char out_package_key[32])
	{
		for (size_t k = 0; k < params.keys.size(); ++k)
		{
			const AgileEncryptedKeyParams& ek = params.keys[k];
			const size_t hash_len = static_cast<size_t>(ek.hash_kind);
			const size_t ek_key_bytes = static_cast<size_t>(ek.key_bits / 8);
			const size_t kd_key_bytes = static_cast<size_t>(params.kd_key_bits / 8);

			unsigned char h_in[64 + 128];
			memcpy(h_in, ek.salt.data(), ek.salt.size());
			const size_t pwd_len = asciiToUtf16LE(password, h_in + ek.salt.size(), 128);

			unsigned char h_n[64];
			computeHash(ek.hash_kind, h_in, ek.salt.size() + pwd_len, h_n);

			unsigned char iter_in[68];
			for (uint32_t i = 0; i < ek.spin_count; ++i)
			{
				writeU32LE(i, iter_in);
				memcpy(iter_in + 4, h_n, hash_len);
				computeHash(ek.hash_kind, iter_in, 4 + hash_len, h_n);
			}

			static const unsigned char kBlockVerInput[8] = {0xFE, 0xA7, 0xD2, 0x76, 0x3B, 0x4B, 0x9E, 0x79};
			static const unsigned char kBlockVerValue[8] = {0xD7, 0xAA, 0x0F, 0x6D, 0x30, 0x61, 0x34, 0x4E};
			static const unsigned char kBlockKeyValue[8] = {0x14, 0x6E, 0x0B, 0xE7, 0xAB, 0xAC, 0xD0, 0xD6};

			unsigned char iv_salt[16];
			deriveAgileIV(ek.hash_kind, ek.salt, NULL, 0, iv_salt);

			unsigned char key_in[32];
			unsigned char key_val[32];
			deriveAgileBlockKey(ek.hash_kind, h_n, kBlockVerInput, ek_key_bytes, key_in);
			deriveAgileBlockKey(ek.hash_kind, h_n, kBlockVerValue, ek_key_bytes, key_val);

			unsigned char dec_ver_in[64];
			unsigned char dec_ver_val[64];
			if (!poppler_aes_cbc_decrypt(key_in, ek.key_bits, iv_salt,
			                             ek.enc_ver_input.data(), ek.enc_ver_input.size(), dec_ver_in) ||
			    !poppler_aes_cbc_decrypt(key_val, ek.key_bits, iv_salt,
			                             ek.enc_ver_value.data(), ek.enc_ver_value.size(), dec_ver_val))
			{
				continue;
			}
			unsigned char actual_hash[64];
			const size_t ver_input_bytes = std::min(ek.enc_ver_input.size(), ek.salt_size);
			computeHash(ek.hash_kind, dec_ver_in, ver_input_bytes, actual_hash);
			if (memcmp(actual_hash, dec_ver_val, hash_len) != 0)
				continue;

			unsigned char key_pkg[32];
			unsigned char package_key[64];
			deriveAgileBlockKey(ek.hash_kind, h_n, kBlockKeyValue, ek_key_bytes, key_pkg);
			if (!poppler_aes_cbc_decrypt(key_pkg, ek.key_bits, iv_salt,
			                             ek.enc_key_value.data(), ek.enc_key_value.size(), package_key))
			{
				continue;
			}
			memcpy(out_package_key, package_key, kd_key_bytes);
			return true;
		}
		return false;
	}

	bool decryptAgilePackageInPlace(ThreadSafeOLEStorage& storage,
	                                const AgileParams& params,
	                                const unsigned char* package_key,
	                                std::string& out_zip)
	{
		if (!readOleStreamBounded(storage, "EncryptedPackage", kMaxEncryptedPackageBytes + 8, out_zip) ||
		    out_zip.size() < 24)
		{
			out_zip.clear();
			return false;
		}
		const uint64_t pkg_size = readU64LE(reinterpret_cast<const unsigned char*>(out_zip.data()));
		const size_t enc_len = out_zip.size() - 8;
		if (pkg_size < 4 || pkg_size > kMaxEncryptedPackageBytes || pkg_size > enc_len || (enc_len % 16) != 0)
		{
			out_zip.clear();
			return false;
		}

		unsigned char* base = reinterpret_cast<unsigned char*>(&out_zip[0]);
		uint32_t seg = 0;
		for (size_t off = 0; off < enc_len; off += 4096, ++seg)
		{
			size_t seg_len = std::min(size_t(4096), enc_len - off);
			unsigned char seg_bk[4];
			writeU32LE(seg, seg_bk);
			unsigned char seg_iv[16];
			deriveAgileIV(params.kd_hash, params.kd_salt, seg_bk, 4, seg_iv);
			if (!poppler_aes_cbc_decrypt(package_key, params.kd_key_bits, seg_iv,
			                             base + 8 + off, seg_len, base + off))
			{
				out_zip.clear();
				return false;
			}
		}
		out_zip.resize(static_cast<size_t>(pkg_size));
		if (out_zip.size() < 4 || out_zip[0] != 'P' || out_zip[1] != 'K')
		{
			out_zip.clear();
			return false;
		}
		return true;
	}
}

OffCryptoResult try_decrypt_offcrypto_source(const std::string& file_name,
                                             const char* buffer,
                                             size_t buffer_size,
                                             std::string& out_decrypted_zip,
                                             std::ostream* log_stream)
{
	out_decrypted_zip.clear();
	if (!has_ole2_header(file_name, buffer, buffer_size))
		return OFFCRYPTO_NOT_ENCRYPTED;

	std::unique_ptr<ThreadSafeOLEStorage> storage(
		buffer ? new ThreadSafeOLEStorage(buffer, buffer_size) : new ThreadSafeOLEStorage(file_name));
	if (!storage->isValid() || !is_encrypted_offcrypto_ole(*storage))
		return OFFCRYPTO_NOT_ENCRYPTED;

	// Read only EncryptionInfo first so real user-password files fail fast without reading EncryptedPackage.
	std::string enc_info_str;
	if (!readOleStreamBounded(*storage, "EncryptionInfo", kMaxEncryptionInfoBytes, enc_info_str) ||
	    enc_info_str.size() < 8)
	{
		return OFFCRYPTO_NEEDS_PASSWORD;
	}

	const unsigned char* enc_info = reinterpret_cast<const unsigned char*>(enc_info_str.data());
	const size_t enc_info_len = enc_info_str.size();
	const U16 v_major = readU16LE(enc_info);
	const U16 v_minor = readU16LE(enc_info + 2);
	static const char* const kDefaultPassword = "VelvetSweatshop";

	if ((v_major == 2 || v_major == 3 || v_major == 4) && v_minor == 2)
	{
		StandardHeaderInfo std_info;
		if (parseStandardHeader(enc_info, enc_info_len, std_info))
		{
			unsigned char key[32];
			if (verifyStandardPassword(std_info, kDefaultPassword, key) &&
			    decryptStandardPackageInPlace(*storage, std_info.aes_bits, key, out_decrypted_zip))
			{
				if (log_stream)
					*log_stream << "Decrypted MS-OFFCRYPTO Standard package with built-in default password.\n";
				return OFFCRYPTO_DECRYPTED;
			}
		}
	}
	else if (v_major == 4 && v_minor == 4)
	{
		AgileParams params;
		if (parseAgileParams(enc_info, enc_info_len, params))
		{
			unsigned char pkg_key[32];
			if (verifyAgilePassword(params, kDefaultPassword, pkg_key) &&
			    decryptAgilePackageInPlace(*storage, params, pkg_key, out_decrypted_zip))
			{
				if (log_stream)
					*log_stream << "Decrypted MS-OFFCRYPTO Agile package with built-in default password.\n";
				return OFFCRYPTO_DECRYPTED;
			}
		}
	}

	return OFFCRYPTO_NEEDS_PASSWORD;
}
