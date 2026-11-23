#ifndef DOCTOTEXT_POPPLER_DECRYPT_H
#define DOCTOTEXT_POPPLER_DECRYPT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum PopplerCryptAlgo
{
	POPPLER_CRYPT_NONE = 0,
	POPPLER_CRYPT_RC4 = 1,
	POPPLER_CRYPT_AES128 = 2,
	POPPLER_CRYPT_AES256 = 3
};

struct PopplerPdfEncryptParams
{
	int v;                // /V (1, 2, 4, 5)
	int r;                // /R (2, 3, 4, 5, 6)
	int key_length_bytes; // key length in bytes (e.g. 5..16 for R2-4, 32 for R5-6)
	int permissions;      // /P (32-bit signed integer)
	int encrypt_metadata; // /EncryptMetadata (1 = true, 0 = false)
	const char* o_val;    // /O raw bytes
	size_t o_len;
	const char* u_val;    // /U raw bytes
	size_t u_len;
	const char* oe_val;   // /OE raw bytes (R=5,6)
	size_t oe_len;
	const char* ue_val;   // /UE raw bytes (R=5,6)
	size_t ue_len;
	const char* file_id;  // first element of /ID array (raw bytes)
	size_t file_id_len;
};

// Authenticates the empty user password ("") against /Encrypt parameters and
// derives the file encryption key. Returns 1 if the document opens with "".
int poppler_decrypt_make_empty_user_key(const struct PopplerPdfEncryptParams* params,
                                        unsigned char* file_key_out,
                                        int* file_key_len_out);

// Decrypts a raw PDF string or stream buffer belonging to indirect object (obj_num, gen_num).
// out_data must have capacity >= in_len; *out_len receives the actual decrypted byte count.
// Returns 1 on success, 0 on error.
int poppler_decrypt_buffer(const unsigned char* file_key,
                           int file_key_len,
                           enum PopplerCryptAlgo algo,
                           unsigned long obj_num,
                           unsigned long gen_num,
                           const unsigned char* in_data,
                           size_t in_len,
                           unsigned char* out_data,
                           size_t* out_len);

// Standalone cryptographic primitives for MS-OFFCRYPTO decryption.
void poppler_sha1(const unsigned char* data, size_t len, unsigned char out20[20]);
void poppler_sha256(const unsigned char* data, size_t len, unsigned char out32[32]);
void poppler_sha384(const unsigned char* data, size_t len, unsigned char out48[48]);
void poppler_sha512(const unsigned char* data, size_t len, unsigned char out64[64]);

// Raw zero-padding AES-128/256 ECB/CBC decryption (key_bits must be 128 or 256, len % 16 == 0).
// Returns 1 on success, 0 on invalid arguments.
int poppler_aes_ecb_decrypt(const unsigned char* key,
                            int key_bits,
                            const unsigned char* in_data,
                            size_t len,
                            unsigned char* out_data);

int poppler_aes_cbc_decrypt(const unsigned char* key,
                            int key_bits,
                            const unsigned char iv[16],
                            const unsigned char* in_data,
                            size_t len,
                            unsigned char* out_data);

#ifdef __cplusplus
}
#endif

#endif
