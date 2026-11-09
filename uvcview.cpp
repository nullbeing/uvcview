#include <libuvc/libuvc.h>
#include <jpeglib.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "font.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <glob.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <mutex>
#include <setjmp.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <ctime>
#include <fstream>
#include <unistd.h>
#include <vector>

namespace {

volatile sig_atomic_t g_running = 1;

void on_signal(int) {
    g_running = 0;
}

struct JpegError {
    jpeg_error_mgr pub;
    jmp_buf jump;
};

void jpeg_error_exit(j_common_ptr cinfo) {
    auto* err = reinterpret_cast<JpegError*>(cinfo->err);
    longjmp(err->jump, 1);
}

bool decode_jpeg(const std::vector<uint8_t>& jpeg,
                 std::vector<uint8_t>& rgb,
                 int& width, int& height) {
    if (jpeg.size() < 4 || jpeg[0] != 0xff || jpeg[1] != 0xd8) {
        return false;
    }

    jpeg_decompress_struct cinfo{};
    JpegError jerr{};
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit;

    if (setjmp(jerr.jump)) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, const_cast<unsigned char*>(jpeg.data()),
                 static_cast<unsigned long>(jpeg.size()));
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    width = static_cast<int>(cinfo.output_width);
    height = static_cast<int>(cinfo.output_height);
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    rgb.resize(static_cast<size_t>(width) * height * 3);
    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW row = &rgb[static_cast<size_t>(cinfo.output_scanline) *
                            width * 3];
        jpeg_read_scanlines(&cinfo, &row, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return true;
}

struct Framebuffer {
    int fd = -1;
    uint8_t* mem = nullptr;
    size_t map_size = 0;
    int width = 0;
    int height = 0;
    int line_length = 0;

    bool open_device(const char* path) {
        fd = open(path, O_RDWR);
        if (fd < 0) {
            std::perror("open framebuffer");
            return false;
        }

        fb_var_screeninfo vinfo{};
        fb_fix_screeninfo finfo{};
        if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
            ioctl(fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
            std::perror("framebuffer ioctl");
            close(fd);
            fd = -1;
            return false;
        }

        if (vinfo.bits_per_pixel != 16) {
            std::fprintf(stderr, "unsupported framebuffer bpp: %u\n",
                         vinfo.bits_per_pixel);
            close(fd);
            fd = -1;
            return false;
        }

        width = static_cast<int>(vinfo.xres);
        height = static_cast<int>(vinfo.yres);
        line_length = static_cast<int>(finfo.line_length);
        map_size = static_cast<size_t>(finfo.line_length) *
                   static_cast<size_t>(vinfo.yres_virtual);

        mem = static_cast<uint8_t*>(mmap(nullptr, map_size,
                                         PROT_READ | PROT_WRITE,
                                         MAP_SHARED, fd, 0));
        if (mem == MAP_FAILED) {
            std::perror("mmap framebuffer");
            mem = nullptr;
            close(fd);
            fd = -1;
            return false;
        }
        return true;
    }

    void unblank() const {
        if (fd >= 0) {
            ioctl(fd, FBIOBLANK, FB_BLANK_UNBLANK);
        }
    }

    void blit(const std::vector<uint16_t>& pixels) const {
        if (!mem || width <= 0 || height <= 0) return;
        const size_t row_bytes = static_cast<size_t>(width) * 2;
        for (int y = 0; y < height; ++y) {
            std::memcpy(mem + static_cast<size_t>(y) * line_length,
                        pixels.data() + static_cast<size_t>(y) * width,
                        row_bytes);
        }
    }

    ~Framebuffer() {
        if (mem) {
            munmap(mem, map_size);
        }
        if (fd >= 0) {
            close(fd);
        }
    }
};

struct Keyboard {
    int fd = -1;

    bool open_device(const char* path) {
        fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            std::perror("open keyboard");
            return false;
        }
        return true;
    }

    int poll_key() {
        if (fd < 0) return 0;
        input_event ev{};
        int result = 0;
        while (read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
            if (ev.type == EV_KEY && ev.value == 1) {
                result = ev.code;
            }
        }
        return result;
    }

    ~Keyboard() {
        if (fd >= 0) close(fd);
    }
};

int read_int_file(const std::string& path, int fallback) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return fallback;
    char buf[32] = {};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return fallback;
    return std::atoi(buf);
}

