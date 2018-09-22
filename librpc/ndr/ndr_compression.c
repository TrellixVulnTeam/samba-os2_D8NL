/*
   Unix SMB/CIFS implementation.

   libndr compression support

   Copyright (C) Stefan Metzmacher 2005
   Copyright (C) Matthieu Suiche 2008

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "includes.h"
#include "../lib/compression/lzxpress.h"
#include "librpc/ndr/libndr.h"
#include "../librpc/ndr/ndr_compression.h"
#include <zlib.h>

struct ndr_compression_state {
	enum ndr_compression_alg type;
	union {
		struct {
			struct z_stream_s *z;
			uint8_t *dict;
			size_t dict_size;
		} mszip;
	};
};

static voidpf ndr_zlib_alloc(voidpf opaque, uInt items, uInt size)
{
	return talloc_zero_size(opaque, items * size);
}

static void  ndr_zlib_free(voidpf opaque, voidpf address)
{
	talloc_free(address);
}

static enum ndr_err_code ndr_pull_compression_mszip_cab_chunk(struct ndr_pull *ndrpull,
							      struct ndr_push *ndrpush,
							      struct ndr_compression_state *state,
							      ssize_t decompressed_len,
							      ssize_t compressed_len)
{
	DATA_BLOB comp_chunk;
	uint32_t comp_chunk_offset;
	uint32_t comp_chunk_size;
	DATA_BLOB plain_chunk;
	uint32_t plain_chunk_offset;
	uint32_t plain_chunk_size;
	z_stream *z = state->mszip.z;
	int z_ret;

	plain_chunk_size = decompressed_len;

	if (plain_chunk_size > 0x00008000) {
		return ndr_pull_error(ndrpull, NDR_ERR_COMPRESSION,
				      "Bad MSZIP CAB plain chunk size %08X > 0x00008000 (PULL)",
				      plain_chunk_size);
	}


	comp_chunk_size = compressed_len;

	DEBUG(9,("MSZIP CAB plain_chunk_size: %08X (%u) comp_chunk_size: %08X (%u)\n",
		 plain_chunk_size, plain_chunk_size, comp_chunk_size, comp_chunk_size));

	comp_chunk_offset = ndrpull->offset;
	NDR_CHECK(ndr_pull_advance(ndrpull, comp_chunk_size));
	comp_chunk.length = comp_chunk_size;
	comp_chunk.data = ndrpull->data + comp_chunk_offset;

	plain_chunk_offset = ndrpush->offset;
	NDR_CHECK(ndr_push_zero(ndrpush, plain_chunk_size));
	plain_chunk.length = plain_chunk_size;
	plain_chunk.data = ndrpush->data + plain_chunk_offset;

	if (comp_chunk.length < 2) {
		return ndr_pull_error(ndrpull, NDR_ERR_COMPRESSION,
				      "Bad MSZIP CAB comp chunk size %u < 2 (PULL)",
				      (unsigned int)comp_chunk.length);
	}
	/* CK = Chris Kirmse, official Microsoft purloiner */
	if (comp_chunk.data[0] != 'C' ||
	    comp_chunk.data[1] != 'K') {
		return ndr_pull_error(ndrpull, NDR_ERR_COMPRESSION,
				      "Bad MSZIP CAB invalid prefix [%c%c] != [CK]",
				      comp_chunk.data[0], comp_chunk.data[1]);
	}

	/*
	 * This is a MSZIP block. It is actually using the deflate
	 * algorithm which can be decompressed by zlib. zlib will try
	 * to decompress as much as it can in each run. If we provide
	 * all the input and enough room for the uncompressed output,
	 * one call is enough. It will loop over all the sub-blocks
	 * that make up a deflate block.
	 *
	 * See corresponding push function for more info.
	 */

	z->next_in = comp_chunk.data + 2;
	z->avail_in = comp_chunk.length - 2;
	z->next_out = plain_chunk.data;
	z->avail_out = plain_chunk.length;

	/*
	 * Each MSZIP CDATA contains a complete deflate stream
	 * i.e. the stream starts and ends in the CFDATA but the
	 * _dictionnary_ is shared between all CFDATA of a CFFOLDER.
	 *
	 * When decompressing, the initial dictionnary of the first
	 * CDATA is empty. All other CFDATA use the previous CFDATA
	 * uncompressed output as dictionnary.
	 */

	if (state->mszip.dict_size) {
		z_ret = inflateSetDictionary(z, state->mszip.dict, state->mszip.dict_size);
		if (z_ret != Z_OK) {
			return ndr_pull_error(ndrpull, NDR_ERR_COMPRESSION,
					      "zlib inflateSetDictionary error %s (%d) %s (PULL)",
					      zError(z_ret), z_ret, z->msg);
		}
	}

	z_ret = inflate(z, Z_FINISH);
	if (z_ret == Z_OK) {
		/*
		 * Z_OK here means there was no error but the stream
		 * hasn't been fully decompressed because there was
		 * not enough room for the output, which should not
		 * happen
		 */
		return ndr_pull_error(ndrpull, NDR_ERR_COMPRESSION,
				      "zlib inflate error not enough space for output (PULL)");
	}
	if (z_ret != Z_STREAM_END) {
		return ndr_pull_error(ndrpull, NDR_ERR_COMPRESSION,
				      "zlib inflate error %s (%d) %s (PULL)", zError(z_ret), z_ret, z->msg);
	}

	if (z->total_out < plain_chunk.length) {
		return ndr_pull_error(ndrpull, NDR_ERR_COMPRESSION,
				      "zlib uncompressed output is smaller than expected (%lu < %zu) (PULL)",
				      z->total_out, plain_chunk.length);
	}

	/*
	 * Keep a copy of the output to set as dictionnary for the
	 * next decompression call.
	 *
	 * The input pointer seems to be still valid between calls, so
	 * we can just store that instead of copying the memory over
	 * the dict temp buffer.
	 */
	state->mszip.dict = plain_chunk.data;
	state->mszip.dict_size = plain_chunk.length;

	z_ret = inflateReset(z);
	if (z_ret != Z_OK) {
		return ndr_pull_error(ndrpull, NDR_ERR_COMPRESSION,
				      "zlib inflateReset error %s (%d) %s (PULL)",
				      zError(z_ret), z_ret, z->msg);
	}

	return NDR_ERR_SUCCESS;
}

