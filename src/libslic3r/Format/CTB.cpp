#include "CTB.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <limits>
#include <memory>

#include <boost/log/trivial.hpp>
#include <openssl/evp.h>

#include "libslic3r/Exception.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/SLA/RasterBase.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/libslic3r.h"

namespace Slic3r {

namespace ctb {

namespace {

constexpr uint32_t MagicChitubox = 0x12FD0106;
constexpr uint32_t MagicGKtwo    = 0xFF220810;
constexpr uint32_t Version       = 4;
constexpr uint32_t SoftwareVersion = 0x01090000; // Chitubox 1.9.0

constexpr size_t HeaderSize       = 112;
constexpr size_t PreviewHeadSize  = 32;
constexpr size_t PrintParamsSize  = 60;
constexpr size_t SlicerInfoSize   = 76;
constexpr size_t LayerDefSize     = 36;
constexpr size_t LayerDefExSize   = 84;
constexpr size_t ExtensionSize    = 84 + 380;   // fields up to the resin address, then reserved space
constexpr uint64_t PageSize       = uint64_t(1) << 32;

constexpr uint32_t MagicCbddlp = 0x12FD0019;
constexpr uint32_t CbddlpVersion = 2;
constexpr uint32_t CbddlpMaxRun = 0x7D;    // what Chitubox writes: runs are cut at 125 pixels

uint8_t plane_threshold(uint32_t plane)
{
    return cbddlp_planes == 1 ? uint8_t(127) : uint8_t(256 / cbddlp_planes * plane - 1);
}


const char *const Disclaimer =
    "Layout and record format for the ctb and cbddlp file types are the copyrighted programs or codes of CBD Technology "
    "(China) Inc..The Customer or User shall not in any manner reproduce, distribute, modify, decompile, disassemble, "
    "decrypt, extract, reverse engineer, lease, assign, or sublicense the said programs or codes.";
const char *const DisclaimerGKtwo =
    "Layout and record format for the jxs,ctb and cbddlp file types are the copyrighted programs or codes of CBD Technology "
    "(China) Inc.The Customer or User shall not in any manner reproduce, distribute, modify, decompile, disassemble, "
    "decrypt, extract, reverse engineer, lease, assign, or sublicense the said programs or codes.";

// A growing little endian buffer with places that are filled in later.
class Buffer
{
public:
    size_t pos() const { return m_data.size(); }
    std::vector<uint8_t> take() { return std::move(m_data); }

    void u8(uint8_t v) { m_data.push_back(v); }
    void u16(uint16_t v) { put(&v, 2); }
    void u32(uint32_t v) { put(&v, 4); }
    void f32(float v) { put(&v, 4); }
    void bytes(const void *p, size_t n) { const auto *b = static_cast<const uint8_t*>(p); m_data.insert(m_data.end(), b, b + n); }
    void zeros(size_t n) { m_data.insert(m_data.end(), n, uint8_t(0)); }
    // A u32 to be written later: returns where it is.
    size_t hole() { size_t at = pos(); u32(0); return at; }
    void patch(size_t at, uint32_t v) { std::memcpy(&m_data[at], &v, 4); }
    uint8_t *data(size_t at) { return &m_data[at]; }

private:
    void put(const void *p, size_t n)
    {
        // The host is little endian on every platform this is built for; the format is little endian.
        bytes(p, n);
    }