void write_int_file(const std::string& path, int value) {
    int fd = open(path.c_str(), O_WRONLY);
    if (fd < 0) return;
    char buf[32];
    int len = std::snprintf(buf, sizeof(buf), "%d\n", value);
    if (len > 0) {
        ssize_t ignored = write(fd, buf, static_cast<size_t>(len));
        (void)ignored;
    }
    close(fd);
}

// The Cardputer backlight can be set to 0 by the desktop idle logic. Keep
// restoring it (and the framebuffer blank state) while the viewer is active.
void keep_display_on(int requested_brightness) {
    glob_t paths{};
    if (glob("/sys/class/backlight/*/brightness", 0, nullptr, &paths) == 0) {
        for (size_t i = 0; i < paths.gl_pathc; ++i) {
            std::string brightness = paths.gl_pathv[i];
            std::string dir = brightness.substr(0, brightness.find_last_of('/'));
            int max_brightness = read_int_file(dir + "/max_brightness", 100);
            int value = requested_brightness < 0
                            ? max_brightness
                            : std::min(requested_brightness, max_brightness);
            write_int_file(brightness, std::max(1, value));
            write_int_file(dir + "/bl_power", 0);
        }
        globfree(&paths);
    }

    glob_t fb_paths{};
    if (glob("/sys/class/graphics/fb*/blank", 0, nullptr, &fb_paths) == 0) {
        for (size_t i = 0; i < fb_paths.gl_pathc; ++i) {
            write_int_file(fb_paths.gl_pathv[i], 0);
        }
        globfree(&fb_paths);
    }
}

void draw_text(std::vector<uint16_t>& out, int dw, int dh,
               int x, int y, const std::string& text,
               uint16_t color, int scale) {
    if (scale < 1) scale = 1;
    for (char ch : text) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (c < 32 || c > 126) c = '?';
        const uint8_t* glyph = font8x16[c - 32];
        for (int row = 0; row < FONT_8X16_HEIGHT; ++row) {
            for (int col = 0; col < FONT_8X16_WIDTH; ++col) {
                if ((glyph[row] & (0x80 >> col)) == 0) continue;
                for (int sy = 0; sy < scale; ++sy) {
                    for (int sx = 0; sx < scale; ++sx) {
                        int px = x + col * scale + sx;
                        int py = y + row * scale + sy;
                        if (px >= 0 && px < dw && py >= 0 && py < dh) {
                            out[static_cast<size_t>(py) * dw + px] = color;
                        }
                    }
                }
            }
        }
        x += FONT_8X16_WIDTH * scale;
    }
}

void show_error(Framebuffer& fb, Keyboard& kbd,
                const std::string& title,
                const std::string& line1,
                const std::string& line2) {
    std::vector<uint16_t> out(static_cast<size_t>(fb.width) * fb.height,
                              0x1082);  // dark blue-gray
    draw_text(out, fb.width, fb.height, 12, 18, title, 0xFFFF, 2);
    draw_text(out, fb.width, fb.height, 12, 70, line1, 0xFFE0, 1);
    if (!line2.empty()) {
        draw_text(out, fb.width, fb.height, 12, 92, line2, 0xFFE0, 1);
    }
    draw_text(out, fb.width, fb.height, 12, 140,
              "PRESS ESC OR Q TO CLOSE", 0x9E9F, 1);
    fb.blit(out);
    keep_display_on(-1);

    // Do not flash the error away. Keep it on screen until the user
    // acknowledges it, or for 15 seconds if the keyboard is unavailable.
    for (int i = 0; i < 300; ++i) {
        int key = kbd.poll_key();
        if (key == KEY_ESC || key == KEY_Q) break;
        usleep(50000);
    }
}

std::mutex g_frame_mutex;
std::vector<uint8_t> g_frame_jpeg;
std::atomic<uint64_t> g_frame_sequence{0};

void frame_callback(uvc_frame_t* frame, void*) {
    if (!frame || !frame->data || frame->data_bytes < 4) return;
    const auto* data = static_cast<const uint8_t*>(frame->data);
    // This HIK device sends JPEG data even when YUYV is negotiated.
    if (data[0] != 0xff || data[1] != 0xd8) return;

    std::lock_guard<std::mutex> lock(g_frame_mutex);
    g_frame_jpeg.assign(data, data + frame->data_bytes);
    g_frame_sequence.fetch_add(1);
}

