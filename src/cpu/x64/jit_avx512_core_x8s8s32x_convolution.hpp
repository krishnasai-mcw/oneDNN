/*******************************************************************************
* Copyright 2016-2025 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#ifndef CPU_X64_JIT_AVX512_CORE_X8S8S32X_CONVOLUTION_HPP
#define CPU_X64_JIT_AVX512_CORE_X8S8S32X_CONVOLUTION_HPP

#include "common/c_types_map.hpp"
#include "common/dnnl_thread.hpp"
#include "common/memory_tracking.hpp"
#include "common/primitive.hpp"
#include "common/utils.hpp"

#include "cpu/cpu_convolution_pd.hpp"

#include "cpu/x64/jit_avx512_core_x8s8s32x_conv_kernel.hpp"

namespace dnnl {
namespace impl {
namespace cpu {
namespace x64 {

struct jit_avx512_core_x8s8s32x_convolution_fwd_t : public primitive_t {
    struct pd_t : public cpu_convolution_fwd_pd_t {
        using cpu_convolution_fwd_pd_t::cpu_convolution_fwd_pd_t;

        DECLARE_COMMON_PD_T(JIT_IMPL_NAME_HELPER("jit_int8:", jcp_.isa, ""),
                jit_avx512_core_x8s8s32x_convolution_fwd_t);

        status_t init(engine_t *engine) {
            using namespace data_type;
            using smask_t = primitive_attr_t::skip_mask_t;
            VDISPATCH_CONV(is_fwd(), VERBOSE_BAD_PROPKIND);

            VDISPATCH_CONV(utils::one_of(src_md(0)->data_type, s8, u8),
                    VERBOSE_UNSUPPORTED_DT);
            VDISPATCH_CONV(
                    weights_md(0)->data_type == s8, VERBOSE_UNSUPPORTED_DT);
            VDISPATCH_CONV(IMPLICATION(with_bias(),
                                   utils::one_of(weights_md(1)->data_type, f32,
                                           s32, s8, u8)),
                    VERBOSE_UNSUPPORTED_BIAS_CFG);
            VDISPATCH_CONV(
                    utils::one_of(dst_md(0)->data_type, f32, s32, s8, u8, bf16),
                    VERBOSE_UNSUPPORTED_DT);
            VDISPATCH_CONV(
                    desc()->accum_data_type == s32, VERBOSE_UNSUPPORTED_DT);

            VDISPATCH_CONV(set_default_alg_kind(alg_kind::convolution_direct),
                    VERBOSE_BAD_ALGORITHM);
            VDISPATCH_CONV(!has_zero_dim_memory(), VERBOSE_EMPTY_TENSOR, "");

            VDISPATCH_CONV(
                    attr()->has_default_values(smask_t::scales
                                    | smask_t::zero_points | smask_t::post_ops
                                    | smask_t::sum_dt,
                            dst_md(0)->data_type),
                    VERBOSE_UNSUPPORTED_ATTR);

            VDISPATCH_CONV(attr()->post_ops_.check_sum_consistency(
                                   dst_md(0)->data_type, /* is_int8 */ true),
                    VERBOSE_UNSUPPORTED_POSTOP);

            VDISPATCH_CONV(attr_scales_ok(), VERBOSE_UNSUPPORTED_SCALES_CFG);
            VDISPATCH_CONV(zero_points_ok(), VERBOSE_UNSUPPORTED_ZP_CFG);

            // TODO: make `init_conf` assign initialized object to `jcp_`
            CHECK(jit_avx512_core_x8s8s32x_fwd_kernel_t::init_conf(jcp_,
                    *desc(), src_md_, weights_md_, dst_md_, bias_md_, attr_,
                    dnnl_get_max_threads()));

            auto scratchpad = scratchpad_registry().registrar();
            jit_avx512_core_x8s8s32x_fwd_kernel_t::init_scratchpad(
                    scratchpad, jcp_, *attr());

            return status::success;
        }

        jit_conv_conf_t jcp_ = utils::zero<decltype(jcp_)>();

    protected:
        bool zero_points_ok() const {
            const auto &zp = attr()->zero_points_;

            if (!zp.has_default_values(DNNL_ARG_SRC)) {
                int mask_src = zp.get_mask(DNNL_ARG_SRC);
                const bool ok = mask_src == 0;
                if (!ok) return false;
            }
            if (!zp.has_default_values(DNNL_ARG_DST)) {
                int mask_dst = zp.get_mask(DNNL_ARG_DST);
                const bool ok = mask_dst == 0;
                if (!ok) return false;
            }

            return zp.has_default_values(DNNL_ARG_WEIGHTS);
        }
    };

    jit_avx512_core_x8s8s32x_convolution_fwd_t(const pd_t *apd)
        : primitive_t(apd) {}

    status_t init(engine_t *engine) override {
        CHECK(safe_ptr_assign(kernel_,
                new jit_avx512_core_x8s8s32x_fwd_kernel_t(
                        pd()->jcp_, *pd()->attr(), *pd()->dst_md(0))));
        return kernel_->create_kernel();
    }

    status_t execute(const exec_ctx_t &ctx) const override {
        const auto &_pd = pd();
        if (_pd->ndims() == 3)
            return execute_forward_1d(ctx);
        else if (_pd->ndims() == 4)
            if (_pd->jcp_.is_depthwise)
                return execute_forward_2d_dw(ctx);
            else
                return execute_forward_2d(ctx);
        else if (_pd->ndims() == 5)
            return execute_forward_3d(ctx);
        return status::unimplemented;
    }

private:
    status_t execute_forward_1d(const exec_ctx_t &ctx) const;
    status_t execute_forward_2d(const exec_ctx_t &ctx) const;
    status_t execute_forward_2d_dw(const exec_ctx_t &ctx) const;
    status_t execute_forward_3d(const exec_ctx_t &ctx) const;
    const pd_t *pd() const { return (const pd_t *)primitive_t::pd().get(); }
    const float *adjust_oscales(const memory_tracking::grantor_t &scratchpad,
            const float *src_scales, const float *wei_scales) const;

    std::unique_ptr<jit_avx512_core_x8s8s32x_fwd_kernel_t> kernel_;
};

} // namespace x64
} // namespace cpu
} // namespace impl
} // namespace dnnl

#endif

// vim: et ts=4 sw=4 cindent cino+=l0,\:4,N-s
