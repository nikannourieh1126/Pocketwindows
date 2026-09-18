/////////////////////////////////////////////////////////////////////////
// $Id$
/////////////////////////////////////////////////////////////////////////
//
//  Copyright (C) 2024-2026  The Bochs Project
//
//  This library is free software; you can redistribute it and/or
//  modify it under the terms of the GNU Lesser General Public
//  License as published by the Free Software Foundation; either
//  version 2 of the License, or (at your option) any later version.
//
//  This library is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//  Lesser General Public License for more details.
//
//  You should have received a copy of the GNU Lesser General Public
//  License along with this library; if not, write to the Free Software
//  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA

#define BX_PLUGGABLE

#include "bochs.h"
#include "gui/gui.h"
#include "plugin.h"
#include "param_names.h"
#include "keymap.h"
#include "iodev.h"

#include <stdlib.h>
#include <stdio.h>
#include <emscripten/emscripten.h>

#include "icon_bochs.h"

class bx_wasmcanvas_gui_c : public bx_gui_c {
public:
  bx_wasmcanvas_gui_c();
  DECLARE_GUI_VIRTUAL_METHODS()
  DECLARE_GUI_NEW_VIRTUAL_METHODS()
  virtual void draw_char(Bit8u ch, Bit8u fc, Bit8u bc, Bit16u xc, Bit16u yc,
                         Bit8u fw, Bit8u fh, Bit8u fx, Bit8u fy,
                         bool gfxcharw9, Bit8u cs, Bit8u ce, bool curs, bool font2);
};

static bx_wasmcanvas_gui_c *theGui = NULL;

PLUGIN_ENTRY_FOR_GUI_MODULE(wasmcanvas)
{
  if (mode == PLUGIN_INIT) {
    genlog->info("installing wasmcanvas module as the Bochs GUI");
    theGui = new bx_wasmcanvas_gui_c ();
    bx_gui = theGui;
  } else if (mode == PLUGIN_FINI) {
    delete theGui;
    bx_gui = NULL;
  } else if (mode == PLUGIN_PROBE) {
    return (int)PLUGTYPE_GUI;
  }
  return 0;
}

#define LOG_THIS theGui->

static unsigned res_x = 720, res_y = 400;
static Bit8u* framebuffer = NULL;
static unsigned headerbar_height = 0;
static Bit32u wasm_palette[256];
static bool frame_dirty = false;
static bool mic_active = false;
static double last_flush_time = 0.0;

static unsigned dirty_min_x = 0, dirty_min_y = 0;
static unsigned dirty_max_x = 0, dirty_max_y = 0;

static void mark_dirty_rect(unsigned x, unsigned y, unsigned w, unsigned h)
{
  if (!frame_dirty) {
    dirty_min_x = x;
    dirty_min_y = y;
    dirty_max_x = x + w;
    dirty_max_y = y + h;
    frame_dirty = true;
  } else {
    if (x < dirty_min_x) dirty_min_x = x;
    if (y < dirty_min_y) dirty_min_y = y;
    if (x + w > dirty_max_x) dirty_max_x = x + w;
    if (y + h > dirty_max_y) dirty_max_y = y + h;
  }
}

extern "C" {
  EMSCRIPTEN_KEEPALIVE
  void bx_wasm_key_event(Bit32u key_code, bool is_press) {
    DEV_kbd_gen_scancode(key_code | (is_press ? 0 : BX_KEY_RELEASED));
  }

  EMSCRIPTEN_KEEPALIVE
  void bx_wasm_mouse_event(int delta_x, int delta_y, int buttons) {
    DEV_mouse_motion(delta_x, delta_y, 0, buttons, 0);
  }

  EMSCRIPTEN_KEEPALIVE
  void bx_wasm_audio_output(int16_t *pcm_data, int sample_count) {
    if (!pcm_data || sample_count <= 0) return;
    EM_ASM({
      if (Module.onAudioOutput) {
        var len = $1 * 2;
        var audioBytes = Module.HEAPU8.subarray($0, $0 + len);
        Module.onAudioOutput(audioBytes);
      }
    }, (unsigned)pcm_data, sample_count);
  }

  EMSCRIPTEN_KEEPALIVE
  void bx_wasm_mic_input(Bit8u *pcm_data, int len) {
    if (!pcm_data || len <= 0) return;
  }

  EMSCRIPTEN_KEEPALIVE
  void bx_wasm_request_mic(bool enable) {
    if (mic_active == enable) return;
    mic_active = enable;
    EM_ASM({
      if (Module.onRequestMic) {
        Module.onRequestMic($0);
      }
    }, enable ? 1 : 0);
  }
}

