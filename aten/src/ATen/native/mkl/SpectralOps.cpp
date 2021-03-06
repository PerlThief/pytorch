#include <ATen/ATen.h>
#include <ATen/NativeFunctions.h>
#include <ATen/native/SpectralOpsUtils.h>
#include <ATen/Config.h>

#if !AT_MKL_ENABLED()

namespace at { namespace native {

REGISTER_NO_CPU_DISPATCH(fft_fill_with_conjugate_symmetry_stub, fft_fill_with_conjugate_symmetry_fn);

Tensor _fft_mkl(const Tensor& input, int64_t signal_ndim,
                bool complex_input, bool complex_output,
                bool inverse, IntArrayRef checked_signal_sizes,
                int64_t normalization, bool onesided,
                IntArrayRef output_sizes) {
  AT_ERROR("fft: ATen not compiled with MKL support");
}

}}

#else // AT_MKL_ENABLED

#include <ATen/ATen.h>
#include <ATen/Config.h>
#include <ATen/Dispatch.h>
#include <ATen/NativeFunctions.h>
#include <ATen/Parallel.h>
#include <ATen/Utils.h>

#include <ATen/native/TensorIterator.h>

#include <algorithm>
#include <vector>
#include <numeric>
#include <cmath>

#include <mkl_dfti.h>
#include <ATen/mkl/Exceptions.h>
#include <ATen/mkl/Descriptors.h>
#include <ATen/mkl/Limits.h>