static enum ndr_err_code ndr_push_compression_mszip_cab_chunk(struct ndr_push *ndrpush,
							      struct ndr_pull *ndrpull,
							      struct ndr_compression_state *state)
{
	DATA_BLOB comp_chunk;
	uint32_t comp_chunk_size;
	DATA_BLOB plain_chunk;
	uint32_t plain_chunk_size;
	uint32_t plain_chunk_offset;
	uint32_t max_plain_size = 0x00008000;
	/*
	 * The maximum compressed size of each MSZIP block is 32k + 12 bytes
	 * header size.
	 */
	uint32_t max_comp_size = 0x00008000 + 12;
	int z_ret;
	z_stream *z;

	if (ndrpull->data_size <= ndrpull->offset) {
		return ndr_push_error(ndrpush, NDR_ERR_COMPRESSION,
				      "strange NDR pull size and offset (integer overflow?)");

	}

	plain_chunk_size = MIN(max_plain_size, ndrpull->data_size - ndrpull->offset);
	plain_chunk_offset = ndrpull->offset;
	NDR_CHECK(ndr_pull_advance(ndrpull, plain_chunk_size));

	plain_chunk.data = ndrpull->data + plain_chunk_offset;
	plain_chunk.length = plain_chunk_size;

	NDR_CHECK(ndr_push_expand(ndrpush, max_comp_size));

	comp_chunk.data = ndrpush->data + ndrpush->offset;
	comp_chunk.length = max_comp_size;

	/* CK = Chris Kirmse, official Microsoft purloiner */
	comp_chunk.data[0] = 'C';
	comp_chunk.data[1] = 'K';

	z = state->mszip.z;
	z->next_in	= plain_chunk.data;
	z->avail_in	= plain_chunk.length;
	z->total_in	= 0;

	z->next_out	= comp_chunk.data + 2;
	z->avail_out	= comp_chunk.length;
	z->total_out	= 0;

	/*
	 * See pull function for explanations of the MSZIP format.
	 *
	 * The CFDATA block contains a full deflate stream. Each stream
	 * uses the uncompressed input of the previous CFDATA in the
	 * same CFFOLDER as a dictionnary for the compression.
	 */

	if (state->mszip.dict_size) {
		z_ret = deflateSetDictionary(z, state->mszip.dict, state->mszip.dict_size);
		if (z_ret != Z_OK) {
			return ndr_pull_error(ndrpull, NDR_ERR_COMPRESSION,
					      "zlib deflateSetDictionary error %s (%d) %s (PUSH)",
					      zError(z_ret), z_ret, z->msg);
		}
	}

