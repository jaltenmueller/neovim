#include <stdio.h>
#include <string.h>

#include "nvim/grid.h"
#include "nvim/mbyte.h"
#include "nvim/tui/termkey/termkey.h"
#include "nvim/vterm/pen.h"
#include "nvim/vterm/screen.h"
#include "nvim/vterm/state.h"
#include "nvim/vterm/vterm.h"
#include "nvim/vterm/vterm_defs.h"
#include "nvim/vterm/vterm_internal_defs.h"

#include "vterm/screen.c.generated.h"

#define UNICODE_SPACE 0x20
#define UNICODE_LINEFEED 0x0a

#undef DEBUG_REFLOW

static inline void clearcell(const VTermScreen *screen, ScreenCell *cell)
{
  cell->schar = 0;
  cell->pen = screen->pen;
}

ScreenCell *getcell(const VTermScreen *screen, int row, int col)
{
  if (row < 0 || row >= screen->rows) {
    return NULL;
  }
  if (col < 0 || col >= screen->cols) {
    return NULL;
  }
  return screen->buffer + (screen->cols * row) + col;
}

static ScreenCell *alloc_buffer(VTermScreen *screen, int rows, int cols)
{
  ScreenCell *new_buffer = vterm_allocator_malloc(screen->vt,
                                                  sizeof(ScreenCell) * (size_t)rows * (size_t)cols);

  for (int row = 0; row < rows; row++) {
    for (int col = 0; col < cols; col++) {
      clearcell(screen, &new_buffer[row * cols + col]);
    }
  }

  return new_buffer;
}

static void damagerect(VTermScreen *screen, VTermRect rect)
{
  VTermRect emit;

  switch (screen->damage_merge) {
  case VTERM_DAMAGE_CELL:
    // Always emit damage event
    emit = rect;
    break;

  case VTERM_DAMAGE_ROW:
    // Emit damage longer than one row. Try to merge with existing damage in the same row
    if (rect.end_row > rect.start_row + 1) {
      // Bigger than 1 line - flush existing, emit this
      vterm_screen_flush_damage(screen);
      emit = rect;
    } else if (screen->damaged.start_row == -1) {
      // None stored yet
      screen->damaged = rect;
      return;
    } else if (rect.start_row == screen->damaged.start_row) {
      // Merge with the stored line
      if (screen->damaged.start_col > rect.start_col) {
        screen->damaged.start_col = rect.start_col;
      }
      if (screen->damaged.end_col < rect.end_col) {
        screen->damaged.end_col = rect.end_col;
      }
      return;
    } else {
      // Emit the currently stored line, store a new one
      emit = screen->damaged;
      screen->damaged = rect;
    }
    break;

  case VTERM_DAMAGE_SCREEN:
  case VTERM_DAMAGE_SCROLL:
    // Never emit damage event
    if (screen->damaged.start_row == -1) {
      screen->damaged = rect;
    } else {
      rect_expand(&screen->damaged, &rect);
    }
    return;

  default:
    DEBUG_LOG("TODO: Maybe merge damage for level %d\n", screen->damage_merge);
    return;
  }

  if (screen->callbacks && screen->callbacks->damage) {
    (*screen->callbacks->damage)(emit, screen->cbdata);
  }
}

static void damagescreen(VTermScreen *screen)
{
  VTermRect rect = {
    .start_row = 0,
    .end_row = screen->rows,
    .start_col = 0,
    .end_col = screen->cols,
  };

  damagerect(screen, rect);
}

static int putglyph(VTermGlyphInfo *info, VTermPos pos, void *user)
{
  VTermScreen *screen = user;
  ScreenCell *cell = getcell(screen, pos.row, pos.col);

  if (!cell) {
    return 0;
  }

  cell->schar = info->schar;
  if (info->schar != 0) {
    cell->pen = screen->pen;
  }

  for (int col = 1; col < info->width; col++) {
    getcell(screen, pos.row, pos.col + col)->schar = (uint32_t)-1;
  }

  VTermRect rect = {
    .start_row = pos.row,
    .end_row = pos.row + 1,
    .start_col = pos.col,
    .end_col = pos.col + info->width,
  };

  cell->pen.protected_cell = info->protected_cell;
  cell->pen.dwl = info->dwl;
  cell->pen.dhl = info->dhl;

  damagerect(screen, rect);

  return 1;
}

static void sb_pushline_from_row(VTermScreen *screen, int row,
                                 const VTermLineInfo *lineinfo)
{
  VTermPos pos = { .row = row };
  for (pos.col = 0; pos.col < screen->cols; pos.col++) {
    vterm_screen_get_cell(screen, pos, screen->sb_buffer + pos.col);
  }

  if (screen->callbacks->sb_pushline_ex) {
    (screen->callbacks->sb_pushline_ex)(screen->cols, screen->sb_buffer,
                                        lineinfo, screen->cbdata);
  } else {
    (screen->callbacks->sb_pushline)(screen->cols, screen->sb_buffer, screen->cbdata);
  }
}

static int moverect_internal(VTermRect dest, VTermRect src, void *user)
{
  VTermScreen *screen = user;

  if (screen->callbacks && (screen->callbacks->sb_pushline || screen->callbacks->sb_pushline_ex)
      && dest.start_row == 0 && dest.start_col == 0           // starts top-left corner
      && dest.end_col == screen->cols                         // full width
      && screen->buffer == screen->buffers[BUFIDX_PRIMARY]) {  // not altscreen
    for (int row = 0; row < src.start_row; row++) {
      const VTermLineInfo *li = (row < screen->pending_sb_lineinfo_count)
                                ? &screen->pending_sb_lineinfo[row] : NULL;
      sb_pushline_from_row(screen, row, li);
    }
    screen->pending_sb_lineinfo_count = 0;
  }

  int cols = src.end_col - src.start_col;
  int downward = src.start_row - dest.start_row;

  int init_row, test_row, inc_row;
  if (downward < 0) {
    init_row = dest.end_row - 1;
    test_row = dest.start_row - 1;
    inc_row = -1;
  } else {
    init_row = dest.start_row;
    test_row = dest.end_row;
    inc_row = +1;
  }

  for (int row = init_row; row != test_row; row += inc_row) {
    memmove(getcell(screen, row, dest.start_col),
            getcell(screen, row + downward, src.start_col),
            (size_t)cols * sizeof(ScreenCell));
  }

  return 1;
}

static int moverect_user(VTermRect dest, VTermRect src, void *user)
{
  VTermScreen *screen = user;

  if (screen->callbacks && screen->callbacks->moverect) {
    if (screen->damage_merge != VTERM_DAMAGE_SCROLL) {
      // Avoid an infinite loop
      vterm_screen_flush_damage(screen);
    }

    if ((*screen->callbacks->moverect)(dest, src, screen->cbdata)) {
      return 1;
    }
  }

  damagerect(screen, dest);

  return 1;
}

static int erase_internal(VTermRect rect, int selective, void *user)
{
  VTermScreen *screen = user;

  for (int row = rect.start_row; row < screen->state->rows && row < rect.end_row; row++) {
    const VTermLineInfo *info = vterm_state_get_lineinfo(screen->state, row);

    for (int col = rect.start_col; col < rect.end_col; col++) {
      ScreenCell *cell = getcell(screen, row, col);

      if (selective && cell->pen.protected_cell) {
        continue;
      }

      cell->schar = 0;
      cell->pen = (ScreenPen){
        // Only copy .fg and .bg; leave things like rv in reset state
        .fg = screen->pen.fg,
        .bg = screen->pen.bg,
      };
      cell->pen.dwl = info->doublewidth;
      cell->pen.dhl = info->doubleheight;
    }
  }

  return 1;
}

static int erase_user(VTermRect rect, int selective, void *user)
{
  VTermScreen *screen = user;

  damagerect(screen, rect);

  return 1;
}

static int erase(VTermRect rect, int selective, void *user)
{
  erase_internal(rect, selective, user);
  return erase_user(rect, 0, user);
}

