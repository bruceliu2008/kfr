/** @addtogroup dft
 *  @{
 */
/*
  Copyright (C) 2016 D Levin (https://www.kfrlib.com)
  This file is part of KFR

  KFR is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  KFR is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with KFR.

  If GPL is not suitable for your project, you must purchase a commercial license to use KFR.
  Buying a commercial license is mandatory as soon as you develop commercial activities without
  disclosing the source code of your own applications.
  See https://www.kfrlib.com for details.
 */

#include "../base/basic_expressions.hpp"
#include "../testo/assert.hpp"
#include "bitrev.hpp"
#include "cache.hpp"
#include "convolution.hpp"
#include "fft.hpp"
#include "ft.hpp"

CMT_PRAGMA_GNU(GCC diagnostic push)
#if CMT_HAS_WARNING("-Wshadow")
CMT_PRAGMA_GNU(GCC diagnostic ignored "-Wshadow")
#endif

CMT_PRAGMA_MSVC(warning(push))
CMT_PRAGMA_MSVC(warning(disable : 4100))

namespace kfr
{

#define DFT_ASSERT TESTO_ASSERT_INACTIVE

template <typename T>
constexpr size_t fft_vector_width = platform<T>::vector_width;

template <typename T>
struct dft_stage
{
    size_t stage_size = 0;
    size_t data_size  = 0;
    size_t temp_size  = 0;
    u8* data          = nullptr;
    size_t repeats    = 1;
    size_t out_offset = 0;
    const char* name;
    bool recursion = false;

    void initialize(size_t size) { do_initialize(size); }