inline uint16_t rgb565(int r, int g, int b) {
    return static_cast<uint16_t>(((r & 0xf8) << 8) |
                                 ((g & 0xfc) << 3) |
                                 (b >> 3));
}

void render(const std::vector<uint8_t>& rgb, int sw, int sh,
            int rotation, int scale_mode, bool flip, bool flip_v,
            std::vector<uint16_t>& out, int dw, int dh) {
    std::fill(out.begin(), out.end(), 0);

    const bool rotated = (rotation == 1 || rotation == 3);
    const int rw = rotated ? sh : sw;
    const int rh = rotated ? sw : sh;
    if (rw <= 0 || rh <= 0) return;

    float scale_x;
    float scale_y;
    int iw;
    int ih;
    int ox;
    int oy;

    if (scale_mode == 2) {
        // Stretch the full source to the full framebuffer, no black bars.
        scale_x = static_cast<float>(dw) / rw;
        scale_y = static_cast<float>(dh) / rh;
        iw = dw;
        ih = dh;
        ox = 0;
        oy = 0;
    } else {
        const float scale = scale_mode == 0
            ? std::min(static_cast<float>(dw) / rw,
                       static_cast<float>(dh) / rh)
            : std::max(static_cast<float>(dw) / rw,
                       static_cast<float>(dh) / rh);
        scale_x = scale;
        scale_y = scale;
        iw = std::max(1, static_cast<int>(rw * scale));
        ih = std::max(1, static_cast<int>(rh * scale));
        ox = (dw - iw) / 2;
        oy = (dh - ih) / 2;
    }

    if (scale_x <= 0.0f || scale_y <= 0.0f) return;

    for (int y = 0; y < dh; ++y) {
        const int dy = y - oy;
        if (dy < 0 || dy >= ih) continue;

        for (int x = 0; x < dw; ++x) {
            const int dx = x - ox;
            if (dx < 0 || dx >= iw) continue;

            int rx = static_cast<int>(dx / scale_x);
            int ry = static_cast<int>(dy / scale_y);
            rx = std::clamp(rx, 0, rw - 1);
            ry = std::clamp(ry, 0, rh - 1);

            int sx = 0;
            int sy = 0;
            switch (rotation) {
                case 0:
                    sx = rx;
                    sy = ry;
                    break;
                case 1:  // 90 degrees clockwise
                    sx = ry;
                    sy = sh - 1 - rx;
                    break;
                case 2:  // 180 degrees
                    sx = sw - 1 - rx;
                    sy = sh - 1 - ry;
                    break;
                case 3:  // 270 degrees clockwise
                    sx = sw - 1 - ry;
                    sy = rx;
                    break;
            }

            sx = std::clamp(sx, 0, sw - 1);
            sy = std::clamp(sy, 0, sh - 1);
            const size_t idx = (static_cast<size_t>(sy) * sw + sx) * 3;
            out[static_cast<size_t>(y) * dw + x] =
                rgb565(rgb[idx], rgb[idx + 1], rgb[idx + 2]);
        }
    }

    if (flip) {
        for (int y = 0; y < dh; ++y) {
            auto* row = out.data() + static_cast<size_t>(y) * dw;
            for (int x = 0; x < dw / 2; ++x) {
                std::swap(row[x], row[dw - 1 - x]);
            }
        }
    }

    if (flip_v) {
        for (int y = 0; y < dh / 2; ++y) {
            uint16_t* top = out.data() + static_cast<size_t>(y) * dw;
            uint16_t* bottom = out.data() + static_cast<size_t>(dh - 1 - y) * dw;
            for (int x = 0; x < dw; ++x) {
                std::swap(top[x], bottom[x]);
            }
        }
    }
}

