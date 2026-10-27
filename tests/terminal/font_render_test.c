/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <font.h>

#define TEST_WIDTH 32U
#define TEST_HEIGHT 24U
#define TEST_PIXEL_SENTINEL 0x13579bdfU
#define TEST_FOREGROUND 0xff123456U
#define TEST_BACKGROUND 0xffabcdefU

extern char _binary_kernel_assets_font_psf_start[];
extern char _binary_kernel_assets_font_psf_end[];

static u32 pixels[TEST_WIDTH * TEST_HEIGHT];

void framebuffer_put_pixel(u32 x, u32 y, u32 color)
{
    if (x < TEST_WIDTH && y < TEST_HEIGHT)
        pixels[y * TEST_WIDTH + x] = color;
}

u32 framebuffer_width(void) { return TEST_WIDTH; }
u32 framebuffer_height(void) { return TEST_HEIGHT; }

static u8 hex_digit(char value)
{
    if (value >= '0' && value <= '9') return (u8)(value - '0');
    if (value >= 'a' && value <= 'f') return (u8)(value - 'a' + 10);
    if (value >= 'A' && value <= 'F') return (u8)(value - 'A' + 10);
    assert(false);
    return 0U;
}

static void render_and_compare(u32 codepoint, const char *expected_hex,
                               bool should_lookup)
{
    u8 expected_rows[16];
    u32 glyph_index = 0U;
    bool found;
    const u32 origin_x = 7U;
    const u32 origin_y = 3U;

    assert(expected_hex && strlen(expected_hex) == sizeof(expected_rows) * 2U);
    for (u32 row = 0U; row < sizeof(expected_rows); row++)
        expected_rows[row] = (u8)((hex_digit(expected_hex[row * 2U]) << 4U) |
                                  hex_digit(expected_hex[row * 2U + 1U]));

    found = font_lookup_codepoint(codepoint, &glyph_index);
    assert(found == should_lookup);
    if (found) assert(glyph_index < 2997U);

    for (usize i = 0U; i < sizeof(pixels) / sizeof(pixels[0]); i++)
        pixels[i] = TEST_PIXEL_SENTINEL;
    draw_codepoint(codepoint, origin_x, origin_y, TEST_FOREGROUND,
                   TEST_BACKGROUND);

    for (u32 row = 0U; row < 16U; row++) {
        for (u32 column = 0U; column < 8U; column++) {
            bool ink = (expected_rows[row] & (u8)(0x80U >> column)) != 0U;
            u32 expected = ink ? TEST_FOREGROUND : TEST_BACKGROUND;
            assert(pixels[(origin_y + row) * TEST_WIDTH + origin_x + column] ==
                   expected);
        }
    }
    assert(pixels[origin_y * TEST_WIDTH + origin_x - 1U] ==
           TEST_PIXEL_SENTINEL);
    assert(pixels[(origin_y + 16U) * TEST_WIDTH + origin_x] ==
           TEST_PIXEL_SENTINEL);
}

static void store_le32(char *destination, u32 value)
{
    destination[0] = (char)value;
    destination[1] = (char)(value >> 8U);
    destination[2] = (char)(value >> 16U);
    destination[3] = (char)(value >> 24U);
}

static void expect_header_field_rejected(usize offset, u32 value)
{
    char saved[4];
    u32 glyph_index;

    memcpy(saved, _binary_kernel_assets_font_psf_start + offset,
           sizeof(saved));
    store_le32(_binary_kernel_assets_font_psf_start + offset, value);
    font_init(NULL);
    assert(!font_lookup_codepoint('A', &glyph_index));
    memcpy(_binary_kernel_assets_font_psf_start + offset, saved,
           sizeof(saved));
}

static void test_loader_rejects_bad_header(void)
{
    char saved[4];
    usize asset_size = (usize)(_binary_kernel_assets_font_psf_end -
                               _binary_kernel_assets_font_psf_start);
    u32 glyph_index;

    memcpy(saved, _binary_kernel_assets_font_psf_start, sizeof(saved));
    _binary_kernel_assets_font_psf_start[0] ^= 1;
    font_init(NULL);
    assert(!font_lookup_codepoint('A', &glyph_index));
    memcpy(_binary_kernel_assets_font_psf_start, saved, sizeof(saved));

    expect_header_field_rejected(4U, 1U); /* Only PSF2 version 0 is supported. */
    expect_header_field_rejected(8U, (u32)(asset_size + 1U));

    font_init(NULL);
    assert(font_lookup_codepoint('A', &glyph_index));
}

int main(void)
{
    u32 glyph_index;

    test_loader_rejects_bad_header();
    assert(font_width() == 8U && font_height() == 16U);

    /* Expected rows below are copied from the checked-in UNSCII source data,
     * not produced through the loader or rasterizer under test. */
    render_and_compare(0x0041U, "0000183c6666667e6666666666000000", true);
    render_and_compare(0x00e9U, "00000c1800003c66667e60603c000000", true);
    render_and_compare(0x010dU, "00663c1800003c66606060663c000000", true);
    render_and_compare(0x015fU, "0000000000003e60603c06067c0c061c", true);

    render_and_compare(0x2500U, "00000000000000ffff00000000000000", true);
    render_and_compare(0x2502U, "18181818181818181818181818181818", true);
    render_and_compare(0x250cU, "000000000000001f1f18181818181818", true);
    render_and_compare(0x2518U, "18181818181818f8f800000000000000", true);

    render_and_compare(0x2588U, "ffffffffffffffffffffffffffffffff", true);
    render_and_compare(0x2591U, "22882288228822882288228822882288", true);

    /* These marks have their own glyphs; the terminal does not compose them
     * over a preceding base character. Each scalar is still one cell. */
    render_and_compare(0x0301U, "0c180000000000000000000000000000", true);
    render_and_compare(0x030cU, "66180000000000000000000000000000", true);
    render_and_compare(0x0327U, "000000000000000000000000000c061c", true);

    /* UNSCII has no U+FFFD or U+4E00 mapping. The loader's established
     * replacement path therefore renders '?' for an unsupported CJK scalar. */
    assert(!font_lookup_codepoint(0xfffdU, &glyph_index));
    assert(!font_lookup_codepoint(0x4e00U, &glyph_index));
    render_and_compare(0x4e00U, "003c6666060c18181800001818000000", false);

    assert(!font_lookup_codepoint(0xd800U, &glyph_index));
    assert(!font_lookup_codepoint(0x110000U, &glyph_index));
    puts("production UNSCII PSF2 loader and rasterizer: mappings, glyph rows, pixels and fallback passed");
    return 0;
}
