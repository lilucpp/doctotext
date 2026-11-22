//========================================================================
//
// ccitt_decoder.cpp
//
// Standalone CCITT Fax (Group 3 / Group 4) decoder for DocToText.
// Derived from Xpdf / Poppler CCITTFaxStream (Copyright 1996-2003 Glyph & Cog, LLC,
// Modified under Poppler project - GPL version 2 or later).
//
//========================================================================

#include "poppler_ccitt.h"
#include "Stream-CCITT.h"

#include <climits>
#include <cstdio>
#include <vector>

namespace poppler_ccitt
{
namespace
{
	class CCITTFaxDecoder
	{
		public:
			CCITTFaxDecoder(const std::vector<unsigned char>& in_bytes, const CCITTFaxParams& params)
				: m_data(in_bytes),
				  m_pos(0),
				  m_encoding(params.encoding),
				  m_end_of_line(params.end_of_line),
				  m_byte_align(params.byte_align),
				  m_columns(params.columns),
				  m_rows(params.rows),
				  m_end_of_block(params.end_of_block),
				  m_black(params.black_is_1),
				  m_eof(false),
				  m_err(false),
				  m_next_line_2d(false),
				  m_row(0),
				  m_input_buf(0),
				  m_input_bits(0),
				  m_a0i(0),
				  m_output_bits(0),
				  m_buf(EOF)
			{
				if (m_columns < 1)
					m_columns = 1;
				else if (m_columns > 65536)
					m_columns = 65536;

				m_coding_line.assign(m_columns + 1, m_columns);
				m_ref_line.assign(m_columns + 2, m_columns);
				m_next_line_2d = (m_encoding < 0);
			}

			bool decode(std::vector<unsigned char>& out_bytes)
			{
				out_bytes.clear();
				if (m_data.empty())
					return false;

				// Skip any initial zero bits and end-of-line marker, and read 2D tag if encoding > 0
				short code1;
				while ((code1 = lookBits(12)) == 0)
					eatBits(1);
				if (code1 == 0x001)
				{
					eatBits(12);
					m_end_of_line = true;
				}
				if (m_encoding > 0)
				{
					m_next_line_2d = !lookBits(1);
					eatBits(1);
				}

				size_t stride = (size_t)(m_columns + 7) / 8;
				if (m_rows > 0)
					out_bytes.reserve(stride * (size_t)m_rows);

				while (true)
				{
					if (m_rows > 0 && out_bytes.size() >= stride * (size_t)m_rows)
						break;
					int ch = nextByte();
					if (ch == EOF)
						break;
					out_bytes.push_back(static_cast<unsigned char>(ch));
				}

				return !out_bytes.empty();
			}

		private:
			const std::vector<unsigned char>& m_data;
			size_t m_pos;
			int m_encoding;
			bool m_end_of_line;
			bool m_byte_align;
			int m_columns;
			int m_rows;
			bool m_end_of_block;
			bool m_black;
			bool m_eof;
			bool m_err;
			bool m_next_line_2d;
			int m_row;
			unsigned int m_input_buf;
			int m_input_bits;
			std::vector<int> m_coding_line;
			std::vector<int> m_ref_line;
			int m_a0i;
			int m_output_bits;
			int m_buf;

			int readRawByte()
			{
				if (m_pos >= m_data.size())
					return EOF;
				return static_cast<int>(m_data[m_pos++]);
			}

			void eatBits(int n)
			{
				if ((m_input_bits -= n) < 0)
					m_input_bits = 0;
			}

			int lookBits(int n)
			{
				while (m_input_bits < n)
				{
					int c = readRawByte();
					if (c == EOF)
					{
						if (m_input_bits == 0)
							return EOF;
						return static_cast<int>((m_input_buf << (n - m_input_bits)) & (0xffffffffu >> (32 - n)));
					}
					m_input_buf = (m_input_buf << 8) + static_cast<unsigned int>(c);
					m_input_bits += 8;
				}
				return static_cast<int>((m_input_buf >> (m_input_bits - n)) & (0xffffffffu >> (32 - n)));
			}