    std::vector<uint8_t> m_data;
};

uint32_t to_u32(size_t v)
{
    if (v > std::numeric_limits<uint32_t>::max())
        throw ExportError("The CTB file is too large for its 32 bit offsets.");
    return uint32_t(v);
}

void add_run(std::vector<uint8_t> &out, uint32_t len, uint8_t v7)
{
    constexpr uint32_t MaxRun = 0xFFFFFFF;
    while (len > 0) {
        const uint32_t n = std::min(len, MaxRun);
        len -= n;
        out.push_back(v7 | (n > 1 ? 0x80 : 0));
        if (n <= 1) {
            // a single pixel has no length
        } else if (n <= 0x7F) {
            out.push_back(uint8_t(n));
        } else if (n <= 0x3FFF) {
            out.push_back(uint8_t(0x80 | (n >> 8)));
            out.push_back(uint8_t(n));
        } else if (n <= 0x1FFFFF) {
            out.push_back(uint8_t(0xC0 | (n >> 16)));
            out.push_back(uint8_t(n >> 8));
            out.push_back(uint8_t(n));
        } else {
            out.push_back(uint8_t(0xE0 | (n >> 24)));
            out.push_back(uint8_t(n >> 16));
            out.push_back(uint8_t(n >> 8));
            out.push_back(uint8_t(n));
        }
    }
}

// The preview image: RGB565-like 16 bit pixels where bit 5 says that a 12 bit repeat count follows.
std::vector<uint8_t> encode_preview(const Preview &p)
{
    std::vector<uint8_t> out;
    const size_t count = size_t(p.width) * p.height;
    if (count == 0 || p.rgb.size() < count * 3)
        return out;

    auto color = [&p](size_t i) {
        return uint16_t((p.rgb[i * 3 + 2] >> 3) | ((p.rgb[i * 3 + 1] >> 2) << 5) | ((p.rgb[i * 3] >> 3) << 11));
    };
    auto put16 = [&out](uint16_t v) { out.push_back(uint8_t(v)); out.push_back(uint8_t(v >> 8)); };
    auto flush = [&](uint16_t c, uint32_t n) {
        if (n == 0)
            return;
        if (n <= 2) {
            for (uint32_t k = 0; k < n; ++k)
                put16(uint16_t(c & ~0x20));
        } else {
            put16(uint16_t(c | 0x20));
            put16(uint16_t((n - 1) | 0x3000));
        }
    };

    uint16_t last = color(0);
    uint32_t n = 0;
    for (size_t i = 0; i < count; ++i) {
        const uint16_t c = color(i);
        // pixels are compared on what is stored, which has the green LSB reused as the flag
        if ((c & ~0x20) == (last & ~0x20)) {
            if (++n == 0xFFF) {
                flush(last, n);
                n = 0;
            }
        } else {
            flush(last, n);
            last = c;
            n = 1;
        }
    }
    flush(last, n);
    return out;
}

void write_preview(Buffer &b, const Preview &p, uint32_t default_w, uint32_t default_h, bool short_head = false)
{
    Preview use = p;
    if (use.width == 0 || use.height == 0 || use.rgb.size() < size_t(use.width) * use.height * 3) {
        use.width = default_w;
        use.height = default_h;
        use.rgb.assign(size_t(default_w) * default_h * 3, 24);
    }
    const std::vector<uint8_t> data = encode_preview(use);
    b.u32(use.width);
    b.u32(use.height);
    b.u32(to_u32(b.pos() + (short_head ? 8 : PreviewHeadSize - 8)));   // where the image starts
    b.u32(to_u32(data.size()));
    if (! short_head)
        b.zeros(16);
    b.bytes(data.data(), data.size());
}

} // namespace

std::vector<uint8_t> rle_encode(const uint8_t *pixels, size_t count)
{
    std::vector<uint8_t> out;
    out.reserve(count / 16 + 16);
    size_t i = 0;
    while (i < count) {
        const uint8_t v7 = pixels[i] >> 1;
        size_t j = i + 1;
        while (j < count && (pixels[j] >> 1) == v7)
            ++j;
        add_run(out, uint32_t(std::min<size_t>(j - i, std::numeric_limits<uint32_t>::max())), v7);
        i = j;
    }
    return out;
}

bool rle_decode(const uint8_t *data, size_t size, uint8_t *pixels, size_t count)
{
    size_t px = 0;
    for (size_t n = 0; n < size; ++n) {
        uint8_t code = data[n];
        uint64_t run = 1;
        if (code & 0x80) {
            code &= 0x7F;
            if (++n >= size)
                return false;
            const uint8_t len = data[n];
            size_t extra = 0;
            if ((len & 0x80) == 0) {
                run = len;
            } else if ((len & 0xC0) == 0x80) {
                extra = 1;
                if (n + extra >= size) return false;
                run = (uint64_t(len & 0x3F) << 8) + data[n + 1];
            } else if ((len & 0xE0) == 0xC0) {
                extra = 2;
                if (n + extra >= size) return false;
                run = (uint64_t(len & 0x1F) << 16) + (uint64_t(data[n + 1]) << 8) + data[n + 2];
            } else if ((len & 0xF0) == 0xE0) {
                extra = 3;
                if (n + extra >= size) return false;
                run = (uint64_t(len & 0x0F) << 24) + (uint64_t(data[n + 1]) << 16) + (uint64_t(data[n + 2]) << 8) + data[n + 3];
            } else {
                return false;
            }
            n += extra;
        }
        if (run > count - px)
            return false;
        const uint8_t value = code ? uint8_t((code << 1) | 1) : 0;
        std::fill(pixels + px, pixels + px + run, value);
        px += size_t(run);
    }
    return true;
}

void layer_cipher(uint8_t *data, size_t size, uint32_t seed, uint32_t layer_index)
{
    if (seed == 0)
        return;
    const uint32_t init = seed * 0x2D83CDACu + 0xD8A83423u;
    uint32_t key = (layer_index * 0x1E1530CDu + 0xEC3D47CDu) * init;
    unsigned index = 0;
    for (size_t i = 0; i < size; ++i) {
        const uint8_t k = uint8_t(key >> (8 * index));
        ++index;
        if ((index & 3) == 0) {
            key += init;
            index = 0;
        }
        data[i] ^= k;
    }
}

std::vector<uint8_t> cbddlp_encode(const uint8_t *pixels, size_t count)
{
    std::vector<uint8_t> blob;
    for (uint32_t plane = 0; plane < cbddlp_planes; ++plane) {
        const uint8_t threshold = plane_threshold(plane);
        std::vector<uint8_t> rle;
        size_t i = 0;
        while (i < count) {
            const bool on = pixels[i] >= threshold;
            size_t j = i + 1;
            while (j < count && (pixels[j] >= threshold) == on)
                ++j;
            for (size_t left = j - i; left > 0;) {
                const size_t n = std::min<size_t>(left, CbddlpMaxRun);
                rle.push_back(uint8_t(n | (on ? 0x80 : 0)));
                left -= n;
            }
            i = j;
        }
        const uint32_t size = uint32_t(rle.size());
        blob.insert(blob.end(), reinterpret_cast<const uint8_t*>(&size), reinterpret_cast<const uint8_t*>(&size) + 4);
        blob.insert(blob.end(), rle.begin(), rle.end());
    }
    return blob;
}

bool cbddlp_split(const std::vector<uint8_t> &blob, std::vector<std::vector<uint8_t>> &planes)
{
    planes.clear();
    size_t at = 0;
    for (uint32_t plane = 0; plane < cbddlp_planes; ++plane) {
        uint32_t size;
        if (at + 4 > blob.size())
            return false;
        std::memcpy(&size, &blob[at], 4);
        at += 4;
        if (size > blob.size() - at)
            return false;
        planes.emplace_back(blob.begin() + at, blob.begin() + at + size);
        at += size;
    }
    return at == blob.size();
}

bool cbddlp_decode(const std::vector<std::vector<uint8_t>> &planes, uint8_t *pixels, size_t count)
{
    std::fill(pixels, pixels + count, uint8_t(0));
    for (const std::vector<uint8_t> &rle : planes) {
        size_t px = 0;
        for (uint8_t code : rle) {
            const size_t n = code & 0x7F;
            if (n > count - px)
                return false;
            if (code & 0x80)
                for (size_t k = 0; k < n; ++k)
                    ++pixels[px + k];
            px += n;
        }
        if (px != count)
            return false;
    }
    const size_t step = 256 / cbddlp_planes;
    for (size_t i = 0; i < count; ++i) {
        const size_t gray = size_t(pixels[i]) * step;
        pixels[i] = uint8_t(gray > 0 ? gray - 1 : 0);
    }
    return true;
}

std::vector<uint8_t> build_cbddlp(const FileParams &p, const std::vector<std::vector<uint8_t>> &images)
{
    if (images.size() != p.layers.size())
        throw ExportError("The cbddlp file needs one image for each layer.");

    const size_t layer_count = p.layers.size();
    Buffer b;

    b.u32(MagicCbddlp);
    b.u32(CbddlpVersion);
    b.f32(p.bed_x_mm); b.f32(p.bed_y_mm); b.f32(p.bed_z_mm);
    b.zeros(8);
    b.f32(layer_count ? p.layers.back().z_mm : 0.f);
    b.f32(p.layer_height_mm);
    b.f32(p.exposure_s);
    b.f32(p.bottom_exposure_s);
    b.f32(p.light_off_s);
    b.u32(p.bottom_layer_count);
    b.u32(p.res_x);
    b.u32(p.res_y);
    const size_t h_large  = b.hole();
    const size_t h_layers = b.hole();
    b.u32(uint32_t(layer_count));
    const size_t h_small  = b.hole();
    b.u32(p.print_time_s);
    b.u32(p.projector_type);
    const size_t h_params = b.hole();
    b.u32(uint32_t(PrintParamsSize));
    b.u32(cbddlp_planes);
    b.u16(p.light_pwm);
    b.u16(p.bottom_light_pwm);
    b.u32(0);                       // not encrypted
    b.u32(0);                       // no slicer info in this version
    b.u32(0);
    if (b.pos() != HeaderSize)
        throw ExportError("cbddlp header size mismatch.");

    b.patch(h_large, to_u32(b.pos()));
    write_preview(b, p.large_preview, 400, 300);
    b.patch(h_small, to_u32(b.pos()));
    write_preview(b, p.small_preview, 200, 125);

    b.patch(h_params, to_u32(b.pos()));
    b.f32(p.bottom_lift_mm); b.f32(p.bottom_lift_mm_min);
    b.f32(p.lift_mm); b.f32(p.lift_mm_min);
    b.f32(p.retract_mm_min);
    b.f32(p.volume_ml); b.f32(p.weight_g); b.f32(p.cost);
    b.f32(p.bottom_light_off_s);
    b.f32(p.light_off_s);
    b.u32(p.bottom_layer_count);
    b.zeros(16);

    // One table of layer definitions for each bit plane, then the plane images.
    b.patch(h_layers, to_u32(b.pos()));
    const size_t table_at = b.pos();
    b.zeros(LayerDefSize * layer_count * cbddlp_planes);

    for (size_t i = 0; i < layer_count; ++i) {
        std::vector<std::vector<uint8_t>> planes;
        if (! cbddlp_split(images[i], planes))
            throw ExportError("A cbddlp layer image is malformed.");
        for (uint32_t k = 0; k < cbddlp_planes; ++k) {
            const LayerParams &l = p.layers[i];
            Buffer def;
            def.f32(l.z_mm); def.f32(l.exposure_s); def.f32(l.light_off_s);
            def.u32(to_u32(b.pos())); def.u32(to_u32(planes[k].size())); def.u32(0);
            def.u32(uint32_t(LayerDefSize)); def.zeros(8);
            const std::vector<uint8_t> bytes = def.take();
            std::memcpy(b.data(table_at + LayerDefSize * (k * layer_count + i)), bytes.data(), bytes.size());
            b.bytes(planes[k].data(), planes[k].size());
        }
    }
    return b.take();
}

namespace {

// The key and IV of the encrypted layout (the same ones UVtools derives and mslicer lists).
const uint8_t EncKey[32] = {
    0xD0, 0x5B, 0x8E, 0x33, 0x71, 0xDE, 0x3D, 0x1A, 0xE5, 0x4F, 0x22, 0xDD, 0xDF, 0x5B, 0xFD, 0x94,
    0xAB, 0x5D, 0x64, 0x3A, 0x9D, 0x7E, 0xBF, 0xAF, 0x42, 0x03, 0xF3, 0x10, 0xD8, 0x52, 0x2A, 0xEA };
const uint8_t EncIv[16] = {
    0x0F, 0x01, 0x0A, 0x05, 0x05, 0x0B, 0x06, 0x07, 0x08, 0x06, 0x0A, 0x0C, 0x0C, 0x0D, 0x09, 0x0F };

constexpr uint32_t MagicEncrypted   = 0x12FD0107;
constexpr uint32_t EncryptedVersion = 5;
constexpr uint32_t EncLayerXorKey   = 0xEFBEADDE;
constexpr uint64_t EncChecksum      = 0xCAFEBABE;
constexpr size_t   EncHeaderSize    = 48;
constexpr size_t   EncSettingsSize  = 288;
constexpr size_t   EncLayerDefSize  = 88;
constexpr size_t   EncPointerSize   = 16;

} // namespace

std::vector<uint8_t> aes_cbc(const std::vector<uint8_t> &data, bool encrypt)
{
    if (data.size() % 16 != 0)
        throw ExportError("AES input is not a multiple of the block size.");
    std::vector<uint8_t> out(data.size());
    if (data.empty())
        return out;
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
    int len = 0, total = 0;
    if (! ctx || EVP_CipherInit_ex(ctx.get(), EVP_aes_256_cbc(), nullptr, EncKey, EncIv, encrypt ? 1 : 0) != 1)
        throw ExportError("AES setup failed.");
    EVP_CIPHER_CTX_set_padding(ctx.get(), 0);
    if (EVP_CipherUpdate(ctx.get(), out.data(), &len, data.data(), int(data.size())) != 1)
        throw ExportError("AES failed.");
    total = len;
    if (EVP_CipherFinal_ex(ctx.get(), out.data() + total, &len) != 1)
        throw ExportError("AES failed.");
    total += len;
    out.resize(size_t(total));
    return out;
}

std::vector<uint8_t> build_ctb_encrypted(const FileParams &p, const std::vector<std::vector<uint8_t>> &images)
{
    if (images.size() != p.layers.size())
        throw ExportError("The encrypted CTB file needs one image for each layer.");
    const size_t layer_count = p.layers.size();

    Buffer b;
    b.zeros(EncHeaderSize + EncSettingsSize);       // both are written at the end

    // Previews, the machine name, the disclaimer and the resin parameters.
    const size_t large_at = b.pos();
    write_preview(b, p.large_preview, 400, 300, true);
    const size_t small_at = b.pos();
    write_preview(b, p.small_preview, 200, 125, true);

    const size_t name_at = b.pos();
    b.bytes(p.machine_name.data(), p.machine_name.size());
    const size_t disclaimer_at = b.pos();
    const std::string disclaimer = Disclaimer;
    b.bytes(disclaimer.data(), disclaimer.size());

    const size_t resin_at = b.pos();
    const std::string resin_type = "Normal", resin_name = "Standard";
    const size_t resin_fixed = 40;
    const size_t machine_addr = resin_at + resin_fixed + resin_type.size() + resin_name.size();
    const size_t name_addr    = resin_at + resin_fixed + resin_type.size();
    const size_t type_addr    = resin_at + resin_fixed;
    b.zeros(4);
    b.u8(0); b.u8(0); b.u8(0); b.u8(0);             // resin color
    b.u32(to_u32(machine_addr));
    b.u32(to_u32(resin_type.size())); b.u32(to_u32(type_addr));
    b.u32(to_u32(resin_name.size())); b.u32(to_u32(name_addr));
    b.u32(to_u32(p.machine_name.size()));
    b.f32(1.1f);
    b.zeros(4);
    b.bytes(resin_type.data(), resin_type.size());
    b.bytes(resin_name.data(), resin_name.size());
    b.bytes(p.machine_name.data(), p.machine_name.size());

    // Layers: a table of pointers, then each layer's definition and image.
    const size_t pointers_at = b.pos();
    b.zeros(EncPointerSize * layer_count);
    for (size_t i = 0; i < layer_count; ++i) {
        const LayerParams &l = p.layers[i];
        const std::vector<uint8_t> &img = images[i];
        const uint64_t at = b.pos();
        Buffer ptr;
        ptr.u32(uint32_t(at % PageSize)); ptr.u32(uint32_t(at / PageSize));
        ptr.u32(uint32_t(EncLayerDefSize)); ptr.u32(0);
        const std::vector<uint8_t> ptr_bytes = ptr.take();
        std::memcpy(b.data(pointers_at + EncPointerSize * i), ptr_bytes.data(), ptr_bytes.size());

        const uint64_t data_at = at + EncLayerDefSize;
        b.u32(uint32_t(EncLayerDefSize));
        b.f32(l.z_mm); b.f32(l.exposure_s); b.f32(l.light_off_s);
        b.u32(uint32_t(data_at % PageSize)); b.u32(uint32_t(data_at / PageSize));
        b.u32(to_u32(img.size()));
        b.u32(0);
        b.u32(0); b.u32(0);                          // no encrypted part of the image
        b.f32(l.lift_mm); b.f32(l.lift_mm_min);
        b.f32(0.f); b.f32(0.f);                      // second stage lift
        b.f32(l.retract_mm_min);
        b.f32(0.f); b.f32(0.f);                      // second stage retract
        b.f32(l.rest_before_lift_s); b.f32(l.rest_after_lift_s); b.f32(l.rest_after_retract_s);
        b.f32(l.light_pwm);
        b.u32(0);
        const size_t img_at = b.pos();
        b.bytes(img.data(), img.size());
        layer_cipher(b.data(img_at), img.size(), EncLayerXorKey, uint32_t(i));
    }

    // The signature: the SHA-256 of the checksum value, encrypted.
    b.u32(0x422052FA); b.u32(0);
    uint64_t checksum = EncChecksum;
    std::vector<uint8_t> digest(32);
    unsigned int digest_len = 0;
    if (EVP_Digest(&checksum, sizeof checksum, digest.data(), &digest_len, EVP_sha256(), nullptr) != 1 || digest_len != 32)
        throw ExportError("SHA-256 failed.");
    const std::vector<uint8_t> signature = aes_cbc(digest, true);
    const size_t signature_at = b.pos();
    b.bytes(signature.data(), signature.size());
    b.u32(0x6D4232B3);

    // The settings, encrypted.
    Buffer s;
    s.u32(uint32_t(checksum)); s.u32(uint32_t(checksum >> 32));
    s.u32(to_u32(pointers_at));
    s.f32(p.bed_x_mm); s.f32(p.bed_y_mm); s.f32(p.bed_z_mm);
    s.zeros(8);
    s.f32(layer_count ? p.layers.back().z_mm : 0.f);
    s.f32(p.layer_height_mm);
    s.f32(p.exposure_s); s.f32(p.bottom_exposure_s); s.f32(p.light_off_s);
    s.u32(p.bottom_layer_count);
    s.u32(p.res_x); s.u32(p.res_y);
    s.u32(uint32_t(layer_count));
    s.u32(to_u32(large_at)); s.u32(to_u32(small_at));
    s.u32(p.print_time_s);
    s.u32(p.projector_type);
    s.f32(p.bottom_lift_mm); s.f32(p.bottom_lift_mm_min);
    s.f32(p.lift_mm); s.f32(p.lift_mm_min);
    s.f32(p.retract_mm_min);
    s.f32(p.volume_ml); s.f32(p.weight_g); s.f32(p.cost);
    s.f32(p.bottom_light_off_s);
    s.u32(1);
    s.u16(p.light_pwm); s.u16(p.bottom_light_pwm);
    s.u32(EncLayerXorKey);
    s.f32(0.f); s.f32(0.f); s.f32(0.f); s.f32(0.f); s.f32(0.f); s.f32(0.f);   // second stage lift and retract
    s.f32(0.f);                                                               // rest time after lift
    s.u32(to_u32(name_at)); s.u32(to_u32(p.machine_name.size()));
    s.u8(0x0F);
    s.zeros(2);
    s.u8(0x40);
    s.u32(p.timestamp_minutes);
    s.u32(8);                                    // anti alias level
    s.f32(p.rest_after_retract_s);
    s.f32(0.f);
    s.u32(p.transition_layer_count);
    s.f32(p.bottom_retract_mm_min); s.f32(0.f);
    s.zeros(4); s.f32(4.f); s.zeros(4); s.f32(4.f);
    s.f32(p.rest_after_retract_s); s.f32(0.f); s.f32(0.f);
    s.f32(0.f);                                  // bottom retract height 2
    s.zeros(8);
    s.u32(4);
    s.u32(layer_count ? uint32_t(layer_count - 1) : 0);
    s.zeros(16);
    s.u32(to_u32(disclaimer_at)); s.u32(to_u32(disclaimer.size()));
    s.u32(0);
    s.u32(to_u32(resin_at));
    s.zeros(8);
    std::vector<uint8_t> settings = s.take();
    if (settings.size() != EncSettingsSize)
        throw ExportError("Encrypted CTB settings size mismatch.");
    settings = aes_cbc(settings, true);

    std::vector<uint8_t> file = b.take();
    std::memcpy(&file[EncHeaderSize], settings.data(), settings.size());

    Buffer h;
    h.u32(MagicEncrypted);
    h.u32(uint32_t(EncSettingsSize)); h.u32(uint32_t(EncHeaderSize));
    h.u32(0);
    h.u32(EncryptedVersion);
    h.u32(uint32_t(signature.size())); h.u32(to_u32(signature_at));
    h.u32(0);
    h.u16(1); h.u16(1);
    h.u32(0);
    h.u32(0x2A);
    h.u32(0);
    const std::vector<uint8_t> head = h.take();
    if (head.size() != EncHeaderSize)
        throw ExportError("Encrypted CTB header size mismatch.");
    std::memcpy(file.data(), head.data(), head.size());
    return file;
}

std::vector<uint8_t> build_ctb_v4(const FileParams &p, const std::vector<std::vector<uint8_t>> &images)
{
    if (images.size() != p.layers.size())
        throw ExportError("The CTB file needs one image for each layer.");

    const bool gktwo = p.dialect == CTBDialect::GKtwo;
    const std::string disclaimer = gktwo ? DisclaimerGKtwo : Disclaimer;
    const size_t layer_count = p.layers.size();
    const float total_height = layer_count ? p.layers.back().z_mm : 0.f;

    Buffer b;

    // Header. The offsets of the sections are patched in once they are known.
    b.u32(gktwo ? MagicGKtwo : MagicChitubox);
    b.u32(Version);
    b.f32(p.bed_x_mm); b.f32(p.bed_y_mm); b.f32(p.bed_z_mm);
    b.zeros(8);
    b.f32(total_height);
    b.f32(p.layer_height_mm);
    b.f32(p.exposure_s);
    b.f32(p.bottom_exposure_s);
    b.f32(p.light_off_s);
    b.u32(p.bottom_layer_count);
    b.u32(p.res_x);
    b.u32(p.res_y);
    const size_t h_large   = b.hole();
    const size_t h_layers  = b.hole();
    b.u32(uint32_t(layer_count));
    const size_t h_small   = b.hole();
    b.u32(p.print_time_s);
    b.u32(p.projector_type);
    const size_t h_params  = b.hole();
    b.u32(uint32_t(PrintParamsSize));
    b.u32(1);                       // anti alias level of the header; the gray levels are in the images
    b.u16(p.light_pwm);
    b.u16(p.bottom_light_pwm);
    b.u32(p.xor_key);
    const size_t h_slicer  = b.hole();
    b.u32(uint32_t(SlicerInfoSize));
    if (b.pos() != HeaderSize)
        throw ExportError("CTB header size mismatch.");

    // Previews: Chitubox shows a 400 x 300 and a 200 x 125 picture.
    b.patch(h_large, to_u32(b.pos()));
    write_preview(b, p.large_preview, 400, 300);
    b.patch(h_small, to_u32(b.pos()));
    write_preview(b, p.small_preview, 200, 125);

    // Print parameters.
    b.patch(h_params, to_u32(b.pos()));
    b.f32(p.bottom_lift_mm); b.f32(p.bottom_lift_mm_min);
    b.f32(p.lift_mm); b.f32(p.lift_mm_min);
    b.f32(p.retract_mm_min);
    b.f32(p.volume_ml); b.f32(p.weight_g); b.f32(p.cost);
    b.f32(p.bottom_light_off_s);
    b.f32(p.light_off_s);
    b.u32(p.bottom_layer_count);
    b.zeros(16);

    // Slicer info, the machine name, the disclaimer and the version 4 extension, in this order.
    b.patch(h_slicer, to_u32(b.pos()));
    const size_t slicer_start = b.pos();
    b.f32(0.f); b.f32(0.f); b.f32(0.f); b.f32(0.f); b.f32(0.f); b.f32(0.f);   // second stage lift and retract: none
    b.f32(0.f);                                                               // rest time after lift
    const size_t h_name    = b.hole();
    b.u32(to_u32(p.machine_name.size()));
    b.u8(0x0F);                    // anti alias flag
    b.zeros(2);
    b.u8(0x40);                    // per layer settings are allowed
    b.u32(p.timestamp_minutes);
    b.u32(1);                      // anti alias level
    b.u32(SoftwareVersion);
    b.f32(p.rest_after_retract_s);
    b.f32(0.f);                    // rest time after lift 2
    b.u32(p.transition_layer_count);
    const size_t h_ext     = b.hole();
    b.zeros(8);
    if (b.pos() - slicer_start != SlicerInfoSize)
        throw ExportError("CTB slicer info size mismatch.");

    b.patch(h_name, to_u32(b.pos()));
    b.bytes(p.machine_name.data(), p.machine_name.size());
    const size_t disclaimer_at = b.pos();
    b.bytes(disclaimer.data(), disclaimer.size());

    b.patch(h_ext, to_u32(b.pos()));
    const size_t ext_start = b.pos();
    b.f32(p.bottom_retract_mm_min);
    b.f32(0.f);                    // bottom retract speed 2
    b.zeros(4);
    b.f32(4.f);
    b.zeros(4);
    b.f32(4.f);
    b.f32(p.rest_after_retract_s);
    b.f32(0.f);                    // rest time after lift
    b.f32(0.f);                    // rest time before lift
    b.f32(0.f);                    // bottom retract height 2
    b.f32(2955.996f);              // not understood; Chitubox writes this
    b.zeros(4);
    b.u32(5);
    b.u32(layer_count ? uint32_t(layer_count - 1) : 0);
    b.zeros(16);
    b.u32(to_u32(disclaimer_at));
    b.u32(to_u32(disclaimer.size()));
    b.u32(0);                      // resin parameters (version 5 only)
    b.zeros(380);
    if (b.pos() - ext_start != ExtensionSize)
        throw ExportError("CTB extension size mismatch.");

    // Layers: a table of definitions, then for each layer an extended definition followed by its image.
    b.patch(h_layers, to_u32(b.pos()));
    const size_t table_at = b.pos();
    b.zeros(LayerDefSize * layer_count);

    for (size_t i = 0; i < layer_count; ++i) {
        const LayerParams &l = p.layers[i];
        const std::vector<uint8_t> &img = images[i];
        const uint64_t data_pos = b.pos() + LayerDefExSize;
        const uint32_t page = uint32_t(data_pos / PageSize);
        const uint32_t addr = uint32_t(data_pos % PageSize);

        // the definition in the table
        Buffer def;
        def.f32(l.z_mm); def.f32(l.exposure_s); def.f32(l.light_off_s);
        def.u32(addr); def.u32(to_u32(img.size())); def.u32(page);
        def.u32(uint32_t(LayerDefExSize)); def.zeros(8);
        const std::vector<uint8_t> def_bytes = def.take();
        std::memcpy(b.data(table_at + LayerDefSize * i), def_bytes.data(), def_bytes.size());

        // the extended definition repeats it
        b.bytes(def_bytes.data(), def_bytes.size());
        b.u32(to_u32(LayerDefExSize + img.size()));
        b.f32(l.lift_mm); b.f32(l.lift_mm_min);
        b.f32(0.f); b.f32(0.f);                       // second stage lift
        b.f32(l.retract_mm_min);
        b.f32(0.f); b.f32(0.f);                       // second stage retract
        b.f32(l.rest_before_lift_s); b.f32(l.rest_after_lift_s); b.f32(l.rest_after_retract_s);
        b.f32(l.light_pwm);

        const size_t at = b.pos();
        b.bytes(img.data(), img.size());
        layer_cipher(b.data(at), img.size(), p.xor_key, uint32_t(i));
    }
    return b.take();
}

} // namespace ctb

namespace {

struct CTBRasterEncoder
{
    bool cbddlp { false };