	/*
	 * Z_FINISH should make deflate process all of the input in
	 * one call. If the stream is not finished there was an error
	 * e.g. not enough room to store the compressed output.
	 */
	z_ret = deflate(z, Z_FINISH);
	if (z_ret != Z_STREAM_END) {
		return ndr_push_error(ndrpush, NDR_ERR_COMPRESSION,
				      "zlib deflate error %s (%d) %s (PUSH)",
				      zError(z_ret), z_ret, z->msg);
	}

	if (z->avail_in) {
		return ndr_push_error(ndrpush, NDR_ERR_COMPRESSION,
				      "MSZIP not all avail_in[%u] bytes consumed (PUSH)",
				      z->avail_in);
	}

	comp_chunk_size = 2 + z->total_out;
	if (comp_chunk_size < z->total_out) {
		return ndr_push_error(ndrpush, NDR_ERR_COMPRESSION,
				      "strange NDR push compressed size (integer overflow?)");
	}

	z_ret = deflateReset(z);
	if (z_ret != Z_OK) {
		return ndr_pull_error(ndrpull, NDR_ERR_COMPRESSION,
				      "zlib deflateReset error %s (%d) %s (PUSH)",
				      zError(z_ret), z_ret, z->msg);
	}

	if (plain_chunk.length > talloc_array_length(state->mszip.dict)) {
		return ndr_pull_error(ndrpull, NDR_ERR_COMPRESSION,
				      "zlib dict buffer is too big (PUSH)");
	}

	/*
	 * Keep a copy of the input to set as dictionnary for the next
	 * compression call.
	 *
	 * Ideally we would just store the input pointer and length
	 * without copying but the memory gets invalidated between the
	 * calls, so we just copy to a dedicated buffer we now is
	 * still going to been valid for the lifetime of the
	 * compressions state object.
	 */
	memcpy(state->mszip.dict, plain_chunk.data, plain_chunk.length);
	state->mszip.dict_size = plain_chunk.length;

	DEBUG(9,("MSZIP comp plain_chunk_size: %08X (%u) comp_chunk_size: %08X (%u)\n",
		 (unsigned int)plain_chunk.length,
		 (unsigned int)plain_chunk.length,
		 comp_chunk_size, comp_chunk_size));

	ndrpush->offset += comp_chunk_size;
	return NDR_ERR_SUCCESS;
}