			void addPixels(int a1, int black_pixels)
			{
				if (a1 > m_coding_line[m_a0i])
				{
					if (a1 > m_columns)
					{
						m_err = true;
						a1 = m_columns;
					}
					if ((m_a0i & 1) ^ black_pixels)
						++m_a0i;
					m_coding_line[m_a0i] = a1;
				}
			}

			void addPixelsNeg(int a1, int black_pixels)
			{
				if (a1 > m_coding_line[m_a0i])
				{
					if (a1 > m_columns)
					{
						m_err = true;
						a1 = m_columns;
					}
					if ((m_a0i & 1) ^ black_pixels)
						++m_a0i;
					m_coding_line[m_a0i] = a1;
				}
				else if (a1 < m_coding_line[m_a0i])
				{
					if (a1 < 0)
					{
						m_err = true;
						a1 = m_columns;
					}
					while (m_a0i > 0 && a1 <= m_coding_line[m_a0i - 1])
						--m_a0i;
					m_coding_line[m_a0i] = a1;
				}
			}

			short getTwoDimCode()
			{
				int code = 0;
				const CCITTCode* p;
				if (m_end_of_block)
				{
					if ((code = lookBits(7)) != EOF)
					{
						p = &twoDimTab1[code];
						if (p->bits > 0)
						{
							eatBits(p->bits);
							return p->n;
						}
					}
				}
				else
				{
					for (int n = 1; n <= 7; ++n)
					{
						if ((code = lookBits(n)) == EOF)
							break;
						if (n < 7)
							code <<= (7 - n);
						p = &twoDimTab1[code];
						if (p->bits == n)
						{
							eatBits(n);
							return p->n;
						}
					}
				}
				return EOF;
			}

			short getWhiteCode()
			{
				short code = 0;
				const CCITTCode* p;
				if (m_end_of_block)
				{
					code = lookBits(12);
					if (code == EOF)
						return 1;
					if ((code >> 5) == 0)
						p = &whiteTab1[code];
					else
						p = &whiteTab2[code >> 3];
					if (p->bits > 0)
					{
						eatBits(p->bits);
						return p->n;
					}
				}
				else
				{
					for (int n = 1; n <= 9; ++n)
					{
						code = lookBits(n);
						if (code == EOF)
							return 1;
						if (n < 9)
							code <<= (9 - n);
						p = &whiteTab2[code];
						if (p->bits == n)
						{
							eatBits(n);
							return p->n;
						}
					}
					for (int n = 11; n <= 12; ++n)
					{
						code = lookBits(n);
						if (code == EOF)
							return 1;
						if (n < 12)
							code <<= (12 - n);
						p = &whiteTab1[code];
						if (p->bits == n)
						{
							eatBits(n);
							return p->n;
						}
					}
				}
				eatBits(1);
				return 1;
			}

			short getBlackCode()
			{
				short code = 0;
				const CCITTCode* p;
				if (m_end_of_block)
				{
					code = lookBits(13);
					if (code == EOF)
						return 1;
					if ((code >> 7) == 0)
						p = &blackTab1[code];
					else if ((code >> 9) == 0 && (code >> 7) != 0)
						p = &blackTab2[(code >> 1) - 64];
					else
						p = &blackTab3[code >> 7];
					if (p->bits > 0)
					{
						eatBits(p->bits);
						return p->n;
					}
				}
				else
				{
					for (int n = 2; n <= 6; ++n)
					{
						code = lookBits(n);
						if (code == EOF)
							return 1;
						if (n < 6)
							code <<= (6 - n);
						p = &blackTab3[code];
						if (p->bits == n)
						{
							eatBits(n);
							return p->n;
						}
					}
					for (int n = 7; n <= 12; ++n)
					{
						code = lookBits(n);
						if (code == EOF)
							return 1;
						if (n < 12)
							code <<= (12 - n);
						if (code >= 64)
						{
							p = &blackTab2[code - 64];
							if (p->bits == n)
							{
								eatBits(n);
								return p->n;
							}
						}
					}
					for (int n = 10; n <= 13; ++n)
					{
						code = lookBits(n);
						if (code == EOF)
							return 1;
						if (n < 13)
							code <<= (13 - n);
						p = &blackTab1[code];
						if (p->bits == n)
						{
							eatBits(n);
							return p->n;
						}
					}
				}
				eatBits(1);
				return 1;
			}