static int scrollrect(VTermRect rect, int downward, int rightward, void *user)
{
  VTermScreen *screen = user;

  if (screen->damage_merge != VTERM_DAMAGE_SCROLL) {
    vterm_scroll_rect(rect, downward, rightward,
                      moverect_internal, erase_internal, screen);

    vterm_screen_flush_damage(screen);

    vterm_scroll_rect(rect, downward, rightward,
                      moverect_user, erase_user, screen);

    return 1;
  }

  if (screen->damaged.start_row != -1
      && !rect_intersects(&rect, &screen->damaged)) {
    vterm_screen_flush_damage(screen);
  }

  if (screen->pending_scrollrect.start_row == -1) {
    screen->pending_scrollrect = rect;
    screen->pending_scroll_downward = downward;
    screen->pending_scroll_rightward = rightward;
  } else if (rect_equal(&screen->pending_scrollrect, &rect)
             && ((screen->pending_scroll_downward == 0 && downward == 0)
                 || (screen->pending_scroll_rightward == 0 && rightward == 0))) {
    screen->pending_scroll_downward += downward;
    screen->pending_scroll_rightward += rightward;
  } else {
    vterm_screen_flush_damage(screen);

    screen->pending_scrollrect = rect;
    screen->pending_scroll_downward = downward;
    screen->pending_scroll_rightward = rightward;
  }

  vterm_scroll_rect(rect, downward, rightward,
                    moverect_internal, erase_internal, screen);

  if (screen->damaged.start_row == -1) {
    return 1;
  }

  if (rect_contains(&rect, &screen->damaged)) {
    // Scroll region entirely contains the damage; just move it
    vterm_rect_move(&screen->damaged, -downward, -rightward);
    rect_clip(&screen->damaged, &rect);
  }
  // There are a number of possible cases here, but lets restrict this to only the common case where
  // we might actually gain some performance by optimising it. Namely, a vertical scroll that neatly
  // cuts the damage region in half.
  else if (rect.start_col <= screen->damaged.start_col
           && rect.end_col >= screen->damaged.end_col
           && rightward == 0) {
    if (screen->damaged.start_row >= rect.start_row
        && screen->damaged.start_row < rect.end_row) {
      screen->damaged.start_row -= downward;
      if (screen->damaged.start_row < rect.start_row) {
        screen->damaged.start_row = rect.start_row;
      }
      if (screen->damaged.start_row > rect.end_row) {
        screen->damaged.start_row = rect.end_row;
      }
    }
    if (screen->damaged.end_row >= rect.start_row
        && screen->damaged.end_row < rect.end_row) {
      screen->damaged.end_row -= downward;
      if (screen->damaged.end_row < rect.start_row) {
        screen->damaged.end_row = rect.start_row;
      }
      if (screen->damaged.end_row > rect.end_row) {
        screen->damaged.end_row = rect.end_row;
      }
    }
  } else {
    DEBUG_LOG("TODO: Just flush and redo damaged=" STRFrect " rect=" STRFrect "\n",
              ARGSrect(screen->damaged), ARGSrect(rect));
  }

  return 1;
}

static int movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user)
{
  VTermScreen *screen = user;

  if (screen->callbacks && screen->callbacks->movecursor) {
    return (*screen->callbacks->movecursor)(pos, oldpos, visible, screen->cbdata);
  }

  return 0;
}

static int setpenattr(VTermAttr attr, VTermValue *val, void *user)
{
  VTermScreen *screen = user;

  switch (attr) {
  case VTERM_ATTR_BOLD:
    screen->pen.bold = (unsigned)val->boolean;
    return 1;
  case VTERM_ATTR_UNDERLINE:
    screen->pen.underline = (unsigned)val->number;
    return 1;
  case VTERM_ATTR_ITALIC:
    screen->pen.italic = (unsigned)val->boolean;
    return 1;
  case VTERM_ATTR_BLINK:
    screen->pen.blink = (unsigned)val->boolean;
    return 1;
  case VTERM_ATTR_REVERSE:
    screen->pen.reverse = (unsigned)val->boolean;
    return 1;
  case VTERM_ATTR_CONCEAL:
    screen->pen.conceal = (unsigned)val->boolean;
    return 1;
  case VTERM_ATTR_STRIKE:
    screen->pen.strike = (unsigned)val->boolean;
    return 1;
  case VTERM_ATTR_FONT:
    screen->pen.font = (unsigned)val->number;
    return 1;
  case VTERM_ATTR_FOREGROUND:
    screen->pen.fg = val->color;
    return 1;
  case VTERM_ATTR_BACKGROUND:
    screen->pen.bg = val->color;
    return 1;
  case VTERM_ATTR_SMALL:
    screen->pen.small = (unsigned)val->boolean;
    return 1;
  case VTERM_ATTR_BASELINE:
    screen->pen.baseline = (unsigned)val->number;
    return 1;
  case VTERM_ATTR_URI:
    screen->pen.uri = val->number;
    return 1;
  case VTERM_ATTR_DIM:
    screen->pen.dim = (unsigned)val->boolean;
    return 1;
  case VTERM_ATTR_OVERLINE:
    screen->pen.overline = (unsigned)val->boolean;
    return 1;

  case VTERM_N_ATTRS:
    return 0;
  }

  return 0;
}

static int settermprop(VTermProp prop, VTermValue *val, void *user)
{
  VTermScreen *screen = user;

  switch (prop) {
  case VTERM_PROP_ALTSCREEN:
    if (val->boolean && !screen->buffers[BUFIDX_ALTSCREEN]) {
      return 0;
    }

    screen->buffer =
      val->boolean ? screen->buffers[BUFIDX_ALTSCREEN] : screen->buffers[BUFIDX_PRIMARY];
    // only send a damage event on disable; because during enable there's an erase that sends a
    // damage anyway
    if (!val->boolean) {
      damagescreen(screen);
    }
    break;
  case VTERM_PROP_REVERSE:
    screen->global_reverse = (unsigned)val->boolean;
    damagescreen(screen);
    break;
  default:
    ;  // ignore
  }

  if (screen->callbacks && screen->callbacks->settermprop) {
    return (*screen->callbacks->settermprop)(prop, val, screen->cbdata);
  }

  return 1;
}

static int bell(void *user)
{
  VTermScreen *screen = user;

  if (screen->callbacks && screen->callbacks->bell) {
    return (*screen->callbacks->bell)(screen->cbdata);
  }

  return 0;
}

/// How many cells are non-blank Returns the position of the first blank cell in the trailing blank
/// end
static int line_popcount(ScreenCell *buffer, int row, int rows, int cols)
{
  int col = cols - 1;
  while (col >= 0 && buffer[row * cols + col].schar == 0) {
    col--;
  }
  return col + 1;
}

static int row_popcount(const ScreenCell *row, int cols)
{
  int col = cols - 1;
  while (col >= 0 && row[col].schar == 0) {
    col--;
  }
  return col + 1;
}

static int line_width(const ScreenCell *buffer, const VTermLineInfo *lineinfo, int row, int rows,
                      int cols)
{
  if (row < rows - 1 && lineinfo && lineinfo[row + 1].continuation) {
    return cols;
  }
  return line_popcount((ScreenCell *)buffer, row, rows, cols);
}

static void screen_cell_from_vterm(const VTermScreen *screen, const VTermScreenCell *src,
                                   ScreenCell *dst)
{
  dst->schar = src->schar;
  dst->pen.bold = src->attrs.bold;
  dst->pen.underline = src->attrs.underline;
  dst->pen.italic = src->attrs.italic;
  dst->pen.blink = src->attrs.blink;
  dst->pen.reverse = src->attrs.reverse ^ screen->global_reverse;
  dst->pen.conceal = src->attrs.conceal;
  dst->pen.strike = src->attrs.strike;
  dst->pen.font = src->attrs.font;
  dst->pen.small = src->attrs.small;
  dst->pen.baseline = src->attrs.baseline;
  dst->pen.dim = src->attrs.dim;
  dst->pen.overline = src->attrs.overline;
  dst->pen.protected_cell = 0;
  dst->pen.dwl = src->attrs.dwl;
  dst->pen.dhl = src->attrs.dhl;
  dst->pen.fg = src->fg;
  dst->pen.bg = src->bg;
  dst->pen.uri = src->uri;
}

static void copy_vterm_row_to_screen(const VTermScreen *screen, const VTermScreenCell *src_row,
                                     int src_cols, ScreenCell *dst_row, int dst_cols)
{
  for (int col = 0; col < dst_cols; col++) {
    clearcell(screen, &dst_row[col]);
  }

  for (int col = 0; col < src_cols && col < dst_cols; ) {
    const VTermScreenCell *src = &src_row[col];
    ScreenCell *dst = &dst_row[col];

    screen_cell_from_vterm(screen, src, dst);

    int cw = src->width > 0 ? src->width : 1;
    if (cw == 2 && col + 1 < dst_cols) {
      (dst + 1)->schar = (uint32_t)-1;
    }
    col += cw;
  }
}

