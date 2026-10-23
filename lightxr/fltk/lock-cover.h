// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <FL/Fl_Widget.H>
#include <FL/fl_draw.H>
#include <ctime>
#include <functional>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <ft2build.h>
#include FT_FREETYPE_H

// Draw native bitmap cells at exactly 2x, avoiding outline substitution and
// fractional scaling in the desktop, GL texture cache and XR frame export.
class LockCover : public Fl_Widget {
  FT_Library library = nullptr;
  FT_Face face = nullptr;
public:
  static constexpr int scale = 2;
  std::function<void()> wake;
  int handle(int event) override {
    if (event == FL_FOCUS) return 1;
    if (event == FL_KEYDOWN || event == FL_PUSH) { if (wake) wake(); return 1; }
    return 0;
  }
  LockCover(int w, int h) : Fl_Widget(0, 0, w, h) {
    const char *path = std::filesystem::is_regular_file(LIGHTXR_INSTALLED_CLOCK_FONT)
        ? LIGHTXR_INSTALLED_CLOCK_FONT : LIGHTXR_CLOCK_FONT;
    if (FT_Init_FreeType(&library) || FT_New_Face(library, path, 0, &face) ||
        FT_IS_SCALABLE(face) || face->num_fixed_sizes != 1 || FT_Select_Size(face, 0))
      throw std::runtime_error("HeadUpDaisy native bitmap clock font is unavailable");
    if (face->available_sizes[0].height != 24)
      throw std::runtime_error("HeadUpDaisy clock requires its native 24px strike");
    hide();
  }
  ~LockCover() override { if (face) FT_Done_Face(face); if (library) FT_Done_FreeType(library); }
  void draw() override {
    fl_color(17, 25, 35); fl_rectf(x(), y(), w(), h());
    const auto seconds = std::to_string(static_cast<long long>(std::time(nullptr)));
    int width = 0;
    for (char c : seconds) {
      if (FT_Load_Char(face, c, FT_LOAD_RENDER | FT_LOAD_MONOCHROME)) continue;
      width += (face->glyph->advance.x >> 6) * scale;
    }
    int pen = x() + (w() - width) / 2;
    const int baseline = y() + h() / 2;
    fl_color(237, 243, 251);
    for (char c : seconds) {
      if (FT_Load_Char(face, c, FT_LOAD_RENDER | FT_LOAD_MONOCHROME)) continue;
      const auto &bitmap = face->glyph->bitmap;
      for (unsigned row = 0; row < bitmap.rows; ++row)
        for (unsigned column = 0; column < bitmap.width; ++column)
          if (bitmap.buffer[row * bitmap.pitch + column / 8] & (0x80 >> (column % 8)))
            fl_rectf(pen + (face->glyph->bitmap_left + column) * scale,
                     baseline + (int(row) - face->glyph->bitmap_top) * scale, scale, scale);
      pen += (face->glyph->advance.x >> 6) * scale;
    }
    fl_font(dx::font, 16);
    fl_draw("Locked", x(), baseline - 130, w(), 40, FL_ALIGN_CENTER);
    fl_draw("UNIX time", x(), baseline + 20, w(), 30, FL_ALIGN_CENTER);
    fl_draw("Press a key or click to unlock", x(), baseline + 95, w(), 40, FL_ALIGN_CENTER);
  }
};
