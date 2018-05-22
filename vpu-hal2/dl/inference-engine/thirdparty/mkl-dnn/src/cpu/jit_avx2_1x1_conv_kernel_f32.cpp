/*******************************************************************************
* Copyright 2016-2017 Intel Corporation
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

#include "c_types_map.hpp"
#include "nstl.hpp"
#include "type_helpers.hpp"
#include "utils.hpp"
#include "cpu_memory.hpp"

#include "jit_avx2_1x1_conv_kernel_f32.hpp"

#define GET_OFF(field) offsetof(jit_1x1_conv_call_s, field)

namespace mkldnn {
namespace impl {
namespace cpu {

using namespace mkldnn::impl::prop_kind;
using namespace mkldnn::impl::memory_format;
using namespace mkldnn::impl::utils;

using namespace Xbyak;

void jit_avx2_1x1_conv_kernel_f32::bcast_loop(int load_loop_blk,
        char load_loop_tag)
{
    mov(aux1_reg_bcast_data, reg_bcast_data);
    mov(aux_reg_output_data, reg_output_data);
    mov(bcast_loop_iter, reg_bcast_loop_work);

    jit_tagged_label bcast_loop("bcast_loop", load_loop_tag);
    jit_tagged_label bcast_loop_tail("bcast_loop_tail", load_loop_tag);

    cmp(bcast_loop_iter, jcp.ur);
    jl(bcast_loop_tail, T_NEAR);

    L(bcast_loop); {
        assert(jcp.bcast_block % jcp.ur == 0);
        int num_substeps = jcp.bcast_block / jcp.ur;
        assert(num_substeps > 0 && num_substeps < 10);
        for (int i = 0; i < num_substeps; i++) {
            reduce_loop(load_loop_blk, jcp.ur, load_loop_tag, '0' + i);
            if (i < num_substeps - 1) {
                add(aux1_reg_bcast_data, jcp.bcast_loop_bcast_substep);
                add(aux_reg_output_data, jcp.bcast_loop_output_substep);
            } else {
                add(aux1_reg_bcast_data, jcp.bcast_loop_bcast_step
                        - (num_substeps - 1) * jcp.bcast_loop_bcast_substep);
                add(aux_reg_output_data, jcp.bcast_loop_output_step
                        - (num_substeps - 1) * jcp.bcast_loop_output_substep);
            }
        }
        sub(bcast_loop_iter, jcp.bcast_block);
        cmp(bcast_loop_iter, jcp.bcast_block);
        jge(bcast_loop, T_NEAR);
    }

    L(bcast_loop_tail);
    if (jcp.ur_tail) {
        jit_tagged_label bcast_loop_tail_out(
                "bcast_loop_tail_out", load_loop_tag);
        cmp(bcast_loop_iter, 0);
        jz(bcast_loop_tail_out, T_NEAR);
        reduce_loop(load_loop_blk, jcp.ur_tail, load_loop_tag, '1');
        L(bcast_loop_tail_out);
    }
}

void jit_avx2_1x1_conv_kernel_f32::reduce_loop(int load_loop_blk, int ur,
        char load_loop_tag, char bcast_loop_tag)
{
    auto vreg_load = [=](int i) {
        return Ymm(ur * load_loop_blk + i);
    };

    auto vreg_accum = [=](int i, int j) {
        return Ymm(j * load_loop_blk + i);
    };

    auto bias_ptr = [=](int i) {
        return ptr[reg_bias_data + sizeof(float) * jcp.oc_block * i];
    };

    auto bcast_ptr = [=](int u, int j) {
        assert(j < jcp.ur);
        assert(u <= jcp.reduce_loop_unroll);
        size_t offt;
        if (one_of(jcp.prop_kind,
                    forward_training, forward_inference, backward_data))
        {
            assert(jcp.reduce_loop_unroll == (jcp.prop_kind == backward_data)
                    ? jcp.oc_block : jcp.ic_block);
            auto height = (jcp.prop_kind == backward_data) ? jcp.os : jcp.is;
            offt = (u == jcp.reduce_loop_unroll)
                ? (height + j) * jcp.reduce_loop_unroll
                : j * jcp.reduce_loop_unroll + u;
        } else
            offt = u * jcp.ic_block + j;
        return ptr[aux_reg_bcast_data + sizeof(float) * offt];
    };

    auto load_ptr = [=](int u, int i) {
        size_t offt;
        size_t u0 = u % jcp.reduce_loop_unroll;
        size_t u1 = u / jcp.reduce_loop_unroll;
        switch (jcp.prop_kind) {
        case backward_data:
            offt = (i * jcp.oc_block + u0) * jcp.ic_block;
            break;
        case backward_weights:
            offt = (i * jcp.os + u0) * jcp.oc_block;
            break;
        default:
            offt = (i * jcp.ic + u0) * jcp.oc_block;
        }
        return ptr[aux_reg_load_data
            + u1 * jcp.reduce_loop_load_step + sizeof(float) * offt];
    };

    auto output_ptr = [=](int i, int j) {
        switch (jcp.prop_kind) {
        case backward_data:
            return ptr[aux_reg_output_data +
                (i * jcp.is + j) * jcp.ic_block * sizeof(float)];
        case backward_weights:
            return ptr[aux_reg_output_data
                + (i ? reg_output_stride * i : 0) // TODO: Xbyak should allow 0 scale
                + sizeof(float) * jcp.oc_block * j];
        default:
            return ptr[aux_reg_output_data +
                (i * jcp.os + j) * jcp.oc_block * sizeof(float)];
        }
    };

    auto init = [=]() {
        jit_tagged_label init_done("init_done", load_loop_tag, bcast_loop_tag);
        jit_tagged_label init_zero("init_zero", load_loop_tag, bcast_loop_tag);

        if (jcp.with_bias && one_of(jcp.prop_kind, forward_training,
                    forward_inference)) {
            test(reg_reduce_pos_flag, FLAG_REDUCE_FIRST);
            jz(init_zero);

            for (int i = 0; i < load_loop_blk; i++)
                for (int j = 0; j < ur; ++j)
                    vmovups(vreg_accum(i, j), bias_ptr(i));
            jmp(init_done);
        }

        L(init_zero);
        for (int i = 0; i < load_loop_blk; ++i)
            for (int j = 0; j < ur; ++j) {
                auto r = vreg_accum(i, j);
                vxorps(r, r, r);
            }

        L(init_done);
        for (int i = 0; i < load_loop_blk; ++i)
            vmovups(vreg_load(i), load_ptr(0, i));
        vbroadcastss(vreg_bcast, bcast_ptr(0, 0));
    };

    auto store = [=]() {
        jit_tagged_label store_done(
                "store_done", load_loop_tag, bcast_loop_tag);
        jit_tagged_label store_noadd(
                "store_noadd", load_loop_tag, bcast_loop_tag);

        if (!jcp.with_sum) {
            test(reg_reduce_pos_flag, FLAG_REDUCE_FIRST);
            jnz(store_noadd, T_NEAR);
        }

        for (int j = 0; j < ur; ++j)
            for (int i = 0; i < load_loop_blk; ++i) {
                auto r = vreg_accum(i, j);
                vaddps(r, r, output_ptr(i, j));
            }

        L(store_noadd);

        if (jcp.with_eltwise) {
            assert(ur * load_loop_blk < 14);

            jit_tagged_label store_norelu(
                    "store_norelu", load_loop_tag, bcast_loop_tag);
            test(reg_reduce_pos_flag, FLAG_REDUCE_LAST);
            jz(store_norelu, T_NEAR);

            vxorps(vzero, vzero, vzero);
            if (jcp.eltwise_alpha == 0) {
               ymm_relu_ns = vzero;
            } else {
               mov(imm_addr64, float2int(jcp.eltwise_alpha));
               movq(xmm_relu_ns, imm_addr64);
               uni_vbroadcastss(ymm_relu_ns, xmm_relu_ns);
            }

            if (this->jcp.eltwise_alg == mkldnn_eltwise_relu) {
                for (int j = 0; j < ur; ++j) {
                    for (int i = 0; i < load_loop_blk; ++i) {
                        vcmpgtps(vmask, vreg_accum(i, j), vzero);
                        vmulps(ymm_res_ns, ymm_relu_ns, vreg_accum(i, j));
                        vblendvps(vreg_accum(i, j), ymm_res_ns,
                                  vreg_accum(i, j), vmask);
                        vmovups(output_ptr(i, j), vreg_accum(i, j));
                    }
                }
            } else if (this->jcp.eltwise_alg == mkldnn_eltwise_elu) {
                mov(reg_table, jit_avx2_1x1_conv_kernel_f32::l_table);

                for (int j = 0; j < ur; ++j) {
                    for (int i = 0; i < load_loop_blk; ++i) {
                        Ymm reg_out = vreg_accum(i, j);

                        // compute exponent
                        uni_vmovups(ymm_src, reg_out);
                        simd_expf(reg_out);

                        // alpha * (exp(x) - 1)
                        uni_vsubps(reg_out, reg_out, ptr[reg_table + 0 * 32]);
                        uni_vmulps(reg_out, reg_out, ymm_relu_ns);

                        // combine with mask
                        vxorps(vzero, vzero, vzero);
                        uni_vcmpgtps(ymm_mask, ymm_src, vzero);
                        uni_vblendvps(reg_out, reg_out, ymm_src, ymm_mask);

                        vmovups(output_ptr(i, j), reg_out);
                    }
                }
            }

            jmp(store_done, T_NEAR);
            L(store_norelu);
        }

        for (int j = 0; j < ur; ++j)
            for (int i = 0; i < load_loop_blk; ++i) {
                vmovups(output_ptr(i, j), vreg_accum(i, j));
            }

        L(store_done);
    };

    auto fma_block = [=](bool last_block) {
        for (int u = 0; u < jcp.reduce_loop_unroll; ++u) {
            for (int j = 0; j < ur; ++j) {
                for (int i = 0; i < load_loop_blk; ++i) {
                    vfmadd231ps(vreg_accum(i, j), vreg_load(i), vreg_bcast);
                    if (j == ur - 1 && !(last_block
                                && u == jcp.reduce_loop_unroll - 1))
                        vmovups(vreg_load(i), load_ptr(u + 1, i));
                }
                if (j < ur - 1)
                    vbroadcastss(vreg_bcast, bcast_ptr(u, j + 1));
            }
            if (!last_block || u < jcp.reduce_loop_unroll - 1)
                vbroadcastss(vreg_bcast, bcast_ptr(u + 1, 0));
        }
    };

    jit_tagged_label reduce_loop("reduce_loop", load_loop_tag, bcast_loop_tag);
    jit_tagged_label reduce_loop_tail(
            "reduce_loop_tail", load_loop_tag, bcast_loop_tag);

    mov(aux_reg_load_data, reg_load_data);
    mov(aux_reg_bcast_data, aux1_reg_bcast_data);

    init();

    mov(reduce_loop_iter, reg_reduce_loop_work);
    sub(reduce_loop_iter, jcp.reduce_loop_unroll);
    jle(reduce_loop_tail, T_NEAR);

    L(reduce_loop); {
        fma_block(false);
        add(aux_reg_bcast_data, jcp.reduce_loop_bcast_step);
        add(aux_reg_load_data, jcp.reduce_loop_load_step);
        sub(reduce_loop_iter, jcp.reduce_loop_unroll);
        jg(reduce_loop, T_NEAR);
    }

    L(reduce_loop_tail);
    fma_block(true);

    store();
}

void jit_avx2_1x1_conv_kernel_f32::diff_bias_loop(int load_loop_blk,
        char load_loop_tag)
{
    if (!jcp.with_bias || jcp.prop_kind != backward_weights)
        return;

    jit_tagged_label diff_bias_loop("diff_bias_loop", load_loop_tag);
    jit_tagged_label diff_bias_loop_out("diff_bias_loop_out", load_loop_tag);
    jit_tagged_label diff_bias_init_out("diff_bias_init_out", load_loop_tag);
    jit_tagged_label diff_bias_load("diff_bias_load", load_loop_tag);

    auto diff_bias_ptr = [=](int i) {
        return ptr[reg_diff_bias_data + i * jcp.oc_block * sizeof(float)];
    };

    auto load_ptr = [=](int u, int i) {
        return ptr[aux_reg_load_data
            + (i * jcp.os + u) * jcp.oc_block * sizeof(float)];
    };

    auto diff_bias_reg = [=](int i) { return Ymm(i); };

    mov(reg_diff_bias_data, ptr[rsp + reg_diff_bias_data_stack_offt]);
    cmp(reg_diff_bias_data, 0);
    je(diff_bias_loop_out, T_NEAR);

    test(reg_reduce_pos_flag, FLAG_REDUCE_FIRST);
    jz(diff_bias_load, T_NEAR);

    for (int i = 0; i < load_loop_blk; ++i) {
        auto r = diff_bias_reg(i);
        vxorps(r, r, r);
    }
    jmp(diff_bias_init_out, T_NEAR);

    L(diff_bias_load);
    for (int i = 0; i < load_loop_blk; ++i)
        vmovups(diff_bias_reg(i), diff_bias_ptr(i));

    L(diff_bias_init_out);
    mov(aux_reg_load_data, reg_load_data);
    mov(reduce_loop_iter, reg_reduce_loop_work);
    L(diff_bias_loop); {
        for(int u = 0; u < jcp.reduce_loop_unroll; ++u)
            for (int i = 0; i < load_loop_blk; ++i)
                vaddps(diff_bias_reg(i), diff_bias_reg(i), load_ptr(u, i));
        assert(jcp.reduce_dim % jcp.reduce_loop_unroll == 0);
        add(aux_reg_load_data, jcp.reduce_loop_load_step);
        sub(reduce_loop_iter, jcp.reduce_loop_unroll);
        jnz(diff_bias_loop, T_NEAR);
    }

    for (int i = 0; i < load_loop_blk; i++)
        vmovups(diff_bias_ptr(i), diff_bias_reg(i));
    add(reg_diff_bias_data, load_loop_blk * jcp.oc_block * sizeof(float));
    mov(ptr[rsp + reg_diff_bias_data_stack_offt], reg_diff_bias_data);

    L(diff_bias_loop_out);
}

void jit_avx2_1x1_conv_kernel_f32::generate()
{
    preamble();

    mov(reg_bcast_data, ptr[param1 + GET_OFF(bcast_data)]);
    mov(reg_load_data, ptr[param1 + GET_OFF(load_data)]);
    mov(reg_output_data, ptr[param1 + GET_OFF(output_data)]);
    if (jcp.with_bias) {
        if (jcp.prop_kind == backward_weights) {
            sub(rsp, stack_space_needed);
            mov(reg_diff_bias_data, ptr[param1 + GET_OFF(bias_data)]);
            mov(ptr[rsp + reg_diff_bias_data_stack_offt], reg_diff_bias_data);
        } else
            mov(reg_bias_data, ptr[param1 + GET_OFF(bias_data)]);
    }

    mov(reg_load_loop_work, ptr[param1 + GET_OFF(load_dim)]);
    mov(reg_bcast_loop_work, ptr[param1 + GET_OFF(bcast_dim)]);
    mov(reg_reduce_loop_work, ptr[param1 + GET_OFF(reduce_dim)]);
    mov(reg_reduce_pos_flag, ptr[param1 + GET_OFF(reduce_pos_flag)]);
    if (jcp.prop_kind == backward_weights)
        mov(reg_output_stride, ptr[param1 + GET_OFF(output_stride)]);

    auto load_loop_body = [=] (int load_loop_blk, char bcast_loop_tag) {
        bcast_loop(load_loop_blk, bcast_loop_tag);
        add(reg_load_data, load_loop_blk * jcp.load_loop_load_step);
        switch (jcp.prop_kind) {
        case forward_training:
        case forward_inference:
            add(reg_bias_data, load_loop_blk * jcp.oc_block * sizeof(float));
            add(reg_output_data,
                    load_loop_blk * jcp.os * jcp.oc_block * sizeof(float));
            break;
        case backward_data:
            add(reg_output_data,
                    load_loop_blk * jcp.is * jcp.ic_block * sizeof(float));
            break;
        case backward_weights:
            for (int i = 0; i < load_loop_blk; i++)
                add(reg_output_data, reg_output_stride);
            break;
        default:
            assert(!"invalid prop_kind");
        }
        sub(reg_load_loop_work, load_loop_blk * jcp.load_loop_iter_step);
    };

    const char *load_loop_blk_8 = "load_loop_blk_8";
    const char *load_loop_blk_16 = "load_loop_blk_16";
    const char *load_loop_blk_24 = "load_loop_blk_24";
    const char *load_loop_blk_end = "load_loop_blk_end";

    cmp(reg_load_loop_work, 8);
    jle(load_loop_blk_8, T_NEAR);

    cmp(reg_load_loop_work, 32);
    je(load_loop_blk_16, T_NEAR);

    cmp(reg_load_loop_work, 16);
    jle(load_loop_blk_16, T_NEAR);

    L(load_loop_blk_24); {
        diff_bias_loop(3, '3');
        load_loop_body(3, '3');
        cmp(reg_load_loop_work, 32);
        je(load_loop_blk_16);
        cmp(reg_load_loop_work, 24);
        jge(load_loop_blk_24);
    }

    cmp(reg_load_loop_work, 8);
    jle(load_loop_blk_8, T_NEAR);

    L(load_loop_blk_16); {
        diff_bias_loop(2, '2');
        load_loop_body(2, '2');
        cmp(reg_load_loop_work, 16);
        jge(load_loop_blk_16);
    }

    L(load_loop_blk_8); {
        cmp(reg_load_loop_work, 0);
        je(load_loop_blk_end, T_NEAR);
        diff_bias_loop(1, '1');
        load_loop_body(1, '1');
    }

    L(load_loop_blk_end);

    if (jcp.with_bias && jcp.prop_kind == backward_weights)
        add(rsp, 8);

    postamble();

    prepare_table();
}

void jit_avx2_1x1_conv_kernel_f32::simd_expf(const Ymm &ymm_src) {
    uni_vminps(ymm_src, ymm_src, ptr[reg_table + 10 * 32]);
    uni_vmaxps(ymm_src, ymm_src, ptr[reg_table + 11 * 32]);
    uni_vmovups(ymm_aux0, ymm_src);
    //calculate exp(x)
    // fx = x * log2ef + 0.5
    uni_vmulps(ymm_src, ymm_src, ptr[reg_table + 2 * 32]);
    uni_vaddps(ymm_src, ymm_src, ptr[reg_table + 1 * 32]);

    // tmp = floorf(fx)
    uni_vroundps(ymm_aux1, ymm_src, _op_floor);
    //keep fx for further computations
    uni_vmovups(ymm_src, ymm_aux1); //ymm_src = fx

    //x = x - fx * ln2
    uni_vfnmadd231ps(ymm_aux0, ymm_aux1, ptr[reg_table + 3 * 32]);

    // compute 2^n
    uni_vcvtps2dq(ymm_aux1, ymm_src);
    uni_vpaddd(ymm_aux1, ymm_aux1, ptr[reg_table + 4 * 32]);
    uni_vpslld(ymm_aux1, ymm_aux1, 23); //Vmm(6) = 2^-fx

    // y = p5
    uni_vmovups(ymm_src, ptr[reg_table + 9 * 32]);
    // y = y * x + p4
    uni_vfmadd213ps(ymm_src, ymm_aux0, ptr[reg_table + 8 * 32]);
    // y = y * x + p3
    uni_vfmadd213ps(ymm_src, ymm_aux0, ptr[reg_table + 7 * 32]);
    // y = y * x + p2
    uni_vfmadd213ps(ymm_src, ymm_aux0, ptr[reg_table + 6 * 32]);
    // y = y * x + p1
    uni_vfmadd213ps(ymm_src, ymm_aux0, ptr[reg_table + 0 * 32]);
    // y = y * x + p0
    uni_vfmadd213ps(ymm_src, ymm_aux0, ptr[reg_table + 5 * 32]);  //exp(q)
    // y = y * 2^n
    uni_vmulps(ymm_src, ymm_src, ymm_aux1);
}

void jit_avx2_1x1_conv_kernel_f32::prepare_table() {
    const unsigned int cvals[] = {
            0x3f800000, // [0] 1.0f
            0x3f000000, // [1] 0.5f
            0x3fb8aa3b, // [2] log2ef = 1.44269502f
            0x3f317218, // [3] ln2f =   0.69314718f
            0x0000007f, // [4] 0x7f
            // exp(x) polynom
            0x3f800001, // [5] p0 = 1.0000001f
            0x3efffe85, // [6] p2 = 0.4999887f
            0x3e2aaa3e, // [7] p3 = 0.16666505f
            0x3d2bb1b1, // [8] p4 = 0.041917507f
            0x3c091ec1, // [9] p5 = 0.008369149f
            0x42b0c0a5, //[10] max logf = 88.3762589f
            0xc1766666  //[11] min logf = -14.5f
    };

    align(64);
    L(l_table);
    for (size_t i = 0; i < sizeof(cvals) / sizeof(cvals[0]); ++i) {
        for (size_t d = 0; d < 8; ++d) {
            dd(cvals[i]);
        }
    }
}

bool jit_avx2_1x1_conv_kernel_f32::post_ops_ok(
        jit_1x1_conv_conf_t &jcp, const primitive_attr_t &attr) {
    using namespace primitive_kind;
    const auto &p = attr.post_ops_;

    auto is_eltwise = [&](int idx) {
        return p.entry_[idx].kind == eltwise
            && p.entry_[idx].eltwise.scale == 1.
            && (p.entry_[idx].eltwise.alg == alg_kind::eltwise_relu ||
                p.entry_[idx].eltwise.alg == alg_kind::eltwise_elu);
//            && p.entry_[idx].eltwise.alpha == 0.;
    };

    switch (p.len_) {
    case 0: return true; // no post_ops
    case 1: return true // sum OR eltwise
                && !jcp.with_eltwise
                && (is_eltwise(0) || p.contain(sum, 0));
    case 2: return true // sum->eltwise
                && !jcp.with_eltwise
                && (p.contain(sum, 0) && is_eltwise(1));
    default: return false;
    }

    return false;
}


status_t jit_avx2_1x1_conv_kernel_f32::init_conf(jit_1x1_conv_conf_t &jcp,
        const convolution_desc_t &cd, const memory_desc_wrapper &src_d,
        const memory_desc_wrapper &weights_d, const memory_desc_wrapper &dst_d,
        const primitive_attr_t &attr, bool with_relu, float relu_negative_slope)
{
    if (!mayiuse(avx2)) return status::unimplemented;

    // TODO (Roma): this code is duplicated from the generic kernel; maybe the
    // configuration struct could do some stuff below
    const bool with_groups = weights_d.ndims() == src_d.ndims() + 1;

    jcp.prop_kind = cd.prop_kind;

    jcp.ngroups = with_groups ? weights_d.dims()[0] : 1;
    jcp.mb = src_d.dims()[0];

    jcp.oc = dst_d.dims()[1] / jcp.ngroups;
    jcp.ic = src_d.dims()[1] / jcp.ngroups;

    jcp.ih = src_d.dims()[2];
    jcp.iw = src_d.dims()[3];
    jcp.oh = dst_d.dims()[2];
    jcp.ow = dst_d.dims()[3];

    jcp.kh = weights_d.dims()[with_groups + 2];
    jcp.kw = weights_d.dims()[with_groups + 3];

    jcp.t_pad = cd.padding[0][0];
    jcp.l_pad = cd.padding[0][1];

    jcp.stride_h = cd.strides[0];
    jcp.stride_w = cd.strides[1];

    jcp.src_fmt = src_d.format();
    jcp.with_bias = cd.bias_desc.format != memory_format::undef;

    jcp.os = jcp.oh * jcp.ow;
    jcp.is = jcp.ih * jcp.iw;

    jcp.with_eltwise = with_relu;
    jcp.eltwise_alg = mkldnn_eltwise_relu;
    jcp.eltwise_alpha = relu_negative_slope;

    if (!post_ops_ok(jcp, attr)) {
        return status::unimplemented;
    }

    const auto &p = attr.post_ops_;
    jcp.with_sum = p.find(primitive_kind::sum) != -1;
    if (!jcp.with_eltwise) {
        int eltwise_ind = p.find(primitive_kind::eltwise);
        if (eltwise_ind != -1) {
            jcp.with_eltwise  = true;
            jcp.eltwise_alg   = p.entry_[eltwise_ind].eltwise.alg;
            jcp.eltwise_alpha = p.entry_[eltwise_ind].eltwise.alpha;
            jcp.eltwise_beta  = p.entry_[eltwise_ind].eltwise.beta;
            jcp.eltwise_scale = p.entry_[eltwise_ind].eltwise.scale;
        }
    }

    constexpr memory_format_t weights_formats[2][2] = {
        { OIhw8i8o, OIhw8o8i },
        { gOIhw8i8o, gOIhw8o8i }
    };
    memory_format_t weights_format
        = weights_formats[with_groups][jcp.prop_kind == backward_data];

    bool args_ok = true
        && jcp.ngroups == 1
        && src_d.format() == nChw8c
        && weights_d.format() == weights_format
        && one_of(cd.bias_desc.format, memory_format::undef, any, x)
        && dst_d.format() == nChw8c;
    if (!args_ok) return status::unimplemented;

    const int simd_w = 8;

    args_ok = true
        && jcp.oc % simd_w == 0 && jcp.ic % simd_w == 0
        && jcp.t_pad == 0 && jcp.l_pad == 0
        && jcp.stride_w == 1 && jcp.stride_h == 1 // TODO: support some strides
        && jcp.kh == 1 && jcp.kw == 1;
    if (!args_ok) return status::unimplemented;

    jcp.ic_block = jcp.oc_block = simd_w;

    jcp.ur = 4;

    int load_blocking{ 0 };
    int load_blocking_max{ 0 };
    int bcast_blocking{ 0 };
    int bcast_blocking_max{ 0 };
    int reduce_blocking{ 0 };

    if (one_of(jcp.prop_kind, forward_training, forward_inference)) {
        jcp.reduce_dim = jcp.ic;
        jcp.reduce_block = jcp.ic_block;

        jcp.load_dim = jcp.oc;
        jcp.load_block = jcp.oc_block;

        jcp.bcast_dim = jcp.is;
        jcp.bcast_block = jcp.ur;

        jcp.reduce_loop_unroll = jcp.reduce_block;
        jcp.reduce_loop_bcast_step
            = jcp.reduce_loop_unroll * jcp.is * sizeof(float);
        jcp.reduce_loop_load_step
            = jcp.reduce_loop_unroll * jcp.oc_block * sizeof(float);

        jcp.bcast_loop_output_step = jcp.ur * jcp.oc_block * sizeof(float);
        jcp.bcast_loop_output_substep = -1; // unused
        jcp.bcast_loop_bcast_step = jcp.ur * jcp.ic_block * sizeof(float);
        jcp.bcast_loop_bcast_substep = -1; // unused

        jcp.load_loop_load_step = jcp.ic * jcp.oc_block * sizeof(float);
        jcp.load_loop_iter_step = jcp.oc_block;

        load_blocking = 120; // assumes the kernel is jcp.ur x 3
        load_blocking_max = 144;
        bcast_blocking = 128; // affects load balancing across threads
        bcast_blocking_max = 192;
        reduce_blocking = 128; // affects L1$ utilization
    } else if (jcp.prop_kind == backward_data) {
        jcp.reduce_dim = jcp.oc;
        jcp.reduce_block = jcp.oc_block;

        jcp.load_dim = jcp.ic;
        jcp.load_block = jcp.oc_block;

        jcp.bcast_dim = jcp.os;
        jcp.bcast_block = jcp.ur;

        jcp.reduce_loop_unroll = jcp.reduce_block;
        jcp.reduce_loop_bcast_step
            = jcp.reduce_loop_unroll * jcp.os * sizeof(float);
        jcp.reduce_loop_load_step
            = jcp.reduce_loop_unroll * jcp.ic * sizeof(float);

        jcp.bcast_loop_output_step = jcp.ur * jcp.ic_block * sizeof(float);
        jcp.bcast_loop_output_substep = -1; // unused
        jcp.bcast_loop_bcast_step = jcp.ur * jcp.oc_block * sizeof(float);
        jcp.bcast_loop_bcast_substep = -1; // unused

        jcp.load_loop_load_step = jcp.oc_block * jcp.ic_block * sizeof(float);
        jcp.load_loop_iter_step = jcp.ic_block;

        load_blocking = 96; // assumes the kernel is jcp.ur x 3
        load_blocking_max = 144;
        bcast_blocking = 128; // affects load balancing across threads
        bcast_blocking_max = 196;
        reduce_blocking = 64; // affects L1$ utilization
    } else if (jcp.prop_kind == backward_weights) {
        jcp.reduce_dim = jcp.os;
        jcp.reduce_block = 1;

        jcp.load_dim = jcp.oc;
        jcp.load_block = jcp.oc_block;

        jcp.bcast_dim = jcp.ic;
        jcp.bcast_block = jcp.ic_block;

        jcp.reduce_loop_unroll = jcp.reduce_block;
        jcp.reduce_loop_bcast_step
            = jcp.reduce_loop_unroll * jcp.ic_block * sizeof(float);
        jcp.reduce_loop_load_step
            = jcp.reduce_loop_unroll * jcp.oc_block * sizeof(float);

        jcp.bcast_loop_output_step = jcp.oc_block * jcp.ic_block * sizeof(float);
        jcp.bcast_loop_output_substep = jcp.oc_block * jcp.ur * sizeof(float);
        jcp.bcast_loop_bcast_step = jcp.ic_block * jcp.is * sizeof(float);
        jcp.bcast_loop_bcast_substep = jcp.ur * sizeof(float);

        jcp.load_loop_load_step = jcp.oc_block * jcp.os * sizeof(float);
        jcp.load_loop_iter_step = jcp.oc_block;

        /* --- */

        load_blocking = div_up(jcp.load_dim, jcp.load_block);
        while (true) {
            if (load_blocking <= 32) break;
            else if (load_blocking % 2 == 0) load_blocking /= 2;
            else if (load_blocking % 3 == 0) load_blocking /= 3;
            else break;
        }
        load_blocking *= jcp.load_block;
        load_blocking_max = load_blocking;
        assert(jcp.load_dim % load_blocking == 0);

        bcast_blocking = div_up(jcp.bcast_dim, jcp.bcast_block);
        while (true) {
            if (bcast_blocking <= 9) break;
            else if (bcast_blocking % 2 == 0) bcast_blocking /= 2;
            else if (bcast_blocking % 3 == 0) bcast_blocking /= 3;
            else break;
        }
        bcast_blocking *= jcp.bcast_block;
        bcast_blocking_max = bcast_blocking;
        assert(jcp.bcast_dim % bcast_blocking == 0);

        reduce_blocking = 128; // affects L1$ utilization
    } else
        return status::unimplemented;

    assert(load_blocking);
    assert(load_blocking_max);
    assert(bcast_blocking);
    assert(bcast_blocking_max);
    assert(reduce_blocking);

    assert(jcp.bcast_block % jcp.ur == 0);
    jcp.ur_tail = jcp.bcast_dim % jcp.ur;

    jcp.nb_bcast_blocking = bcast_blocking / jcp.bcast_block;
    jcp.nb_bcast_blocking_max = bcast_blocking_max / jcp.bcast_block;
    jcp.nb_load_blocking = load_blocking / jcp.load_block;
    jcp.nb_load_blocking_max = load_blocking_max / jcp.load_block;
    jcp.nb_reduce_blocking = reduce_blocking / jcp.reduce_block;

    jcp.nb_bcast = div_up(jcp.bcast_dim, jcp.bcast_block);
    jcp.nb_load = div_up(jcp.load_dim, jcp.load_block);
    jcp.nb_reduce = div_up(jcp.reduce_dim, jcp.reduce_block);

    return status::success;
}

}
}
}