static void vterm_cell_from_screen(const VTermScreen *screen, const ScreenCell *src,
                                   const ScreenCell *next, VTermScreenCell *dst)
{
  *dst = (VTermScreenCell){ 0 };
  if (src == NULL || src->schar == (uint32_t)-1) {
    dst->width = 1;
    return;
  }

  dst->schar = src->schar;
  dst->attrs.bold = src->pen.bold;
  dst->attrs.underline = src->pen.underline;
  dst->attrs.italic = src->pen.italic;
  dst->attrs.blink = src->pen.blink;
  dst->attrs.reverse = src->pen.reverse ^ screen->global_reverse;
  dst->attrs.conceal = src->pen.conceal;
  dst->attrs.strike = src->pen.strike;
  dst->attrs.font = src->pen.font;
  dst->attrs.dwl = src->pen.dwl;
  dst->attrs.dhl = src->pen.dhl;
  dst->attrs.small = src->pen.small;
  dst->attrs.baseline = src->pen.baseline;
  dst->attrs.dim = src->pen.dim;
  dst->attrs.overline = src->pen.overline;
  dst->fg = src->pen.fg;
  dst->bg = src->pen.bg;
  dst->uri = src->pen.uri;
  dst->width = (next && next->schar == (uint32_t)-1) ? 2 : 1;
}

static void copy_screen_row_to_vterm(const VTermScreen *screen, const ScreenCell *src_row,
                                     int src_cols, VTermScreenCell *dst_row, int dst_cols)
{
  for (int col = 0; col < dst_cols; col++) {
    const ScreenCell *src = (src_row != NULL && col < src_cols) ? &src_row[col] : NULL;
    const ScreenCell *next = (src_row != NULL && col + 1 < src_cols) ? &src_row[col + 1] : NULL;
    vterm_cell_from_screen(screen, src, next, &dst_row[col]);
  }
}

static int staged_row_width(const ScreenCell *row, int stored_cols, bool next_is_continuation)
{
  return next_is_continuation ? stored_cols : row_popcount(row, stored_cols);
}

static void *grow_array(VTerm *vt, void *ptr, size_t old_count, size_t new_count, size_t item_size)
{
  void *new_ptr = vterm_allocator_malloc(vt, new_count * item_size);

  if (ptr != NULL && old_count > 0) {
    memcpy(new_ptr, ptr, old_count * item_size);
    vterm_allocator_free(vt, ptr);
  }

  return new_ptr;
}

static void push_scrollback_row(VTermScreen *screen, const ScreenCell *src_row, int src_cols,
                                int cols, bool continuation)
{
  copy_screen_row_to_vterm(screen, src_row, src_cols, screen->sb_buffer, cols);

  if (screen->callbacks->sb_pushline_ex) {
    VTermLineInfo li = { 0 };
    li.continuation = continuation;
    screen->callbacks->sb_pushline_ex(cols, screen->sb_buffer, &li, screen->cbdata);
  } else if (screen->callbacks->sb_pushline) {
    screen->callbacks->sb_pushline(cols, screen->sb_buffer, screen->cbdata);
  }
}

static void push_reflowed_scrollback_line(VTermScreen *screen, const ScreenCell *old_buffer,
                                          const VTermLineInfo *old_lineinfo, int old_row_start,
                                          int old_row_end, int old_rows, int old_cols,
                                          int new_cols)
{
  int width = 0;
  for (int row = old_row_start; row <= old_row_end; row++) {
    width += line_width(old_buffer, old_lineinfo, row, old_rows, old_cols);
  }

  size_t total_width = (size_t)width;
  ScreenCell *flat = total_width > 0
                     ? vterm_allocator_malloc(screen->vt, sizeof(*flat) * total_width)
                     : NULL;
  size_t flat_off = 0;

  for (int row = old_row_start; row <= old_row_end; row++) {
    int row_width = line_width(old_buffer, old_lineinfo, row, old_rows, old_cols);
    if (row_width > 0) {
      memcpy(&flat[flat_off], &old_buffer[row * old_cols], (size_t)row_width * sizeof(*flat));
    }
    flat_off += (size_t)row_width;
  }

  int new_height = width > 0 ? (width + new_cols - 1) / new_cols : 1;
  bool first_row_continuation = screen->reflow && old_lineinfo
                                && old_lineinfo[old_row_start].continuation;

  flat_off = 0;
  for (int row = 0; row < new_height; row++) {
    int count = 0;
    if (total_width > flat_off) {
      size_t remaining = total_width - flat_off;
      count = (int)MIN(remaining, (size_t)new_cols);
    }

    bool continuation = row > 0 || (row == 0 && first_row_continuation);
    push_scrollback_row(screen, count > 0 ? &flat[flat_off] : NULL, count, new_cols,
                        continuation);
    flat_off += (size_t)count;
  }

  if (flat != NULL) {
    vterm_allocator_free(screen->vt, flat);
  }
}

