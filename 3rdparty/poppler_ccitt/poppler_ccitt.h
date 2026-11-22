//========================================================================
//
// ccitt_decoder.h
//
// Standalone CCITT Fax (Group 3 / Group 4) decoder for DocToText.
// Derived from Xpdf / Poppler CCITTFaxStream.
// Licensed under GNU General Public License version 2 or later.
//
//========================================================================

#ifndef POPPLER_CCITT_H
#define POPPLER_CCITT_H

#include <vector>

namespace poppler_ccitt
{
	struct CCITTFaxParams
	{
		int encoding;         // K: < 0 Group 4 (2D), 0 Group 3 (1D), > 0 Group 3 (mixed 1D/2D)
		bool end_of_line;     // EndOfLine flag
		bool byte_align;      // EncodedByteAlign flag
		int columns;          // Width in pixels (default 1728)
		int rows;             // Expected row count (0 = unknown/until EOF)
		bool end_of_block;    // EndOfBlock / RTC marker flag (default true)
		bool black_is_1;      // BlackIs1 flag (default false)

		CCITTFaxParams()
			: encoding(0),
			  end_of_line(false),
			  byte_align(false),
			  columns(1728),
			  rows(0),
			  end_of_block(true),
			  black_is_1(false)
		{}
	};

	bool decodeCCITTFax(const std::vector<unsigned char>& in_bytes,
	                    const CCITTFaxParams& params,
	                    std::vector<unsigned char>& out_bytes);
}

#endif // POPPLER_CCITT_H