static enum ndr_err_code ndr_pull_compression_mszip_chunk(struct ndr_pull *ndrpull,
						 struct ndr_push *ndrpush,
						 z_stream *z,
						 bool *last)
{
	DATA_BLOB comp_chunk;
	uint32_t comp_chunk_offset;
	uint32_t comp_chunk_size;
	DATA_BLOB plain_chunk;
	uint32_t plain_chunk_offset;
	uint32_t plain_chunk_size;
	int z_ret;

	NDR_CHECK(ndr_pull_uint32(ndrpull, NDR_SCALARS, &plain_chunk_size));
	if (plain_chunk_size > 0x00008000) {
		return ndr_pull_error(ndrpull, NDR_ERR_COMPRESSION, "Bad MSZIP plain chunk size %08X > 0x00008000 (PULL)",
				      plain_chunk_size);
	}

	NDR_CHECK(ndr_pull_uint32(ndrpull, NDR_SCALARS, &comp_chunk_size));

	DEBUG(9,("MSZIP plain_chunk_size: %08X (%u) comp_chunk_size: %08X (%u)\n",
		 plain_chunk_size, plain_chunk_size, comp_chunk_size, comp_chunk_size));

	comp_chunk_offset = ndrpull->offset;
	NDR_CHECK(ndr_pull_advance(ndrpull, comp_chunk_size));
	comp_chunk.length = comp_chunk_size;
	comp_chunk.data = ndrpull->data + comp_chunk_offset;

	plain_chunk_offset = ndrpush->offset;
	NDR_CHECK(ndr_push_zero(ndrpush, plain_chunk_size));
	plain_chunk.length = plain_chunk_size;
	plain_chunk.data = ndrpush->data + plain_chunk_offset;

	if (comp_chunk.length < 2) {
		return ndr_pull_error(ndrpull, NDR_ERR_COMPRESSION,
				      "Bad MSZIP comp chunk size %u < 2 (PULL)",
				      (unsigned int)comp_chunk.length);
	}
	/* CK = Chris Kirmse, official Microsoft purloiner */
	if (comp_chunk.data[0] != 'C' ||
	    comp_chunk.data[1] != 'K') {
		return ndr_pull_error(ndrpull, NDR_ERR_COMPRESSION,
				      "Bad MSZIP invalid prefix [%c%c] != [CK]",
				      comp_chunk.data[0], comp_chunk.data[1]);
	}

	z->next_in	= comp_chunk.data + 2;
	z->avail_in	= comp_chunk.length -2;
	z->total_in	= 0;

	z->next_out	= plain_chunk.data;
	z->avail_out	= plain_chunk.length;
	z->total_out	= 0;

	if (!z->opaque) {
		/* the first time we need to intialize completely */
		z->zalloc	= ndr_zlib_alloc;
		z->zfree	= ndr_zlib_free;
		z->opaque	= ndrpull;

		z_ret = inflateInit2(z, -MAX_WBITS);
		if (z_ret != Z_OK) {
			return ndr_pull_error(ndrpull, NDR_ERR_COMPRESSION,
					      "Bad inflateInit2 error %s(%d) (PULL)",
					      zError(z_ret), z_ret);

		}
	}

	/* call inflate untill we get Z_STREAM_END or an error */
	while (true) {
		z_ret = inflate(z, Z_BLOCK);
		if (z_ret != Z_OK) break;
	}

	if (z_ret != Z_STREAM_END) {
		return ndr_pull_error(ndrpull, NDR_ERR_COMPRESSION,
				      "Bad inflate(Z_BLOCK) error %s(%d) (PULL)",
				      zError(z_ret), z_ret);
	}

	if (z->avail_in) {
		return ndr_pull_error(ndrpull, NDR_ERR_COMPRESSION,
				      "MSZIP not all avail_in[%u] bytes consumed (PULL)",
				      z->avail_in);
	}

	if (z->avail_out) {
		return ndr_pull_error(ndrpull, NDR_ERR_COMPRESSION,
				      "MSZIP not all avail_out[%u] bytes consumed (PULL)",
				      z->avail_out);
	}

	if ((plain_chunk_size < 0x00008000) || (ndrpull->offset+4 >= ndrpull->data_size)) {
		/* this is the last chunk */
		*last = true;
	}

	z_ret = inflateReset(z);
	if (z_ret != Z_OK) {
		return ndr_pull_error(ndrpull, NDR_ERR_COMPRESSION,
				      "Bad inflateReset error %s(%d) (PULL)",
				      zError(z_ret), z_ret);
	}

	z_ret = inflateSetDictionary(z, plain_chunk.data, plain_chunk.length);
	if (z_ret != Z_OK) {
		return ndr_pull_error(ndrpull, NDR_ERR_COMPRESSION,
				      "Bad inflateSetDictionary error %s(%d) (PULL)",
				      zError(z_ret), z_ret);
	}

	return NDR_ERR_SUCCESS;
}