static void reflow_scrollback_boundary(VTermScreen *screen, ScreenCell *new_buffer,
                                       VTermLineInfo *new_lineinfo, int new_rows, int new_cols,
                                       int pop_cols, int *new_row, VTermPos *new_cursor,
                                       bool active)
{
  if (!screen->reflow || !screen->callbacks || !screen->callbacks->sb_popline_ex) {
    return;
  }

  int content_start = *new_row + 1;
  if (content_start >= new_rows || !new_lineinfo[content_start].continuation) {
    return;
  }

  int suffix_end = content_start;
  while (suffix_end + 1 < new_rows && new_lineinfo[suffix_end + 1].continuation) {
    suffix_end++;
  }

  int prefix_cap = 16;
  int prefix_count = 0;
  ScreenCell *prefix_rows = vterm_allocator_malloc(screen->vt,
                                                   sizeof(*prefix_rows) * (size_t)prefix_cap
                                                   * (size_t)pop_cols);
  int *prefix_widths = vterm_allocator_malloc(screen->vt,
                                              sizeof(*prefix_widths) * (size_t)prefix_cap);
  int *prefix_storecols = vterm_allocator_malloc(screen->vt,
                                                 sizeof(*prefix_storecols) * (size_t)prefix_cap);
  VTermLineInfo *prefix_lineinfo = vterm_allocator_malloc(screen->vt,
                                                          sizeof(*prefix_lineinfo)
                                                          * (size_t)prefix_cap);

  while (true) {
    VTermLineInfo li = { 0 };
    int stored_cols = pop_cols;
    if (!screen->callbacks->sb_popline_ex(pop_cols, screen->sb_buffer, &li, &stored_cols,
                                          screen->cbdata)) {
      break;
    }

    if (prefix_count == prefix_cap) {
      size_t old_cap = (size_t)prefix_cap;
      prefix_cap *= 2;
      prefix_rows = grow_array(screen->vt, prefix_rows, old_cap * (size_t)pop_cols,
                               (size_t)prefix_cap * (size_t)pop_cols, sizeof(*prefix_rows));
      prefix_widths = grow_array(screen->vt, prefix_widths, old_cap, (size_t)prefix_cap,
                                 sizeof(*prefix_widths));
      prefix_storecols = grow_array(screen->vt, prefix_storecols, old_cap, (size_t)prefix_cap,
                                    sizeof(*prefix_storecols));
      prefix_lineinfo = grow_array(screen->vt, prefix_lineinfo, old_cap, (size_t)prefix_cap,
                                   sizeof(*prefix_lineinfo));
    }

    prefix_storecols[prefix_count] = stored_cols;
    prefix_lineinfo[prefix_count] = li;
    copy_vterm_row_to_screen(screen, screen->sb_buffer, stored_cols,
                             &prefix_rows[prefix_count * pop_cols], pop_cols);
    prefix_count++;

    if (!li.continuation) {
      break;
    }
  }

  if (prefix_count == 0) {
    new_lineinfo[content_start].continuation = 0;
    vterm_allocator_free(screen->vt, prefix_rows);
    vterm_allocator_free(screen->vt, prefix_widths);
    vterm_allocator_free(screen->vt, prefix_storecols);
    vterm_allocator_free(screen->vt, prefix_lineinfo);
    return;
  }

  size_t prefix_width = 0;
  for (int i = prefix_count - 1; i >= 0; i--) {
    bool next_is_continuation = (i > 0) ? prefix_lineinfo[i - 1].continuation : true;
    prefix_widths[i] = staged_row_width(&prefix_rows[i * pop_cols], prefix_storecols[i],
                                        next_is_continuation);
    prefix_width += (size_t)prefix_widths[i];
  }

  size_t suffix_width = 0;
  size_t suffix_cursor_offset = 0;
  bool cursor_in_suffix = active && new_cursor->row >= content_start && new_cursor->row <= suffix_end;
  for (int row = content_start; row <= suffix_end; row++) {
    int row_width = line_width(new_buffer, new_lineinfo, row, new_rows, new_cols);
    suffix_width += (size_t)row_width;

    if (!cursor_in_suffix) {
      continue;
    }
    if (row < new_cursor->row) {
      suffix_cursor_offset += (size_t)row_width;
    } else if (row == new_cursor->row) {
      int max_col = row < suffix_end ? new_cols : row_width;
      int cursor_col = new_cursor->col;
      if (cursor_col > max_col) {
        cursor_col = max_col;
      }
      suffix_cursor_offset += (size_t)cursor_col;
    }
  }

  size_t total_width = prefix_width + suffix_width;
  int total_rows = total_width > 0 ? (int)((total_width + (size_t)new_cols - 1) / (size_t)new_cols)
                                   : 1;
  int available_rows = suffix_end + 1;
  int overflow_rows = MAX(0, total_rows - available_rows);
  int visible_rows = total_rows - overflow_rows;
  int dst_start = suffix_end - visible_rows + 1;

  ScreenCell *flat = total_width > 0
                     ? vterm_allocator_malloc(screen->vt, sizeof(*flat) * total_width)
                     : NULL;

  size_t flat_off = 0;
  for (int i = prefix_count - 1; i >= 0; i--) {
    size_t row_width = (size_t)prefix_widths[i];
    memcpy(&flat[flat_off], &prefix_rows[i * pop_cols], row_width * sizeof(*flat));
    flat_off += row_width;
  }
  for (int row = content_start; row <= suffix_end; row++) {
    int row_width = line_width(new_buffer, new_lineinfo, row, new_rows, new_cols);
    memcpy(&flat[flat_off], &new_buffer[row * new_cols], (size_t)row_width * sizeof(*flat));
    flat_off += (size_t)row_width;
  }

  int cursor_line_row = -1;
  int cursor_line_col = 0;
  if (cursor_in_suffix) {
    size_t cursor_offset = prefix_width + suffix_cursor_offset;
    if (total_width == 0) {
      cursor_line_row = 0;
    } else if (cursor_offset >= total_width) {
      cursor_line_row = total_rows - 1;
      cursor_line_col = (int)(total_width
                              - (size_t)(total_rows - 1) * (size_t)new_cols);
    } else {
      cursor_line_row = (int)(cursor_offset / (size_t)new_cols);
      cursor_line_col = (int)(cursor_offset % (size_t)new_cols);
    }
  }

  for (int row = 0; row < dst_start; row++) {
    for (int col = 0; col < new_cols; col++) {
      clearcell(screen, &new_buffer[row * new_cols + col]);
    }
    new_lineinfo[row] = (VTermLineInfo){ 0 };
  }

  flat_off = 0;
  bool cursor_updated = false;
  for (int row = 0; row < total_rows; row++) {
    int count = 0;
    if (total_width > flat_off) {
      size_t remaining = total_width - flat_off;
      count = (int)MIN(remaining, (size_t)new_cols);
    }

    if (row < overflow_rows) {
      push_scrollback_row(screen, count > 0 ? &flat[flat_off] : NULL, count, new_cols, row > 0);
    } else {
      int dst_row = dst_start + row - overflow_rows;
      if (count > 0) {
        memcpy(&new_buffer[dst_row * new_cols], &flat[flat_off], (size_t)count * sizeof(*flat));
      }
      for (int col = count; col < new_cols; col++) {
        clearcell(screen, &new_buffer[dst_row * new_cols + col]);
      }
      new_lineinfo[dst_row].continuation = row > 0;

      if (cursor_in_suffix && cursor_line_row == row) {
        new_cursor->row = dst_row;
        new_cursor->col = cursor_line_col;
        cursor_updated = true;
      }
    }

    flat_off += (size_t)count;
  }

  if (cursor_in_suffix && !cursor_updated) {
    new_cursor->row = dst_start;
    new_cursor->col = 0;
  }

  *new_row = dst_start - 1;

  vterm_allocator_free(screen->vt, prefix_rows);
  vterm_allocator_free(screen->vt, prefix_widths);
  vterm_allocator_free(screen->vt, prefix_storecols);
  vterm_allocator_free(screen->vt, prefix_lineinfo);
  if (flat != NULL) {
    vterm_allocator_free(screen->vt, flat);
  }
}