bx_wasmcanvas_gui_c::bx_wasmcanvas_gui_c()
{
  theGui = this;
}

void bx_wasmcanvas_gui_c::specific_init(int argc, char **argv, unsigned headerbar_y)
{
  put("WASMCANVAS");
  headerbar_height = headerbar_y;
  new_text_api = 1;
  host_bpp = 32;
  host_xres = res_x;
  host_yres = res_y;
  host_pitch = res_x * 4;

  if (x_tilesize == 0) x_tilesize = 16;
  if (y_tilesize == 0) y_tilesize = 24;

  framebuffer = (Bit8u*)malloc(res_x * res_y * 4);
  if (framebuffer) {
    memset(framebuffer, 0, res_x * res_y * 4);
  }
  mark_dirty_rect(0, 0, res_x, res_y);

  EM_ASM({
    if (Module.onDimensionChange) {
      Module.onDimensionChange($0, $1);
    }
  }, res_x, res_y);
}

void bx_wasmcanvas_gui_c::handle_events(void)
{
  if (frame_dirty) {
    flush();
  }
}

void bx_wasmcanvas_gui_c::flush(void)
{
  if (!framebuffer || !frame_dirty) return;

  double now = emscripten_get_now();
  if (now - last_flush_time < 16.0) {
    // Throttle frame flush to max 60 real FPS (~16.6ms wall-clock interval)
    // Dirty rect and frame_dirty remain set to accumulate for next flush
    return;
  }
  last_flush_time = now;

  if (dirty_max_x > res_x) dirty_max_x = res_x;
  if (dirty_max_y > res_y) dirty_max_y = res_y;
  if (dirty_min_x >= dirty_max_x || dirty_min_y >= dirty_max_y) {
    dirty_min_x = 0; dirty_min_y = 0;
    dirty_max_x = res_x; dirty_max_y = res_y;
  }

  unsigned dw = dirty_max_x - dirty_min_x;
  unsigned dh = dirty_max_y - dirty_min_y;

  EM_ASM({
    if (Module.onFrame) {
      var fbBytes = $1 * $2 * 4;
      var fbData = Module.HEAPU8.subarray($0, $0 + fbBytes);
      Module.onFrame(fbData, $1, $2, $3, $4, $5, $6);
    }
  }, (unsigned)framebuffer, res_x, res_y, dirty_min_x, dirty_min_y, dw, dh);

  frame_dirty = false;
}

void bx_wasmcanvas_gui_c::clear_screen(void)
{
  if (framebuffer) {
    memset(framebuffer, 0, res_x * res_y * 4);
    mark_dirty_rect(0, 0, res_x, res_y);
  }
}

void bx_wasmcanvas_gui_c::draw_char(Bit8u ch, Bit8u fc, Bit8u bc, Bit16u xc, Bit16u yc,
                                    Bit8u fw, Bit8u fh, Bit8u fx, Bit8u fy,
                                    bool gfxcharw9, Bit8u cs, Bit8u ce, bool curs, bool font2)
{
  Bit32u *buf;
  Bit8u font_row;
  Bit8u *font_ptr;
  Bit32u fgcolor, bgcolor;

  if (!framebuffer) return;

  fgcolor = wasm_palette[fc];
  bgcolor = wasm_palette[bc];

  font_ptr = font2 ? &vga_charmap[1][(ch << 5) + fy] : &vga_charmap[0][(ch << 5) + fy];

  for (Bit8u h = 0; h < fh; h++, fy++) {
    font_row = *font_ptr++;
    buf = (Bit32u*)framebuffer + (yc + h) * res_x + xc;
    bool is_cursor_row = curs && (fy >= cs) && (fy <= ce);

    for (Bit8u w = 0; w < fw; w++) {
      bool bit;
      if (w < 8) {
        bit = (font_row & (0x80 >> w)) != 0;
      } else {
        bit = gfxcharw9 ? ((font_row & 0x01) != 0) : false;
      }
      if (is_cursor_row) bit = !bit;

      *buf++ = bit ? fgcolor : bgcolor;
    }
  }

  mark_dirty_rect(xc, yc, fw, fh);
}

