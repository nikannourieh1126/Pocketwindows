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

#define BX_WITH_WASMCANVAS 1
#define _MULTI_THREAD
#define BX_PLUGGABLE

#include "bochs.h"
#include "param_names.h"
#include "keymap.h"
#include "iodev.h"

#include <stdlib.h>
#include <emscripten/emscripten.h>

#include "icon_bochs.h"
#include "gui/gui.h"

class bx_wasmcanvas_gui_c : public bx_gui_c {
public:
  bx_wasmcanvas_gui_c();
  DECLARE_GUI_VIRTUAL_METHODS()
  virtual void draw_char(Bit8u ch, Bit8u fc, Bit8u bc, Bit16u xc, Bit16u yc,
                         Bit8u fw, Bit8u fh, Bit8u fx, Bit8u fy,
                         bool gfxcharw9, Bit8u cs, Bit8u ce, bool curs, bool font2);
};

static bx_wasmcanvas_gui_c *theGui = NULL;
IMPLEMENT_GUI_PLUGIN_CODE(wasmcanvas)

#define LOG_THIS theGui->

static unsigned res_x = 0, res_y = 0;
static unsigned disp_bpp = 8;
static Bit8u* framebuffer = NULL;
static unsigned headerbar_height = 0;
static Bit32u wasm_palette[256];

bx_wasmcanvas_gui_c::bx_wasmcanvas_gui_c()
{
  theGui = this;
}

void bx_wasmcanvas_gui_c::specific_init(int argc, char **argv, unsigned header_bar_y)
{
  BX_INFO(("WASM Canvas GUI specific_init called"));
  headerbar_height = header_bar_y;
  new_text_api = 1;
}

void bx_wasmcanvas_gui_c::dimension_update(unsigned x, unsigned y, unsigned fheight, unsigned fwidth, unsigned bpp)
{
  BX_INFO(("WASM Canvas dimension_update: %dx%d, bpp=%d", x, y, bpp));
  res_x = x;
  res_y = y;
  disp_bpp = bpp;
  
  if (framebuffer) {
    free(framebuffer);
  }

  // Allocate framebuffer: 4 bytes per pixel (RGBA)
  framebuffer = (Bit8u*)malloc(x * y * 4);
  if (!framebuffer) {
    BX_PANIC(("Failed to allocate framebuffer"));
    return;
  }
  
  // Initialize to black
  memset(framebuffer, 0, x * y * 4);
  
  // Notify JS of dimension change
  EM_ASM({
    if (Module.onDimensionChange) {
      Module.onDimensionChange($0, $1);
    }
  }, res_x, res_y + headerbar_height);
}

bool bx_wasmcanvas_gui_c::palette_change(Bit8u index, Bit8u red, Bit8u green, Bit8u blue)
{
  wasm_palette[index] = (255 << 24) | (blue << 16) | (green << 8) | red;
  return 0;
}

void bx_wasmcanvas_gui_c::draw_char(Bit8u ch, Bit8u fc, Bit8u bc, Bit16u xc, Bit16u yc,
                                    Bit8u fw, Bit8u fh, Bit8u fx, Bit8u fy,
                                    bool gfxcharw9, Bit8u cs, Bit8u ce, bool curs, bool font2)
{
  if (!framebuffer) return;

  Bit8u *font_ptr = (font2) ? &vga_charmap[1][(ch << 5) + fy]
                            : &vga_charmap[0][(ch << 5) + fy];

  Bit32u fgcolor = wasm_palette[fc];
  Bit32u bgcolor = wasm_palette[bc];

  for (Bit8u h = 0; h < fh; h++, fy++) {
    Bit32u *buf = (Bit32u*)framebuffer + (yc + h) * res_x + xc;
    Bit16u font_row = *font_ptr++;

    if (gfxcharw9) {
      font_row = (font_row << 1) | (font_row & 0x01);
    }

    bool draw_cursor = (curs && (fy >= cs) && (fy <= ce));

    // bit 7 is the left-most pixel (0x80 or 0x100 if gfxcharw9 shifted)
    Bit16u bit_mask = gfxcharw9 ? 0x100 : 0x80;
    if (fx > 0) {
      bit_mask >>= fx;
    }

    for (Bit8u w = 0; w < fw; w++) {
      bool bit = (font_row & bit_mask) != 0;
      if (draw_cursor) bit = !bit;
      buf[w] = bit ? fgcolor : bgcolor;
      bit_mask >>= 1;
    }
  }
}