static void resize_buffer(VTermScreen *screen, int bufidx, int new_rows, int new_cols, bool active,
                          VTermStateFields *statefields)
{
  int old_rows = screen->rows;
  int old_cols = screen->cols;

  ScreenCell *old_buffer = screen->buffers[bufidx];
  VTermLineInfo *old_lineinfo = statefields->lineinfos[bufidx];

  ScreenCell *new_buffer = vterm_allocator_malloc(screen->vt,
                                                  sizeof(ScreenCell) * (size_t)new_rows *
                                                  (size_t)new_cols);
  VTermLineInfo *new_lineinfo = vterm_allocator_malloc(screen->vt,
                                                       sizeof(new_lineinfo[0]) * (size_t)new_rows);

  int old_row = old_rows - 1;
  int new_row = new_rows - 1;
  int rows_pushed_in_reflow = 0;  // Track rows pushed during on-screen reflow overflow

  VTermPos old_cursor = statefields->pos;
  VTermPos new_cursor = { -1, -1 };
  ScreenCell *staged_prefix_rows = NULL;
  int *staged_prefix_counts = NULL;
  bool *staged_prefix_continuation = NULL;
  int staged_prefix_count = 0;

#ifdef DEBUG_REFLOW
  fprintf(stderr, "Resizing from %dx%d to %dx%d; cursor was at (%d,%d)\n",
          old_cols, old_rows, new_cols, new_rows, old_cursor.col, old_cursor.row);
#endif

  // Trim trailing blank, non-continuation rows below the cursor.
  // These waste new-buffer slots and leave fewer slots for scrollback pop.
  // Preserve a blank cursor row: after a real newline the cursor legitimately
  // sits on the empty next row, and dropping it breaks round-trips.
  if (screen->reflow && bufidx == BUFIDX_PRIMARY && active && old_lineinfo) {
    while (old_row > 0
           && line_popcount(old_buffer, old_row, old_rows, old_cols) == 0
           && !old_lineinfo[old_row].continuation
           && old_row > old_cursor.row) {
      old_row--;
    }
  }

  // Keep track of the final row that is known to be blank, so we know what spare space we have for
  // scrolling into
  int final_blank_row = new_rows;

  bool do_reflow = screen->reflow && (bufidx == BUFIDX_PRIMARY);

  while (old_row >= 0) {
    int old_row_end = old_row;
    // TODO(vterm): Stop if dwl or dhl
    while (do_reflow && old_lineinfo && old_row > 0 && old_lineinfo[old_row].continuation) {
      old_row--;
    }
    int old_row_start = old_row;
    int orig_old_row_start = old_row;  // Save for end-of-iteration advancement

    int width = 0;
    for (int row = old_row_start; row <= old_row_end; row++) {
      width += line_width(old_buffer, old_lineinfo, row, old_rows, old_cols);
    }

    if (final_blank_row == (new_row + 1) && width == 0) {
      final_blank_row = new_row;
    }

    int new_height = do_reflow
                     ? width ? (width + new_cols - 1) / new_cols : 1
                     : 1;

    int new_row_end = new_row;
    int new_row_start = new_row - new_height + 1;

    old_row = old_row_start;
    int old_col = 0;

    int spare_rows = new_rows - final_blank_row;

    if (new_row_start < 0    // we'd fall off the top
        && spare_rows >= 0    // we actually have spare rows
        && (!active || new_cursor.row == -1 || (new_cursor.row - new_row_start) < new_rows)) {
      // Attempt to scroll content down into the blank rows at the bottom to make it fit
      int downwards = -new_row_start;
      if (downwards > spare_rows) {
        downwards = spare_rows;
      }
      int rowcount = new_rows - downwards;

#ifdef DEBUG_REFLOW
      fprintf(stderr, "  scroll %d rows +%d downwards\n", rowcount, downwards);
#endif

      memmove(&new_buffer[downwards * new_cols], &new_buffer[0],
              (size_t)rowcount * (size_t)new_cols * sizeof(ScreenCell));
      memmove(&new_lineinfo[downwards],          &new_lineinfo[0],
              (size_t)rowcount * sizeof(new_lineinfo[0]));

      new_row += downwards;
      new_row_start += downwards;
      new_row_end += downwards;

      if (new_cursor.row >= 0) {
        new_cursor.row += downwards;
      }

      final_blank_row += downwards;
    }

#ifdef DEBUG_REFLOW
    fprintf(stderr, "  rows [%d..%d] <- [%d..%d] width=%d\n",
            new_row_start, new_row_end, old_row_start, old_row_end, width);
#endif

    bool first_row_continuation = screen->reflow && old_lineinfo
                                  && old_lineinfo[old_row_start].continuation;
    if (new_row_start < 0
        && screen->reflow
        && bufidx == BUFIDX_PRIMARY
        && active
        && screen->callbacks
        && (screen->callbacks->sb_pushline || screen->callbacks->sb_pushline_ex)) {
      // This logical line crosses the new top-of-screen boundary. Reflow the
      // entire visible suffix first, then split it between scrollback and
      // screen at the new width. Pushing old-width prefix rows one-by-one
      // creates boundary fragments that can later merge across hard newlines.
      size_t total_width = (size_t)width;
      ScreenCell *flat = total_width > 0
                         ? vterm_allocator_malloc(screen->vt, sizeof(*flat) * total_width)
                         : NULL;

      size_t flat_off = 0;
      bool cursor_in_line = active && old_row_start <= old_cursor.row && old_cursor.row <= old_row_end;
      size_t cursor_offset = 0;
      for (int row = old_row_start; row <= old_row_end; row++) {
        int row_width = line_width(old_buffer, old_lineinfo, row, old_rows, old_cols);

        if (row_width > 0) {
          memcpy(&flat[flat_off], &old_buffer[row * old_cols], (size_t)row_width * sizeof(*flat));
        }

        if (cursor_in_line) {
          if (row < old_cursor.row) {
            cursor_offset += (size_t)row_width;
          } else if (row == old_cursor.row) {
            int cursor_col = old_cursor.col;
            if (cursor_col > row_width) {
              cursor_col = row_width;
            }
            cursor_offset += (size_t)cursor_col;
          }
        }

        flat_off += (size_t)row_width;
      }

      int available_rows = new_row + 1;
      int overflow_rows = MAX(0, new_height - available_rows);
      int visible_rows = new_height - overflow_rows;
      int dst_start = new_row - visible_rows + 1;

      int cursor_line_row = -1;
      int cursor_line_col = 0;
      if (cursor_in_line) {
        if (total_width == 0) {
          cursor_line_row = 0;
        } else if (cursor_offset >= total_width) {
          cursor_line_row = new_height - 1;
          cursor_line_col = (int)(total_width
                                  - (size_t)(new_height - 1) * (size_t)new_cols);
        } else {
          cursor_line_row = (int)(cursor_offset / (size_t)new_cols);
          cursor_line_col = (int)(cursor_offset % (size_t)new_cols);
        }
      }

      if (visible_rows == 0) {
        if (flat != NULL) {
          vterm_allocator_free(screen->vt, flat);
        }

        // No part of this logical line fits on-screen. Leave it, and all
        // older rows above it, for the post-loop scrollback pass so they are
        // pushed in chronological order at the new width.
        old_row = old_row_end;
        final_blank_row = -1;
        break;
      }

      flat_off = 0;
      bool cursor_updated = false;
      for (int row = 0; row < new_height; row++) {
        int count = 0;
        if (total_width > flat_off) {
          size_t remaining = total_width - flat_off;
          count = (int)MIN(remaining, (size_t)new_cols);
        }

        bool continuation = row > 0 || (row == 0 && first_row_continuation);
        if (row < overflow_rows) {
          if (visible_rows > 0) {
            if (staged_prefix_rows == NULL) {
              staged_prefix_rows = vterm_allocator_malloc(screen->vt,
                                                          sizeof(*staged_prefix_rows)
                                                          * (size_t)overflow_rows
                                                          * (size_t)new_cols);
              staged_prefix_counts = vterm_allocator_malloc(screen->vt,
                                                            sizeof(*staged_prefix_counts)
                                                            * (size_t)overflow_rows);
              staged_prefix_continuation = vterm_allocator_malloc(screen->vt,
                                                                  sizeof(*staged_prefix_continuation)
                                                                  * (size_t)overflow_rows);
            }

            staged_prefix_counts[staged_prefix_count] = count;
            staged_prefix_continuation[staged_prefix_count] = continuation;
            if (count > 0) {
              memcpy(&staged_prefix_rows[staged_prefix_count * new_cols], &flat[flat_off],
                     (size_t)count * sizeof(*flat));
            }
            for (int col = count; col < new_cols; col++) {
              clearcell(screen, &staged_prefix_rows[staged_prefix_count * new_cols + col]);
            }
            staged_prefix_count++;
          } else {
            push_scrollback_row(screen, count > 0 ? &flat[flat_off] : NULL, count, new_cols,
                                continuation);
          }
        } else {
          int dst_row = dst_start + row - overflow_rows;
          if (count > 0) {
            memcpy(&new_buffer[dst_row * new_cols], &flat[flat_off], (size_t)count * sizeof(*flat));
          }
          for (int col = count; col < new_cols; col++) {
            clearcell(screen, &new_buffer[dst_row * new_cols + col]);
          }
          new_lineinfo[dst_row].continuation = continuation;

          if (cursor_in_line && cursor_line_row == row) {
            new_cursor.row = dst_row;
            new_cursor.col = cursor_line_col;
            cursor_updated = true;
          }
        }

        flat_off += (size_t)count;
      }

      if (cursor_in_line && !cursor_updated) {
        new_cursor.row = 0;
        new_cursor.col = cursor_line_col;
        if (new_cursor.col >= new_cols) {
          new_cursor.col = new_cols - 1;
        }
      }

      if (flat != NULL) {
        vterm_allocator_free(screen->vt, flat);
      }

      old_row = orig_old_row_start - 1;
      new_row = dst_start - 1;
      continue;
    }

    if (new_row_start < 0) {
      // Still doesn't fit and cannot be split across scrollback.
      if (old_row_start <= old_cursor.row && old_cursor.row <= old_row_end) {
        new_cursor.row = 0;
        new_cursor.col = old_cursor.col;
        if (new_cursor.col >= new_cols) {
          new_cursor.col = new_cols - 1;
        }
      }
      // Ensure the entire remaining logical line is included
      // in the push-to-scrollback below, so no content is lost.
      old_row = old_row_end;
      break;
    }

    for (new_row = new_row_start, old_row = old_row_start; new_row <= new_row_end; new_row++) {
      int count = width >= new_cols ? new_cols : width;
      width -= count;

      int new_col = 0;

      while (count) {
        // TODO(vterm): This could surely be done a lot faster by memcpy()'ing the entire range
        new_buffer[new_row * new_cols + new_col] = old_buffer[old_row * old_cols + old_col];

        if (old_cursor.row == old_row && old_cursor.col == old_col) {
          new_cursor.row = new_row, new_cursor.col = new_col;
        }

        old_col++;
        if (old_col == old_cols) {
          old_row++;

          if (!do_reflow) {
            new_col++;
            break;
          }
          old_col = 0;
        }

        new_col++;
        count--;
      }

      if (old_row <= old_row_end && old_cursor.row == old_row && old_cursor.col >= old_col) {
        new_cursor.row = new_row, new_cursor.col = (old_cursor.col - old_col + new_col);
        if (new_cursor.col >= new_cols) {
          new_cursor.col = new_cols - 1;
        }
      }

      while (new_col < new_cols) {
        clearcell(screen, &new_buffer[new_row * new_cols + new_col]);
        new_col++;
      }

      new_lineinfo[new_row].continuation = (new_row > new_row_start)
                                           || (new_row == new_row_start
                                               && first_row_continuation);
    }

    old_row = orig_old_row_start - 1;
    new_row = new_row_start - 1;
  }

  // If all on-screen rows were blank (e.g., after all content was pushed to scrollback
  // during a previous resize), allow the pop section to use the entire screen.
  if (new_row < 0 && final_blank_row == 0) {
    new_row = new_rows - 1;
  }

  if (old_cursor.row <= old_row) {
    // cursor would have moved entirely off the top of the screen; lets just bring it within range
    new_cursor.row = 0, new_cursor.col = old_cursor.col;
    if (new_cursor.col >= new_cols) {
      new_cursor.col = new_cols - 1;
    }
  }

  // A blank cursor row at column 0 represents a real line break; do not let
  // boundary reflow merge it back into the preceding soft-wrapped line.
  if (active && new_cursor.row >= 0 && new_cursor.col == 0
      && line_popcount(new_buffer, new_cursor.row, new_rows, new_cols) == 0) {
    new_lineinfo[new_cursor.row].continuation = 0;
  }

  // We really expect the cursor position to be set by now
  if (active && (new_cursor.row == -1 || new_cursor.col == -1)) {
    fprintf(stderr, "screen_resize failed to update cursor position\n");
    abort();
  }

  if (old_row >= 0 && bufidx == BUFIDX_PRIMARY) {
    // Push spare lines to scrollback buffer (skip rows already pushed during reflow)
    if (screen->callbacks
        && (screen->callbacks->sb_pushline || screen->callbacks->sb_pushline_ex)) {
      if (screen->reflow && active && old_lineinfo) {
        for (int row = rows_pushed_in_reflow; row <= old_row; ) {
          int line_end = row;
          while (line_end + 1 <= old_row && old_lineinfo[line_end + 1].continuation) {
            line_end++;
          }

          push_reflowed_scrollback_line(screen, old_buffer, old_lineinfo, row, line_end, old_rows,
                                        old_cols, new_cols);
          row = line_end + 1;
        }
      } else {
        for (int row = rows_pushed_in_reflow; row <= old_row; row++) {
          const VTermLineInfo *li = old_lineinfo ? &old_lineinfo[row] : NULL;
          sb_pushline_from_row(screen, row, li);
        }
      }
    }
    if (active) {
      statefields->pos.row -= (old_row + 1 - rows_pushed_in_reflow);
    }
  }
  if (staged_prefix_count > 0) {
    for (int row = 0; row < staged_prefix_count; row++) {
      push_scrollback_row(screen, &staged_prefix_rows[row * new_cols], staged_prefix_counts[row],
                          new_cols, staged_prefix_continuation[row]);
    }
    vterm_allocator_free(screen->vt, staged_prefix_rows);
    vterm_allocator_free(screen->vt, staged_prefix_counts);
    vterm_allocator_free(screen->vt, staged_prefix_continuation);
  }
  if (bufidx == BUFIDX_PRIMARY) {
    int pop_cols = old_cols > new_cols ? old_cols : new_cols;
    reflow_scrollback_boundary(screen, new_buffer, new_lineinfo, new_rows, new_cols,
                               pop_cols, &new_row, &new_cursor, active);
  }
  if (new_row >= 0 && bufidx == BUFIDX_PRIMARY
      && screen->callbacks
      && (screen->callbacks->sb_popline_ex || screen->callbacks->sb_popline)) {
    if (screen->reflow) {
      // Reflow-aware scrollback pop: pop one complete logical line at a time,
      // then reflow it to the new width. Popping fixed-size raw row batches can
      // split a logical line in the middle, which corrupts hard line breaks when
      // that truncated suffix is later regrouped as its own line.
      int total_rows_placed = 0;
      int pop_cols = old_cols > new_cols ? old_cols : new_cols;

      while (new_row >= 0) {
        int line_cap = 8;
        int line_count = 0;

        // Staging buffers store popped rows newest-first.
        ScreenCell *line_buffer = vterm_allocator_malloc(screen->vt,
                                                         sizeof(*line_buffer)
                                                         * (size_t)line_cap
                                                         * (size_t)pop_cols);
        VTermScreenCell *line_vtcells = vterm_allocator_malloc(screen->vt,
                                                               sizeof(*line_vtcells)
                                                               * (size_t)line_cap
                                                               * (size_t)pop_cols);
        VTermLineInfo *line_li = vterm_allocator_malloc(screen->vt,
                                                        sizeof(*line_li)
                                                        * (size_t)line_cap);
        int *line_widths = vterm_allocator_malloc(screen->vt,
                                                  sizeof(*line_widths)
                                                  * (size_t)line_cap);
        int *line_storecols = vterm_allocator_malloc(screen->vt,
                                                     sizeof(*line_storecols)
                                                     * (size_t)line_cap);
        // Pop one full logical line from scrollback. Continuation metadata is on
        // the newer row, so keep popping until the oldest staged row is not a
        // continuation, or until scrollback is exhausted.
        while (true) {
          VTermLineInfo li = { 0 };
          int ok;
          int stored_cols = pop_cols;

          if (line_count == line_cap) {
            size_t old_cap = (size_t)line_cap;
            line_cap *= 2;
            line_buffer = grow_array(screen->vt, line_buffer,
                                     old_cap * (size_t)pop_cols,
                                     (size_t)line_cap * (size_t)pop_cols,
                                     sizeof(*line_buffer));
            line_vtcells = grow_array(screen->vt, line_vtcells,
                                      old_cap * (size_t)pop_cols,
                                      (size_t)line_cap * (size_t)pop_cols,
                                      sizeof(*line_vtcells));
            line_li = grow_array(screen->vt, line_li, old_cap, (size_t)line_cap,
                                 sizeof(*line_li));
            line_widths = grow_array(screen->vt, line_widths, old_cap,
                                     (size_t)line_cap, sizeof(*line_widths));
            line_storecols = grow_array(screen->vt, line_storecols, old_cap,
                                        (size_t)line_cap, sizeof(*line_storecols));
          }

          if (screen->callbacks->sb_popline_ex) {
            ok = screen->callbacks->sb_popline_ex(pop_cols, screen->sb_buffer,
                                                   &li, &stored_cols, screen->cbdata);
          } else {
            ok = screen->callbacks->sb_popline(pop_cols, screen->sb_buffer,
                                                screen->cbdata);
          }
          if (!ok) {
            break;
          }

          line_li[line_count] = li;
          line_storecols[line_count] = stored_cols;

          memcpy(&line_vtcells[line_count * pop_cols], screen->sb_buffer,
                 sizeof(VTermScreenCell) * (size_t)pop_cols);
          copy_vterm_row_to_screen(screen, screen->sb_buffer, stored_cols,
                                   &line_buffer[line_count * pop_cols], pop_cols);
          line_count++;

          if (!li.continuation) {
            break;
          }
        }

        if (line_count == 0) {
          vterm_allocator_free(screen->vt, line_buffer);
          vterm_allocator_free(screen->vt, line_vtcells);
          vterm_allocator_free(screen->vt, line_li);
          vterm_allocator_free(screen->vt, line_widths);
          vterm_allocator_free(screen->vt, line_storecols);
          break;
        }

        int width = 0;
        for (int row = line_count - 1; row >= 0; row--) {
          bool next_is_continuation = row > 0 && line_li[row - 1].continuation;
          line_widths[row] = staged_row_width(&line_buffer[row * pop_cols],
                                              line_storecols[row], next_is_continuation);
          width += line_widths[row];
        }

        int new_height = width ? (width + new_cols - 1) / new_cols : 1;
        int new_row_start = new_row - new_height + 1;

        if (new_row_start < 0
            && screen->callbacks
            && (screen->callbacks->sb_pushline_ex || screen->callbacks->sb_pushline)) {
          // This logical line does not fit in the remaining screen space.
          // Push it back exactly as it was popped so the scrollback state is
          // preserved for the next refresh.
          for (int row = line_count - 1; row >= 0; row--) {
            if (screen->callbacks->sb_pushline_ex) {
              screen->callbacks->sb_pushline_ex(line_storecols[row],
                                                &line_vtcells[row * pop_cols],
                                                &line_li[row], screen->cbdata);
            } else {
              screen->callbacks->sb_pushline(line_storecols[row],
                                             &line_vtcells[row * pop_cols],
                                             screen->cbdata);
            }
          }
          vterm_allocator_free(screen->vt, line_buffer);
          vterm_allocator_free(screen->vt, line_vtcells);
          vterm_allocator_free(screen->vt, line_li);
          vterm_allocator_free(screen->vt, line_widths);
          vterm_allocator_free(screen->vt, line_storecols);
          break;
        }

        int remaining = width;
        int src_row = line_count - 1;
        int src_col = 0;

        for (int dst_row = new_row_start; dst_row <= new_row; dst_row++) {
          int count = remaining >= new_cols ? new_cols : remaining;
          remaining -= count;
          int dst_col = 0;

          while (count) {
            new_buffer[dst_row * new_cols + dst_col] =
                line_buffer[src_row * pop_cols + src_col];

            src_col++;
            if (src_col >= line_widths[src_row]) {
              src_row--;
              src_col = 0;
            }

            dst_col++;
            count--;
          }

          while (dst_col < new_cols) {
            clearcell(screen, &new_buffer[dst_row * new_cols + dst_col]);
            dst_col++;
          }

          new_lineinfo[dst_row].continuation = (dst_row > new_row_start);
        }

        total_rows_placed += new_height;
        new_row = new_row_start - 1;

        vterm_allocator_free(screen->vt, line_buffer);
        vterm_allocator_free(screen->vt, line_vtcells);
        vterm_allocator_free(screen->vt, line_li);
        vterm_allocator_free(screen->vt, line_widths);
        vterm_allocator_free(screen->vt, line_storecols);
      }

      if (active) {
        statefields->pos.row += total_rows_placed;
      }
    } else {
      // Non-reflow: original behavior (raw copy without regrouping)
      while (new_row >= 0) {
        int ok;
        int stored_cols = old_cols;
        if (screen->callbacks->sb_popline_ex) {
          VTermLineInfo li = { 0 };
          ok = screen->callbacks->sb_popline_ex(old_cols, screen->sb_buffer,
                                                 &li, &stored_cols, screen->cbdata);
        } else if (screen->callbacks->sb_popline) {
          ok = screen->callbacks->sb_popline(old_cols, screen->sb_buffer,
                                              screen->cbdata);
        } else {
          break;
        }
        if (!ok) {
          break;
        }

        copy_vterm_row_to_screen(screen, screen->sb_buffer, stored_cols,
                                 &new_buffer[new_row * new_cols], new_cols);
        new_row--;

        if (active) {
          statefields->pos.row++;
        }
      }
    }
  }
  if (new_row >= 0) {
    // Scroll new rows back up to the top and fill in blanks at the bottom
    int moverows = new_rows - new_row - 1;
    memmove(&new_buffer[0], &new_buffer[(new_row + 1) * new_cols],
            (size_t)moverows * (size_t)new_cols * sizeof(ScreenCell));
    memmove(&new_lineinfo[0], &new_lineinfo[new_row + 1],
            (size_t)moverows * sizeof(new_lineinfo[0]));

    new_cursor.row -= (new_row + 1);

    for (new_row = moverows; new_row < new_rows; new_row++) {
      for (int col = 0; col < new_cols; col++) {
        clearcell(screen, &new_buffer[new_row * new_cols + col]);
      }
      new_lineinfo[new_row] = (VTermLineInfo){ 0 };
    }
  }

  vterm_allocator_free(screen->vt, old_buffer);
  screen->buffers[bufidx] = new_buffer;

  vterm_allocator_free(screen->vt, old_lineinfo);
  statefields->lineinfos[bufidx] = new_lineinfo;

  if (active) {
    statefields->pos = new_cursor;
  }
}