			int nextByte()
			{
				if (m_output_bits == 0)
				{
					if (m_eof)
						return EOF;
					if (m_rows > 0 && m_row >= m_rows)
					{
						m_eof = true;
						return EOF;
					}

					m_err = false;
					int code1, code2, code3;
					int b1i, black_pixels, i;

					if (m_next_line_2d)
					{
						for (i = 0; i < m_columns && m_coding_line[i] < m_columns; ++i)
							m_ref_line[i] = m_coding_line[i];
						for (; i < m_columns + 2; ++i)
							m_ref_line[i] = m_columns;
						m_coding_line[0] = 0;
						m_a0i = 0;
						b1i = 0;
						black_pixels = 0;

						while (m_coding_line[m_a0i] < m_columns && !m_err)
						{
							code1 = getTwoDimCode();
							switch (code1)
							{
								case twoDimPass:
									if (b1i + 1 < m_columns + 2)
									{
										addPixels(m_ref_line[b1i + 1], black_pixels);
										if (m_ref_line[b1i + 1] < m_columns)
											b1i += 2;
									}
									break;
								case twoDimHoriz:
									code1 = code2 = 0;
									if (black_pixels)
									{
										do { code1 += code3 = getBlackCode(); } while (code3 >= 64);
										do { code2 += code3 = getWhiteCode(); } while (code3 >= 64);
									}
									else
									{
										do { code1 += code3 = getWhiteCode(); } while (code3 >= 64);
										do { code2 += code3 = getBlackCode(); } while (code3 >= 64);
									}
									addPixels(m_coding_line[m_a0i] + code1, black_pixels);
									if (m_coding_line[m_a0i] < m_columns)
										addPixels(m_coding_line[m_a0i] + code2, black_pixels ^ 1);
									while (m_ref_line[b1i] <= m_coding_line[m_a0i] && m_ref_line[b1i] < m_columns)
									{
										b1i += 2;
										if (b1i > m_columns + 1)
										{
											m_err = true;
											break;
										}
									}
									break;
								case twoDimVertR3:
								case twoDimVertR2:
								case twoDimVertR1:
								case twoDimVert0:
								{
									static const int offsets[] = { 0, 0, 0, 1, 0, 2, 0, 3 };
									int offset = offsets[code1];
									if (b1i > m_columns + 1)
									{
										m_err = true;
										break;
									}
									addPixels(m_ref_line[b1i] + offset, black_pixels);
									black_pixels ^= 1;
									if (m_coding_line[m_a0i] < m_columns)
									{
										++b1i;
										while (m_ref_line[b1i] <= m_coding_line[m_a0i] && m_ref_line[b1i] < m_columns)
										{
											b1i += 2;
											if (b1i > m_columns + 1)
											{
												m_err = true;
												break;
											}
										}
									}
									break;
								}
								case twoDimVertL3:
								case twoDimVertL2:
								case twoDimVertL1:
								{
									int offset = (code1 == twoDimVertL1) ? 1 : (code1 == twoDimVertL2 ? 2 : 3);
									if (b1i > m_columns + 1)
									{
										m_err = true;
										break;
									}
									addPixelsNeg(m_ref_line[b1i] - offset, black_pixels);
									black_pixels ^= 1;
									if (m_coding_line[m_a0i] < m_columns)
									{
										if (b1i > 0)
											--b1i;
										else
											++b1i;
										while (m_ref_line[b1i] <= m_coding_line[m_a0i] && m_ref_line[b1i] < m_columns)
										{
											b1i += 2;
											if (b1i > m_columns + 1)
											{
												m_err = true;
												break;
											}
										}
									}
									break;
								}
								case EOF:
									addPixels(m_columns, 0);
									m_eof = true;
									break;
								default:
									addPixels(m_columns, 0);
									m_err = true;
									break;
							}
						}
					}
					else
					{
						m_coding_line[0] = 0;
						m_a0i = 0;
						black_pixels = 0;
						while (m_coding_line[m_a0i] < m_columns)
						{
							code1 = 0;
							if (black_pixels)
							{
								do { code1 += code3 = getBlackCode(); } while (code3 >= 64);
							}
							else
							{
								do { code1 += code3 = getWhiteCode(); } while (code3 >= 64);
							}
							addPixels(m_coding_line[m_a0i] + code1, black_pixels);
							black_pixels ^= 1;
						}
					}

					bool got_eol = false;
					if (!m_end_of_block && m_rows > 0 && m_row == m_rows - 1)
					{
						m_eof = true;
					}
					else if (m_end_of_line || !m_byte_align)
					{
						code1 = lookBits(12);
						if (m_end_of_line)
						{
							while (code1 != EOF && code1 != 0x001)
							{
								eatBits(1);
								code1 = lookBits(12);
							}
						}
						else
						{
							while (code1 == 0)
							{
								eatBits(1);
								code1 = lookBits(12);
							}
						}
						if (code1 == 0x001)
						{
							eatBits(12);
							got_eol = true;
						}
					}

					if (m_byte_align && !got_eol)
						m_input_bits &= ~7;

					if (lookBits(1) == EOF)
						m_eof = true;

					if (!m_eof && m_encoding > 0)
					{
						m_next_line_2d = !lookBits(1);
						eatBits(1);
					}

					if (m_end_of_block && !m_end_of_line && m_byte_align)
					{
						if (lookBits(24) == 0x001001)
						{
							eatBits(12);
							got_eol = true;
						}
					}
					if (m_end_of_block && got_eol)
					{
						code1 = lookBits(12);
						if (code1 == 0x001)
						{
							eatBits(12);
							if (m_encoding > 0)
							{
								lookBits(1);
								eatBits(1);
							}
							if (m_encoding >= 0)
							{
								for (i = 0; i < 4; ++i)
								{
									eatBits(12);
									if (m_encoding > 0)
									{
										lookBits(1);
										eatBits(1);
									}
								}
							}
							m_eof = true;
						}
					}
					else if (m_err && m_end_of_line)
					{
						while (true)
						{
							code1 = lookBits(13);
							if (code1 == EOF)
							{
								m_eof = true;
								return EOF;
							}
							if ((code1 >> 1) == 0x001)
								break;
							eatBits(1);
						}
						eatBits(12);
						if (m_encoding > 0)
						{
							eatBits(1);
							m_next_line_2d = !(code1 & 1);
						}
					}

					if (m_coding_line[0] > 0)
						m_output_bits = m_coding_line[m_a0i = 0];
					else
						m_output_bits = m_coding_line[m_a0i = 1];

					++m_row;
				}

				if (m_output_bits >= 8)
				{
					m_buf = (m_a0i & 1) ? 0x00 : 0xff;
					m_output_bits -= 8;
					if (m_output_bits == 0 && m_coding_line[m_a0i] < m_columns)
					{
						++m_a0i;
						m_output_bits = m_coding_line[m_a0i] - m_coding_line[m_a0i - 1];
					}
				}
				else
				{
					int bits = 8;
					m_buf = 0;
					do
					{
						if (m_output_bits > bits)
						{
							m_buf <<= bits;
							if (!(m_a0i & 1))
								m_buf |= 0xff >> (8 - bits);
							m_output_bits -= bits;
							bits = 0;
						}
						else
						{
							m_buf <<= m_output_bits;
							if (!(m_a0i & 1))
								m_buf |= 0xff >> (8 - m_output_bits);
							bits -= m_output_bits;
							m_output_bits = 0;
							if (m_coding_line[m_a0i] < m_columns)
							{
								++m_a0i;
								if (m_a0i > m_columns)
								{
									m_err = true;
									break;
								}
								m_output_bits = m_coding_line[m_a0i] - m_coding_line[m_a0i - 1];
							}
							else if (bits > 0)
							{
								m_buf <<= bits;
								bits = 0;
							}
						}
					} while (bits);
				}

				if (m_black)
					m_buf ^= 0xff;
				return m_buf;
			}
	};
} // namespace

bool decodeCCITTFax(const std::vector<unsigned char>& in_bytes,
                    const CCITTFaxParams& params,
                    std::vector<unsigned char>& out_bytes)
{
	CCITTFaxDecoder decoder(in_bytes, params);
	return decoder.decode(out_bytes);
}

} // namespace poppler_ccitt
