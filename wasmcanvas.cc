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

#define _MULTI_THREAD

// Define BX_PLUGGABLE in files that can be compiled into plugins.  For
// platforms that require a special tag on exported symbols, BX_PLUGGABLE
// is used to know when we are exporting symbols and when we are importing.
#define BX_PLUGGABLE

#include "bochs.h"
#include "param_names.h"
#include "keymap.h"
#include "iodev.h"
#if BX_WITH_WASMCANVAS

#include <stdlib.h>
#include <emscripten/emscripten.h>

#include "icon_bochs.h"
#include "gui/gui.h"

class bx_wasmcanvas_gui_c : public bx_gui_c {
public:
  bx_wasmcanvas_gui_c();
  DECLARE_GUI_VIRTUAL_METHODS()
  DECLARE_GUI_NEW_VIRTUAL_METHODS()
  virtual void draw_char(Bit8u ch, Bit8u fc, Bit8u bc, Bit16u xc, Bit16u yc,
                         Bit8u fw, Bit8u fh, Bit8u fx, Bit8u fy,
                         bool gfxcharw9, Bit8u cs, Bit8u ce, bool curs, bool font2);
  virtual void text_update(Bit8u *old_text, Bit8u *new_text,
                          unsigned long cursor_x,
                          unsigned long cursor_y,
                          bx_vga_tminfo_t *tm_info);
};

// declare one instance of the gui object and call macro to insert the
// plugin code
static bx_wasmcanvas_gui_c *theGui = NULL;
IMPLEMENT_GUI_PLUGIN_CODE(wasmcanvas)

#define LOG_THIS theGui->

static unsigned res_x, res_y;
static unsigned disp_bpp;
static Bit8u* framebuffer = NULL;
static unsigned headerbar_height = 0;
static Uint32 wasm_palette[256];

bx_wasmcanvas_gui_c::bx_wasmcanvas_gui_c()
{
  theGui = this;
}

void bx_wasmcanvas_gui_c::init(unsigned xres, unsigned yres)
{
  BX_INFO(("WASM Canvas GUI initialized"));
  res_x = xres;
  res_y = yres;
  
  // Allocate framebuffer: 4 bytes per pixel (RGBA)
  framebuffer = (Bit8u*)malloc(xres * yres * 4);
  if (!framebuffer) {
    BX_PANIC(("Failed to allocate framebuffer"));
    return;
  }
  
  // Initialize to black
  memset(framebuffer, 0, xres * yres * 4);
  
  // Notify JS of dimension change
  EM_ASM({
    if (Module.onDimensionChange) {
      Module.onDimensionChange($0, $1);
    }
  }, res_x, res_y + headerbar_height);
}

void bx_wasmcanvas_gui_c::cleanup(void)
{
  if (framebuffer) {
    free(framebuffer);
    framebuffer = NULL;
  }
}

bool bx_wasmcanvas_gui_c::palette_change(Bit8u index, Bit8u red, Bit8u green, Bit8u blue)
{
  wasm_palette[index] = (red << 16) | (green << 8) | blue;
  return 0;
}

void bx_wasmcanvas_gui_c::draw_char(Bit8u ch, Bit8u fc, Bit8u bc, Bit16u xc, Bit16u yc,
                                    Bit8u fw, Bit8u fh, Bit8u fx, Bit8u fy,
                                    bool gfxcharw9, Bit8u cs, Bit8u ce, bool curs, bool font2)
{
  Bit32u *buf;
  Bit16u font_row, mask;
  Bit8u *font_ptr, fontpixels;
  Bit32u fgcolor, bgcolor;
  
  if (!framebuffer) return;
  
  buf = (Bit32u*)framebuffer + yc * res_x + xc;
  fgcolor = wasm_palette[fc];
  bgcolor = wasm_palette[bc];
  
  if (font2) {
    font_ptr = &vga_charmap[1][(ch << 5) + fy];
  } else {
    font_ptr = &vga_charmap[0][(ch << 5) + fy];
  }
  
  do {
    font_row = *font_ptr++;
    if (gfxcharw9) {
      font_row = (font_row << 1) | (font_row & 0x01);
    } else {
      font_row <<= 1;
    }
    if (fx > 0) {
      font_row <<= fx;
    }
    fontpixels = fw;
    if (curs && (fy >= cs) && (fy <= ce))
      mask = 0x100;
    else
      mask = 0x00;
    do {
      if ((font_row & 0x100) == mask)
        *buf = bgcolor;
      else
        *buf = fgcolor;
      buf++;
      if (fontpixels & 1) font_row <<= 1;
    } while (--fontpixels);
    buf += (res_x - fw);
    fy++;
  } while (--fh);
}

void bx_wasmcanvas_gui_c::text_update(Bit8u *old_text, Bit8u *new_text,
                                      unsigned long cursor_x,
                                      unsigned long cursor_y,
                                      bx_vga_tminfo_t *tm_info)
{
  // Use text_update_common to iterate through characters and call draw_char
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
          // Convert RGB565 to RGB888
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
  if (!framebuffer) return;
  
  EM_ASM({
    if (Module.onFrame) {
      // Copy framebuffer from WASM heap to JS
      var fbBytes = $1 * $2 * 4;
      var fbData = new Uint8Array(Module.HEAPU8.buffer, $0, fbBytes);
      var imageData = new ImageData(new Uint8ClampedArray(fbData), $1, $2);
      Module.onFrame(imageData);
    }
  }, (unsigned)framebuffer, res_x, res_y);
}

void bx_wasmcanvas_gui_c::clear_screen(void)
{
  if (framebuffer) {
    memset(framebuffer, 0, res_x * res_y * 4);
  }
}

void bx_wasmcanvas_gui_c::handle_events(void)
{
  // Handle keyboard/mouse events from JS
}

#endif // BX_WITH_WASMCANVAS