    KFR_INTRIN void execute(complex<T>* out, const complex<T>* in, u8* temp) { do_execute(out, in, temp); }
    virtual ~dft_stage() {}

protected:
    virtual void do_initialize(size_t) {}
    virtual void do_execute(complex<T>*, const complex<T>*, u8* temp) = 0;
};

CMT_PRAGMA_GNU(GCC diagnostic push)
#if CMT_HAS_WARNING("-Wassume")
CMT_PRAGMA_GNU(GCC diagnostic ignored "-Wassume")
#endif

namespace internal
{

template <size_t width, bool inverse, typename T>
KFR_SINTRIN cvec<T, width> radix4_apply_twiddle(csize_t<width>, cfalse_t /*split_format*/, cbool_t<inverse>,
                                                const cvec<T, width>& w, const cvec<T, width>& tw)
{
    cvec<T, width> ww  = w;
    cvec<T, width> tw_ = tw;
    cvec<T, width> b1  = ww * dupeven(tw_);
    ww                 = swap<2>(ww);

    if (inverse)
        tw_ = -(tw_);
    ww = subadd(b1, ww * dupodd(tw_));
    return ww;
}

template <size_t width, bool use_br2, bool inverse, bool aligned, typename T>
KFR_SINTRIN void radix4_body(size_t N, csize_t<width>, cfalse_t, cfalse_t, cfalse_t, cbool_t<use_br2>,
                             cbool_t<inverse>, cbool_t<aligned>, complex<T>* out, const complex<T>* in,
                             const complex<T>* twiddle)
{
    const size_t N4 = N / 4;
    cvec<T, width> w1, w2, w3;

    cvec<T, width> sum02, sum13, diff02, diff13;

    cvec<T, width> a0, a1, a2, a3;
    a0    = cread<width, aligned>(in + 0);
    a2    = cread<width, aligned>(in + N4 * 2);
    sum02 = a0 + a2;

    a1    = cread<width, aligned>(in + N4);
    a3    = cread<width, aligned>(in + N4 * 3);
    sum13 = a1 + a3;

    cwrite<width, aligned>(out, sum02 + sum13);
    w2 = sum02 - sum13;
    cwrite<width, aligned>(out + N4 * (use_br2 ? 1 : 2),
                           radix4_apply_twiddle(csize_t<width>(), cfalse, cbool_t<inverse>(), w2,
                                                cread<width, true>(twiddle + width)));
    diff02 = a0 - a2;
    diff13 = a1 - a3;
    if (inverse)
    {
        diff13 = (diff13 ^ broadcast<width * 2, T>(T(), -T()));
        diff13 = swap<2>(diff13);
    }
    else
    {
        diff13 = swap<2>(diff13);
        diff13 = (diff13 ^ broadcast<width * 2, T>(T(), -T()));
    }

    w1 = diff02 + diff13;

    cwrite<width, aligned>(out + N4 * (use_br2 ? 2 : 1),
                           radix4_apply_twiddle(csize_t<width>(), cfalse, cbool_t<inverse>(), w1,
                                                cread<width, true>(twiddle + 0)));
    w3 = diff02 - diff13;
    cwrite<width, aligned>(out + N4 * 3, radix4_apply_twiddle(csize_t<width>(), cfalse, cbool_t<inverse>(),
                                                              w3, cread<width, true>(twiddle + width * 2)));
}

template <size_t width, bool inverse, typename T>
KFR_SINTRIN cvec<T, width> radix4_apply_twiddle(csize_t<width>, ctrue_t /*split_format*/, cbool_t<inverse>,
                                                const cvec<T, width>& w, const cvec<T, width>& tw)
{
    vec<T, width> re1, im1, twre, twim;
    split(w, re1, im1);
    split(tw, twre, twim);

    const vec<T, width> b1re = re1 * twre;
    const vec<T, width> b1im = im1 * twre;
    if (inverse)
        return concat(b1re + im1 * twim, b1im - re1 * twim);
    else
        return concat(b1re - im1 * twim, b1im + re1 * twim);
}

template <size_t width, bool splitout, bool splitin, bool use_br2, bool inverse, bool aligned, typename T>
KFR_SINTRIN void radix4_body(size_t N, csize_t<width>, ctrue_t, cbool_t<splitout>, cbool_t<splitin>,
                             cbool_t<use_br2>, cbool_t<inverse>, cbool_t<aligned>, complex<T>* out,
                             const complex<T>* in, const complex<T>* twiddle)
{
    const size_t N4 = N / 4;
    cvec<T, width> w1, w2, w3;
    constexpr bool read_split  = !splitin && splitout;
    constexpr bool write_split = splitin && !splitout;

    vec<T, width> re0, im0, re1, im1, re2, im2, re3, im3;

    split(cread_split<width, aligned, read_split>(in + N4 * 0), re0, im0);
    split(cread_split<width, aligned, read_split>(in + N4 * 1), re1, im1);
    split(cread_split<width, aligned, read_split>(in + N4 * 2), re2, im2);
    split(cread_split<width, aligned, read_split>(in + N4 * 3), re3, im3);

    const vec<T, width> sum02re = re0 + re2;
    const vec<T, width> sum02im = im0 + im2;
    const vec<T, width> sum13re = re1 + re3;
    const vec<T, width> sum13im = im1 + im3;

    cwrite_split<width, aligned, write_split>(out, concat(sum02re + sum13re, sum02im + sum13im));
    w2 = concat(sum02re - sum13re, sum02im - sum13im);
    cwrite_split<width, aligned, write_split>(
        out + N4 * (use_br2 ? 1 : 2), radix4_apply_twiddle(csize_t<width>(), ctrue, cbool_t<inverse>(), w2,
                                                           cread<width, true>(twiddle + width)));

    const vec<T, width> diff02re = re0 - re2;
    const vec<T, width> diff02im = im0 - im2;
    const vec<T, width> diff13re = re1 - re3;
    const vec<T, width> diff13im = im1 - im3;

    (inverse ? w1 : w3) = concat(diff02re - diff13im, diff02im + diff13re);
    (inverse ? w3 : w1) = concat(diff02re + diff13im, diff02im - diff13re);

    cwrite_split<width, aligned, write_split>(
        out + N4 * (use_br2 ? 2 : 1), radix4_apply_twiddle(csize_t<width>(), ctrue, cbool_t<inverse>(), w1,
                                                           cread<width, true>(twiddle + 0)));
    cwrite_split<width, aligned, write_split>(
        out + N4 * 3, radix4_apply_twiddle(csize_t<width>(), ctrue, cbool_t<inverse>(), w3,
                                           cread<width, true>(twiddle + width * 2)));
}

template <typename T>
CMT_NOINLINE cvec<T, 1> calculate_twiddle(size_t n, size_t size)
{
    if (n == 0)
    {
        return make_vector(static_cast<T>(1), static_cast<T>(0));
    }
    else if (n == size / 4)
    {
        return make_vector(static_cast<T>(0), static_cast<T>(-1));
    }
    else if (n == size / 2)
    {
        return make_vector(static_cast<T>(-1), static_cast<T>(0));
    }
    else if (n == size * 3 / 4)
    {
        return make_vector(static_cast<T>(0), static_cast<T>(1));
    }
    else
    {
        fbase kth  = c_pi<fbase, 2> * (n / static_cast<fbase>(size));
        fbase tcos = +kfr::cos(kth);
        fbase tsin = -kfr::sin(kth);
        return make_vector(static_cast<T>(tcos), static_cast<T>(tsin));
    }
}

template <typename T, size_t width>
KFR_SINTRIN void initialize_twiddles_impl(complex<T>*& twiddle, size_t nn, size_t nnstep, size_t size,
                                          bool split_format)
{
    vec<T, 2 * width> result = T();
    CMT_LOOP_UNROLL
    for (size_t i = 0; i < width; i++)
    {
        const cvec<T, 1> r = calculate_twiddle<T>(nn + nnstep * i, size);
        result[i * 2]      = r[0];
        result[i * 2 + 1]  = r[1];
    }
    if (split_format)
        ref_cast<cvec<T, width>>(twiddle[0]) = splitpairs(result);
    else
        ref_cast<cvec<T, width>>(twiddle[0]) = result;
    twiddle += width;
}

template <typename T, size_t width>
CMT_NOINLINE void initialize_twiddles(complex<T>*& twiddle, size_t stage_size, size_t size, bool split_format)
{
    const size_t count = stage_size / 4;
    size_t nnstep      = size / stage_size;
    DFT_ASSERT(width <= count);
    CMT_LOOP_NOUNROLL
    for (size_t n = 0; n < count; n += width)
    {
        initialize_twiddles_impl<T, width>(twiddle, n * nnstep * 1, nnstep * 1, size, split_format);
        initialize_twiddles_impl<T, width>(twiddle, n * nnstep * 2, nnstep * 2, size, split_format);
        initialize_twiddles_impl<T, width>(twiddle, n * nnstep * 3, nnstep * 3, size, split_format);
    }
}

#if defined CMT_ARCH_SSE
#ifdef CMT_COMPILER_GNU
#define KFR_PREFETCH(addr) __builtin_prefetch(::kfr::ptr_cast<void>(addr), 0, _MM_HINT_T0);
#else
#define KFR_PREFETCH(addr) _mm_prefetch(::kfr::ptr_cast<char>(addr), _MM_HINT_T0);
#endif
#else
#define KFR_PREFETCH(addr) __builtin_prefetch(::kfr::ptr_cast<void>(addr));
#endif

template <typename T>
KFR_SINTRIN void prefetch_one(const complex<T>* in)
{
    KFR_PREFETCH(in);
}

template <typename T>
KFR_SINTRIN void prefetch_four(size_t stride, const complex<T>* in)
{
    KFR_PREFETCH(in);
    KFR_PREFETCH(in + stride);
    KFR_PREFETCH(in + stride * 2);
    KFR_PREFETCH(in + stride * 3);
}

template <typename Ntype, size_t width, bool splitout, bool splitin, bool prefetch, bool use_br2,
          bool inverse, bool aligned, typename T>
KFR_SINTRIN cfalse_t radix4_pass(Ntype N, size_t blocks, csize_t<width>, cbool_t<splitout>, cbool_t<splitin>,
                                 cbool_t<use_br2>, cbool_t<prefetch>, cbool_t<inverse>, cbool_t<aligned>,
                                 complex<T>* out, const complex<T>* in, const complex<T>*& twiddle)
{
    constexpr static size_t prefetch_offset = width * 8;
    const auto N4                           = N / csize_t<4>();
    const auto N43                          = N4 * csize_t<3>();
    CMT_ASSUME(blocks > 0);
    CMT_ASSUME(N > 0);
    CMT_ASSUME(N4 > 0);
    DFT_ASSERT(width <= N4);
    CMT_LOOP_NOUNROLL for (size_t b = 0; b < blocks; b++)
    {
        CMT_PRAGMA_CLANG(clang loop unroll_count(2))
        for (size_t n2 = 0; n2 < N4; n2 += width)
        {
            if (prefetch)
                prefetch_four(N4, in + prefetch_offset);
            radix4_body(N, csize_t<width>(), cbool_t<(splitout || splitin)>(), cbool_t<splitout>(),
                        cbool_t<splitin>(), cbool_t<use_br2>(), cbool_t<inverse>(), cbool_t<aligned>(), out,
                        in, twiddle + n2 * 3);
            in += width;
            out += width;
        }
        in += N43;
        out += N43;
    }
    twiddle += N43;
    return {};
}

template <size_t width, bool prefetch, bool use_br2, bool inverse, bool aligned, typename T>
KFR_SINTRIN ctrue_t radix4_pass(csize_t<32>, size_t blocks, csize_t<width>, cfalse_t, cfalse_t,
                                cbool_t<use_br2>, cbool_t<prefetch>, cbool_t<inverse>, cbool_t<aligned>,
                                complex<T>* out, const complex<T>*, const complex<T>*& /*twiddle*/)
{
    CMT_ASSUME(blocks > 0);
    constexpr static size_t prefetch_offset = 32 * 4;
    for (size_t b = 0; b < blocks; b++)
    {
        if (prefetch)
            prefetch_four(csize_t<64>(), out + prefetch_offset);
        cvec<T, 4> w0, w1, w2, w3, w4, w5, w6, w7;
        split(cread<8, aligned>(out + 0), w0, w1);
        split(cread<8, aligned>(out + 8), w2, w3);
        split(cread<8, aligned>(out + 16), w4, w5);
        split(cread<8, aligned>(out + 24), w6, w7);

        butterfly8<4, inverse>(w0, w1, w2, w3, w4, w5, w6, w7);

        w1 = cmul(w1, fixed_twiddle<T, 4, 32, 0, 1, inverse>());
        w2 = cmul(w2, fixed_twiddle<T, 4, 32, 0, 2, inverse>());
        w3 = cmul(w3, fixed_twiddle<T, 4, 32, 0, 3, inverse>());
        w4 = cmul(w4, fixed_twiddle<T, 4, 32, 0, 4, inverse>());
        w5 = cmul(w5, fixed_twiddle<T, 4, 32, 0, 5, inverse>());
        w6 = cmul(w6, fixed_twiddle<T, 4, 32, 0, 6, inverse>());
        w7 = cmul(w7, fixed_twiddle<T, 4, 32, 0, 7, inverse>());

        cvec<T, 8> z0, z1, z2, z3;
        transpose4x8(w0, w1, w2, w3, w4, w5, w6, w7, z0, z1, z2, z3);

        butterfly4<8, inverse>(cfalse, z0, z1, z2, z3, z0, z1, z2, z3);
        cwrite<32, aligned>(out, bitreverse<2>(concat(z0, z1, z2, z3)));
        out += 32;
    }
    return {};
}

template <size_t width, bool prefetch, bool use_br2, bool inverse, bool aligned, typename T>
KFR_SINTRIN ctrue_t radix4_pass(csize_t<8>, size_t blocks, csize_t<width>, cfalse_t, cfalse_t,
                                cbool_t<use_br2>, cbool_t<prefetch>, cbool_t<inverse>, cbool_t<aligned>,
                                complex<T>* out, const complex<T>*, const complex<T>*& /*twiddle*/)
{
    CMT_ASSUME(blocks > 0);
    DFT_ASSERT(2 <= blocks);
    constexpr static size_t prefetch_offset = width * 16;
    for (size_t b = 0; b < blocks; b += 2)
    {
        if (prefetch)
            prefetch_one(out + prefetch_offset);

        cvec<T, 8> vlo = cread<8, aligned>(out + 0);
        cvec<T, 8> vhi = cread<8, aligned>(out + 8);
        butterfly8<inverse>(vlo);
        butterfly8<inverse>(vhi);
        vlo = permutegroups<(2), 0, 4, 2, 6, 1, 5, 3, 7>(vlo);
        vhi = permutegroups<(2), 0, 4, 2, 6, 1, 5, 3, 7>(vhi);
        cwrite<8, aligned>(out, vlo);
        cwrite<8, aligned>(out + 8, vhi);
        out += 16;
    }
    return {};
}

template <size_t width, bool prefetch, bool use_br2, bool inverse, bool aligned, typename T>
KFR_SINTRIN ctrue_t radix4_pass(csize_t<16>, size_t blocks, csize_t<width>, cfalse_t, cfalse_t,
                                cbool_t<use_br2>, cbool_t<prefetch>, cbool_t<inverse>, cbool_t<aligned>,
                                complex<T>* out, const complex<T>*, const complex<T>*& /*twiddle*/)
{
    CMT_ASSUME(blocks > 0);
    constexpr static size_t prefetch_offset = width * 4;
    DFT_ASSERT(2 <= blocks);
    CMT_PRAGMA_CLANG(clang loop unroll_count(2))
    for (size_t b = 0; b < blocks; b += 2)
    {
        if (prefetch)
            prefetch_one(out + prefetch_offset);

        cvec<T, 16> vlo = cread<16, aligned>(out);
        cvec<T, 16> vhi = cread<16, aligned>(out + 16);
        butterfly4<4, inverse>(vlo);
        butterfly4<4, inverse>(vhi);
        apply_twiddles4<0, 4, 4, inverse>(vlo);
        apply_twiddles4<0, 4, 4, inverse>(vhi);
        vlo = digitreverse4<2>(vlo);
        vhi = digitreverse4<2>(vhi);
        butterfly4<4, inverse>(vlo);
        butterfly4<4, inverse>(vhi);

        use_br2 ? cbitreverse_write(out, vlo) : cdigitreverse4_write(out, vlo);
        use_br2 ? cbitreverse_write(out + 16, vhi) : cdigitreverse4_write(out + 16, vhi);
        out += 32;
    }
    return {};
}

template <size_t width, bool prefetch, bool use_br2, bool inverse, bool aligned, typename T>
KFR_SINTRIN ctrue_t radix4_pass(csize_t<4>, size_t blocks, csize_t<width>, cfalse_t, cfalse_t,
                                cbool_t<use_br2>, cbool_t<prefetch>, cbool_t<inverse>, cbool_t<aligned>,
                                complex<T>* out, const complex<T>*, const complex<T>*& /*twiddle*/)
{
    constexpr static size_t prefetch_offset = width * 4;
    CMT_ASSUME(blocks > 0);
    DFT_ASSERT(4 <= blocks);
    CMT_LOOP_NOUNROLL
    for (size_t b = 0; b < blocks; b += 4)
    {
        if (prefetch)
            prefetch_one(out + prefetch_offset);

        cvec<T, 16> v16 = cdigitreverse4_read<16, aligned>(out);
        butterfly4<4, inverse>(v16);
        cdigitreverse4_write<aligned>(out, v16);

        out += 4 * 4;
    }
    return {};
}

template <typename T, bool splitin, bool is_even, bool inverse>
struct fft_stage_impl : dft_stage<T>
{
    fft_stage_impl(size_t stage_size)
    {
        this->stage_size = stage_size;
        this->repeats    = 4;
        this->recursion  = true;
        this->data_size =
            align_up(sizeof(complex<T>) * stage_size / 4 * 3, platform<>::native_cache_alignment);
    }

protected:
    constexpr static bool prefetch = true;
    constexpr static bool aligned  = false;
    constexpr static size_t width  = fft_vector_width<T>;