static enum ndr_err_code ndr_push_compression_mszip_chunk(struct ndr_push *ndrpush,
							  struct ndr_pull *ndrpull,
							  z_stream *z,
							  bool *last)
{
	DATA_BLOB comp_chunk;
	uint32_t comp_chunk_size;
	uint32_t comp_chunk_size_offset;
	DATA_BLOB plain_chunk;
	uint32_t plain_chunk_size;
	uint32_t plain_chunk_offset;
	uint32_t max_plain_size = 0x00008000;
	/*
	 * The maximum compressed size of each MSZIP block is 32k + 12 bytes
	 * header size.
	 */
	uint32_t max_comp_size = 0x00008000 + 12;
	uint32_t tmp_offset;
	int z_ret;

	plain_chunk_size = MIN(max_plain_size, ndrpull->data_size - ndrpull->offset);
	plain_chunk_offset = ndrpull->offset;
	NDR_CHECK(ndr_pull_advance(ndrpull, plain_chunk_size));

	plain_chunk.data = ndrpull->data + plain_chunk_offset;
	plain_chunk.length = plain_chunk_size;

	if (plain_chunk_size < max_plain_size) {
		*last = true;
	}

	NDR_CHECK(ndr_push_uint32(ndrpush, NDR_SCALARS, plain_chunk_size));
	comp_chunk_size_offset = ndrpush->offset;
	NDR_CHECK(ndr_push_uint32(ndrpush, NDR_SCALARS, 0xFEFEFEFE));

	NDR_CHECK(ndr_push_expand(ndrpush, max_comp_size));

	comp_chunk.data = ndrpush->data + ndrpush->offset;
	comp_chunk.length = max_comp_size;

	/* CK = Chris Kirmse, official Microsoft purloiner */
	comp_chunk.data[0] = 'C';
	comp_chunk.data[1] = 'K';

	z->next_in	= plain_chunk.data;
	z->avail_in	= plain_chunk.length;
	z->total_in	= 0;

	z->next_out	= comp_chunk.data + 2;
	z->avail_out	= comp_chunk.length;
	z->total_out	= 0;

	if (!z->opaque) {
		/* the first time we need to intialize completely */
		z->zalloc	= ndr_zlib_alloc;
		z->zfree	= ndr_zlib_free;
		z->opaque	= ndrpull;

		/* TODO: find how to trigger the same parameters windows uses */
		z_ret = deflateInit2(z,
				     Z_DEFAULT_COMPRESSION,
				     Z_DEFLATED,
				     -MAX_WBITS,
				     8, /* memLevel */
				     Z_DEFAULT_STRATEGY);
		if (z_ret != Z_OK) {
			return ndr_push_error(ndrpush, NDR_ERR_COMPRESSION,
					      "Bad deflateInit2 error %s(%d) (PUSH)",
					      zError(z_ret), z_ret);

		}
	}

	/* call deflate untill we get Z_STREAM_END or an error */
	while (true) {
		z_ret = deflate(z, Z_FINISH);
		if (z_ret != Z_OK) break;
	}
	if (z_ret != Z_STREAM_END) {
		return ndr_push_error(ndrpush, NDR_ERR_COMPRESSION,
				      "Bad delate(Z_BLOCK) error %s(%d) (PUSH)",
				      zError(z_ret), z_ret);
	}

	if (z->avail_in) {
		return ndr_push_error(ndrpush, NDR_ERR_COMPRESSION,
				      "MSZIP not all avail_in[%u] bytes consumed (PUSH)",
				      z->avail_in);
	}

	comp_chunk_size = 2 + z->total_out;

	z_ret = deflateReset(z);
	if (z_ret != Z_OK) {
		return ndr_pull_error(ndrpull, NDR_ERR_COMPRESSION,
				      "Bad deflateReset error %s(%d) (PULL)",
				      zError(z_ret), z_ret);
	}

	z_ret = deflateSetDictionary(z, plain_chunk.data, plain_chunk.length);
	if (z_ret != Z_OK) {
		return ndr_pull_error(ndrpull, NDR_ERR_COMPRESSION,
				      "Bad deflateSetDictionary error %s(%d) (PULL)",
				      zError(z_ret), z_ret);
	}

	tmp_offset = ndrpush->offset;
	ndrpush->offset = comp_chunk_size_offset;
	NDR_CHECK(ndr_push_uint32(ndrpush, NDR_SCALARS, comp_chunk_size));
	ndrpush->offset = tmp_offset;

	DEBUG(9,("MSZIP comp plain_chunk_size: %08X (%u) comp_chunk_size: %08X (%u)\n",
		 (unsigned int)plain_chunk.length,
		 (unsigned int)plain_chunk.length,
		 comp_chunk_size, comp_chunk_size));

	ndrpush->offset += comp_chunk_size;
	return NDR_ERR_SUCCESS;
}

static enum ndr_err_code ndr_pull_compression_xpress_chunk(struct ndr_pull *ndrpull,
						  struct ndr_push *ndrpush,
						  bool *last)
{
	DATA_BLOB comp_chunk;
	DATA_BLOB plain_chunk;
	uint32_t comp_chunk_offset;
	uint32_t plain_chunk_offset;
	uint32_t comp_chunk_size;
	uint32_t plain_chunk_size;
	ssize_t ret;

	NDR_CHECK(ndr_pull_uint32(ndrpull, NDR_SCALARS, &plain_chunk_size));
	if (plain_chunk_size > 0x00010000) {
		return ndr_pull_error(ndrpull, NDR_ERR_COMPRESSION, "Bad XPRESS plain chunk size %08X > 0x00010000 (PULL)",
				      plain_chunk_size);
	}

	NDR_CHECK(ndr_pull_uint32(ndrpull, NDR_SCALARS, &comp_chunk_size));

	comp_chunk_offset = ndrpull->offset;
	NDR_CHECK(ndr_pull_advance(ndrpull, comp_chunk_size));
	comp_chunk.length = comp_chunk_size;
	comp_chunk.data = ndrpull->data + comp_chunk_offset;

	plain_chunk_offset = ndrpush->offset;
	NDR_CHECK(ndr_push_zero(ndrpush, plain_chunk_size));
	plain_chunk.length = plain_chunk_size;
	plain_chunk.data = ndrpush->data + plain_chunk_offset;

	DEBUG(9,("XPRESS plain_chunk_size: %08X (%u) comp_chunk_size: %08X (%u)\n",
		 plain_chunk_size, plain_chunk_size, comp_chunk_size, comp_chunk_size));

	/* Uncompressing the buffer using LZ Xpress algorithm */
	ret = lzxpress_decompress(comp_chunk.data,
				  comp_chunk.length,
				  plain_chunk.data,
				  plain_chunk.length);
	if (ret < 0) {
		return ndr_pull_error(ndrpull, NDR_ERR_COMPRESSION,
				      "XPRESS lzxpress_decompress() returned %d\n",
				      (int)ret);
	}
	plain_chunk.length = ret;

	if ((plain_chunk_size < 0x00010000) || (ndrpull->offset+4 >= ndrpull->data_size)) {
		/* this is the last chunk */
		*last = true;
	}

	return NDR_ERR_SUCCESS;
}

