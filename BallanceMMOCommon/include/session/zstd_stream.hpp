#pragma once

// The session journal's compression (zstd v1.5.7, submodule/zstd; the user's
// decision of 2026-11-05: zstd level 3, flushed every 66 ticks, the size cap
// on the bytes on disk).  The writer side is an io_transform: each chunk the
// journal hands its queue (one 66-tick segment) is compressed and flushed, so
// the file decodes up to the last segment written even when the process dies
// before the frame's end.  The reader side decompresses a file and stops
// where its data stops (a frame cut short included).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <string>
#include <vector>

#include <zstd.h>

#include "io_queue.hpp"

namespace bmmo::session {

    constexpr int kJournalZstdLevel = 3;

    // A zstd frame's magic number (little-endian 0xFD2FB528).
    inline bool is_zstd_frame(const char* bytes, size_t size) {
        return size >= 4 && static_cast<uint8_t>(bytes[0]) == 0x28 && static_cast<uint8_t>(bytes[1]) == 0xB5
            && static_cast<uint8_t>(bytes[2]) == 0x2F && static_cast<uint8_t>(bytes[3]) == 0xFD;
    }

    class zstd_compressor final : public io_detail::io_transform {
    public:
        explicit zstd_compressor(int level) : ctx_(ZSTD_createCCtx()), buffer_(ZSTD_CStreamOutSize()) {
            if (ctx_) ZSTD_CCtx_setParameter(ctx_, ZSTD_c_compressionLevel, level);
        }
        ~zstd_compressor() override { ZSTD_freeCCtx(ctx_); }
        zstd_compressor(const zstd_compressor&) = delete;
        zstd_compressor& operator=(const zstd_compressor&) = delete;

        bool put(const std::string& chunk, std::string& out) override { return run(chunk, ZSTD_e_flush, out); }
        bool finish(std::string& out) override { return run({}, ZSTD_e_end, out); }

    private:
        // Everything of `in` consumed and flushed (e_flush) or the frame closed (e_end).
        bool run(const std::string& in, ZSTD_EndDirective mode, std::string& out) {
            if (!ctx_) return false;
            ZSTD_inBuffer input{in.data(), in.size(), 0};
            for (;;) {
                ZSTD_outBuffer output{buffer_.data(), buffer_.size(), 0};
                const size_t left = ZSTD_compressStream2(ctx_, &output, &input, mode);
                if (ZSTD_isError(left)) return false;
                out.append(buffer_.data(), output.pos);
                if (left == 0) return true;
            }
        }

        ZSTD_CCtx* ctx_;
        std::vector<char> buffer_;
    };

    // The decompressed bytes of a stream: read() hands out what there is, less
    // than asked only where the data ends.
    class zstd_reader {
    public:
        explicit zstd_reader(std::istream& in)
            : in_(in), ctx_(ZSTD_createDCtx()), input_(ZSTD_DStreamInSize()), output_(ZSTD_DStreamOutSize()) {}
        ~zstd_reader() { ZSTD_freeDCtx(ctx_); }
        zstd_reader(const zstd_reader&) = delete;
        zstd_reader& operator=(const zstd_reader&) = delete;

        size_t read(char* destination, size_t size) {
            size_t done = 0;
            while (done < size) {
                if (out_pos_ == out_end_ && !refill()) break;
                const size_t take = std::min(size - done, out_end_ - out_pos_);
                std::copy_n(output_.data() + out_pos_, take, destination + done);
                out_pos_ += take;
                done += take;
            }
            return done;
        }

    private:
        // More decompressed bytes in output_; false at the end of the data or on corrupt input.
        bool refill() {
            if (!ctx_ || broken_) return false;
            for (;;) {
                if (in_pos_ == in_end_) {
                    in_.read(input_.data(), static_cast<std::streamsize>(input_.size()));
                    in_end_ = static_cast<size_t>(in_.gcount());
                    in_pos_ = 0;
                    if (in_end_ == 0) return false;
                }
                ZSTD_inBuffer input{input_.data(), in_end_, in_pos_};
                ZSTD_outBuffer output{output_.data(), output_.size(), 0};
                const size_t result = ZSTD_decompressStream(ctx_, &output, &input);
                in_pos_ = input.pos;
                if (ZSTD_isError(result)) {
                    broken_ = true;
                    return false;
                }
                if (output.pos != 0) {
                    out_pos_ = 0;
                    out_end_ = output.pos;
                    return true;
                }
            }
        }

        std::istream& in_;
        ZSTD_DCtx* ctx_;
        std::vector<char> input_, output_;
        size_t in_pos_ = 0, in_end_ = 0, out_pos_ = 0, out_end_ = 0;
        bool broken_ = false;
    };

}