void bx_wasmcanvas_gui_c::text_update(Bit8u *old_text, Bit8u *new_text,
                                      unsigned long cursor_x, unsigned long cursor_y,
                                      bx_vga_tminfo_t *tm_info)
{
  Bit16u cursor_address = (Bit16u)(cursor_y * tm_info->line_offset + cursor_x);
  text_update_common(old_text, new_text, cursor_address, tm_info);
  flush();
}

int bx_wasmcanvas_gui_c::get_clipboard_text(Bit8u **bytes, Bit32s *nbytes)
{
  return 0;
}

int bx_wasmcanvas_gui_c::set_clipboard_text(char *text_snapshot, Bit32u len)
{
  return 0;
}

bool bx_wasmcanvas_gui_c::palette_change(Bit8u index, Bit8u red, Bit8u green, Bit8u blue)
{
  wasm_palette[index] = 0xFF000000 | (blue << 16) | (green << 8) | red;
  mark_dirty_rect(0, 0, res_x, res_y);
  return 0;
}

bx_svga_tileinfo_t *bx_wasmcanvas_gui_c::graphics_tile_info(bx_svga_tileinfo_t *info)
{
  if (!info) {
    info = new bx_svga_tileinfo_t;
    if (!info) return NULL;
  }
  info->bpp = 32;
  info->pitch = res_x * 4;
  info->red_shift = 0;
  info->green_shift = 8;
  info->blue_shift = 16;
  info->red_mask = 0x000000ff;
  info->green_mask = 0x0000ff00;
  info->blue_mask = 0x00ff0000;
  info->is_indexed = 0;
  info->is_little_endian = 1;
  return info;
}

Bit8u *bx_wasmcanvas_gui_c::graphics_tile_get(unsigned x0, unsigned y0, unsigned *w, unsigned *h)
{
  x_tilesize = 16;
  y_tilesize = 24;

  if (x0 + x_tilesize > res_x) *w = (x0 < res_x) ? (res_x - x0) : 0;
  else *w = x_tilesize;

  if (y0 + y_tilesize > res_y) *h = (y0 < res_y) ? (res_y - y0) : 0;
  else *h = y_tilesize;

  if (!framebuffer) return NULL;
  return framebuffer + y0 * (res_x * 4) + x0 * 4;
}

void bx_wasmcanvas_gui_c::graphics_tile_update_in_place(unsigned x0, unsigned y0, unsigned w, unsigned h)
{
  if (!framebuffer) return;
  for (unsigned y = y0; y < y0 + h && y < res_y; y++) {
    Bit32u *buf = (Bit32u*)framebuffer + y * res_x + x0;
    for (unsigned x = 0; x < w && (x0 + x) < res_x; x++) {
      buf[x] |= 0xFF000000;
    }
  }
  mark_dirty_rect(x0, y0, w, h);
}

