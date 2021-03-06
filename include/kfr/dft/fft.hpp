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
#pragma once

#include "../base/complex.hpp"
#include "../base/constants.hpp"
#include "../base/memory.hpp"
#include "../base/read_write.hpp"
#include "../base/small_buffer.hpp"
#include "../base/vec.hpp"

CMT_PRAGMA_GNU(GCC diagnostic push)
#if CMT_HAS_WARNING("-Wshadow")
CMT_PRAGMA_GNU(GCC diagnostic ignored "-Wshadow")
#endif
#if CMT_HAS_WARNING("-Wundefined-inline")
CMT_PRAGMA_GNU(GCC diagnostic ignored "-Wundefined-inline")
#endif

CMT_PRAGMA_MSVC(warning(push))
CMT_PRAGMA_MSVC(warning(disable : 4100))

namespace kfr
{

namespace dft_type
{
constexpr cbools_t<true, true> both{};
constexpr cbools_t<true, false> direct{};
constexpr cbools_t<false, true> inverse{};
} // namespace dft_type

template <typename T>
struct dft_stage;

template <typename T>
struct dft_plan
{
    using dft_stage_ptr = std::unique_ptr<dft_stage<T>>;

    size_t size;
    size_t temp_size;

    template <bool direct = true, bool inverse = true>
    dft_plan(size_t size, cbools_t<direct, inverse> type = dft_type::both);

    KFR_INTRIN void execute(complex<T>* out, const complex<T>* in, u8* temp, bool inverse = false) const
    {
        if (inverse)
            execute_dft(ctrue, out, in, temp);
        else
            execute_dft(cfalse, out, in, temp);
    }
    ~dft_plan();
    template <bool inverse>
    KFR_INTRIN void execute(complex<T>* out, const complex<T>* in, u8* temp, cbool_t<inverse> inv) const
    {
        execute_dft(inv, out, in, temp);
    }

    template <size_t Tag1, size_t Tag2, size_t Tag3>
    KFR_INTRIN void execute(univector<complex<T>, Tag1>& out, const univector<complex<T>, Tag2>& in,
                            univector<u8, Tag3>& temp, bool inverse = false) const
    {
        if (inverse)
            execute_dft(ctrue, out.data(), in.data(), temp.data());
        else
            execute_dft(cfalse, out.data(), in.data(), temp.data());
    }
    template <bool inverse, size_t Tag1, size_t Tag2, size_t Tag3>
    KFR_INTRIN void execute(univector<complex<T>, Tag1>& out, const univector<complex<T>, Tag2>& in,
                            univector<u8, Tag3>& temp, cbool_t<inverse> inv) const
    {
        execute_dft(inv, out.data(), in.data(), temp.data());
    }

protected:
    autofree<u8> data;
    size_t data_size;
    std::vector<dft_stage_ptr> stages[2];

    template <template <bool inverse> class Stage>
    void add_stage(size_t stage_size, cbools_t<true, true>);
    template <template <bool inverse> class Stage>
    void add_stage(size_t stage_size, cbools_t<true, false>);
    template <template <bool inverse> class Stage>
    void add_stage(size_t stage_size, cbools_t<false, true>);

    template <bool direct, bool inverse, bool is_even, bool first>
    void make_fft(size_t stage_size, cbools_t<direct, inverse> type, cbool_t<is_even>, cbool_t<first>);

    template <bool direct, bool inverse>
    void initialize(cbools_t<direct, inverse>);
    template <bool inverse>
    void execute_dft(cbool_t<inverse>, complex<T>* out, const complex<T>* in, u8* temp) const;
};

enum class dft_pack_format
{
    Perm,
    CCs
};

template <typename T>
struct dft_plan_real : dft_plan<T>
{
    size_t size;
    template <bool direct = true, bool inverse = true>
    dft_plan_real(size_t size, cbools_t<direct, inverse> type = dft_type::both);

    KFR_INTRIN void execute(complex<T>* out, const T* in, u8* temp,
                            dft_pack_format fmt = dft_pack_format::CCs) const
    {
        this->execute_dft(cfalse, out, ptr_cast<complex<T>>(in), temp);
        to_fmt(out, fmt);
    }
    KFR_INTRIN void execute(T* out, const complex<T>* in, u8* temp,
                            dft_pack_format fmt = dft_pack_format::CCs) const
    {
        complex<T>* outdata = ptr_cast<complex<T>>(out);
        from_fmt(outdata, in, fmt);
        this->execute_dft(ctrue, outdata, outdata, temp);
    }

    template <size_t Tag1, size_t Tag2, size_t Tag3>
    KFR_INTRIN void execute(univector<complex<T>, Tag1>& out, const univector<T, Tag2>& in,
                            univector<u8, Tag3>& temp, dft_pack_format fmt = dft_pack_format::CCs) const
    {
        this->execute_dft(cfalse, out.data(), ptr_cast<complex<T>>(in.data()), temp.data());
        to_fmt(out.data(), fmt);
    }
    template <size_t Tag1, size_t Tag2, size_t Tag3>
    KFR_INTRIN void execute(univector<T, Tag1>& out, const univector<complex<T>, Tag2>& in,
                            univector<u8, Tag3>& temp, dft_pack_format fmt = dft_pack_format::CCs) const
    {
        complex<T>* outdata = ptr_cast<complex<T>>(out.data());
        from_fmt(outdata, in.data(), fmt);
        this->execute_dft(ctrue, outdata, outdata, temp.data());
    }

private:
    univector<complex<T>> rtwiddle;

    void to_fmt(complex<T>* out, dft_pack_format fmt) const;
    void from_fmt(complex<T>* out, const complex<T>* in, dft_pack_format fmt) const;
};

template <typename T, size_t Tag1, size_t Tag2, size_t Tag3>
void fft_multiply(univector<complex<T>, Tag1>& dest, const univector<complex<T>, Tag2>& src1,
                  const univector<complex<T>, Tag3>& src2, dft_pack_format fmt = dft_pack_format::CCs)
{
    const complex<T> f0(src1[0].real() * src2[0].real(), src1[0].imag() * src2[0].imag());

    dest = src1 * src2;

    if (fmt == dft_pack_format::Perm)
        dest[0] = f0;
}

template <typename T, size_t Tag1, size_t Tag2, size_t Tag3>
void fft_multiply_accumulate(univector<complex<T>, Tag1>& dest, const univector<complex<T>, Tag2>& src1,
                             const univector<complex<T>, Tag3>& src2,
                             dft_pack_format fmt = dft_pack_format::CCs)
{
    const complex<T> f0(dest[0].real() + src1[0].real() * src2[0].real(),
                        dest[0].imag() + src1[0].imag() * src2[0].imag());

    dest = dest + src1 * src2;

    if (fmt == dft_pack_format::Perm)
        dest[0] = f0;
}
template <typename T, size_t Tag1, size_t Tag2, size_t Tag3, size_t Tag4>
void fft_multiply_accumulate(univector<complex<T>, Tag1>& dest, const univector<complex<T>, Tag2>& src1,
                             const univector<complex<T>, Tag3>& src2, const univector<complex<T>, Tag4>& src3,
                             dft_pack_format fmt = dft_pack_format::CCs)
{
    const complex<T> f0(src1[0].real() + src2[0].real() * src3[0].real(),
                        src1[0].imag() + src2[0].imag() * src3[0].imag());

    dest = src1 + src2 * src3;

    if (fmt == dft_pack_format::Perm)
        dest[0] = f0;
}
} // namespace kfr

CMT_PRAGMA_GNU(GCC diagnostic pop)

CMT_PRAGMA_MSVC(warning(pop))
