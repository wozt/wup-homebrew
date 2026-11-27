/* freetype's sfnt module references BrotliDecoderDecompress for WOFF2 fonts.
   This installer only embeds plain TTF/BDF fonts, so WOFF2 support is dead
   weight (~300 KiB of decoder + dictionary). This stub makes the reference
   resolve while reporting every WOFF2 as undecodable. */
#include <stddef.h>
#include <stdint.h>

typedef enum {
    BROTLI_DECODER_RESULT_ERROR = 0,
    BROTLI_DECODER_RESULT_SUCCESS = 1,
    BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT = 2,
    BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT = 3
} BrotliDecoderResult;

BrotliDecoderResult BrotliDecoderDecompress(size_t encoded_size,
                                            const uint8_t *encoded_buffer,
                                            size_t *decoded_size,
                                            uint8_t *decoded_buffer)
{
    (void)encoded_size;
    (void)encoded_buffer;
    (void)decoded_size;
    (void)decoded_buffer;
    return BROTLI_DECODER_RESULT_ERROR;
}
