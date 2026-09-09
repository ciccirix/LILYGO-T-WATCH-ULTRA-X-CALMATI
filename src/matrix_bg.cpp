#include "matrix_bg.h"
#include <esp_system.h>   // esp_random
#include <stdio.h>

#define MX_COLS    22
#define MX_ROWS    26
#define MX_COL_W   18     // 22 * 18 = 396 px, centred on the 410 px panel
#define MX_TICK_MS 120    // opt-in effect; modest rate keeps the QSPI flush sane

static lv_obj_t  *mx_cont = nullptr;
static lv_obj_t  *mx_col[MX_COLS];
static lv_timer_t *mx_timer = nullptr;
static bool       mx_enabled = false;

static int  head[MX_COLS];          // current head row; negative = still entering
static int  tlen[MX_COLS];          // trail length
static char cell[MX_COLS][MX_ROWS]; // glyphs for this column (tiled MX_WORD)

// Every column rains this word, tiled top-to-bottom, instead of random
// glyphs. No '#' — it is the LVGL recolor escape character and would
// corrupt parsing.
static const char MX_WORD[] = "CALMATI";
static const int  MX_WORD_LEN = sizeof(MX_WORD) - 1;

static void col_reset(int c)
{
    tlen[c] = MX_ROWS;
    head[c] = -(int)(esp_random() % MX_ROWS);   // stagger entry from above
    int offset = c % MX_WORD_LEN;               // stagger the word per column
    for (int r = 0; r < MX_ROWS; r++)
        cell[c][r] = MX_WORD[(r + offset) % MX_WORD_LEN];
}

// Distance 0 = bright head, increasing distance = dimmer trail.
static const char *shade(int dist, int len)
{
    if (dist == 0)        return "CCFFCC";
    if (dist <= len / 4)  return "5BFF8C";
    if (dist <= len / 2)  return "22BB44";
    return "0E6622";
}

static void render_col(int c)
{
    char buf[MX_ROWS * 12];
    int  n = 0;
    for (int r = 0; r < MX_ROWS; r++) {
        int dist = head[c] - r;   // 0 at head, grows up the trail
        if (head[c] >= 0 && r <= head[c] && dist < tlen[c]) {
            n += snprintf(buf + n, sizeof(buf) - n,
                          "#%s %c#\n", shade(dist, tlen[c]), cell[c][r]);
        } else {
            buf[n++] = '\n';      // empty row keeps vertical alignment
        }
    }
    if (n > 0 && buf[n - 1] == '\n') n--;   // trim trailing newline
    buf[n] = '\0';
    lv_label_set_text(mx_col[c], buf);
}

static void mx_tick(lv_timer_t *)
{
    for (int c = 0; c < MX_COLS; c++) {
        head[c]++;
        if (head[c] - tlen[c] > MX_ROWS)
            col_reset(c);
        render_col(c);
    }
}

lv_obj_t *matrix_bg_create(lv_obj_t *parent)
{
    mx_cont = lv_obj_create(parent);
    lv_obj_remove_style_all(mx_cont);
    lv_obj_set_size(mx_cont, MX_COL_W * MX_COLS, 502);
    lv_obj_align(mx_cont, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_scrollbar_mode(mx_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(mx_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(mx_cont, LV_OBJ_FLAG_CLICKABLE);

    for (int c = 0; c < MX_COLS; c++) {
        lv_obj_t *l = lv_label_create(mx_cont);
        lv_label_set_recolor(l, true);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(l, MX_COL_W);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_16, LV_PART_MAIN);
        // Non-recoloured glyphs (blank rows) render invisibly on the black screen
        lv_obj_set_style_text_color(l, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_pos(l, c * MX_COL_W, 0);
        mx_col[c] = l;
        col_reset(c);
    }

    lv_obj_add_flag(mx_cont, LV_OBJ_FLAG_HIDDEN);
    return mx_cont;
}

void matrix_bg_set_enabled(bool en)
{
    mx_enabled = en;
    if (!mx_cont) return;
    if (en) {
        lv_obj_clear_flag(mx_cont, LV_OBJ_FLAG_HIDDEN);
        if (!mx_timer) mx_timer = lv_timer_create(mx_tick, MX_TICK_MS, nullptr);
    } else {
        lv_obj_add_flag(mx_cont, LV_OBJ_FLAG_HIDDEN);
        if (mx_timer) { lv_timer_del(mx_timer); mx_timer = nullptr; }
    }
}

bool matrix_bg_is_enabled() { return mx_enabled; }

void matrix_bg_set_paused(bool paused)
{
    if (!mx_timer) return;
    if (paused) lv_timer_pause(mx_timer);
    else        lv_timer_resume(mx_timer);
}
