// Tencent is pleased to support the open source community by making ncnn available.
//
// Copyright (C) 2019 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include "concat_vulkan.h"
#include <algorithm>

namespace ncnn {

DEFINE_LAYER_CREATOR(Concat_vulkan)

Concat_vulkan::Concat_vulkan()
{
    support_vulkan = true;

    pipeline_concat[0] = 0;
    pipeline_concat[1] = 0;
    pipeline_concat_pack4[0] = 0;
    pipeline_concat_pack4[1] = 0;
    pipeline_concat_pack4to1[0] = 0;
    pipeline_concat_pack4to1[1] = 0;
}

int Concat_vulkan::create_pipeline(const Option& opt)
{
    std::vector<vk_specialization_type> specializations(1);
    specializations[0].i = axis;

    // pack1
    {
        pipeline_concat[0] = new Pipeline(vkdev);
        pipeline_concat[0]->set_optimal_local_size_xyz();
        pipeline_concat[0]->create("concat", specializations, 2, 11);
        pipeline_concat[1] = new Pipeline(vkdev);
        pipeline_concat[1]->set_optimal_local_size_xyz();
        pipeline_concat[1]->create("concat", specializations, 2, 11);
    }

    // pack4
    {
        pipeline_concat_pack4[0] = new Pipeline(vkdev);
        pipeline_concat_pack4[0]->set_optimal_local_size_xyz();
        pipeline_concat_pack4[0]->create("concat_pack4", specializations, 2, 11);
        pipeline_concat_pack4[1] = new Pipeline(vkdev);
        pipeline_concat_pack4[1]->set_optimal_local_size_xyz();
        pipeline_concat_pack4[1]->create("concat_pack4", specializations, 2, 11);
    }

    // pack4to1
    {
        pipeline_concat_pack4to1[0] = new Pipeline(vkdev);
        pipeline_concat_pack4to1[0]->set_optimal_local_size_xyz();
        pipeline_concat_pack4to1[0]->create("concat_pack4to1", specializations, 2, 11);
        pipeline_concat_pack4to1[1] = new Pipeline(vkdev);
        pipeline_concat_pack4to1[1]->set_optimal_local_size_xyz();
        pipeline_concat_pack4to1[1]->create("concat_pack4to1", specializations, 2, 11);
    }

    return 0;
}

int Concat_vulkan::destroy_pipeline(const Option& opt)
{
    delete pipeline_concat[0];
    delete pipeline_concat[1];
    pipeline_concat[0] = 0;
    pipeline_concat[1] = 0;

    delete pipeline_concat_pack4[0];
    delete pipeline_concat_pack4[1];
    pipeline_concat_pack4[0] = 0;
    pipeline_concat_pack4[1] = 0;

    delete pipeline_concat_pack4to1[0];
    delete pipeline_concat_pack4to1[1];
    pipeline_concat_pack4to1[0] = 0;
    pipeline_concat_pack4to1[1] = 0;

    return 0;
}

int Concat_vulkan::forward(const std::vector<VkMat>& bottom_blobs, std::vector<VkMat>& top_blobs, VkCompute& cmd, const Option& opt) const
{
    int dims = bottom_blobs[0].dims;

    if (dims == 1) // axis == 0
    {
        // concat vector
        // total length
        size_t elemsize = bottom_blobs[0].elemsize;
        int packing = bottom_blobs[0].packing;
        int top_w = 0;
        for (size_t b=0; b<bottom_blobs.size(); b++)
        {
            const VkMat& bottom_blob = bottom_blobs[b];
            elemsize = std::min(elemsize, bottom_blob.elemsize);
            packing = std::min(packing, bottom_blob.packing);
            top_w += bottom_blob.w * bottom_blob.packing;
        }

        int out_packing = top_w % 4 == 0 ? 4 : 1;
        size_t out_elemsize = elemsize / packing * out_packing;

        // TODO pack1to4 and pack4to1to4 make sense ?
        if (packing == 1)
        {
            out_packing = 1;
            out_elemsize = elemsize / packing;
        }

        VkMat& top_blob = top_blobs[0];
        top_blob.create(top_w / out_packing, out_elemsize, out_packing, opt.blob_vkallocator, opt.staging_vkallocator);
        if (top_blob.empty())
            return -100;

        int woffset = 0;
        for (size_t b=0; b<bottom_blobs.size(); b++)
        {
            const VkMat& bottom_blob = bottom_blobs[b];

            std::vector<VkMat> bindings(2);
            bindings[0] = bottom_blob;
            bindings[1] = top_blob;

            std::vector<vk_constant_type> constants(11);
            constants[0].i = bottom_blob.dims;
            constants[1].i = bottom_blob.w;
            constants[2].i = bottom_blob.h;
            constants[3].i = bottom_blob.c;
            constants[4].i = bottom_blob.cstep;
            constants[5].i = top_blob.dims;
            constants[6].i = top_blob.w;
            constants[7].i = top_blob.h;
            constants[8].i = top_blob.c;
            constants[9].i = top_blob.cstep;
            constants[10].i = woffset;

            const Pipeline* pipeline = 0;
            if (bottom_blob.packing == 1 && out_packing == 1)
            {
                pipeline = pipeline_concat[b%2];
            }
            else if (bottom_blob.packing == 4 && out_packing == 4)
            {
                pipeline = pipeline_concat_pack4[b%2];
            }
            else if (bottom_blob.packing == 4 && out_packing == 1)
            {
                pipeline = pipeline_concat_pack4to1[b%2];
            }

            cmd.record_pipeline(pipeline, bindings, constants, bottom_blob);

            woffset += bottom_blob.w * bottom_blob.packing / out_packing;
        }

        return 0;
    }

    if (dims == 2 && axis == 0)
    {
        // concat image
        int w = bottom_blobs[0].w;

        // total height
        size_t elemsize = bottom_blobs[0].elemsize;
        int packing = bottom_blobs[0].packing;
        int top_h = 0;
        for (size_t b=0; b<bottom_blobs.size(); b++)
        {
            const VkMat& bottom_blob = bottom_blobs[b];
            elemsize = std::min(elemsize, bottom_blob.elemsize);
            packing = std::min(packing, bottom_blob.packing);
            top_h += bottom_blob.h * bottom_blob.packing;
        }

        int out_packing = top_h % 4 == 0 ? 4 : 1;
        size_t out_elemsize = elemsize / packing * out_packing;

        // TODO pack1to4 and pack4to1to4 make sense ?
        if (packing == 1)
        {
            out_packing = 1;
            out_elemsize = elemsize / packing;
        }

        VkMat& top_blob = top_blobs[0];
        top_blob.create(w, top_h / out_packing, out_elemsize, out_packing, opt.blob_vkallocator, opt.staging_vkallocator);
        if (top_blob.empty())
            return -100;

        int hoffset = 0;
        for (size_t b=0; b<bottom_blobs.size(); b++)
        {
            const VkMat& bottom_blob = bottom_blobs[b];

            std::vector<VkMat> bindings(2);
            bindings[0] = bottom_blob;
            bindings[1] = top_blob;

            std::vector<vk_constant_type> constants(11);
            constants[0].i = bottom_blob.dims;
            constants[1].i = bottom_blob.w;
            constants[2].i = bottom_blob.h;
            constants[3].i = bottom_blob.c;
            constants[4].i = bottom_blob.cstep;
            constants[5].i = top_blob.dims;
            constants[6].i = top_blob.w;
            constants[7].i = top_blob.h;
            constants[8].i = top_blob.c;
            constants[9].i = top_blob.cstep;
            constants[10].i = hoffset;

            const Pipeline* pipeline = 0;
            if (bottom_blob.packing == 1 && out_packing == 1)
            {
                pipeline = pipeline_concat[b%2];
            }
            else if (bottom_blob.packing == 4 && out_packing == 4)
            {
                pipeline = pipeline_concat_pack4[b%2];
            }
            else if (bottom_blob.packing == 4 && out_packing == 1)
            {
                pipeline = pipeline_concat_pack4to1[b%2];
            }

            cmd.record_pipeline(pipeline, bindings, constants, bottom_blob);

            hoffset += bottom_blob.h * bottom_blob.packing / out_packing;
        }

        return 0;
    }

    if (dims == 2 && axis == 1)
    {
        // interleave image row
        int h = bottom_blobs[0].h;
        size_t elemsize = bottom_blobs[0].elemsize;
        int packing = bottom_blobs[0].packing;

        // total width
        int top_w = 0;
        for (size_t b=0; b<bottom_blobs.size(); b++)
        {
            const VkMat& bottom_blob = bottom_blobs[b];
            top_w += bottom_blob.w;
        }

        VkMat& top_blob = top_blobs[0];
        top_blob.create(top_w, h, elemsize, packing, opt.blob_vkallocator, opt.staging_vkallocator);
        if (top_blob.empty())
            return -100;

        int woffset = 0;
        for (size_t b=0; b<bottom_blobs.size(); b++)
        {
            const VkMat& bottom_blob = bottom_blobs[b];

            std::vector<VkMat> bindings(2);
            bindings[0] = bottom_blob;
            bindings[1] = top_blob;

            std::vector<vk_constant_type> constants(11);
            constants[0].i = bottom_blob.dims;
            constants[1].i = bottom_blob.w;
            constants[2].i = bottom_blob.h;
            constants[3].i = bottom_blob.c;
            constants[4].i = bottom_blob.cstep;
            constants[5].i = top_blob.dims;
            constants[6].i = top_blob.w;
            constants[7].i = top_blob.h;
            constants[8].i = top_blob.c;
            constants[9].i = top_blob.cstep;
            constants[10].i = woffset;

            const Pipeline* pipeline = packing == 4 ? pipeline_concat_pack4[b%2] : pipeline_concat[b%2];

            cmd.record_pipeline(pipeline, bindings, constants, bottom_blob);

            woffset += bottom_blob.w;
        }

        return 0;
    }

    if (dims == 3 && axis == 0)
    {
        // concat dim
        int w = bottom_blobs[0].w;
        int h = bottom_blobs[0].h;

        // total channels
        size_t elemsize = bottom_blobs[0].elemsize;
        int packing = bottom_blobs[0].packing;
        int top_channels = 0;
        for (size_t b=0; b<bottom_blobs.size(); b++)
        {
            const VkMat& bottom_blob = bottom_blobs[b];
            elemsize = std::min(elemsize, bottom_blob.elemsize);
            packing = std::min(packing, bottom_blob.packing);
            top_channels += bottom_blob.c * bottom_blob.packing;
        }

        int out_packing = top_channels % 4 == 0 ? 4 : 1;
        size_t out_elemsize = elemsize / packing * out_packing;

        // TODO pack1to4 and pack4to1to4 make sense ?
        if (packing == 1)
        {
            out_packing = 1;
            out_elemsize = elemsize / packing;
        }

        VkMat& top_blob = top_blobs[0];
        top_blob.create(w, h, top_channels / out_packing, out_elemsize, out_packing, opt.blob_vkallocator, opt.staging_vkallocator);
        if (top_blob.empty())
            return -100;

        int coffset = 0;
        for (size_t b=0; b<bottom_blobs.size(); b++)
        {
            const VkMat& bottom_blob = bottom_blobs[b];

            std::vector<VkMat> bindings(2);
            bindings[0] = bottom_blob;
            bindings[1] = top_blob;

            std::vector<vk_constant_type> constants(11);
            constants[0].i = bottom_blob.dims;
            constants[1].i = bottom_blob.w;
            constants[2].i = bottom_blob.h;
            constants[3].i = bottom_blob.c;
            constants[4].i = bottom_blob.cstep;
            constants[5].i = top_blob.dims;
            constants[6].i = top_blob.w;
            constants[7].i = top_blob.h;
            constants[8].i = top_blob.c;
            constants[9].i = top_blob.cstep;
            constants[10].i = coffset;

            const Pipeline* pipeline = 0;
            if (bottom_blob.packing == 1 && out_packing == 1)
            {
                pipeline = pipeline_concat[b%2];
            }
            else if (bottom_blob.packing == 4 && out_packing == 4)
            {
                pipeline = pipeline_concat_pack4[b%2];
            }
            else if (bottom_blob.packing == 4 && out_packing == 1)
            {
                pipeline = pipeline_concat_pack4to1[b%2];
            }

            cmd.record_pipeline(pipeline, bindings, constants, bottom_blob);

            coffset += bottom_blob.c * bottom_blob.packing / out_packing;
        }

        return 0;
    }

    if (dims == 3 && axis == 1)
    {
        // interleave dim height
        int w = bottom_blobs[0].w;
        int channels = bottom_blobs[0].c;
        size_t elemsize = bottom_blobs[0].elemsize;
        int packing = bottom_blobs[0].packing;

        // total height
        int top_h = 0;
        for (size_t b=0; b<bottom_blobs.size(); b++)
        {
            const VkMat& bottom_blob = bottom_blobs[b];
            top_h += bottom_blob.h;
        }

        VkMat& top_blob = top_blobs[0];
        top_blob.create(w, top_h, channels, elemsize, packing, opt.blob_vkallocator, opt.staging_vkallocator);
        if (top_blob.empty())
            return -100;

        int hoffset = 0;
        for (size_t b=0; b<bottom_blobs.size(); b++)
        {
            const VkMat& bottom_blob = bottom_blobs[b];

            std::vector<VkMat> bindings(2);
            bindings[0] = bottom_blob;
            bindings[1] = top_blob;

            std::vector<vk_constant_type> constants(11);
            constants[0].i = bottom_blob.dims;
            constants[1].i = bottom_blob.w;
            constants[2].i = bottom_blob.h;
            constants[3].i = bottom_blob.c;
            constants[4].i = bottom_blob.cstep;
            constants[5].i = top_blob.dims;
            constants[6].i = top_blob.w;
            constants[7].i = top_blob.h;
            constants[8].i = top_blob.c;
            constants[9].i = top_blob.cstep;
            constants[10].i = hoffset;

            const Pipeline* pipeline = packing == 4 ? pipeline_concat_pack4[b%2] : pipeline_concat[b%2];

            cmd.record_pipeline(pipeline, bindings, constants, bottom_blob);

            hoffset += bottom_blob.h;
        }

        return 0;
    }

    if (dims == 3 && axis == 2)
    {
        // interleave dim width
        int h = bottom_blobs[0].h;
        int channels = bottom_blobs[0].c;
        size_t elemsize = bottom_blobs[0].elemsize;
        int packing = bottom_blobs[0].packing;

        // total height
        int top_w = 0;
        for (size_t b=0; b<bottom_blobs.size(); b++)
        {
            const VkMat& bottom_blob = bottom_blobs[b];
            top_w += bottom_blob.w;
        }

        VkMat& top_blob = top_blobs[0];
        top_blob.create(top_w, h, channels, elemsize, packing, opt.blob_vkallocator, opt.staging_vkallocator);
        if (top_blob.empty())
            return -100;

        int woffset = 0;
        for (size_t b=0; b<bottom_blobs.size(); b++)
        {
            const VkMat& bottom_blob = bottom_blobs[b];

            std::vector<VkMat> bindings(2);
            bindings[0] = bottom_blob;
            bindings[1] = top_blob;

            std::vector<vk_constant_type> constants(11);
            constants[0].i = bottom_blob.dims;
            constants[1].i = bottom_blob.w;
            constants[2].i = bottom_blob.h;
            constants[3].i = bottom_blob.c;
            constants[4].i = bottom_blob.cstep;
            constants[5].i = top_blob.dims;
            constants[6].i = top_blob.w;
            constants[7].i = top_blob.h;
            constants[8].i = top_blob.c;
            constants[9].i = top_blob.cstep;
            constants[10].i = woffset;

            const Pipeline* pipeline = packing == 4 ? pipeline_concat_pack4[b%2] : pipeline_concat[b%2];

            cmd.record_pipeline(pipeline, bindings, constants, bottom_blob);

            woffset += bottom_blob.w;
        }

        return 0;
    }

    return 0;
}

} // namespace ncnn