static enum ndr_err_code ndr_push_compression_xpress_chunk(struct ndr_push *ndrpush,
							   struct ndr_pull *ndrpull,
							   bool *last)
{
	DATA_BLOB comp_chunk;
	uint32_t comp_chunk_size_offset;
	DATA_BLOB plain_chunk;
	uint32_t plain_chunk_size;
	uint32_t plain_chunk_offset;
	uint32_t max_plain_size = 0x00010000;
	uint32_t max_comp_size = 0x00020000 + 2; /* TODO: use the correct value here */
	uint32_t tmp_offset;
	ssize_t ret;

	plain_chunk_size = MIN(max_plain_size, ndrpull->data_size - ndrpull->offset);
	plain_chunk_offset = ndrpull->offset;
	NDR_CHECK(ndr_pull_advance(ndrpull, plain_chunk_size));

	plain_chunk.data = ndrpull->data + plain_chunk_offset;
	plain_chunk.length = plain_chunk_size;

	if (plain_chunk_size < max_plain_size) {
		*last = true;
	}

	NDR_CHECK(ndr_push_uint32(ndrpush, NDR_SCALARS, plain_chunk_size));
	comp_chunk_size_offset = ndrpush->offset;
	NDR_CHECK(ndr_push_uint32(ndrpush, NDR_SCALARS, 0xFEFEFEFE));

	NDR_CHECK(ndr_push_expand(ndrpush, max_comp_size));

	comp_chunk.data = ndrpush->data + ndrpush->offset;
	comp_chunk.length = max_comp_size;

	/* Compressing the buffer using LZ Xpress algorithm */
	ret = lzxpress_compress(plain_chunk.data,
				plain_chunk.length,
				comp_chunk.data,
				comp_chunk.length);
	if (ret < 0) {
		return ndr_pull_error(ndrpull, NDR_ERR_COMPRESSION,
				      "XPRESS lzxpress_compress() returned %d\n",
				      (int)ret);
	}
	comp_chunk.length = ret;

	tmp_offset = ndrpush->offset;
	ndrpush->offset = comp_chunk_size_offset;
	NDR_CHECK(ndr_push_uint32(ndrpush, NDR_SCALARS, comp_chunk.length));
	ndrpush->offset = tmp_offset;

	ndrpush->offset += comp_chunk.length;
	return NDR_ERR_SUCCESS;
}