    virtual void do_initialize(size_t size) override final
    {
        complex<T>* twiddle = ptr_cast<complex<T>>(this->data);
        initialize_twiddles<T, width>(twiddle, this->stage_size, size, true);
    }

    virtual void do_execute(complex<T>* out, const complex<T>* in, u8* /*temp*/) override final
    {
        const complex<T>* twiddle = ptr_cast<complex<T>>(this->data);
        if (splitin)
            in = out;
        const size_t stg_size = this->stage_size;
        CMT_ASSUME(stg_size >= 2048);
        CMT_ASSUME(stg_size % 2048 == 0);
        radix4_pass(stg_size, 1, csize_t<width>(), ctrue, cbool_t<splitin>(), cbool_t<!is_even>(),
                    cbool_t<prefetch>(), cbool_t<inverse>(), cbool_t<aligned>(), out, in, twiddle);
    }
};

template <typename T, bool splitin, size_t size, bool inverse>
struct fft_final_stage_impl : dft_stage<T>
{
    fft_final_stage_impl(size_t)
    {
        this->stage_size = size;
        this->out_offset = size;
        this->repeats    = 4;
        this->recursion  = true;
        this->data_size  = align_up(sizeof(complex<T>) * size * 3 / 2, platform<>::native_cache_alignment);
    }

protected:
    constexpr static size_t width  = fft_vector_width<T>;
    constexpr static bool is_even  = cometa::is_even(ilog2(size));
    constexpr static bool use_br2  = !is_even;
    constexpr static bool aligned  = false;
    constexpr static bool prefetch = splitin;

