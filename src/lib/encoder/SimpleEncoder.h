/* This code is subject to the terms of the Mozilla Public License, v.2.0. http://mozilla.org/MPL/2.0/. */
#pragma once

#include "reed_solomon_stream.h"
#include "bit_file/bitreader.h"
#include "bit_file/bitbuffer.h"
#include "cimb_translator/CimbWriter.h"
#include "cimb_translator/Config.h"
#include "compression/zstd_compressor.h"
#include "fountain/fountain_encoder_stream.h"

#include <opencv2/opencv.hpp>
#include <optional>
#include <string>

#include <iostream>

class SimpleEncoder
{
public:
	SimpleEncoder(int ecc_bytes=-1, unsigned bits_per_symbol=0, int bits_per_color=-1);
	void set_encode_id(uint8_t encode_id); // [0-127] -- the high bit is ignored.

	template <typename STREAM>
	std::optional<cv::Mat> encode_next(STREAM& stream, int canvas_size=0);

	template <typename STREAM>
	fountain_encoder_stream::ptr create_fountain_encoder(STREAM& stream, int compression_level=6);

protected:
	unsigned _eccBytes;
	unsigned _eccBlockSize;
	unsigned _bitsPerSymbol;
	unsigned _bitsPerColor;
	bool _dark;
	unsigned _colorMode;
	uint8_t _encodeId = 0;
};

inline SimpleEncoder::SimpleEncoder(int ecc_bytes, unsigned bits_per_symbol, int bits_per_color)
	: _eccBytes(ecc_bytes >= 0? ecc_bytes : cimbar::Config::ecc_bytes())
	, _eccBlockSize(cimbar::Config::ecc_block_size())
	, _bitsPerSymbol(bits_per_symbol? bits_per_symbol : cimbar::Config::symbol_bits())
	, _bitsPerColor(bits_per_color >= 0? bits_per_color : cimbar::Config::color_bits())
	, _dark(cimbar::Config::dark())
	, _colorMode(cimbar::Config::color_mode())
{
}

inline void SimpleEncoder::set_encode_id(uint8_t encode_id)
{
	_encodeId = encode_id;
}

/* while bits == f.read(buffer, 8192)
 *     encode(bits)
 *
 * char buffer[2000];
 * while f.read(buffer)
 *     bit_buffer bb(buffer)
 *     while bb
 *         bits1 = bb.get(_bitsPerSymbol)
 *         bits2 = bb.get(_bitsPerColor)
 *
 * */

template <typename STREAM>
inline std::optional<cv::Mat> SimpleEncoder::encode_next(STREAM& stream, int canvas_size)
{
	if (!stream.good())
		return std::nullopt;

	unsigned bits_per_op = _bitsPerColor + _bitsPerSymbol;
	CimbWriter writer(_bitsPerSymbol, _bitsPerColor, _dark, _colorMode, canvas_size);

	unsigned numCells = writer.num_cells();
	bitbuffer bb(cimbar::Config::capacity(bits_per_op));

	unsigned bitPos = 0;
	unsigned endBitPos = numCells*bits_per_op;

	int progress = 0;

	reed_solomon_stream rss(stream, _eccBytes, _eccBlockSize);
	bitreader br;
	while (rss.good())
	{
		unsigned bytes = rss.readsome();
		if (bytes == 0)
			break;
		br.assign_new_buffer(rss.buffer(), bytes);

		// reorder. We're encoding the symbol bits and striping them across the whole image
		// then encoding the color bits and striping them in the same way (filling in the gaps)
		if (progress == 0)
			while (!br.empty())
			{
				unsigned bits = br.read(_bitsPerSymbol);
				if (!br.partial())
					bb.write(bits, bitPos, bits_per_op);
				bitPos += bits_per_op;

				if (bitPos >= endBitPos)
				{
					bitPos = 0;
					progress = 1;
					break;
				}
			}

		if (progress == 1)
			while (!br.empty())
			{
				unsigned bits = br.read(_bitsPerColor);
				if (!br.partial())
					bb.write(bits, bitPos, _bitsPerColor);
				bitPos += bits_per_op;

				if (bitPos >= endBitPos)
				{
					bitPos = 0;
					progress = 2;
					break;
				}
			}

		if (progress == 2)
			break;
	}

	// dump whatever we have to image
	for (bitPos = 0; bitPos < endBitPos; bitPos+=bits_per_op)
	{
		unsigned bits = bb.read(bitPos, bits_per_op);
		writer.write(bits);
	}

	std::cout << "is writer done? " << writer.done() << std::endl;

	// return what we've got
	return writer.image();
}

template <typename STREAM>
inline fountain_encoder_stream::ptr SimpleEncoder::create_fountain_encoder(STREAM& stream, int compression_level)
{
	unsigned chunk_size = cimbar::Config::fountain_chunk_size(_eccBytes, _bitsPerColor + _bitsPerSymbol);

	std::stringstream ss;
	if (compression_level <= 0)
		ss << stream.rdbuf();
	else
	{
		cimbar::zstd_compressor<std::stringstream> f;
		if (!f.compress(stream))
			return nullptr;

		// find size of compressed zstd stream, and pad it if necessary.
		size_t compressedSize = f.size();
		if (compressedSize < chunk_size)
			f.pad(chunk_size - compressedSize + 1);
		ss = std::move(f);
	}

	return fountain_encoder_stream::create(ss, chunk_size, _encodeId);
}

