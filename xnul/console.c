/*
 * Minimal text console on the LK splash framebuffer. With the MMU off
 * every access is Device memory, so stay aligned (-mstrict-align).
 */
#include "console.h"
#include "font.h"

#define MARGIN		8
#define LABEL_COLS	8

static volatile uint32_t con_scale = 2;

static int con_ch_w(void) { return 8 * con_scale; }
static int con_ch_h(void) { return 16 * con_scale; }

void con_set_scale(uint32_t scale)
{
	if (scale >= 1 && scale <= 4)
		con_scale = scale;
}

volatile uint32_t *con_fb = (volatile uint32_t *)FB_BASE;
static uint32_t con_label_fg = YELLOW;
static uint32_t con_value_fg = WHITE;
static uint32_t con_bg;
static int row;

void con_clear(uint32_t bg)
{
	for (int y = 0; y < FB_H; y++) {
		volatile uint32_t *line = con_fb + y * (FB_STRIDE / 4);
		for (int x = 0; x < FB_W; x++)
			line[x] = bg;
	}
	con_bg = bg;
	row = 0;
}

static void put_char(int col, char c, uint32_t fg)
{
	if (c < 0x20 || c > 0x7e)
		c = '?';
	const unsigned char *g = font8x16[c - 0x20];
	int cw = con_ch_w();
	int chh = con_ch_h();
	int x0 = MARGIN + col * cw, y0 = MARGIN + row * chh;
	if (x0 + cw > FB_W || y0 + chh > FB_H)
		return;
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++) {
			uint32_t pc = (g[gy] & (0x80 >> gx)) ? fg : con_bg;
			for (int sy = 0; sy < (int)con_scale; sy++) {
				volatile uint32_t *p = con_fb + (y0 + gy * con_scale + sy) * (FB_STRIDE / 4)
						       + x0 + gx * con_scale;
				for (int sx = 0; sx < (int)con_scale; sx++)
					p[sx] = pc;
			}
		}
}

static int put_str(int col, const char *s, uint32_t fg)
{
	while (*s)
		put_char(col++, *s++, fg);
	return col;
}

static void fmt_hex(char *buf, uint64_t v, int digits)
{
	static const char hex[] = "0123456789ABCDEF";
	for (int i = 0; i < digits; i++)
		buf[i] = hex[(v >> (4 * (digits - 1 - i))) & 0xf];
	buf[digits] = 0;
}

void con_skip(void)
{
	row++;
}

void con_set_row(int r)
{
	row = r;
}

void con_wrap(const char *s, uint32_t fg, int width)
{
	int col = 0;

	while (*s) {
		if (*s == ' ') {
			s++;
			continue;
		}
		const char *word = s;
		int wlen = 0;

		while (s[wlen] && s[wlen] != ' ')
			wlen++;
		if (col != 0 && col + wlen + 1 > width) {
			row++;
			col = 0;
		}
		if (col != 0) {
			put_char(col, ' ', fg);
			col++;
		}
		for (int i = 0; i < wlen; i++) {
			put_char(col, word[i], fg);
			col++;
		}
		s += wlen;
	}
	row++;
}

void con_set_scheme(uint32_t label, uint32_t value)
{
	con_label_fg = label;
	con_value_fg = value;
}

void con_str(const char *label, const char *s)
{
	put_str(0, label, con_label_fg);
	put_str(LABEL_COLS, s, con_value_fg);
	row++;
}

void con_hex(const char *label, uint64_t v)
{
	char buf[17];
	fmt_hex(buf, v, 16);
	con_str(label, buf);
}

void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb)
{
	char buf[9];
	int col = put_str(0, label, con_label_fg);
	col = col < LABEL_COLS ? LABEL_COLS : col + 1;
	col = put_str(col, a, con_label_fg);
	fmt_hex(buf, va, 8);
	col = put_str(col, buf, con_value_fg);
	if (b) {
		col = put_str(col + 1, b, con_label_fg);
		fmt_hex(buf, vb, 8);
		put_str(col, buf, con_value_fg);
	}
	row++;
}