uint16_t blend565(uint16_t dst, uint16_t src, uint8_t alpha) {
    const int dr = (dst >> 11) & 0x1f;
    const int dg = (dst >> 5) & 0x3f;
    const int db = dst & 0x1f;
    const int sr = (src >> 11) & 0x1f;
    const int sg = (src >> 5) & 0x3f;
    const int sb = src & 0x1f;
    const int r = (dr * (255 - alpha) + sr * alpha) / 255;
    const int g = (dg * (255 - alpha) + sg * alpha) / 255;
    const int b = (db * (255 - alpha) + sb * alpha) / 255;
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

struct FontGlyph {
    int width = 0;
    int height = 0;
    int bearing_x = 0;
    int bearing_y = 0;
    int advance = 0;
    std::vector<uint8_t> bitmap;
};

class FontRenderer {
public:
    FontRenderer() = default;
    ~FontRenderer() {
        if (face_) FT_Done_Face(face_);
        if (library_) FT_Done_FreeType(library_);
    }

    bool load(const char* path, int pixel_size) {
        if (FT_Init_FreeType(&library_) != 0) return false;
        if (FT_New_Face(library_, path, 0, &face_) != 0) {
            FT_Done_FreeType(library_);
            library_ = nullptr;
            return false;
        }
        FT_Set_Pixel_Sizes(face_, 0, static_cast<FT_UInt>(pixel_size));
        for (int c = 32; c <= 126; ++c) {
            if (FT_Load_Char(face_, c, FT_LOAD_RENDER) != 0) continue;
            FT_GlyphSlot slot = face_->glyph;
            const FT_Bitmap& bmp = slot->bitmap;
            FontGlyph& glyph = glyphs_[c - 32];
            glyph.width = static_cast<int>(bmp.width);
            glyph.height = static_cast<int>(bmp.rows);
            glyph.bearing_x = slot->bitmap_left;
            glyph.bearing_y = slot->bitmap_top;
            glyph.advance = static_cast<int>(slot->advance.x >> 6);
            glyph.bitmap.resize(static_cast<size_t>(bmp.width) * bmp.rows);
            for (unsigned row = 0; row < bmp.rows; ++row) {
                for (unsigned col = 0; col < bmp.width; ++col) {
                    glyph.bitmap[static_cast<size_t>(row) * bmp.width + col] =
                        bmp.buffer[row * bmp.pitch + col];
                }
            }
        }
        valid_ = true;
        return true;
    }

    bool valid() const { return valid_; }

    int text_width(const std::string& text) const {
        int width = 0;
        for (char ch : text) {
            unsigned char c = static_cast<unsigned char>(ch);
            if (c < 32 || c > 126) c = '?';
            width += glyphs_[c - 32].advance;
        }
        return width;
    }

    void draw(std::vector<uint16_t>& out, int dw, int dh,
              int x, int baseline, const std::string& text,
              uint16_t color, uint8_t alpha) const {
        if (!valid_) return;
        int pen_x = x;
        for (char ch : text) {
            unsigned char c = static_cast<unsigned char>(ch);
            if (c < 32 || c > 126) c = '?';
            const FontGlyph& glyph = glyphs_[c - 32];
            const int gx = pen_x + glyph.bearing_x;
            const int gy = baseline - glyph.bearing_y;
            for (int row = 0; row < glyph.height; ++row) {
                for (int col = 0; col < glyph.width; ++col) {
                    const uint8_t coverage =
                        glyph.bitmap[static_cast<size_t>(row) * glyph.width + col];
                    if (coverage == 0) continue;
                    const int px = gx + col;
                    const int py = gy + row;
                    if (px < 0 || px >= dw || py < 0 || py >= dh) continue;
                    auto& pixel = out[static_cast<size_t>(py) * dw + px];
                    pixel = blend565(pixel, color,
                                     static_cast<uint8_t>(
                                         (coverage * alpha) / 255));
                }
            }
            pen_x += glyph.advance;
        }
    }

private:
    FT_Library library_ = nullptr;
    FT_Face face_ = nullptr;
    FontGlyph glyphs_[95];
    bool valid_ = false;
};

FontRenderer g_font;

void draw_pill_button(std::vector<uint16_t>& out, int dw, int dh,
                      int cx, int cy, int half_w, int radius,
                      bool active) {
    // Selected buttons keep the same neutral thin border but get a
    // brighter/whiter translucent fill.
    const uint16_t border_color = 0xd69a;
    const uint16_t fill_color = active ? 0xffff : 0x0000;
    const uint8_t border_alpha = active ? 210 : 170;
    const uint8_t fill_alpha = active ? 70 : 45;

    const float stroke_width = 1.0f;

    for (int y = cy - radius - 1; y <= cy + radius + 1; ++y) {
        if (y < 0 || y >= dh) continue;
        for (int x = cx - half_w - 1; x <= cx + half_w + 1; ++x) {
            if (x < 0 || x >= dw) continue;
            const float dx = std::abs(static_cast<float>(x - cx)) -
                             static_cast<float>(half_w - radius);
            const float qx = std::max(dx, 0.0f);
            const float dy = static_cast<float>(y - cy);
            const float distance = std::sqrt(qx * qx + dy * dy);

            // Anti-aliased capsule coverage. Using the difference between
            // the outer and inner capsule gives a stroke with a uniform
            // width, including around the rounded ends.
            const float outer = std::clamp(radius - distance + 0.5f,
                                           0.0f, 1.0f);
            const float inner = std::clamp(radius - stroke_width - distance + 0.5f,
                                           0.0f, 1.0f);
            const float border_coverage = std::max(0.0f, outer - inner);

            auto& pixel = out[static_cast<size_t>(y) * dw + x];
            if (border_coverage > 0.0f) {
                pixel = blend565(pixel, border_color,
                                 static_cast<uint8_t>(
                                     border_alpha * border_coverage));
            }
            if (inner > 0.0f) {
                pixel = blend565(pixel, fill_color,
                                 static_cast<uint8_t>(fill_alpha * inner));
            }
        }
    }
}

void draw_overlay(std::vector<uint16_t>& out, int dw, int dh,
                  bool flip_v, bool flip_h, bool photo_mode,
                  int scale_mode, const std::string& status) {
    if (!status.empty()) {
        // Dark translucent strip behind the status text.
        for (int y = 0; y < 18 && y < dh; ++y) {
            for (int x = 0; x < dw; ++x) {
                auto& pixel = out[static_cast<size_t>(y) * dw + x];
                pixel = blend565(pixel, 0x0000, 120);
            }
        }
        if (g_font.valid()) {
            const int width = g_font.text_width(status);
            g_font.draw(out, dw, dh, std::max(0, (dw - width) / 2), 14,
                        status, 0xffff, 220);
        } else {
            const int text_width = static_cast<int>(status.size()) * 8;
            draw_text(out, dw, dh, std::max(0, (dw - text_width) / 2), 1,
                      status, 0xffff, 1);
        }
    }

    const char* labels[5] = {"4 AR", "5 VM", "6 HM", "7 PIC", "8 VIEW"};
    const bool active[5] = {scale_mode == 0, flip_v, flip_h,
                            false, photo_mode};
    const int half_w = 24;
    const int radius = 11;
    // Official CardputerZero key-hint layout (cp0-keys): the five most used
    // actions sit on physical keys 4..8. Recommended icon centres on a
    // 320x170 screen are x = 49, 105, 161, 217, 273 and y = 153.
    const int cy = std::min(dh - radius - 2, dh * 153 / 170);

    for (int i = 0; i < 5; ++i) {
        const int cx = (49 + i * 56) * dw / 320;
        draw_pill_button(out, dw, dh, cx, cy, half_w, radius, active[i]);
        const uint16_t label_color = active[i] ? 0x0000 : 0xffff;
        if (g_font.valid()) {
            const int width = g_font.text_width(labels[i]);
            g_font.draw(out, dw, dh, cx - width / 2, cy + 4,
                        labels[i], label_color, 235);
        } else {
            const int width = static_cast<int>(std::strlen(labels[i])) * 8;
            draw_text(out, dw, dh, cx - width / 2, cy - 8,
                      labels[i], label_color, 1);
        }
    }
}

const char* kPhotoDir = "/home/pi/Pictures/Thermal";

bool read_file(const std::string& path, std::vector<uint8_t>& data) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    if (size <= 0) return false;
    in.seekg(0, std::ios::beg);
    data.resize(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(data.data()), size);
    return in.good() || in.eof();
}