    KFR_INTRIN void init_twiddles(csize_t<8>, size_t, cfalse_t, complex<T>*&) {}
    KFR_INTRIN void init_twiddles(csize_t<4>, size_t, cfalse_t, complex<T>*&) {}

    template <size_t N, bool pass_splitin>
    KFR_INTRIN void init_twiddles(csize_t<N>, size_t total_size, cbool_t<pass_splitin>, complex<T>*& twiddle)
    {
        constexpr bool pass_split   = N / 4 > 8 && N / 4 / 4 >= width;
        constexpr size_t pass_width = const_min(width, N / 4);
        initialize_twiddles<T, pass_width>(twiddle, N, total_size, pass_split || pass_splitin);
        init_twiddles(csize<N / 4>, total_size, cbool<pass_split>, twiddle);
    }

    virtual void do_initialize(size_t total_size) override final
    {
        complex<T>* twiddle = ptr_cast<complex<T>>(this->data);
        init_twiddles(csize<size>, total_size, cbool<splitin>, twiddle);
    }

    virtual void do_execute(complex<T>* out, const complex<T>* in, u8* /*temp*/) override
    {
        const complex<T>* twiddle = ptr_cast<complex<T>>(this->data);
        final_stage(csize<size>, 1, cbool<splitin>, out, in, twiddle);
    }

    //    KFR_INTRIN void final_stage(csize_t<32>, size_t invN, cfalse_t, complex<T>* out, const complex<T>*,
    //                                const complex<T>*& twiddle)
    //    {
    //        radix4_pass(csize_t<32>(), invN, csize_t<width>(), cfalse, cfalse, cbool_t<use_br2>(),
    //                    cbool_t<prefetch>(), cbool_t<inverse>(), cbool_t<aligned>(), out, out, twiddle);
    //    }
    //
    //    KFR_INTRIN void final_stage(csize_t<16>, size_t invN, cfalse_t, complex<T>* out, const complex<T>*,
    //                                const complex<T>*& twiddle)
    //    {
    //        radix4_pass(csize_t<16>(), invN, csize_t<width>(), cfalse, cfalse, cbool_t<use_br2>(),
    //                    cbool_t<prefetch>(), cbool_t<inverse>(), cbool_t<aligned>(), out, out, twiddle);
    //    }

    KFR_INTRIN void final_stage(csize_t<8>, size_t invN, cfalse_t, complex<T>* out, const complex<T>*,
                                const complex<T>*& twiddle)
    {
        radix4_pass(csize_t<8>(), invN, csize_t<width>(), cfalse, cfalse, cbool_t<use_br2>(),
                    cbool_t<prefetch>(), cbool_t<inverse>(), cbool_t<aligned>(), out, out, twiddle);
    }

    KFR_INTRIN void final_stage(csize_t<4>, size_t invN, cfalse_t, complex<T>* out, const complex<T>*,
                                const complex<T>*& twiddle)
    {
        radix4_pass(csize_t<4>(), invN, csize_t<width>(), cfalse, cfalse, cbool_t<use_br2>(),
                    cbool_t<prefetch>(), cbool_t<inverse>(), cbool_t<aligned>(), out, out, twiddle);
    }

    template <size_t N, bool pass_splitin>
    KFR_INTRIN void final_stage(csize_t<N>, size_t invN, cbool_t<pass_splitin>, complex<T>* out,
                                const complex<T>* in, const complex<T>*& twiddle)
    {
        static_assert(N > 8, "");
        constexpr bool pass_split   = N / 4 > 8 && N / 4 / 4 >= width;
        constexpr size_t pass_width = const_min(width, N / 4);
        static_assert(pass_width == width || (pass_split == pass_splitin), "");
        static_assert(pass_width <= N / 4, "");
        radix4_pass(N, invN, csize_t<pass_width>(), cbool<pass_split>, cbool_t<pass_splitin>(),
                    cbool_t<use_br2>(), cbool_t<prefetch>(), cbool_t<inverse>(), cbool_t<aligned>(), out, in,
                    twiddle);
        final_stage(csize<N / 4>, invN * 4, cbool<pass_split>, out, out, twiddle);
    }
};

template <typename T, bool is_even>
struct fft_reorder_stage_impl : dft_stage<T>
{
    fft_reorder_stage_impl(size_t stage_size)
    {
        this->stage_size = stage_size;
        log2n            = ilog2(stage_size);
        this->data_size  = 0;
    }

protected:
    size_t log2n;

    virtual void do_initialize(size_t) override final {}