static int resize(int new_rows, int new_cols, VTermStateFields *fields, void *user)
{
  VTermScreen *screen = user;

  int altscreen_active = (screen->buffers[BUFIDX_ALTSCREEN]
                          && screen->buffer == screen->buffers[BUFIDX_ALTSCREEN]);

  int old_rows = screen->rows;
  int old_cols = screen->cols;

  if (new_cols > old_cols) {
    // Ensure that ->sb_buffer is large enough for a new or and old row
    if (screen->sb_buffer) {
      vterm_allocator_free(screen->vt, screen->sb_buffer);
    }

    screen->sb_buffer = vterm_allocator_malloc(screen->vt,
                                               sizeof(VTermScreenCell) * (size_t)new_cols);
  }

  resize_buffer(screen, 0, new_rows, new_cols, !altscreen_active, fields);
  if (screen->buffers[BUFIDX_ALTSCREEN]) {
    resize_buffer(screen, 1, new_rows, new_cols, altscreen_active, fields);
  } else if (new_rows != old_rows) {
    // We don't need a full resize of the altscreen because it isn't enabled but we should at least
    // keep the lineinfo the right size
    vterm_allocator_free(screen->vt, fields->lineinfos[BUFIDX_ALTSCREEN]);

    VTermLineInfo *new_lineinfo = vterm_allocator_malloc(screen->vt,
                                                         sizeof(new_lineinfo[0]) *
                                                         (size_t)new_rows);
    for (int row = 0; row < new_rows; row++) {
      new_lineinfo[row] = (VTermLineInfo){ 0 };
    }

    fields->lineinfos[BUFIDX_ALTSCREEN] = new_lineinfo;
  }

  screen->buffer =
    altscreen_active ? screen->buffers[BUFIDX_ALTSCREEN] : screen->buffers[BUFIDX_PRIMARY];

  screen->rows = new_rows;
  screen->cols = new_cols;

  // Reallocate pending_sb_lineinfo if rows changed
  if (new_rows != old_rows) {
    if (screen->pending_sb_lineinfo) {
      vterm_allocator_free(screen->vt, screen->pending_sb_lineinfo);
    }
    screen->pending_sb_lineinfo = vterm_allocator_malloc(screen->vt,
                                                         sizeof(VTermLineInfo) * (size_t)new_rows);
    screen->pending_sb_lineinfo_count = 0;
  }

  if (new_cols <= old_cols) {
    if (screen->sb_buffer) {
      vterm_allocator_free(screen->vt, screen->sb_buffer);
    }

    screen->sb_buffer = vterm_allocator_malloc(screen->vt,
                                               sizeof(VTermScreenCell) * (size_t)new_cols);
  }

  // TODO(vterm): Maaaaybe we can optimise this if there's no reflow happening
  damagescreen(screen);

  if (screen->callbacks && screen->callbacks->resize) {
    return (*screen->callbacks->resize)(new_rows, new_cols, screen->cbdata);
  }

  return 1;
}

