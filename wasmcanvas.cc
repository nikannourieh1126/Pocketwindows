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
#include <emscripten/emscripten.h>

#include "icon_bochs.h"

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

static unsigned res_x = 720, res_y = 400;
static Bit8u* framebuffer = NULL;
static unsigned headerbar_height = 0;
static Bit32u wasm_palette[256];
static bool frame_dirty = false;
static bool mic_active = false;

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

  framebuffer = (Bit8u*)malloc(res_x * res_y * 4);
  if (framebuffer) {
    memset(framebuffer, 0, res_x * res_y * 4);
  }
  frame_dirty = true;

  EM_ASM({
    if (Module.onDimensionChange) {
      Module.onDimensionChange($0, $1);
    }
  }, res_x, res_y + headerbar_height);
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

  EM_ASM({
    if (Module.onFrame) {
      var fbBytes = $1 * $2 * 4;
      var fbData = Module.HEAPU8.subarray($0, $0 + fbBytes);
      Module.onFrame(fbData, $1, $2);
    }
  }, (unsigned)framebuffer, res_x, res_y);

  frame_dirty = false;
}

void bx_wasmcanvas_gui_c::clear_screen(void)
{
  if (framebuffer) {
    memset(framebuffer, 0, res_x * res_y * 4);
    frame_dirty = true;
  }
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

  frame_dirty = true;
}

void bx_wasmcanvas_gui_c::text_update(Bit8u *old_text, Bit8u *new_text,
                                      unsigned long cursor_x, unsigned long cursor_y,
                                      bx_vga_tminfo_t *tm_info)
{
  text_update_common(old_text, new_text, cursor_x, tm_info);
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
  wasm_palette[index] = (red << 16) | (green << 8) | blue;
  frame_dirty = true;
  return 0;
}

void bx_wasmcanvas_gui_c::graphics_tile_update(Bit8u *snapshot, unsigned x, unsigned y)
{
  if (!framebuffer) return;

  Bit32u *buf = (Bit32u*)framebuffer + y * res_x + x;
  int i = y_tilesize;
  if (i + y > res_y) i = res_y - y;

  switch (guest_bpp) {
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
      break;
  }
  frame_dirty = true;
}

void bx_wasmcanvas_gui_c::dimension_update(unsigned x, unsigned y, unsigned fheight, unsigned fwidth, unsigned bpp)
{
  guest_textmode = (fheight > 0);
  guest_xres = x;
  guest_yres = y;
  guest_bpp = bpp;

  res_x = x;
  res_y = y;

  if (framebuffer) free(framebuffer);
  framebuffer = (Bit8u*)malloc(res_x * res_y * 4);
  if (framebuffer) {
    memset(framebuffer, 0, res_x * res_y * 4);
  }
  frame_dirty = true;

  EM_ASM({
    if (Module.onDimensionChange) {
      Module.onDimensionChange($0, $1);
    }
  }, res_x, res_y + headerbar_height);
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