bool save_jpeg_photo(const std::vector<uint8_t>& jpeg, std::string& path) {
    if (jpeg.size() < 4 || jpeg[0] != 0xff || jpeg[1] != 0xd8) {
        return false;
    }

    mkdir("/home/pi/Pictures", 0755);
    mkdir(kPhotoDir, 0755);

    struct timeval tv{};
    gettimeofday(&tv, nullptr);
    struct tm local{};
    localtime_r(&tv.tv_sec, &local);
    char name[96];
    std::snprintf(name, sizeof(name),
                  "%s/thermal_%04d%02d%02d_%02d%02d%02d_%03d.jpg",
                  kPhotoDir,
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec,
                  static_cast<int>(tv.tv_usec / 1000));

    std::ofstream out(name, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(jpeg.data()),
              static_cast<std::streamsize>(jpeg.size()));
    if (!out) return false;
    path = name;
    return true;
}

std::vector<std::string> list_photos() {
    std::vector<std::string> photos;
    glob_t paths{};
    if (glob("/home/pi/Pictures/Thermal/*.jpg", 0, nullptr, &paths) == 0) {
        for (size_t i = 0; i < paths.gl_pathc; ++i) {
            photos.emplace_back(paths.gl_pathv[i]);
        }
        globfree(&paths);
    }
    std::sort(photos.begin(), photos.end());
    return photos;
}

}  // namespace