void bx_wasmcanvas_gui_c::text_update(Bit8u *old_text, Bit8u *new_text,
                                      unsigned long cursor_x,
                                      unsigned long cursor_y,
                                      bx_vga_tminfo_t *tm_info)
{
  text_update_common(old_text, new_text, cursor_x, tm_info);
  flush();
}

void bx_wasmcanvas_gui_c::graphics_tile_update(Bit8u *snapshot, unsigned x, unsigned y)
{
  if (!framebuffer) return;
  
  Bit32u *buf = (Bit32u*)framebuffer + y * res_x + x;
  int i = y_tilesize;
  if (i + y > res_y) i = res_y - y;
  
  switch (disp_bpp) {
    case 8:
      do {
        Bit32u *buf_row = buf;
        int j = x_tilesize;
        do {
          Bit8u pixel = *snapshot++;
          Bit32u color = wasm_palette[pixel];
          *buf++ = color;
        } while(--j);
        buf = buf_row + res_x;
      } while(--i);
      break;
    case 16:
      do {
        Bit32u *buf_row = buf;
        int j = x_tilesize;
        do {
          Bit16u pixel = *(Bit16u*)snapshot;
          snapshot += 2;
          Bit8u r = (pixel >> 11) & 0x1F;
          Bit8u g = (pixel >> 5) & 0x3F;
          Bit8u b = pixel & 0x1F;
          Bit32u color = ((r << 3) | (r >> 2)) << 16 |
                         ((g << 2) | (g >> 4)) << 8 |
                         ((b << 3) | (b >> 2));
          *buf++ = color;
        } while(--j);
        buf = buf_row + res_x;
      } while(--i);
      break;
    case 24:
      do {
        Bit32u *buf_row = buf;
        int j = x_tilesize;
        do {
          Bit32u color = snapshot[0] << 16 | snapshot[1] << 8 | snapshot[2];
          snapshot += 3;
          *buf++ = color;
        } while(--j);
        buf = buf_row + res_x;
      } while(--i);
      break;
    case 32:
      do {
        Bit32u *buf_row = buf;
        int j = x_tilesize;
        do {
          Bit32u color = snapshot[0] << 16 | snapshot[1] << 8 | snapshot[2];
          snapshot += 4;
          *buf++ = color;
        } while(--j);
        buf = buf_row + res_x;
      } while(--i);
      break;
    default:
      BX_PANIC(("%u bpp not implemented", disp_bpp));
      return;
  }
}

void bx_wasmcanvas_gui_c::flush(void)
{
  if (!framebuffer || res_x == 0 || res_y == 0) return;
  
  EM_ASM({
    if (Module.onFrame) {
      var ptr = $0;
      var w = $1;
      var h = $2;
      var fbBytes = w * h * 4;
      var fbData = new Uint8Array(Module.HEAPU8.buffer, ptr, fbBytes);
      var clamped = new Uint8ClampedArray(fbData.slice());
      var imageData = new ImageData(clamped, w, h);
      Module.onFrame(imageData);
    }
  }, (uintptr_t)framebuffer, res_x, res_y);
}

void bx_wasmcanvas_gui_c::clear_screen(void)
{
  if (framebuffer) {
    memset(framebuffer, 0, res_x * res_y * 4);
  }
}

void bx_wasmcanvas_gui_c::handle_events(void)
{
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
}

void bx_wasmcanvas_gui_c::mouse_enabled_changed_specific(bool val)
{
}

int bx_wasmcanvas_gui_c::get_clipboard_text(Bit8u **bytes, Bit32s *nbytes)
{
  return 0;
}

int bx_wasmcanvas_gui_c::set_clipboard_text(char *snapshot, Bit32u len)
{
  return 0;
}

extern "C" {
EMSCRIPTEN_KEEPALIVE void bx_wasm_key_event(Bit32u key, bool release) {
  if (release)
    DEV_kbd_gen_scancode(key | BX_KEY_RELEASED);
  else
    DEV_kbd_gen_scancode(key);
}

EMSCRIPTEN_KEEPALIVE void bx_wasm_mouse_event(int x, int y, int z, int button_state, bool abs_mode) {
  DEV_mouse_motion(x, y, z, button_state, abs_mode);
}
}
