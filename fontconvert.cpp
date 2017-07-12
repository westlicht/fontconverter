
#include "BitmapFont.h"

#include "lib/args.hxx"
#include "lib/tinyformat.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "lib/stb_image_write.h"

#include <ft2build.h>
#include FT_GLYPH_H

#include <set>
#include <vector>
#include <iostream>
#include <fstream>

#define DPI 100

static std::string toUpperCase(const std::string &s) {
    std::string result = s;
    for (auto &c : result) {
        c = toupper(c);
    }
    return result;
}

template<size_t Size>
class BitEncoder {
public:
    BitEncoder(std::vector<uint8_t> &data) : _data(data) {}

    void encode(uint8_t value) {
        value &= Mask;
        _buf |= value << _shift;
        _shift += Size;
        if (_shift >= 8) {
            _data.emplace_back(_buf);
            _buf = 0;
            _shift = 0;
        }
    }

    void commit() {
        if (_shift != 0) {
            _data.emplace_back(_buf);
        }
        _buf = 0;
        _shift = 0;
    }
private:
    static_assert(8 % Size == 0, "Invalid encoding size");
    static const uint8_t Mask = (1 << Size) - 1;
    std::vector<uint8_t> &_data;
    uint8_t _buf = 0;
    size_t _shift = 0;
};

template<size_t Size>
class BitDecoder {
public:
    BitDecoder(uint8_t *data) : _data(data) {}

    uint8_t decode() {
        uint8_t value = (*_data >> _shift) & Mask;
        _shift += Size;
        if (_shift >= 8) {
            ++_data;
            _shift = 0;
        }
        return value;
    }

private:
    static_assert(8 % Size == 0, "Invalid decoding size");
    static const uint8_t Mask = (1 << Size) - 1;
    uint8_t *_data;
    size_t _shift = 0;
};


class Converter {
public:
    Converter(
        const std::string &filename,
        const std::string &name,
        int size = 10,
        int bpp = 4,
        int first = ' ',
        int last = '~'
    ) :
        _filename(filename),
        _name(name),
        _size(size),
        _bpp(bpp),
        _first(first),
        _last(last)
    {}

    template<size_t N>
    void convertBitmap(const FT_Bitmap &bitmap, std::vector<uint8_t> &dst) {
        if (N == 1) {
            BitEncoder<1> encoder(dst);
            for (int y = 0; y < bitmap.rows; y++) {
                for (int x = 0; x < bitmap.width; x++) {
                    int byte = x / 8;
                    int bit  = 0x80 >> (x & 7);
                    bool pixel = bitmap.buffer[y * bitmap.pitch + byte] & bit;
                    encoder.encode(pixel);
                }
            }
            encoder.commit();    
        } else {
            BitEncoder<N> encoder(dst);
            for (int y = 0; y < bitmap.rows; y++) {
                for (int x = 0; x < bitmap.width; x++) {
                    int pixel = bitmap.buffer[y * bitmap.pitch + x];
                    encoder.encode(pixel >> (8 - N));
                }
            }
            encoder.commit();    
        }
    }

    void operator()() {
        FT_Library library;

        if (auto error = FT_Init_FreeType(&library)) {
            tfm::printfln("Failed to initialize FreeType library (error: %d)", error);
            return;
        }

        FT_Face face;
        if (auto error = FT_New_Face(library, _filename.c_str(), 0, &face)) {
            tfm::printfln("Failed to load font from '%s' (error: %d)", _filename, error);
            return;
        }

        FT_Set_Char_Size(face, _size << 6, 0, DPI, 0);

        for (int i = _first; i <= _last; ++i) {
            if (auto error = FT_Load_Char(face, i, _bpp == 1 ? FT_LOAD_TARGET_MONO : FT_LOAD_TARGET_NORMAL)) {
                tfm::printfln("Failed to load glyph (error: %d)", error);
                continue;
            }
            if (auto error = FT_Render_Glyph(face->glyph, _bpp == 1 ? FT_RENDER_MODE_MONO : FT_RENDER_MODE_NORMAL)) {
                tfm::printfln("Failed to render glyph (error: %d)", error);
                continue;
            }
            FT_Glyph glyph;
            if (auto error = FT_Get_Glyph(face->glyph, &glyph)) {
                tfm::printfln("Failed to get glyph data (error: %d)", error);
                continue;
            }

            const auto &bitmap = face->glyph->bitmap;
            const auto g = (FT_BitmapGlyphRec *) glyph;

            BitmapFontGlyph bfGlyph;
            bfGlyph.offset = _bitmap.size();
            bfGlyph.width = bitmap.width;
            bfGlyph.height = bitmap.rows;
            bfGlyph.xAdvance = face->glyph->advance.x >> 6;
            bfGlyph.xOffset = g->left;
            bfGlyph.yOffset = 1 - g->top;
            _glyphs.emplace_back(bfGlyph);

            switch (_bpp) {
            case 1: convertBitmap<1>(bitmap, _bitmap); break;
            case 2: convertBitmap<2>(bitmap, _bitmap); break;
            case 4: convertBitmap<4>(bitmap, _bitmap); break;
            case 8: convertBitmap<8>(bitmap, _bitmap); break;
            }
        }

        _yAdvance = face->size->metrics.height >> 6;
    }