int main(int argc, char** argv) {
    const char* fb_path = "/dev/fb_lcd";
    const char* kbd_path =
        "/dev/input/by-path/platform-3f804000.i2c-event";
    int rotation = 1;  // default: 90 degrees clockwise for the Cardputer display
    int scale_mode = 2;  // 0=fit, 1=fill, 2=stretch (default: full screen)
    bool flip = true;  // this camera needs a horizontal mirror
    bool flip_v = false;
    int brightness = -1;  // -1 = use the backlight maximum

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            fb_path = argv[++i];
        } else if (std::strcmp(argv[i], "-k") == 0 && i + 1 < argc) {
            kbd_path = argv[++i];
        } else if (std::strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            rotation = std::atoi(argv[++i]);
            rotation = ((rotation % 360) + 360) % 360;
            rotation /= 90;
        } else if (std::strcmp(argv[i], "-f") == 0) {
            scale_mode = 1;  // fill/crop
        } else if (std::strcmp(argv[i], "-t") == 0) {
            scale_mode = 2;  // stretch to the whole framebuffer
        } else if (std::strcmp(argv[i], "-n") == 0) {
            flip = false;
        } else if (std::strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            brightness = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "-h") == 0) {
            std::printf("usage: %s [-d /dev/fb_lcd] [-k keyboard] "
                        "[-r 0|90|180|270] [-f] [-t] [-n] [-b brightness]\n",
                        argv[0]);
            return 0;
        }
    }

    if (geteuid() != 0) {
        std::fprintf(stderr,
                     "note: running as user; USB access via udev rule "
                     "(99-uvcview.rules)\n");
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const char* font_candidates[] = {
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Light.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-ExtraLight.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    };
    for (const char* font_path : font_candidates) {
        if (g_font.load(font_path, 12)) {
            std::printf("font: %s\n", font_path);
            break;
        }
    }

    Framebuffer fb;
    if (!fb.open_device(fb_path)) return 1;

    fb.unblank();
    keep_display_on(brightness);

    Keyboard kbd;
    if (!kbd.open_device(kbd_path)) {
        std::fprintf(stderr, "warning: keyboard unavailable, Ctrl+C to quit\n");
    }

    uvc_context_t* ctx = nullptr;
    uvc_device_t* device = nullptr;
    uvc_device_handle_t* handle = nullptr;
    uvc_stream_ctrl_t ctrl{};

    uvc_error_t res = uvc_init(&ctx, nullptr);
    if (res < 0) {
        show_error(fb, kbd, "THERMAL CAMERA",
                   "UVC INITIALIZATION FAILED", uvc_strerror(res));
        return 1;
    }

    res = uvc_find_device(ctx, &device, 0x2bdf, 0x0101, nullptr);
    if (res < 0) {
        show_error(fb, kbd, "THERMAL CAMERA",
                   "CAMERA NOT FOUND", "CHECK USB CABLE");
        uvc_exit(ctx);
        return 1;
    }

    res = uvc_open(device, &handle);
    if (res < 0) {
        show_error(fb, kbd, "THERMAL CAMERA",
                   "CAMERA BUSY OR ACCESS DENIED", uvc_strerror(res));
        uvc_exit(ctx);
        return 1;
    }

    res = uvc_get_stream_ctrl_format_size(
        handle, &ctrl, UVC_FRAME_FORMAT_MJPEG, 240, 320, 30);
    if (res < 0) {
        std::fprintf(stderr, "MJPEG 240x320@30 unavailable, trying YUYV: %s\n",
                     uvc_strerror(res));
        res = uvc_get_stream_ctrl_format_size(
            handle, &ctrl, UVC_FRAME_FORMAT_YUYV, 240, 320, 30);
    }
    if (res < 0) {
        show_error(fb, kbd, "THERMAL CAMERA",
                   "CAMERA FORMAT ERROR", uvc_strerror(res));
        uvc_close(handle);
        uvc_exit(ctx);
        return 1;
    }

    res = uvc_start_streaming(handle, &ctrl, frame_callback, nullptr, 0);
    if (res < 0) {
        show_error(fb, kbd, "THERMAL CAMERA",
                   "STREAM START FAILED", uvc_strerror(res));
        uvc_close(handle);
        uvc_exit(ctx);
        return 1;
    }

    const char* mode_name = scale_mode == 0 ? "fit"
                          : scale_mode == 1 ? "fill" : "stretch";
    std::printf("thermal viewer: %dx%d, rotation=%d, mode=%s, flip=%d, "
                "flip_v=%d\n",
                fb.width, fb.height, rotation * 90, mode_name,
                flip ? 1 : 0, flip_v ? 1 : 0);
    std::printf("keys: Esc/Q quit, R rotate, M scale, "
                "5 vertical mirror, 6 horizontal mirror, 7 photo, 8 view, "
                "arrows browse photos\n");

    std::vector<uint16_t> back(static_cast<size_t>(fb.width) * fb.height);
    std::vector<uint8_t> jpeg;
    std::vector<uint8_t> rgb;
    std::vector<uint8_t> live_jpeg;
    std::vector<uint8_t> displayed_jpeg;
    uint64_t last_sequence = 0;
    int decoded_w = 0;
    int decoded_h = 0;
    bool decoded_any = false;
    bool photo_mode = false;
    std::vector<std::string> photos;
    size_t photo_index = 0;
    std::string status;
    auto status_until = std::chrono::steady_clock::now();
    auto started_at = std::chrono::steady_clock::now();
    auto last_display_keepalive = std::chrono::steady_clock::now();

    auto set_status = [&](const std::string& text, int duration_ms) {
        status = text;
        status_until = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(duration_ms);
    };

    auto draw_current = [&](const std::vector<uint8_t>& data) -> bool {
        int w = 0;
        int h = 0;
        std::vector<uint8_t> decoded;
        if (!decode_jpeg(data, decoded, w, h)) return false;
        render(decoded, w, h, rotation, scale_mode, flip, flip_v,
               back, fb.width, fb.height);
        draw_overlay(back, fb.width, fb.height, flip_v, flip,
                     photo_mode, scale_mode, status);
        fb.blit(back);
        return true;
    };

    auto load_photo = [&]() -> bool {
        if (photos.empty() || photo_index >= photos.size()) return false;
        std::vector<uint8_t> data;
        if (!read_file(photos[photo_index], data)) return false;
        if (!draw_current(data)) return false;
        displayed_jpeg = data;
        return true;
    };

    while (g_running) {
        const auto now = std::chrono::steady_clock::now();
        if (!photo_mode && !decoded_any &&
            now - started_at >= std::chrono::seconds(5)) {
            show_error(fb, kbd, "THERMAL CAMERA",
                       "NO VIDEO FRAMES", "CHECK CAMERA / USB");
            break;
        }
        if (now - last_display_keepalive >= std::chrono::seconds(1)) {
            fb.unblank();
            keep_display_on(brightness);
            last_display_keepalive = now;
        }

        if (!photo_mode && !status.empty() && now > status_until) {
            status.clear();
        }

        const uint64_t sequence = g_frame_sequence.load();
        if (sequence != last_sequence) {
            if (photo_mode) {
                // Keep consuming the sequence counter while browsing photos.
                last_sequence = sequence;
            } else {
                {
                    std::lock_guard<std::mutex> lock(g_frame_mutex);
                    jpeg = g_frame_jpeg;
                }
                last_sequence = sequence;
                live_jpeg = jpeg;
                displayed_jpeg = live_jpeg;
                if (draw_current(live_jpeg)) {
                    decoded_any = true;
                }
            }
        }

        const int key = kbd.poll_key();
        if (key == KEY_ESC) {
            if (photo_mode) {
                photo_mode = false;
                last_sequence = 0;
                set_status("LIVE", 1200);
            } else {
                break;
            }
        } else if (key == KEY_Q) {
            break;
        } else if (key == KEY_5) {
            flip_v = !flip_v;
            set_status(flip_v ? "VERTICAL MIRROR ON" : "VERTICAL MIRROR OFF",
                       1500);
            if (photo_mode && !displayed_jpeg.empty()) {
                draw_current(displayed_jpeg);
            }
        } else if (key == KEY_6) {
            flip = !flip;
            set_status(flip ? "HORIZONTAL MIRROR ON"
                            : "HORIZONTAL MIRROR OFF", 1500);
            if (photo_mode && !displayed_jpeg.empty()) {
                draw_current(displayed_jpeg);
            }
        } else if (key == KEY_7) {
            if (live_jpeg.empty()) {
                set_status("NO FRAME TO SAVE", 1500);
            } else {
                std::string path;
                if (save_jpeg_photo(live_jpeg, path)) {
                    set_status("PHOTO SAVED", 2000);
                    std::printf("saved %s\n", path.c_str());
                } else {
                    set_status("SAVE FAILED", 2000);
                }
            }
            if (photo_mode) {
                photo_mode = false;
                last_sequence = 0;
            }
            if (!displayed_jpeg.empty()) {
                draw_current(displayed_jpeg);
            }
        } else if (key == KEY_8) {
            photos = list_photos();
            if (photos.empty()) {
                set_status("NO PHOTOS", 2000);
                if (photo_mode) {
                    photo_mode = false;
                    last_sequence = 0;
                }
            } else if (!photo_mode) {
                photo_mode = true;
                photo_index = photos.size() - 1;
                status = "VIEW " + std::to_string(photo_index + 1) + "/" +
                         std::to_string(photos.size());
                if (!load_photo()) {
                    set_status("PHOTO ERROR", 2000);
                }
            } else if (photo_index > 0) {
                --photo_index;
                status = "VIEW " + std::to_string(photo_index + 1) + "/" +
                         std::to_string(photos.size());
                if (!load_photo()) {
                    set_status("PHOTO ERROR", 2000);
                }
            } else {
                photo_mode = false;
                last_sequence = 0;
                set_status("LIVE", 1200);
            }
        } else if (photo_mode &&
                   (key == KEY_LEFT || key == KEY_UP ||
                    key == KEY_RIGHT || key == KEY_DOWN ||
                    key == KEY_D || key == KEY_X)) {
            photos = list_photos();
            if (photos.empty()) {
                photo_mode = false;
                last_sequence = 0;
                set_status("NO PHOTOS", 2000);
            } else {
                if (key == KEY_LEFT || key == KEY_UP || key == KEY_D) {
                    if (photo_index == 0) {
                        photo_index = photos.size() - 1;
                    } else {
                        --photo_index;
                    }
                } else {
                    photo_index = (photo_index + 1) % photos.size();
                }
                status = "VIEW " + std::to_string(photo_index + 1) + "/" +
                         std::to_string(photos.size());
                if (!load_photo()) {
                    set_status("PHOTO ERROR", 2000);
                }
            }
        } else if (key == KEY_R) {
            rotation = (rotation + 1) % 4;
            std::printf("rotation=%d\n", rotation * 90);
            if (photo_mode && !displayed_jpeg.empty()) {
                draw_current(displayed_jpeg);
            }
        } else if (key == KEY_4 || key == KEY_M) {
            scale_mode = (scale_mode + 1) % 3;
            const char* mode = scale_mode == 0 ? "fit"
                             : scale_mode == 1 ? "fill" : "stretch";
            set_status(std::string("RATIO: ") +
                       (scale_mode == 0 ? "ORIGINAL" : mode), 1500);
            std::printf("mode=%s\n", mode);
            if (photo_mode && !displayed_jpeg.empty()) {
                draw_current(displayed_jpeg);
            }
        }

        usleep(5000);
    }

    std::printf("stopping...\n");
    uvc_stop_streaming(handle);
    uvc_close(handle);
    uvc_exit(ctx);
    return 0;
}