static int theme(bool *dark, void *user)
{
  VTermScreen *screen = user;

  if (screen->callbacks && screen->callbacks->theme) {
    return (*screen->callbacks->theme)(dark, screen->cbdata);
  }

  return 1;
}

static int setlineinfo(int row, const VTermLineInfo *newinfo, const VTermLineInfo *oldinfo,
                       void *user)
{
  VTermScreen *screen = user;

  if (newinfo->doublewidth != oldinfo->doublewidth
      || newinfo->doubleheight != oldinfo->doubleheight) {
    for (int col = 0; col < screen->cols; col++) {
      ScreenCell *cell = getcell(screen, row, col);
      cell->pen.dwl = newinfo->doublewidth;
      cell->pen.dhl = newinfo->doubleheight;
    }

    VTermRect rect = {
      .start_row = row,
      .end_row = row + 1,
      .start_col = 0,
      .end_col = newinfo->doublewidth ? screen->cols / 2 : screen->cols,
    };
    damagerect(screen, rect);

    if (newinfo->doublewidth) {
      rect.start_col = screen->cols / 2;
      rect.end_col = screen->cols;

      erase_internal(rect, 0, user);
    }
  }

  return 1;
}

static int sb_clear(void *user)
{
  VTermScreen *screen = user;

  if (screen->callbacks && screen->callbacks->sb_clear) {
    if ((*screen->callbacks->sb_clear)(screen->cbdata)) {
      return 1;
    }
  }

  return 0;
}

// Called before lineinfo is shifted during scroll. Saves lineinfo for rows
// that will be pushed to scrollback, since the lineinfo will be overwritten
// by the memmove in state.c:scroll() before the scrollrect callback fires.
static int pre_scrollrect(VTermRect rect, int downward, int rightward, void *user)
{
  VTermScreen *screen = user;

  if (downward > 0 && rect.start_row == 0 && rect.start_col == 0
      && rect.end_col == screen->cols
      && screen->buffer == screen->buffers[BUFIDX_PRIMARY]
      && screen->callbacks
      && (screen->callbacks->sb_pushline_ex || screen->callbacks->sb_pushline)) {
    int count = downward < screen->rows ? downward : screen->rows;
    for (int row = 0; row < count; row++) {
      screen->pending_sb_lineinfo[row] =
          *vterm_state_get_lineinfo(screen->state, row);
    }
    screen->pending_sb_lineinfo_count = count;
  }
  return 0;
}

static VTermStateCallbacks state_cbs = {
  .putglyph = &putglyph,
  .movecursor = &movecursor,
  .scrollrect = &scrollrect,
  .erase = &erase,
  .setpenattr = &setpenattr,
  .settermprop = &settermprop,
  .bell = &bell,
  .resize = &resize,
  .theme = &theme,
  .setlineinfo = &setlineinfo,
  .sb_clear = &sb_clear,
  .pre_scrollrect = &pre_scrollrect,
};