    virtual void do_execute(complex<T>* out, const complex<T>*, u8* /*temp*/) override final
    {
        fft_reorder(out, log2n, cbool_t<!is_even>());
    }
};

template <typename T, size_t log2n, bool inverse>
struct fft_specialization;

template <typename T, bool inverse>
struct fft_specialization<T, 1, inverse> : dft_stage<T>
{
    fft_specialization(size_t) {}

protected:
    constexpr static bool aligned = false;
    virtual void do_execute(complex<T>* out, const complex<T>* in, u8*) override final
    {
        cvec<T, 1> a0, a1;
        split(cread<2, aligned>(in), a0, a1);
        cwrite<2, aligned>(out, concat(a0 + a1, a0 - a1));
    }
};

template <typename T, bool inverse>
struct fft_specialization<T, 2, inverse> : dft_stage<T>
{
    fft_specialization(size_t) {}

protected:
    constexpr static bool aligned = false;
    virtual void do_execute(complex<T>* out, const complex<T>* in, u8*) override final
    {
        cvec<T, 1> a0, a1, a2, a3;
        split(cread<4>(in), a0, a1, a2, a3);
        butterfly(cbool_t<inverse>(), a0, a1, a2, a3, a0, a1, a2, a3);
        cwrite<4>(out, concat(a0, a1, a2, a3));
    }
};

template <typename T, bool inverse>
struct fft_specialization<T, 3, inverse> : dft_stage<T>
{
    fft_specialization(size_t) {}

protected:
    constexpr static bool aligned = false;
    virtual void do_execute(complex<T>* out, const complex<T>* in, u8*) override final
    {
        cvec<T, 8> v8 = cread<8, aligned>(in);
        butterfly8<inverse>(v8);
        cwrite<8, aligned>(out, v8);
    }
};

template <typename T, bool inverse>
struct fft_specialization<T, 4, inverse> : dft_stage<T>
{
    fft_specialization(size_t) {}

protected:
    constexpr static bool aligned = false;
    virtual void do_execute(complex<T>* out, const complex<T>* in, u8*) override final
    {
        cvec<T, 16> v16 = cread<16, aligned>(in);
        butterfly16<inverse>(v16);
        cwrite<16, aligned>(out, v16);
    }
};

template <typename T, bool inverse>
struct fft_specialization<T, 5, inverse> : dft_stage<T>
{
    fft_specialization(size_t) {}

protected:
    constexpr static bool aligned = false;
    virtual void do_execute(complex<T>* out, const complex<T>* in, u8*) override final
    {
        cvec<T, 32> v32 = cread<32, aligned>(in);
        butterfly32<inverse>(v32);
        cwrite<32, aligned>(out, v32);
    }
};

template <typename T, bool inverse>
struct fft_specialization<T, 6, inverse> : dft_stage<T>
{
    fft_specialization(size_t) {}

protected:
    constexpr static bool aligned = false;
    virtual void do_execute(complex<T>* out, const complex<T>* in, u8*) override final
    {
        butterfly64(cbool_t<inverse>(), cbool_t<aligned>(), out, in);
    }
};

template <typename T, bool inverse>
struct fft_specialization<T, 7, inverse> : dft_stage<T>
{
    fft_specialization(size_t)
    {
        this->stage_size = 128;
        this->data_size  = align_up(sizeof(complex<T>) * 128 * 3 / 2, platform<>::native_cache_alignment);
    }

protected:
    constexpr static bool aligned        = false;
    constexpr static size_t width        = platform<T>::vector_width;
    constexpr static bool use_br2        = true;
    constexpr static bool prefetch       = false;
    constexpr static bool is_double      = sizeof(T) == 8;
    constexpr static size_t final_size   = is_double ? 8 : 32;
    constexpr static size_t split_format = final_size == 8;

    virtual void do_initialize(size_t total_size) override final
    {
        complex<T>* twiddle = ptr_cast<complex<T>>(this->data);
        initialize_twiddles<T, width>(twiddle, 128, total_size, split_format);
        initialize_twiddles<T, width>(twiddle, 32, total_size, split_format);
        initialize_twiddles<T, width>(twiddle, 8, total_size, split_format);
    }

    virtual void do_execute(complex<T>* out, const complex<T>* in, u8* /*temp*/) override final
    {
        const complex<T>* twiddle = ptr_cast<complex<T>>(this->data);
        final_pass(csize_t<final_size>(), out, in, twiddle);
        fft_reorder(out, csize_t<7>());
    }

    KFR_INTRIN void final_pass(csize_t<8>, complex<T>* out, const complex<T>* in, const complex<T>* twiddle)
    {
        radix4_pass(128, 1, csize_t<width>(), ctrue, cfalse, cbool_t<use_br2>(), cbool_t<prefetch>(),
                    cbool_t<inverse>(), cbool_t<aligned>(), out, in, twiddle);
        radix4_pass(32, 4, csize_t<width>(), cfalse, ctrue, cbool_t<use_br2>(), cbool_t<prefetch>(),
                    cbool_t<inverse>(), cbool_t<aligned>(), out, out, twiddle);
        radix4_pass(csize_t<8>(), 16, csize_t<width>(), cfalse, cfalse, cbool_t<use_br2>(),
                    cbool_t<prefetch>(), cbool_t<inverse>(), cbool_t<aligned>(), out, out, twiddle);
    }

    KFR_INTRIN void final_pass(csize_t<32>, complex<T>* out, const complex<T>* in, const complex<T>* twiddle)
    {
        radix4_pass(128, 1, csize_t<width>(), cfalse, cfalse, cbool_t<use_br2>(), cbool_t<prefetch>(),
                    cbool_t<inverse>(), cbool_t<aligned>(), out, in, twiddle);
        radix4_pass(csize_t<32>(), 4, csize_t<width>(), cfalse, cfalse, cbool_t<use_br2>(),
                    cbool_t<prefetch>(), cbool_t<inverse>(), cbool_t<aligned>(), out, out, twiddle);
    }
};

template <bool inverse>
struct fft_specialization<float, 8, inverse> : dft_stage<float>
{
    fft_specialization(size_t) { this->temp_size = sizeof(complex<float>) * 256; }

protected:
    virtual void do_execute(complex<float>* out, const complex<float>* in, u8* temp) override final
    {
        complex<float>* scratch = ptr_cast<complex<float>>(temp);
        if (out == in)
        {
            butterfly16_multi_flip<0, inverse>(scratch, out);
            butterfly16_multi_flip<1, inverse>(scratch, out);
            butterfly16_multi_flip<2, inverse>(scratch, out);
            butterfly16_multi_flip<3, inverse>(scratch, out);

            butterfly16_multi_natural<0, inverse>(out, scratch);
            butterfly16_multi_natural<1, inverse>(out, scratch);
            butterfly16_multi_natural<2, inverse>(out, scratch);
            butterfly16_multi_natural<3, inverse>(out, scratch);
        }
        else
        {
            butterfly16_multi_flip<0, inverse>(out, in);
            butterfly16_multi_flip<1, inverse>(out, in);
            butterfly16_multi_flip<2, inverse>(out, in);
            butterfly16_multi_flip<3, inverse>(out, in);

            butterfly16_multi_natural<0, inverse>(out, out);
            butterfly16_multi_natural<1, inverse>(out, out);
            butterfly16_multi_natural<2, inverse>(out, out);
            butterfly16_multi_natural<3, inverse>(out, out);
        }
    }
};

template <bool inverse>
struct fft_specialization<double, 8, inverse> : fft_final_stage_impl<double, false, 256, inverse>
{
    using T = double;
    using fft_final_stage_impl<double, false, 256, inverse>::fft_final_stage_impl;