/*
  handle compressed subcontext buffers, which in midl land are user-marshalled, but
  we use magic in pidl to make them easier to cope with
*/
enum ndr_err_code ndr_pull_compression_start(struct ndr_pull *subndr,
				    struct ndr_pull **_comndr,
				    enum ndr_compression_alg compression_alg,
				    ssize_t decompressed_len,
				    ssize_t compressed_len)
{
	struct ndr_push *ndrpush;
	struct ndr_pull *comndr;
	DATA_BLOB uncompressed;
	bool last = false;
	z_stream z;

	ndrpush = ndr_push_init_ctx(subndr);
	NDR_ERR_HAVE_NO_MEMORY(ndrpush);

	switch (compression_alg) {
	case NDR_COMPRESSION_MSZIP_CAB:
		NDR_CHECK(ndr_pull_compression_mszip_cab_chunk(subndr, ndrpush,
							       subndr->cstate,
							       decompressed_len,
							       compressed_len));
		break;
	case NDR_COMPRESSION_MSZIP:
		ZERO_STRUCT(z);
		while (!last) {
			NDR_CHECK(ndr_pull_compression_mszip_chunk(subndr, ndrpush, &z, &last));
		}
		break;

	case NDR_COMPRESSION_XPRESS:
		while (!last) {
			NDR_CHECK(ndr_pull_compression_xpress_chunk(subndr, ndrpush, &last));
		}
		break;

	default:
		return ndr_pull_error(subndr, NDR_ERR_COMPRESSION, "Bad compression algorithm %d (PULL)",
				      compression_alg);
	}

	uncompressed = ndr_push_blob(ndrpush);
	if (uncompressed.length != decompressed_len) {
		return ndr_pull_error(subndr, NDR_ERR_COMPRESSION,
				      "Bad uncompressed_len [%u] != [%u](0x%08X) (PULL)",
				      (int)uncompressed.length,
				      (int)decompressed_len,
				      (int)decompressed_len);
	}

	comndr = talloc_zero(subndr, struct ndr_pull);
	NDR_ERR_HAVE_NO_MEMORY(comndr);
	comndr->flags		= subndr->flags;
	comndr->current_mem_ctx	= subndr->current_mem_ctx;

	comndr->data		= uncompressed.data;
	comndr->data_size	= uncompressed.length;
	comndr->offset		= 0;

	*_comndr = comndr;
	return NDR_ERR_SUCCESS;
}

enum ndr_err_code ndr_pull_compression_end(struct ndr_pull *subndr,
				  struct ndr_pull *comndr,
				  enum ndr_compression_alg compression_alg,
				  ssize_t decompressed_len)
{
	return NDR_ERR_SUCCESS;
}

/*
  push a compressed subcontext
*/
enum ndr_err_code ndr_push_compression_start(struct ndr_push *subndr,
				    struct ndr_push **_uncomndr,
				    enum ndr_compression_alg compression_alg,
				    ssize_t decompressed_len)
{
	struct ndr_push *uncomndr;

	switch (compression_alg) {
	case NDR_COMPRESSION_MSZIP_CAB:
	case NDR_COMPRESSION_MSZIP:
	case NDR_COMPRESSION_XPRESS:
		break;
	default:
		return ndr_push_error(subndr, NDR_ERR_COMPRESSION,
				      "Bad compression algorithm %d (PUSH)",
				      compression_alg);
	}

	uncomndr = ndr_push_init_ctx(subndr);
	NDR_ERR_HAVE_NO_MEMORY(uncomndr);
	uncomndr->flags	= subndr->flags;

	*_uncomndr = uncomndr;
	return NDR_ERR_SUCCESS;
}

/*
  push a compressed subcontext
*/
enum ndr_err_code ndr_push_compression_end(struct ndr_push *subndr,
				  struct ndr_push *uncomndr,
				  enum ndr_compression_alg compression_alg,
				  ssize_t decompressed_len)
{
	struct ndr_pull *ndrpull;
	bool last = false;
	z_stream z;

	ndrpull = talloc_zero(uncomndr, struct ndr_pull);
	NDR_ERR_HAVE_NO_MEMORY(ndrpull);
	ndrpull->flags		= uncomndr->flags;
	ndrpull->data		= uncomndr->data;
	ndrpull->data_size	= uncomndr->offset;
	ndrpull->offset		= 0;

	switch (compression_alg) {
	case NDR_COMPRESSION_MSZIP_CAB:
		NDR_CHECK(ndr_push_compression_mszip_cab_chunk(subndr, ndrpull, subndr->cstate));
		break;

	case NDR_COMPRESSION_MSZIP:
		ZERO_STRUCT(z);
		while (!last) {
			NDR_CHECK(ndr_push_compression_mszip_chunk(subndr, ndrpull, &z, &last));
		}
		break;

	case NDR_COMPRESSION_XPRESS:
		while (!last) {
			NDR_CHECK(ndr_push_compression_xpress_chunk(subndr, ndrpull, &last));
		}
		break;

	default:
		return ndr_push_error(subndr, NDR_ERR_COMPRESSION, "Bad compression algorithm %d (PUSH)",
				      compression_alg);
	}

	talloc_free(uncomndr);
	return NDR_ERR_SUCCESS;
}

