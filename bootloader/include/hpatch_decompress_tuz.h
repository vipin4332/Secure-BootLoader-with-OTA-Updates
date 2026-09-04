/**
 * @file    hpatch_decompress_tuz.h
 * @brief   HPatchLite tinyuz decompressor glue (subset of HPatchLite decompresser_demo.h).
 */
#ifndef HPATCH_DECOMPRESS_TUZ_H
#define HPATCH_DECOMPRESS_TUZ_H

#include "hpatch_lite_types.h"
#include "tuz_dec.h"

static inline hpi_size_t hpatch_tuz_reserved_mem_size(
    hpi_TInputStreamHandle codeStream, hpi_TInputStream_read readCode)
{
    const tuz_size_t dictSize = tuz_TStream_read_dict_size(codeStream, readCode);
    if (((tuz_size_t)(dictSize - 1)) >= tuz_kMaxOfDictSize) {
        return 0;
    }
    return (hpi_size_t)dictSize;
}

static inline hpi_BOOL hpatch_tuz_stream_decompress(hpi_TInputStreamHandle diffStream,
                                                    hpi_byte *out_part_data,
                                                    hpi_size_t *data_size)
{
    return (hpi_BOOL)(tuz_STREAM_END
                      >= tuz_TStream_decompress_partial((tuz_TStream *)diffStream,
                                                        out_part_data, data_size));
}

#endif /* HPATCH_DECOMPRESS_TUZ_H */