    virtual void do_execute(complex<T>* out, const complex<T>* in, u8* /*temp*/) override final
    {
        fft_final_stage_impl<double, false, 256, inverse>::do_execute(out, in, nullptr);
        fft_reorder(out, csize_t<8>());
    }
};

template <typename T, bool splitin, bool is_even>
struct fft_stage_impl_t
{
    template <bool inverse>
    using type = internal::fft_stage_impl<T, splitin, is_even, inverse>;
};
template <typename T, bool splitin, size_t size>
struct fft_final_stage_impl_t
{
    template <bool inverse>
    using type = internal::fft_final_stage_impl<T, splitin, size, inverse>;
};
template <typename T, bool is_even>
struct fft_reorder_stage_impl_t
{
    template <bool>
    using type = internal::fft_reorder_stage_impl<T, is_even>;
};
template <typename T, size_t log2n, bool aligned>
struct fft_specialization_t
{
    template <bool inverse>
    using type = internal::fft_specialization<T, log2n, inverse>;
};
} // namespace internal

//

template <typename T>
template <template <bool inverse> class Stage>
void dft_plan<T>::add_stage(size_t stage_size, cbools_t<true, true>)
{
    dft_stage<T>* direct_stage  = new Stage<false>(stage_size);
    direct_stage->name          = nullptr;
    dft_stage<T>* inverse_stage = new Stage<true>(stage_size);
    inverse_stage->name         = nullptr;
    this->data_size += direct_stage->data_size;
    this->temp_size += direct_stage->temp_size;
    stages[0].push_back(dft_stage_ptr(direct_stage));
    stages[1].push_back(dft_stage_ptr(inverse_stage));
}

template <typename T>
template <template <bool inverse> class Stage>
void dft_plan<T>::add_stage(size_t stage_size, cbools_t<true, false>)
{
    dft_stage<T>* direct_stage = new Stage<false>(stage_size);
    direct_stage->name         = nullptr;
    this->data_size += direct_stage->data_size;
    this->temp_size += direct_stage->temp_size;
    stages[0].push_back(dft_stage_ptr(direct_stage));
}

template <typename T>
template <template <bool inverse> class Stage>
void dft_plan<T>::add_stage(size_t stage_size, cbools_t<false, true>)
{
    dft_stage<T>* inverse_stage = new Stage<true>(stage_size);
    inverse_stage->name         = nullptr;
    this->data_size += inverse_stage->data_size;
    this->temp_size += inverse_stage->temp_size;
    stages[1].push_back(dft_stage_ptr(inverse_stage));
}

template <typename T>
template <bool direct, bool inverse, bool is_even, bool first>
void dft_plan<T>::make_fft(size_t stage_size, cbools_t<direct, inverse> type, cbool_t<is_even>,
                           cbool_t<first>)
{
    constexpr size_t final_size = is_even ? 1024 : 512;

    using fft_stage_impl_t       = internal::fft_stage_impl_t<T, !first, is_even>;
    using fft_final_stage_impl_t = internal::fft_final_stage_impl_t<T, !first, final_size>;

    if (stage_size >= 2048)
    {
        add_stage<fft_stage_impl_t::template type>(stage_size, type);

        make_fft(stage_size / 4, cbools_t<direct, inverse>(), cbool_t<is_even>(), cfalse);
    }
    else
    {
        add_stage<fft_final_stage_impl_t::template type>(final_size, type);
    }
}

template <typename T>
template <bool direct, bool inverse>
void dft_plan<T>::initialize(cbools_t<direct, inverse>)
{
    data = autofree<u8>(data_size);
    if (direct)
    {
        size_t offset = 0;
        for (dft_stage_ptr& stage : stages[0])
        {
            stage->data = data.data() + offset;
            stage->initialize(this->size);
            offset += stage->data_size;
        }
    }
    if (inverse)
    {
        size_t offset = 0;
        for (dft_stage_ptr& stage : stages[1])
        {
            stage->data = data.data() + offset;
            if (!direct)
                stage->initialize(this->size);
            offset += stage->data_size;
        }
    }
}

template <typename T>
template <bool inverse>
void dft_plan<T>::execute_dft(cbool_t<inverse>, complex<T>* out, const complex<T>* in, u8* temp) const
{
    size_t stack[32] = { 0 };

    const size_t count = stages[inverse].size();

    for (size_t depth = 0; depth < count;)
    {
        if (stages[inverse][depth]->recursion)
        {
            complex<T>* rout      = out;
            const complex<T>* rin = in;
            size_t rdepth         = depth;
            size_t maxdepth       = depth;
            do
            {
                if (stack[rdepth] == stages[inverse][rdepth]->repeats)
                {
                    stack[rdepth] = 0;
                    rdepth--;
                }
                else
                {
                    stages[inverse][rdepth]->execute(rout, rin, temp);
                    rout += stages[inverse][rdepth]->out_offset;
                    rin = rout;
                    stack[rdepth]++;
                    if (rdepth < count - 1 && stages[inverse][rdepth + 1]->recursion)
                        rdepth++;
                    else
                        maxdepth = rdepth;
                }
            } while (rdepth != depth);
            depth = maxdepth + 1;
        }
        else
        {
            stages[inverse][depth]->execute(out, in, temp);
            depth++;
        }
        in = out;
    }
}

template <typename T>
template <bool direct, bool inverse>
dft_plan<T>::dft_plan(size_t size, cbools_t<direct, inverse> type) : size(size), temp_size(0), data_size(0)
{
    if (is_poweroftwo(size))
    {
        const size_t log2n = ilog2(size);
        cswitch(
            csizes_t<1, 2, 3, 4, 5, 6, 7, 8>(), log2n,
            [&](auto log2n) {
                (void)log2n;
                this->add_stage<
                    internal::fft_specialization_t<T, val_of(decltype(log2n)()), false>::template type>(size,
                                                                                                        type);
            },
            [&]() {
                cswitch(cfalse_true, is_even(log2n), [&](auto is_even) {
                    this->make_fft(size, type, is_even, ctrue);
                    this->add_stage<
                        internal::fft_reorder_stage_impl_t<T, val_of(decltype(is_even)())>::template type>(
                        size, type);
                });
            });
        initialize(type);
    }
}

template <typename T>
template <bool direct, bool inverse>
dft_plan_real<T>::dft_plan_real(size_t size, cbools_t<direct, inverse> type)
    : dft_plan<T>(size / 2, type), size(size), rtwiddle(size / 4)
{
    using namespace internal;

    constexpr size_t width = platform<T>::vector_width * 2;

    block_process(size / 4, csizes_t<width, 1>(), [=](size_t i, auto w) {
        constexpr size_t width = val_of(decltype(w)());
        cwrite<width>(rtwiddle.data() + i,
                      cossin(dup(-constants<T>::pi * ((enumerate<T, width>() + i + size / 4) / (size / 2)))));
    });
}

template <typename T>
void dft_plan_real<T>::to_fmt(complex<T>* out, dft_pack_format fmt) const
{
    using namespace internal;
    size_t csize = this->size / 2; // const size_t causes internal compiler error: in tsubst_copy in GCC 5.2

    constexpr size_t width = platform<T>::vector_width * 2;
    const cvec<T, 1> dc    = cread<1>(out);
    const size_t count     = csize / 2;

    block_process(count, csizes_t<width, 1>(), [=](size_t i, auto w) {
        constexpr size_t width    = val_of(decltype(w)());
        constexpr size_t widthm1  = width - 1;
        const cvec<T, width> tw   = cread<width>(rtwiddle.data() + i);
        const cvec<T, width> fpk  = cread<width>(out + i);
        const cvec<T, width> fpnk = reverse<2>(negodd(cread<width>(out + csize - i - widthm1)));

        const cvec<T, width> f1k = fpk + fpnk;
        const cvec<T, width> f2k = fpk - fpnk;
        const cvec<T, width> t   = cmul(f2k, tw);
        cwrite<width>(out + i, T(0.5) * (f1k + t));
        cwrite<width>(out + csize - i - widthm1, reverse<2>(negodd(T(0.5) * (f1k - t))));
    });

    {
        size_t k              = csize / 2;
        const cvec<T, 1> fpk  = cread<1>(out + k);
        const cvec<T, 1> fpnk = negodd(fpk);
        cwrite<1>(out + k, fpnk);
    }
    if (fmt == dft_pack_format::CCs)
    {
        cwrite<1>(out, pack(dc[0] + dc[1], 0));
        cwrite<1>(out + csize, pack(dc[0] - dc[1], 0));
    }
    else
    {
        cwrite<1>(out, pack(dc[0] + dc[1], dc[0] - dc[1]));
    }
}

template <typename T>
void dft_plan_real<T>::from_fmt(complex<T>* out, const complex<T>* in, dft_pack_format fmt) const
{
    using namespace internal;

    const size_t csize = this->size / 2;

    cvec<T, 1> dc;

    if (fmt == dft_pack_format::CCs)
    {
        dc = pack(in[0].real() + in[csize].real(), in[0].real() - in[csize].real());
    }
    else
    {
        dc = pack(in[0].real() + in[0].imag(), in[0].real() - in[0].imag());
    }

    constexpr size_t width = platform<T>::vector_width * 2;
    const size_t count     = csize / 2;

    block_process(count, csizes_t<width, 1>(), [=](size_t i, auto w) {
        i++;
        constexpr size_t width    = val_of(decltype(w)());
        constexpr size_t widthm1  = width - 1;
        const cvec<T, width> tw   = cread<width>(rtwiddle.data() + i);
        const cvec<T, width> fpk  = cread<width>(in + i);
        const cvec<T, width> fpnk = reverse<2>(negodd(cread<width>(in + csize - i - widthm1)));

        const cvec<T, width> f1k = fpk + fpnk;
        const cvec<T, width> f2k = fpk - fpnk;
        const cvec<T, width> t   = cmul_conj(f2k, tw);
        cwrite<width>(out + i, f1k + t);
        cwrite<width>(out + csize - i - widthm1, reverse<2>(negodd(f1k - t)));
    });

    {
        size_t k              = csize / 2;
        const cvec<T, 1> fpk  = cread<1>(in + k);
        const cvec<T, 1> fpnk = 2 * negodd(fpk);
        cwrite<1>(out + k, fpnk);
    }
    cwrite<1>(out, dc);
}

template <typename T>
dft_plan<T>::~dft_plan() {}

namespace internal
{

template <typename T>
univector<T> convolve(const univector_ref<const T>& src1, const univector_ref<const T>& src2)
{
    const size_t size                = next_poweroftwo(src1.size() + src2.size() - 1);
    univector<complex<T>> src1padded = src1;
    univector<complex<T>> src2padded = src2;
    src1padded.resize(size, 0);
    src2padded.resize(size, 0);

    dft_plan_ptr<T> dft = dft_cache::instance().get(ctype_t<T>(), size);
    univector<u8> temp(dft->temp_size);
    dft->execute(src1padded, src1padded, temp);
    dft->execute(src2padded, src2padded, temp);
    src1padded = src1padded * src2padded;
    dft->execute(src1padded, src1padded, temp, true);
    const T invsize = reciprocal<T>(size);
    return truncate(real(src1padded), src1.size() + src2.size() - 1) * invsize;
}

template <typename T>
univector<T> correlate(const univector_ref<const T>& src1, const univector_ref<const T>& src2)
{
    const size_t size                = next_poweroftwo(src1.size() + src2.size() - 1);
    univector<complex<T>> src1padded = src1;
    univector<complex<T>> src2padded = reverse(src2);
    src1padded.resize(size, 0);
    src2padded.resize(size, 0);
    dft_plan_ptr<T> dft = dft_cache::instance().get(ctype_t<T>(), size);
    univector<u8> temp(dft->temp_size);
    dft->execute(src1padded, src1padded, temp);
    dft->execute(src2padded, src2padded, temp);
    src1padded = src1padded * src2padded;
    dft->execute(src1padded, src1padded, temp, true);
    const T invsize = reciprocal<T>(size);
    return truncate(real(src1padded), src1.size() + src2.size() - 1) * invsize;
}

template <typename T>
univector<T> autocorrelate(const univector_ref<const T>& src1)
{
    return internal::autocorrelate(src1.slice());
    univector<T> result = correlate(src1, src1);
    result              = result.slice(result.size() / 2);
    return result;
}

template univector<float> convolve<float>(const univector_ref<const float>&,
                                          const univector_ref<const float>&);
template univector<double> convolve<double>(const univector_ref<const double>&,
                                            const univector_ref<const double>&);
template univector<float> correlate<float>(const univector_ref<const float>&,
                                           const univector_ref<const float>&);
template univector<double> correlate<double>(const univector_ref<const double>&,
                                             const univector_ref<const double>&);

template univector<float> autocorrelate<float>(const univector_ref<const float>&);
template univector<double> autocorrelate<double>(const univector_ref<const double>&);

} // namespace internal

template <typename T>
convolve_filter<T>::convolve_filter(size_t size, size_t block_size)
    : fft(2 * next_poweroftwo(block_size)), size(size), block_size(block_size), temp(fft.temp_size),
      segments((size + block_size - 1) / block_size)
{
}

template <typename T>
convolve_filter<T>::convolve_filter(const univector<T>& data, size_t block_size)
    : fft(2 * next_poweroftwo(block_size)), size(data.size()), block_size(next_poweroftwo(block_size)),
      temp(fft.temp_size),
      segments((data.size() + next_poweroftwo(block_size) - 1) / next_poweroftwo(block_size)),
      ir_segments((data.size() + next_poweroftwo(block_size) - 1) / next_poweroftwo(block_size)),
      input_position(0), position(0)
{
    set_data(data);
}

template <typename T>
void convolve_filter<T>::set_data(const univector<T>& data)
{
    univector<T> input(fft.size);
    const T ifftsize = reciprocal(T(fft.size));
    for (size_t i = 0; i < ir_segments.size(); i++)
    {
        segments[i].resize(block_size);
        ir_segments[i].resize(block_size, 0);
        input = padded(data.slice(i * block_size, block_size));

        fft.execute(ir_segments[i], input, temp, dft_pack_format::Perm);
        process(ir_segments[i], ir_segments[i] * ifftsize);
    }
    saved_input.resize(block_size, 0);
    scratch.resize(block_size * 2);
    premul.resize(block_size, 0);
    cscratch.resize(block_size);
    overlap.resize(block_size, 0);
}

template <typename T>
void convolve_filter<T>::process_buffer(T* output, const T* input, size_t size)
{
    size_t processed = 0;
    while (processed < size)
    {
        const size_t processing = std::min(size - processed, block_size - input_position);
        internal::builtin_memcpy(saved_input.data() + input_position, input + processed,
                                 processing * sizeof(T));

        process(scratch, padded(saved_input));
        fft.execute(segments[position], scratch, temp, dft_pack_format::Perm);

        if (input_position == 0)
        {
            process(premul, zeros());
            for (size_t i = 1; i < segments.size(); i++)
            {
                const size_t n = (position + i) % segments.size();
                fft_multiply_accumulate(premul, ir_segments[i], segments[n], dft_pack_format::Perm);
            }
        }
        fft_multiply_accumulate(cscratch, premul, ir_segments[0], segments[position], dft_pack_format::Perm);

        fft.execute(scratch, cscratch, temp, dft_pack_format::Perm);

        process(make_univector(output + processed, processing),
                scratch.slice(input_position) + overlap.slice(input_position));

        input_position += processing;
        if (input_position == block_size)
        {
            input_position = 0;
            process(saved_input, zeros());

            internal::builtin_memcpy(overlap.data(), scratch.data() + block_size, block_size * sizeof(T));

            position = position > 0 ? position - 1 : segments.size() - 1;
        }

        processed += processing;
    }
}

template convolve_filter<float>::convolve_filter(size_t, size_t);
template convolve_filter<double>::convolve_filter(size_t, size_t);

template convolve_filter<float>::convolve_filter(const univector<float>&, size_t);
template convolve_filter<double>::convolve_filter(const univector<double>&, size_t);

template void convolve_filter<float>::set_data(const univector<float>&);
template void convolve_filter<double>::set_data(const univector<double>&);

template void convolve_filter<float>::process_buffer(float* output, const float* input, size_t size);
template void convolve_filter<double>::process_buffer(double* output, const double* input, size_t size);

template dft_plan<float>::dft_plan(size_t, cbools_t<false, true>);
template dft_plan<float>::dft_plan(size_t, cbools_t<true, false>);
template dft_plan<float>::dft_plan(size_t, cbools_t<true, true>);
template dft_plan<float>::~dft_plan();
template void dft_plan<float>::execute_dft(cometa::cbool_t<false>, kfr::complex<float>* out,
                                           const kfr::complex<float>* in, kfr::u8* temp) const;
template void dft_plan<float>::execute_dft(cometa::cbool_t<true>, kfr::complex<float>* out,
                                           const kfr::complex<float>* in, kfr::u8* temp) const;
template dft_plan_real<float>::dft_plan_real(size_t, cbools_t<false, true>);
template dft_plan_real<float>::dft_plan_real(size_t, cbools_t<true, false>);
template dft_plan_real<float>::dft_plan_real(size_t, cbools_t<true, true>);
template void dft_plan_real<float>::from_fmt(kfr::complex<float>* out, const kfr::complex<float>* in,
                                             kfr::dft_pack_format fmt) const;
template void dft_plan_real<float>::to_fmt(kfr::complex<float>* out, kfr::dft_pack_format fmt) const;

template dft_plan<double>::dft_plan(size_t, cbools_t<false, true>);
template dft_plan<double>::dft_plan(size_t, cbools_t<true, false>);
template dft_plan<double>::dft_plan(size_t, cbools_t<true, true>);
template dft_plan<double>::~dft_plan();
template void dft_plan<double>::execute_dft(cometa::cbool_t<false>, kfr::complex<double>* out,
                                            const kfr::complex<double>* in, kfr::u8* temp) const;
template void dft_plan<double>::execute_dft(cometa::cbool_t<true>, kfr::complex<double>* out,
                                            const kfr::complex<double>* in, kfr::u8* temp) const;
template dft_plan_real<double>::dft_plan_real(size_t, cbools_t<false, true>);
template dft_plan_real<double>::dft_plan_real(size_t, cbools_t<true, false>);
template dft_plan_real<double>::dft_plan_real(size_t, cbools_t<true, true>);
template void dft_plan_real<double>::from_fmt(kfr::complex<double>* out, const kfr::complex<double>* in,
                                              kfr::dft_pack_format fmt) const;
template void dft_plan_real<double>::to_fmt(kfr::complex<double>* out, kfr::dft_pack_format fmt) const;

} // namespace kfr

CMT_PRAGMA_GNU(GCC diagnostic pop)

CMT_PRAGMA_MSVC(warning(pop))