void bx_wasmcanvas_gui_c::graphics_tile_update(Bit8u *snapshot, unsigned x, unsigned y)
{
  if (!framebuffer) return;

  if (x_tilesize == 0) x_tilesize = 16;
  if (y_tilesize == 0) y_tilesize = 24;

  Bit32u *buf = (Bit32u*)framebuffer + y * res_x + x;
  int i = y_tilesize;
  if (i + y > res_y) i = res_y - y;
  if (i <= 0) return;

  int tile_w = x_tilesize;
  if (tile_w + x > res_x) tile_w = res_x - x;
  if (tile_w <= 0) return;

  switch (guest_bpp) {
    case 8:
      do {
        Bit32u *buf_row = buf;
        int j = tile_w;
        do {
          Bit8u pixel = *snapshot++;
          Bit32u color = wasm_palette[pixel];
          *buf++ = color;
        } while(--j);
        snapshot += (x_tilesize - tile_w);
        buf = buf_row + res_x;
      } while(--i);
      break;
    case 16:
      do {
        Bit32u *buf_row = buf;
        int j = tile_w;
        do {
          Bit16u pixel = *(Bit16u*)snapshot;
          snapshot += 2;
          Bit8u r = (pixel >> 11) & 0x1F;
          Bit8u g = (pixel >> 5) & 0x3F;
          Bit8u b = pixel & 0x1F;
          Bit32u color = 0xFF000000 |
                         (((b << 3) | (b >> 2)) << 16) |
                         (((g << 2) | (g >> 4)) << 8) |
                         ((r << 3) | (r >> 2));
          *buf++ = color;
        } while(--j);
        snapshot += (x_tilesize - tile_w) * 2;
        buf = buf_row + res_x;
      } while(--i);
      break;
    case 24:
      do {
        Bit32u *buf_row = buf;
        int j = tile_w;
        do {
          Bit32u color = 0xFF000000 | (snapshot[2] << 16) | (snapshot[1] << 8) | snapshot[0];
          snapshot += 3;
          *buf++ = color;
        } while(--j);
        snapshot += (x_tilesize - tile_w) * 3;
        buf = buf_row + res_x;
      } while(--i);
      break;
    case 32:
      do {
        Bit32u *buf_row = buf;
        int j = tile_w;
        do {
          Bit32u color = 0xFF000000 | (snapshot[2] << 16) | (snapshot[1] << 8) | snapshot[0];
          snapshot += 4;
          *buf++ = color;
        } while(--j);
        snapshot += (x_tilesize - tile_w) * 4;
        buf = buf_row + res_x;
      } while(--i);
      break;
    default:
      break;
  }
  mark_dirty_rect(x, y, tile_w, y_tilesize - i + (y_tilesize - (y_tilesize - i)));
}

void bx_wasmcanvas_gui_c::dimension_update(unsigned x, unsigned y, unsigned fheight, unsigned fwidth, unsigned bpp)
{
  guest_textmode = (fheight > 0);
  guest_fwidth = fwidth;
  guest_fheight = fheight;
  guest_bpp = bpp;

  res_x = x;
  host_xres = x;
  guest_xres = x;

  res_y = y;
  host_yres = y;
  guest_yres = y;

  host_pitch = res_x * 4;
  host_bpp = 32;

  if (framebuffer) free(framebuffer);
  framebuffer = (Bit8u*)malloc(res_x * res_y * 4);
  if (framebuffer) {
    memset(framebuffer, 0, res_x * res_y * 4);
  }
  mark_dirty_rect(0, 0, res_x, res_y);

  EM_ASM({
    if (Module.onDimensionChange) {
      Module.onDimensionChange($0, $1);
    }
  }, res_x, res_y);
}

unsigned bx_wasmcanvas_gui_c::create_bitmap(const unsigned char *bmap, unsigned xdim, unsigned ydim)
{
  return 0;
}

unsigned bx_wasmcanvas_gui_c::headerbar_bitmap(unsigned bmap_id, unsigned alignment, void (*f)(void))
{
  return 0;
}

void bx_wasmcanvas_gui_c::show_headerbar(void)
{
}

void bx_wasmcanvas_gui_c::replace_bitmap(unsigned hbar_id, unsigned bmap_id)
{
}

void bx_wasmcanvas_gui_c::exit(void)
{
  if (framebuffer) {
    free(framebuffer);
    framebuffer = NULL;
  }
}

void bx_wasmcanvas_gui_c::mouse_enabled_changed_specific(bool val)
{
}