    std::string header() const {
        std::string guard = "__" + toUpperCase(_name) + "_H__";

        std::string result;
        result += tfm::format("#ifndef %s\n", guard);
        result += tfm::format("#define %s\n", guard);
        result += "\n";
        result += "#include \"BitmapFont.h\"\n";
        result += "\n";

        result += tfm::format("static uint8_t %s_bitmap[] = {\n", _name);
        for (size_t i = 0; i < _bitmap.size(); ++i) {
            if (i % 16 == 0) {
                result += "    ";
            }
            result += tfm::format("0x%02x", _bitmap[i]);
            if (i == _bitmap.size() - 1) {
                break;
            }
            result += ", ";
            if (i % 16 == 15) {
                result += "\n";
            }
        }
        result += tfm::format("\n};\n");
        result += "\n";

        result += tfm::format("static BitmapFontGlyph %s_glyphs[] = {\n", _name);
        for (const auto &g : _glyphs) {
            result += tfm::format("    { %d, %d, %d, %d, %d, %d },\n", g.offset, g.width, g.height, g.xAdvance, int(g.xOffset), int(g.yOffset));
        }

        result += tfm::format("};\n");
        result += "\n";

        result += tfm::format("static BitmapFont %s = {\n", _name);
        result += tfm::format("    %d, %s_bitmap, %s_glyphs, %d, %d, %d\n", _bpp, _name, _name, _first, _last, _yAdvance);
        result += "};\n";
        result += "\n";
        result += "#endif // " + guard + "\n";

        return result;
    }

    template<size_t N>
    void renderGlyph(const BitmapFontGlyph &g, int x, int y, uint8_t *image, int width, int height) {
        BitDecoder<N> decoder(&_bitmap[g.offset]);
        int scale = 255 / ((1 << N) - 1);
        for (int sy = 0; sy < g.height; ++sy) {
            for (int sx = 0; sx < g.width; ++sx) {
//                uint8_t pixel = decoder.decode() << (8 - N);
                uint8_t pixel = decoder.decode() * scale;
                int dx = x + sx + g.xOffset;
                int dy = y + sy + g.yOffset;
                image[dy * width + dx] = pixel;
            }
        }
    }

    void renderFont(const std::string &filename) {
        int width = 0;
        int height = _yAdvance;
        int minYOffset = 0;
        for (const auto &g : _glyphs) {
            width += g.xAdvance;
            minYOffset = std::min(minYOffset, int(g.yOffset));
        }

        std::vector<uint8_t> image(width * height, 0);

        int x = 0;
        int y = -minYOffset;
        for (const auto &g : _glyphs) {
            switch (_bpp) {
            case 1: renderGlyph<1>(g, x, y, image.data(), width, height); break;
            case 2: renderGlyph<2>(g, x, y, image.data(), width, height); break;
            case 4: renderGlyph<4>(g, x, y, image.data(), width, height); break;
            case 8: renderGlyph<8>(g, x, y, image.data(), width, height); break;
            }
            x += g.xAdvance;
        }

        stbi_write_bmp(filename.c_str(), width, height, 1, image.data());
    }

private:
    std::string _filename;
    std::string _name;
    int _size;
    int _bpp;
    int _first;
    int _last;

    std::vector<uint8_t> _bitmap;
    std::vector<BitmapFontGlyph> _glyphs;
    uint8_t _yAdvance;
};


int main(int argc, char **argv) {
    args::ArgumentParser parser("Bitmap Font Converter", "This goes after the options.");
    args::ValueFlag<int> size(parser, "size", "Font size", { 's', "size" }, 10);
    args::ValueFlag<int> bpp(parser, "bpp", "Bits per pixel", { 'b', "bpp" }, 1);
    args::ValueFlag<int> first(parser, "first", "First ASCII character", { 'f', "first" }, ' ');
    args::ValueFlag<int> last(parser, "last", "Last ASCII character", { 'l', "last" }, '~');
    args::HelpFlag help(parser, "help", "Display help", { 'h', "help" });
    args::Positional<std::string> font(parser, "font", "Font file");
    args::Positional<std::string> name(parser, "name", "Font name");

    try {
        parser.ParseCLI(argc, argv);
    } catch (args::Help) {
        std::cout << parser;
        return 0;
    } catch (args::ParseError e) {
        std::cerr << e.what() << std::endl;
        std::cerr << parser;
        return 1;
    }

    if (!font) {
        std::cout << "Provide a font file!" << std::endl;
        return 1;
    }
    if (!name) {
        std::cout << "Provide a font name" << std::endl;
        return 1;
    }
    if (std::set<int>({ 1, 2, 4, 8 }).count(args::get(bpp)) == 0) {
        std::cout << "Only 1, 2, 4 and 8 bpp are supported!" << std::endl;
        return 1;
    }

    Converter converter(args::get(font), args::get(name), args::get(size), args::get(bpp), args::get(first), args::get(last));
    converter();

    auto header = converter.header();
    std::cout << header;

    std::ofstream ofs(args::get(name) + ".h");
    ofs << header;
    ofs.close();

    converter.renderFont(args::get(name) + ".bmp");

    return 0;
}