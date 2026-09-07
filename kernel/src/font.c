/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <font.h>
#include <framebuffer.h>

#define PSF2_HAS_UNICODE_TABLE 0x00000001U
/* Regular UNSCII-16 supplies a few thousand bounded 8x16 mappings. */
#define FONT_MAX_MAPPINGS 4096U
#define FONT_REPLACEMENT_CODEPOINT 0xFFFDU

#define PSF2_MAGIC0 0x72
#define PSF2_MAGIC1 0xB5
#define PSF2_MAGIC2 0x4A
#define PSF2_MAGIC3 0x86

typedef struct {
    u8  magic[4];
    u32 version;
    u32 headerfile;
    u32 flags;
    u32 numglyph;
    u32 bytesperglyph;
    u32 height;
    u32 width;
} psf2_header_t;

typedef struct {
    u32 codepoint;
    u32 glyph_index;
} font_mapping_t;

extern char _binary_kernel_assets_font_psf_start[];
extern char _binary_kernel_assets_font_psf_end[];

static psf2_header_t *font_header = 0;
static const u8 *font_asset_start = 0;
static usize font_asset_size = 0;
static const u8 *glyph_buffer = 0;
static usize glyph_buffer_size = 0;
static font_mapping_t font_mappings[FONT_MAX_MAPPINGS];
static u32 font_mapping_count = 0;
static bool font_unicode_table_valid = false;
static bool font_ready = false;

static bool font_utf8_continuation(u8 value)
{
    return (value & 0xC0U) == 0x80U;
}

static bool font_decode_utf8(const u8 *bytes, usize available,
                             u32 *codepoint, usize *consumed)
{
    u32 value;
    u32 minimum;
    u8 first;
    u8 length;

    if (!bytes || !codepoint || !consumed || available == 0) return false;
    first = bytes[0];
    if (first < 0x80U) {
        *codepoint = first;
        *consumed = 1;
        return true;
    }
    if (first >= 0xC2U && first <= 0xDFU) {
        value = first & 0x1FU;
        minimum = 0x80U;
        length = 2;
    } else if (first >= 0xE0U && first <= 0xEFU) {
        value = first & 0x0FU;
        minimum = 0x800U;
        length = 3;
    } else if (first >= 0xF0U && first <= 0xF4U) {
        value = first & 0x07U;
        minimum = 0x10000U;
        length = 4;
    } else {
        return false;
    }

    if (available < length) return false;
    for (u8 index = 1; index < length; index++) {
        if (!font_utf8_continuation(bytes[index])) return false;
        value = (value << 6) | (bytes[index] & 0x3FU);
    }
    if (value < minimum || value > 0x10FFFFU ||
        (value >= 0xD800U && value <= 0xDFFFU)) return false;

    *codepoint = value;
    *consumed = length;
    return true;
}

static bool font_mapping_add(u32 codepoint, u32 glyph_index)
{
    if (font_mapping_count >= FONT_MAX_MAPPINGS) return false;
    font_mappings[font_mapping_count].codepoint = codepoint;
    font_mappings[font_mapping_count].glyph_index = glyph_index;
    font_mapping_count++;
    return true;
}

static bool font_parse_unicode_table(const u8 *table, usize table_size,
                                     u32 glyph_count)
{
    usize offset = 0;

    font_mapping_count = 0;
    for (u32 glyph = 0; glyph < glyph_count; glyph++) {
        bool glyph_terminated = false;
        while (offset < table_size) {
            u8 value = table[offset];
            u32 codepoint;
            usize consumed;

            offset++;
            if (value == 0xFFU) {
                glyph_terminated = true;
                break;
            }
            if (value == 0xFEU) {
                /* PSF2 sequence mappings describe a glyph sequence rather
                 * than a single scalar.  The terminal is single-cell and
                 * does not use them; skip the sequence through its entry
                 * terminator without treating its bytes as code points. */
                while (offset < table_size && table[offset] != 0xFFU)
                    offset++;
                if (offset >= table_size) return false;
                offset++;
                glyph_terminated = true;
                break;
            }

            if (!font_decode_utf8(table + offset - 1,
                                  table_size - (offset - 1),
                                  &codepoint, &consumed)) {
                return false;
            }
            offset += consumed - 1;
            if (!font_mapping_add(codepoint, glyph)) return false;
        }
        if (!glyph_terminated) return false;
    }
    return true;
}