namespace at { namespace native {

// In real-to-complex transform, MKL FFT only fills half of the values due to
// conjugate symmetry. See native/SpectralUtils.h for more details.
// The following structs are used to fill in the other half with symmetry in
// case of real-to-complex transform with onesided=False flag.
// See NOTE [ Fourier Transform Conjugate Symmetry ] in native/SpectralOpsUtils.h.

template <typename scalar_t>
static __ubsan_ignore_undefined__  // UBSAN gives false positives on using negative indexes with a pointer
void _fft_fill_with_conjugate_symmetry_slice(
    Range range, at::ArrayRef<bool> is_mirrored_dim, IntArrayRef signal_half_sizes,
    IntArrayRef in_strides, const scalar_t * in_ptr,
    IntArrayRef out_strides, scalar_t * out_ptr) {
  const auto ndim = signal_half_sizes.size();
  DimVector iter_index(ndim, 0);

  // We explicitly loop over one row, then use this lambda to iterate over
  // n-dimensions. This advances iter_index by one row, while updating in_ptr
  // and out_ptr to point to the new row of data.
  auto advance_index = [&] {
    for (size_t i = 1; i < iter_index.size(); ++i) {
      if (iter_index[i] + 1 < signal_half_sizes[i]) {
        ++iter_index[i];
        in_ptr += in_strides[i];
        if (is_mirrored_dim[i]) {
          if (iter_index[i] == 1) {
            out_ptr += (signal_half_sizes[i] - 1) * out_strides[i];
          } else {
            out_ptr -= out_strides[i];
          }
        } else {
          out_ptr += out_strides[i];
        }
        return;
      }

      in_ptr -= in_strides[i] * iter_index[i];
      if (is_mirrored_dim[i]) {
        out_ptr -= out_strides[i];
      } else {
        out_ptr -= out_strides[i] * iter_index[i];
      }
      iter_index[i] = 0;
    }
  };

  // The data slice we operate on may start part-way into the data
  // Update iter_index and pointers to reference the start of the slice
  if (range.begin > 0) {
    iter_index[0] = range.begin % signal_half_sizes[0];
    auto linear_idx = range.begin / signal_half_sizes[0];

    for (size_t i = 1; i < ndim && linear_idx > 0; ++i) {
      iter_index[i] = linear_idx % signal_half_sizes[i];
      linear_idx = linear_idx / signal_half_sizes[i];

      if (iter_index[i] > 0) {
        in_ptr += in_strides[i] * iter_index[i];
        if (is_mirrored_dim[i]) {
          out_ptr += out_strides[i] * (signal_half_sizes[i] - iter_index[i]);
        } else {
          out_ptr += out_strides[i] * iter_index[i];
        }
      }
    }
  }

  auto numel_remaining = range.end - range.begin;

  if (is_mirrored_dim[0]) {
    // Explicitly loop over a Hermitian mirrored dimension
    if (iter_index[0] > 0) {
      auto end = std::min(signal_half_sizes[0], iter_index[0] + numel_remaining);
      for (int64_t i = iter_index[0]; i < end; ++i) {
        out_ptr[(signal_half_sizes[0] - i) * out_strides[0]] = std::conj(in_ptr[i * in_strides[0]]);
      }
      numel_remaining -= (end - iter_index[0]);
      iter_index[0] = 0;
      advance_index();
    }

    while (numel_remaining > 0) {
      auto end = std::min(signal_half_sizes[0], numel_remaining);
      out_ptr[0] = std::conj(in_ptr[0]);
      for (int64_t i = 1; i < end; ++i) {
        out_ptr[(signal_half_sizes[0] - i) * out_strides[0]] = std::conj(in_ptr[i * in_strides[0]]);
      }
      numel_remaining -= end;
      advance_index();
    }
  } else {
    // Explicit loop over a non-mirrored dimension, so just a simple conjugated copy
    while (numel_remaining > 0) {
      auto end = std::min(signal_half_sizes[0], iter_index[0] + numel_remaining);
      for (int64_t i = iter_index[0]; i != end; ++i) {
        out_ptr[i * out_strides[0]] = std::conj(in_ptr[i * in_strides[0]]);
      }
      numel_remaining -= (end - iter_index[0]);
      iter_index[0] = 0;
      advance_index();
    }
  }
}

static void _fft_fill_with_conjugate_symmetry_cpu_(
    ScalarType dtype, IntArrayRef mirror_dims, IntArrayRef signal_half_sizes,
    IntArrayRef in_strides_bytes, const void * in_data,
    IntArrayRef out_strides_bytes, void * out_data) {

  // Convert strides from bytes to elements
  const auto element_size = scalarTypeToTypeMeta(dtype).itemsize();
  const auto ndim = signal_half_sizes.size();
  DimVector in_strides(ndim), out_strides(ndim);
  for (int64_t i = 0; i < ndim; ++i) {
    TORCH_INTERNAL_ASSERT(in_strides_bytes[i] % element_size == 0);
    in_strides[i] = in_strides_bytes[i] / element_size;
    TORCH_INTERNAL_ASSERT(out_strides_bytes[i] % element_size == 0);
    out_strides[i] = out_strides_bytes[i] / element_size;
  }

  // Construct boolean mask for mirrored dims
  c10::SmallVector<bool, at::kDimVectorStaticSize> is_mirrored_dim(ndim, false);
  for (const auto& dim : mirror_dims) {
    is_mirrored_dim[dim] = true;
  }

  const auto numel = at::prod_intlist(signal_half_sizes);
  AT_DISPATCH_COMPLEX_TYPES(dtype, "_fft_fill_with_conjugate_symmetry", [&] {
    at::parallel_for(0, numel, at::internal::GRAIN_SIZE,
        [&](int64_t begin, int64_t end) {
          _fft_fill_with_conjugate_symmetry_slice(
              {begin, end}, is_mirrored_dim, signal_half_sizes,
              in_strides, static_cast<const scalar_t*>(in_data),
              out_strides, static_cast<scalar_t*>(out_data));
        });
  });
}

// Register this one implementation for all cpu types instead of compiling multiple times
REGISTER_ARCH_DISPATCH(fft_fill_with_conjugate_symmetry_stub, DEFAULT, &_fft_fill_with_conjugate_symmetry_cpu_)
REGISTER_AVX_DISPATCH(fft_fill_with_conjugate_symmetry_stub, &_fft_fill_with_conjugate_symmetry_cpu_)
REGISTER_AVX2_DISPATCH(fft_fill_with_conjugate_symmetry_stub, &_fft_fill_with_conjugate_symmetry_cpu_)

// MKL DFTI
Tensor _fft_mkl(const Tensor& self, int64_t signal_ndim,
                bool complex_input, bool complex_output,
                bool inverse, IntArrayRef checked_signal_sizes,
                int64_t normalization, bool onesided,
                IntArrayRef output_sizes) {
  int64_t batch = self.size(0);
  Tensor input = self;
  // real/imag dimension must aligned when viewed as of complex type
  if (complex_input) {
    bool need_contiguous = input.stride(-1) != 1;
    for (int64_t i = 0; !need_contiguous && i <= signal_ndim; i++) {
      need_contiguous |= input.stride(i) % 2 != 0;
    }
    if (need_contiguous) {
      input = input.contiguous();
    }
  }

  // check if we can use MKL because MKL_LONG is 32bit on some OS, e.g. Windows
  // need to check input and output size and strides
  // be careful about complex domain, where the stride needs to be divided by 2
  // only need to test upper bound MKL_LONG_MAX as these values are non-negative
  if (sizeof(MKL_LONG) < sizeof(int64_t)) {
    bool need_contiguous = false;
    int64_t inumel = 1 /* istride if we contiguous-fy */, onumel = 1;
    int64_t isize, osize, istride, ostride;
    for (int64_t i = signal_ndim; i >= 0; i--) {
      isize = input.size(i);
      osize = output_sizes[i];
      istride = complex_input ? input.stride(i) >> 1 : input.stride(i);
      ostride = onumel;
      TORCH_CHECK(isize <= MKL_LONG_MAX && osize <= MKL_LONG_MAX && ostride <= MKL_LONG_MAX,
               "MKL FFT: input signal numel exceeds allowed range [1 ~ ", MKL_LONG_MAX, "]");
      if (!need_contiguous && istride > MKL_LONG_MAX) {
        // If we didn't plan to contiguous-fy but the `istride` exceeds bound,
        // check if we can stride (equal to `inumel`) get back within bound if
        // we contiguous-fy. If so, then we need to always check `inumel`
        // instead for the remaining iterations. The iterations before this are
        // fine as `inumel` is non-decreasing.
        need_contiguous = true;
      }
      TORCH_CHECK(!need_contiguous || inumel <= MKL_LONG_MAX,
               "MKL FFT: input signal numel exceeds allowed range [1 ~ ", MKL_LONG_MAX, "]");
      inumel *= isize;
      onumel *= osize;
    }
  }
  Tensor output = at::empty(output_sizes, input.options());

  // precision
  DFTI_CONFIG_VALUE prec;
  if (input.scalar_type() == ScalarType::Float) {
    prec = DFTI_SINGLE;
  } else if (input.scalar_type() == ScalarType::Double) {
    prec = DFTI_DOUBLE;
  } else {
    std::ostringstream ss;
    ss << "MKL FFT doesn't support tensor of type: "
       << toString(input.scalar_type());
    AT_ERROR(ss.str());
  }
  // signal type
  DFTI_CONFIG_VALUE signal_type;
  if (!inverse) {
    signal_type = complex_input ? DFTI_COMPLEX : DFTI_REAL;
  } else {
    signal_type = complex_output ? DFTI_COMPLEX : DFTI_REAL;
  }
  // create descriptor with signal size
  std::vector<MKL_LONG> mkl_signal_sizes(checked_signal_sizes.begin(), checked_signal_sizes.end());
  DftiDescriptor descriptor;
  descriptor.init(prec, signal_type, signal_ndim, mkl_signal_sizes.data());
  // out of place FFT
  MKL_DFTI_CHECK(DftiSetValue(descriptor.get(), DFTI_PLACEMENT, DFTI_NOT_INPLACE));
  // batch mode
  MKL_DFTI_CHECK(DftiSetValue(descriptor.get(), DFTI_NUMBER_OF_TRANSFORMS, batch));

  auto istrides = input.strides();
  auto ostrides = output.strides();
  // batch dim stride, i.e., dist between each data
  MKL_LONG idist = complex_input ? istrides[0] >> 1 : istrides[0];
  MKL_LONG odist = complex_output ? ostrides[0] >> 1 : ostrides[0];
  MKL_DFTI_CHECK(DftiSetValue(descriptor.get(), DFTI_INPUT_DISTANCE, idist));
  MKL_DFTI_CHECK(DftiSetValue(descriptor.get(), DFTI_OUTPUT_DISTANCE, odist));
  // signal strides
  // first val is offset, set to zero (ignored)
  std::vector<MKL_LONG> mkl_istrides(1 + signal_ndim, 0), mkl_ostrides(1 + signal_ndim, 0);
  for (int64_t i = 1; i <= signal_ndim; i++) {
    mkl_istrides[i] = complex_input ? istrides[i] >> 1 : istrides[i];
    mkl_ostrides[i] = complex_output ? ostrides[i] >> 1 : ostrides[i];
  }
  MKL_DFTI_CHECK(DftiSetValue(descriptor.get(), DFTI_INPUT_STRIDES, mkl_istrides.data()));
  MKL_DFTI_CHECK(DftiSetValue(descriptor.get(), DFTI_OUTPUT_STRIDES, mkl_ostrides.data()));
  // if conjugate domain of real is involved, set standard CCE storage type
  // this will become default in MKL in future
  if (!complex_input || !complex_output) {
    MKL_DFTI_CHECK(DftiSetValue(descriptor.get(), DFTI_CONJUGATE_EVEN_STORAGE, DFTI_COMPLEX_COMPLEX));
  }
  // rescale if requested
  const auto norm = static_cast<fft_norm_mode>(normalization);
  if (norm != fft_norm_mode::none) {
    auto signal_numel = at::prod_intlist(checked_signal_sizes);
    double double_scale;
    if (norm == fft_norm_mode::by_root_n) {
      double_scale = 1.0 / std::sqrt(static_cast<double>(signal_numel));
    } else {
      double_scale = 1.0 / static_cast<double>(signal_numel);
    }
    MKL_DFTI_CHECK(DftiSetValue(descriptor.get(),
      inverse ? DFTI_BACKWARD_SCALE : DFTI_FORWARD_SCALE,
      prec == DFTI_DOUBLE ? double_scale : static_cast<float>(double_scale)));
  }
  // finalize
  MKL_DFTI_CHECK(DftiCommitDescriptor(descriptor.get()));
  // run
  if (!inverse) {
    MKL_DFTI_CHECK(DftiComputeForward(descriptor.get(), input.data_ptr(), output.data_ptr()));
  } else {
    MKL_DFTI_CHECK(DftiComputeBackward(descriptor.get(), input.data_ptr(), output.data_ptr()));
  }
  // now if needed, fill out the other half using Hermitian symmetry dim
  if (!complex_input && complex_output && !onesided) {
    DimVector signal_dims(signal_ndim);
    std::iota(signal_dims.begin(), signal_dims.end(), 1);
    auto out_as_complex = at::view_as_complex(output);
    at::native::_fft_fill_with_conjugate_symmetry_(out_as_complex, signal_dims);
  }
  return output;
}

}} // namespace at::native

#endif