static enum ndr_err_code generic_mszip_init(TALLOC_CTX *mem_ctx,
					    struct ndr_compression_state *state)
{
	z_stream *z = talloc_zero(mem_ctx, z_stream);
	NDR_ERR_HAVE_NO_MEMORY(z);

	z->zalloc = ndr_zlib_alloc;
	z->zfree  = ndr_zlib_free;
	z->opaque = mem_ctx;

	state->mszip.z = z;
	state->mszip.dict_size = 0;
	/* pre-alloc dictionnary */
	state->mszip.dict = talloc_array(mem_ctx, uint8_t, 0x8000);
	NDR_ERR_HAVE_NO_MEMORY(state->mszip.dict);

	return NDR_ERR_SUCCESS;
}

static void generic_mszip_free(struct ndr_compression_state *state)
{
	if (state == NULL) {
		return;
	}

	TALLOC_FREE(state->mszip.z);
	TALLOC_FREE(state->mszip.dict);
}


enum ndr_err_code ndr_pull_compression_state_init(struct ndr_pull *ndr,
						  enum ndr_compression_alg compression_alg,
						  struct ndr_compression_state **state)
{
	struct ndr_compression_state *s;
	int z_ret;

	s = talloc_zero(ndr, struct ndr_compression_state);
	NDR_ERR_HAVE_NO_MEMORY(s);
	s->type = compression_alg;

	switch (compression_alg) {
	case NDR_COMPRESSION_MSZIP:
	case NDR_COMPRESSION_XPRESS:
		break;
	case NDR_COMPRESSION_MSZIP_CAB:
		NDR_CHECK(generic_mszip_init(ndr, s));
		z_ret = inflateInit2(s->mszip.z, -MAX_WBITS);
		if (z_ret != Z_OK) {
			return ndr_pull_error(ndr, NDR_ERR_COMPRESSION,
					      "zlib inflateinit2 error %s (%d) %s (PULL)",
					      zError(z_ret), z_ret, s->mszip.z->msg);
		}
		break;
	default:
		return ndr_pull_error(ndr, NDR_ERR_COMPRESSION,
				      "Bad compression algorithm %d (PULL)",
				      compression_alg);
		break;
	}

	*state = s;

	return NDR_ERR_SUCCESS;
}

void ndr_pull_compression_state_free(struct ndr_compression_state *state)
{
	if (state == NULL) {
		return;
	}

	switch (state->type) {
	case NDR_COMPRESSION_MSZIP:
	case NDR_COMPRESSION_XPRESS:
		break;
	case NDR_COMPRESSION_MSZIP_CAB:
		generic_mszip_free(state);
		break;
	default:
		break;
	}
	TALLOC_FREE(state);
}

enum ndr_err_code ndr_push_compression_state_init(struct ndr_push *ndr,
						  enum ndr_compression_alg compression_alg,
						  struct ndr_compression_state **state)
{
	struct ndr_compression_state *s;
	int z_ret;

	s = talloc_zero(ndr, struct ndr_compression_state);
	NDR_ERR_HAVE_NO_MEMORY(s);
	s->type = compression_alg;

	switch (compression_alg) {
	case NDR_COMPRESSION_XPRESS:
	case NDR_COMPRESSION_MSZIP:
		break;
	case NDR_COMPRESSION_MSZIP_CAB:
		NDR_CHECK(generic_mszip_init(ndr, s));
		z_ret = deflateInit2(s->mszip.z,
				     Z_DEFAULT_COMPRESSION,
				     Z_DEFLATED,
				     -MAX_WBITS,
				     8, /* memLevel */
				     Z_DEFAULT_STRATEGY);
		if (z_ret != Z_OK) {
			return ndr_push_error(ndr, NDR_ERR_COMPRESSION,
					      "zlib inflateinit2 error %s (%d) %s (PUSH)",
					      zError(z_ret), z_ret, s->mszip.z->msg);
		}
		break;
	default:
		return ndr_push_error(ndr, NDR_ERR_COMPRESSION,
				      "Bad compression algorithm %d (PUSH)",
				      compression_alg);
		break;
	}

	*state = s;

	return NDR_ERR_SUCCESS;
}

void ndr_push_compression_state_free(struct ndr_compression_state *state)
{
	if (state == NULL) {
		return;
	}

	switch (state->type) {
	case NDR_COMPRESSION_MSZIP:
	case NDR_COMPRESSION_XPRESS:
		break;
	case NDR_COMPRESSION_MSZIP_CAB:
		generic_mszip_free(state);
		break;
	default:
		break;
	}
	TALLOC_FREE(state);
}