static bool font_raw_lookup(u32 codepoint, u32 *glyph_index)
{
    if (!glyph_index) return false;
    if (font_unicode_table_valid) {
        for (u32 index = 0; index < font_mapping_count; index++) {
            if (font_mappings[index].codepoint == codepoint) {
                *glyph_index = font_mappings[index].glyph_index;
                return true;
            }
        }
    }

    /* Preserve the historical byte-index behavior for ASCII and legacy
     * single-byte fonts when no explicit mapping exists. */
    if (codepoint <= 0xFFU && font_header &&
        codepoint < font_header->numglyph) {
        *glyph_index = codepoint;
        return true;
    }
    return false;
}


void font_init(BOOT_INFO *BootInfo)
{
    u64 glyph_bytes;
    usize map_offset;

    (void)BootInfo;
    font_asset_start = (const u8 *)_binary_kernel_assets_font_psf_start;
    font_asset_size = (usize)((const u8 *)_binary_kernel_assets_font_psf_end -
                              font_asset_start);
    font_header = 0;
    glyph_buffer = 0;
    glyph_buffer_size = 0;
    font_mapping_count = 0;
    font_unicode_table_valid = false;
    font_ready = false;

    if (font_asset_size < sizeof(psf2_header_t)) return;
    font_header = (psf2_header_t *)font_asset_start;

    if (font_header->magic[0] != PSF2_MAGIC0 ||
        font_header->magic[1] != PSF2_MAGIC1 ||
        font_header->magic[2] != PSF2_MAGIC2 ||
        font_header->magic[3] != PSF2_MAGIC3)
        return;

    if (font_header->headerfile < sizeof(psf2_header_t) ||
        font_header->headerfile > font_asset_size ||
        font_header->numglyph == 0 || font_header->bytesperglyph == 0 ||
        font_header->width == 0 || font_header->height == 0)
        return;

    glyph_bytes = (u64)font_header->numglyph *
                  (u64)font_header->bytesperglyph;
    if (glyph_bytes > (u64)(font_asset_size - font_header->headerfile))
        return;

    glyph_buffer =
        (u8 *)_binary_kernel_assets_font_psf_start + font_header->headerfile;
    glyph_buffer_size = (usize)glyph_bytes;
    font_ready = true;

    map_offset = (usize)font_header->headerfile + glyph_buffer_size;
    if ((font_header->flags & PSF2_HAS_UNICODE_TABLE) != 0U &&
        map_offset <= font_asset_size &&
        font_parse_unicode_table(font_asset_start + map_offset,
                                 font_asset_size - map_offset,
                                 font_header->numglyph)) {
        font_unicode_table_valid = true;
    } else {
        font_mapping_count = 0;
    }
}

u32 font_width(void)
{
    return font_header && font_header->width ? font_header->width : 8U;
}

u32 font_height(void)
{
    return font_header && font_header->height ? font_header->height : 16U;
}

bool font_lookup_codepoint(u32 codepoint, u32 *glyph_index)
{
    if (!font_ready || !glyph_index || codepoint > 0x10FFFFU ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) return false;
    if (!font_raw_lookup(codepoint, glyph_index) ||
        (u64)(*glyph_index) * font_header->bytesperglyph >= glyph_buffer_size)
        return false;
    return true;
}

static void draw_glyph(u32 glyph_index,
                       u32 x,
                       u32 y,
                       u32 fg_color,
                       u32 bg_color)
{
    const u8 *glyph;

    if (!font_ready || glyph_index >= font_header->numglyph ||
        (u64)glyph_index * font_header->bytesperglyph >= glyph_buffer_size)
        return;
    glyph = glyph_buffer +
        (glyph_index * font_header->bytesperglyph);



    for (u32 py = 0; py < font_header->height; py++) {

        if (y + py >= framebuffer_height())
            break;

        u8 row = glyph[py];

        for (u32 px = 0; px < font_header->width; px++) {

            if (x + px >= framebuffer_width())
                break;

            framebuffer_put_pixel(x + px, y + py,
                ((row << px) & 0x80) ? fg_color : bg_color);
        }
    }
}

void draw_codepoint(u32 codepoint,
                    u32 x,
                    u32 y,
                    u32 fg_color,
                    u32 bg_color)
{
    u32 glyph_index;

    if (!font_lookup_codepoint(codepoint, &glyph_index)) {
        if (!font_raw_lookup(FONT_REPLACEMENT_CODEPOINT, &glyph_index) &&
            !font_raw_lookup('?', &glyph_index) &&
            !font_raw_lookup(' ', &glyph_index))
            return;
    }
    draw_glyph(glyph_index, x, y, fg_color, bg_color);
}