static VTermScreen *screen_new(VTerm *vt)
{
  VTermState *state = vterm_obtain_state(vt);
  if (!state) {
    return NULL;
  }

  VTermScreen *screen = vterm_allocator_malloc(vt, sizeof(VTermScreen));
  int rows, cols;

  vterm_get_size(vt, &rows, &cols);

  screen->vt = vt;
  screen->state = state;

  screen->damage_merge = VTERM_DAMAGE_CELL;
  screen->damaged.start_row = -1;
  screen->pending_scrollrect.start_row = -1;

  screen->rows = rows;
  screen->cols = cols;

  screen->global_reverse = false;
  screen->reflow = false;

  screen->callbacks = NULL;
  screen->cbdata = NULL;

  screen->buffers[BUFIDX_PRIMARY] = alloc_buffer(screen, rows, cols);

  screen->buffer = screen->buffers[BUFIDX_PRIMARY];

  screen->sb_buffer = vterm_allocator_malloc(screen->vt, sizeof(VTermScreenCell) * (size_t)cols);

  screen->pending_sb_lineinfo = vterm_allocator_malloc(screen->vt,
                                                       sizeof(VTermLineInfo) * (size_t)rows);
  screen->pending_sb_lineinfo_count = 0;

  vterm_state_set_callbacks(screen->state, &state_cbs, screen);

  return screen;
}

void vterm_screen_free(VTermScreen *screen)
{
  vterm_allocator_free(screen->vt, screen->buffers[BUFIDX_PRIMARY]);
  if (screen->buffers[BUFIDX_ALTSCREEN]) {
    vterm_allocator_free(screen->vt, screen->buffers[BUFIDX_ALTSCREEN]);
  }

  vterm_allocator_free(screen->vt, screen->sb_buffer);
  vterm_allocator_free(screen->vt, screen->pending_sb_lineinfo);

  vterm_allocator_free(screen->vt, screen);
}

void vterm_screen_reset(VTermScreen *screen, int hard)
{
  screen->damaged.start_row = -1;
  screen->pending_scrollrect.start_row = -1;
  vterm_state_reset(screen->state, hard);
  vterm_screen_flush_damage(screen);
}

// Copy internal to external representation of a screen cell
int vterm_screen_get_cell(const VTermScreen *screen, VTermPos pos, VTermScreenCell *cell)
{
  ScreenCell *intcell = getcell(screen, pos.row, pos.col);
  if (!intcell) {
    return 0;
  }

  cell->schar = (intcell->schar == (uint32_t)-1) ? 0 : intcell->schar;

  cell->attrs.bold = intcell->pen.bold;
  cell->attrs.underline = intcell->pen.underline;
  cell->attrs.italic = intcell->pen.italic;
  cell->attrs.blink = intcell->pen.blink;
  cell->attrs.reverse = intcell->pen.reverse ^ screen->global_reverse;
  cell->attrs.conceal = intcell->pen.conceal;
  cell->attrs.strike = intcell->pen.strike;
  cell->attrs.font = intcell->pen.font;
  cell->attrs.small = intcell->pen.small;
  cell->attrs.baseline = intcell->pen.baseline;
  cell->attrs.dim = intcell->pen.dim;
  cell->attrs.overline = intcell->pen.overline;

  cell->attrs.dwl = intcell->pen.dwl;
  cell->attrs.dhl = intcell->pen.dhl;

  cell->fg = intcell->pen.fg;
  cell->bg = intcell->pen.bg;

  cell->uri = intcell->pen.uri;

  if (pos.col < (screen->cols - 1)
      && getcell(screen, pos.row, pos.col + 1)->schar == (uint32_t)-1) {
    cell->width = 2;
  } else {
    cell->width = 1;
  }

  return 1;
}

VTermScreen *vterm_obtain_screen(VTerm *vt)
{
  if (vt->screen) {
    return vt->screen;
  }

  VTermScreen *screen = screen_new(vt);
  vt->screen = screen;

  return screen;
}

void vterm_screen_enable_reflow(VTermScreen *screen, bool reflow)
{
  screen->reflow = reflow;
}

#undef vterm_screen_set_reflow
void vterm_screen_set_reflow(VTermScreen *screen, bool reflow)
{
  vterm_screen_enable_reflow(screen, reflow);
}

void vterm_screen_enable_altscreen(VTermScreen *screen, int altscreen)
{
  if (!screen->buffers[BUFIDX_ALTSCREEN] && altscreen) {
    int rows, cols;
    vterm_get_size(screen->vt, &rows, &cols);

    screen->buffers[BUFIDX_ALTSCREEN] = alloc_buffer(screen, rows, cols);
  }
}

void vterm_screen_set_callbacks(VTermScreen *screen, const VTermScreenCallbacks *callbacks,
                                void *user)
{
  screen->callbacks = callbacks;
  screen->cbdata = user;
}

void vterm_screen_set_unrecognised_fallbacks(VTermScreen *screen,
                                             const VTermStateFallbacks *fallbacks, void *user)
{
  vterm_state_set_unrecognised_fallbacks(screen->state, fallbacks, user);
}

void vterm_screen_flush_damage(VTermScreen *screen)
{
  if (screen->pending_scrollrect.start_row != -1) {
    vterm_scroll_rect(screen->pending_scrollrect, screen->pending_scroll_downward,
                      screen->pending_scroll_rightward,
                      moverect_user, erase_user, screen);

    screen->pending_scrollrect.start_row = -1;
  }

  if (screen->damaged.start_row != -1) {
    if (screen->callbacks && screen->callbacks->damage) {
      (*screen->callbacks->damage)(screen->damaged, screen->cbdata);
    }

    screen->damaged.start_row = -1;
  }
}

void vterm_screen_set_damage_merge(VTermScreen *screen, VTermDamageSize size)
{
  vterm_screen_flush_damage(screen);
  screen->damage_merge = size;
}

/// Same as vterm_state_convert_color_to_rgb(), but takes a `screen` instead of a `state` instance.
void vterm_screen_convert_color_to_rgb(const VTermScreen *screen, VTermColor *col)
{
  vterm_state_convert_color_to_rgb(screen->state, col);
}

// Some utility functions on VTermRect structures

#define STRFrect "(%d,%d-%d,%d)"
#define ARGSrect(r) (r).start_row, (r).start_col, (r).end_row, (r).end_col

// Expand dst to contain src as well
void rect_expand(VTermRect *dst, VTermRect *src)
{
  if (dst->start_row > src->start_row) {
    dst->start_row = src->start_row;
  }
  if (dst->start_col > src->start_col) {
    dst->start_col = src->start_col;
  }
  if (dst->end_row < src->end_row) {
    dst->end_row = src->end_row;
  }
  if (dst->end_col < src->end_col) {
    dst->end_col = src->end_col;
  }
}

// Clip the dst to ensure it does not step outside of bounds
void rect_clip(VTermRect *dst, VTermRect *bounds)
{
  if (dst->start_row < bounds->start_row) {
    dst->start_row = bounds->start_row;
  }
  if (dst->start_col < bounds->start_col) {
    dst->start_col = bounds->start_col;
  }
  if (dst->end_row > bounds->end_row) {
    dst->end_row = bounds->end_row;
  }
  if (dst->end_col > bounds->end_col) {
    dst->end_col = bounds->end_col;
  }
  // Ensure it doesn't end up negatively-sized
  if (dst->end_row < dst->start_row) {
    dst->end_row = dst->start_row;
  }
  if (dst->end_col < dst->start_col) {
    dst->end_col = dst->start_col;
  }
}

// True if the two rectangles are equal
int rect_equal(VTermRect *a, VTermRect *b)
{
  return (a->start_row == b->start_row)
         && (a->start_col == b->start_col)
         && (a->end_row == b->end_row)
         && (a->end_col == b->end_col);
}

// True if small is contained entirely within big
int rect_contains(VTermRect *big, VTermRect *small)
{
  if (small->start_row < big->start_row) {
    return 0;
  }
  if (small->start_col < big->start_col) {
    return 0;
  }
  if (small->end_row > big->end_row) {
    return 0;
  }
  if (small->end_col > big->end_col) {
    return 0;
  }
  return 1;
}

// True if the rectangles overlap at all
int rect_intersects(VTermRect *a, VTermRect *b)
{
  if (a->start_row > b->end_row || b->start_row > a->end_row) {
    return 0;
  }
  if (a->start_col > b->end_col || b->start_col > a->end_col) {
    return 0;
  }
  return 1;
}