    sla::EncodedRaster operator()(const void *ptr, size_t w, size_t h, size_t num_components)
    {
        // One 8 bit component: the grayscale, anti aliased layer image.
        if (cbddlp)
            return sla::EncodedRaster(ctb::cbddlp_encode(static_cast<const uint8_t*>(ptr), w * h * num_components), "cbddlp");
        std::vector<uint8_t> out = ctb::rle_encode(static_cast<const uint8_t*>(ptr), w * h * num_components);
        return sla::EncodedRaster(std::move(out), "ctb");
    }
};

// The size in pixels of the layer images, which follows the display orientation.
struct DisplayGeometry
{
    double   width_mm, height_mm;
    size_t   width_px, height_px;
    bool     portrait;
};

DisplayGeometry display_geometry(const SLAPrinterConfig &cfg)
{
    DisplayGeometry g{ cfg.display_width.getFloat(), cfg.display_height.getFloat(),
                       size_t(cfg.display_pixels_x.getInt()), size_t(cfg.display_pixels_y.getInt()),
                       cfg.display_orientation.getInt() == sla::RasterBase::roPortrait };
    if (g.portrait) {
        std::swap(g.width_mm, g.height_mm);
        std::swap(g.width_px, g.height_px);
    }
    return g;
}

// A nearest neighbour copy of a thumbnail (which is stored bottom to top) at a given size.
ctb::Preview scale_thumbnail(const ThumbnailData &t, uint32_t w, uint32_t h)
{
    ctb::Preview p;
    p.width = w;
    p.height = h;
    p.rgb.resize(size_t(w) * h * 3);
    for (uint32_t y = 0; y < h; ++y) {
        const size_t sy = t.height - 1 - std::min<size_t>(t.height - 1, size_t(y) * t.height / h);
        for (uint32_t x = 0; x < w; ++x) {
            const size_t sx = std::min<size_t>(t.width - 1, size_t(x) * t.width / w);
            const unsigned char *src = &t.pixels[(sy * t.width + sx) * 4];
            unsigned char *dst = &p.rgb[(size_t(y) * w + x) * 3];
            dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
        }
    }
    return p;
}

} // namespace

std::unique_ptr<sla::RasterBase> CTBArchive::create_raster() const
{
    const DisplayGeometry g = display_geometry(m_cfg);
    const sla::Resolution res{g.width_px, g.height_px};
    const sla::PixelDim   pxdim{g.width_mm / g.width_px, g.height_mm / g.height_px};
    const std::array<bool, 2> mirror{ m_cfg.display_mirror_x.getBool(), m_cfg.display_mirror_y.getBool() };
    const sla::RasterBase::Trafo tr{ g.portrait ? sla::RasterBase::roPortrait : sla::RasterBase::roLandscape, mirror };
    return sla::create_raster_grayscale_aa(res, pxdim, m_cfg.gamma_correction.getFloat(), tr);
}

sla::RasterEncoder CTBArchive::get_encoder() const
{
    return CTBRasterEncoder{ m_dialect == CTBDialect::Cbddlp };
}

void CTBArchive::export_print(const std::string     fname,
                              const SLAPrint       &print,
                              const ThumbnailsList &thumbnails,
                              const std::string    &/*projectname*/)
{
    const SLAMaterialConfig    &mat = print.material_config();
    const SLAPrintObjectConfig &obj = print.default_object_config();
    const DisplayGeometry       g   = display_geometry(m_cfg);
    const auto &levels = print.print_layers();
    const size_t layer_count = std::min(m_layers.size(), levels.size());

    ctb::FileParams p;
    p.dialect = m_dialect;
    p.bed_x_mm = float(g.width_mm); p.bed_y_mm = float(g.height_mm); p.bed_z_mm = m_cfg.printable_height.getFloat();
    p.res_x = uint32_t(g.width_px); p.res_y = uint32_t(g.height_px);
    p.layer_height_mm = float(obj.layer_height.getFloat());
    p.exposure_s = float(mat.exposure_time.getFloat());
    p.bottom_exposure_s = float(mat.initial_exposure_time.getFloat());
    p.bottom_layer_count = uint32_t(std::clamp<size_t>(size_t(std::max(0, mat.bottom_layer_count.getInt())), 0, layer_count));
    p.transition_layer_count = uint32_t(std::min<size_t>(size_t(std::max(0, obj.faded_layers.getInt())), layer_count - p.bottom_layer_count));
    p.light_off_s = p.bottom_light_off_s = float(mat.delay_after_exposure.get_at(0));
    p.rest_after_retract_s = float(mat.delay_before_exposure.get_at(0));
    p.light_pwm = p.bottom_light_pwm = uint16_t(std::clamp(mat.light_pwm.getInt(), 0, 255));
    p.lift_mm = p.bottom_lift_mm = float(mat.lift_distance.getFloat());
    p.lift_mm_min = p.bottom_lift_mm_min = float(mat.lift_speed.getFloat());
    p.retract_mm_min = p.bottom_retract_mm_min = float(mat.retract_speed.getFloat());
    p.projector_type = m_cfg.display_mirror_x.getBool() ? 1 : 0;
    p.xor_key = 0x5A7E3C91;
    p.timestamp_minutes = uint32_t(std::time(nullptr) / 60);
    p.machine_name = m_cfg.printer_model.value.empty() ? std::string("Pseudorca") : m_cfg.printer_model.value;

    const SLAPrintStatistics &stats = print.print_statistics();
    p.volume_ml = float((stats.objects_used_material + stats.support_used_material) / 1000.);
    p.weight_g  = p.volume_ml * float(mat.material_density.getFloat());
    const double bottle_ml = mat.bottle_volume.getFloat();
    p.cost = bottle_ml > 0. ? float(p.volume_ml * mat.bottle_cost.getFloat() / bottle_ml) : 0.f;

    // Per layer settings: the bottom layers, then a linear fade from the bottom to the normal exposure.
    double time_s = 0.;
    for (size_t i = 0; i < layer_count; ++i) {
        ctb::LayerParams l;
        l.z_mm = float(unscaled<double>(levels[i].level()));
        if (i < p.bottom_layer_count) {
            l.exposure_s = p.bottom_exposure_s;
        } else if (i < size_t(p.bottom_layer_count) + p.transition_layer_count) {
            const double k = double(i - p.bottom_layer_count + 1) / double(p.transition_layer_count + 1);
            l.exposure_s = float(p.bottom_exposure_s + (p.exposure_s - p.bottom_exposure_s) * k);
        } else {
            l.exposure_s = p.exposure_s;
        }
        l.light_off_s = p.light_off_s;
        l.lift_mm = p.lift_mm; l.lift_mm_min = p.lift_mm_min; l.retract_mm_min = p.retract_mm_min;
        l.rest_after_retract_s = p.rest_after_retract_s;
        l.light_pwm = float(p.light_pwm);
        p.layers.push_back(l);

        time_s += l.exposure_s + l.light_off_s + l.rest_after_retract_s;
        if (l.lift_mm_min > 0.f)
            time_s += l.lift_mm / l.lift_mm_min * 60.;
        if (l.retract_mm_min > 0.f)
            time_s += l.lift_mm / l.retract_mm_min * 60.;
    }
    p.print_time_s = uint32_t(std::ceil(time_s));

    if (! thumbnails.empty() && thumbnails.front().is_valid()) {
        p.large_preview = scale_thumbnail(thumbnails.front(), 400, 300);
        p.small_preview = scale_thumbnail(thumbnails.front(), 200, 125);
    }

    std::vector<std::vector<uint8_t>> images;
    images.reserve(layer_count);
    for (size_t i = 0; i < layer_count; ++i) {
        const auto *data = static_cast<const uint8_t*>(m_layers[i].data());
        images.emplace_back(data, data + m_layers[i].size());
    }

    const std::vector<uint8_t> file = m_dialect == CTBDialect::Cbddlp   ? ctb::build_cbddlp(p, images) :
                                      m_dialect == CTBDialect::Encrypted ? ctb::build_ctb_encrypted(p, images) :
                                                                           ctb::build_ctb_v4(p, images);
    std::ofstream out(fname, std::ios::binary | std::ios::out | std::ios::trunc);
    if (! out)
        throw ExportError("Cannot open " + fname + " for writing.");
    out.write(reinterpret_cast<const char*>(file.data()), std::streamsize(file.size()));
    out.close();
    if (! out)
        throw ExportError("Writing " + fname + " failed.");
}

} // namespace Slic3r
