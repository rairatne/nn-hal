// Copyright (c) 2017 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ie_util_internal.hpp"
#include "graph_tools.hpp"
#include "caseless.hpp"
#include "ie_utils.hpp"

#include <ie_layers.h>

#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <functional>
#include <string>
#include <deque>
#include <cassert>
#include <memory>
#include <utility>
#include <iomanip>

namespace {

InferenceEngine::LayerComplexity getComplexity(const InferenceEngine::CNNLayerPtr &layer) {
    using namespace InferenceEngine;
    using namespace std::placeholders;
    auto type = layer->type;
    auto &outDims = layer->outData[0]->getDims();
    auto &inDims = layer->insData[0].lock()->getDims();
    unsigned long flops = 0, params = 0;

    size_t out_size = accumulate(outDims.begin(), outDims.end(),
        1u, std::multiplies<size_t>{});
    size_t in_size = accumulate(inDims.begin(), inDims.end(),
        1u, std::multiplies<size_t>{});

    auto eltwise_complexity = [&](CNNLayer &l, size_t flops_rate, size_t params_rate) {
        flops = flops_rate * out_size;
        params = params_rate * out_size;
    };

    auto scale_complexity = [&](CNNLayer &l) {
        flops = 2 * out_size;
        params = 2 * outDims[1];
    };
    const caseless_unordered_map<std::string,
                                 std::function<void(CNNLayer &)>> layerComplexityLookup = {
        {"Convolution", [&](CNNLayer &l) {
            auto* conv = dynamic_cast<ConvolutionLayer*>(&l);
            unsigned long filter_m = conv->_kernel_x * conv->_kernel_y * (inDims[1] / conv->_group);
            flops = 2 * out_size * filter_m;
            params = filter_m * conv->_out_depth + conv->_out_depth;
        }},

        {"Deconvolution", [&](CNNLayer &l) {
            auto* deconv = dynamic_cast<DeconvolutionLayer*>(&l);
            unsigned long filter_m = deconv->_kernel_x * deconv->_kernel_y * (inDims[1] / deconv->_group);
            flops = 2 * out_size * filter_m;
            params = filter_m * deconv->_out_depth + deconv->_out_depth;
        }},

        {"FullyConnected", [&](CNNLayer &l) {
            auto* fc = dynamic_cast<FullyConnectedLayer*>(&l);
            flops = 2 * in_size * fc->_out_num;
            params = (in_size + 1) * fc->_out_num;
        }},

        {"Norm", [&](CNNLayer &l) {
            auto* lrn = dynamic_cast<NormLayer*>(&l);
            int size = lrn->_size;
            int flopsPerElement = lrn->_isAcrossMaps ? 2 * size * size : 2 * size;

            flops = in_size * flopsPerElement;
        }},

        {"Pooling", [&](CNNLayer &l) {
            auto* pool = dynamic_cast<PoolingLayer*>(&l);
            if (pool->_type == PoolingLayer::PoolType::ROI) {
                // real kernel sizes are read from weights, so approximation is used.
                unsigned long kernel_w = inDims[2] / outDims[2];
                unsigned long kernel_h = inDims[3] / outDims[3];

                flops = out_size * kernel_h * kernel_w;
            } else {
                flops = out_size * (pool->_kernel_y * pool->_kernel_y);
            }
        }},

        {"Eltwise", [&](CNNLayer &l) {
            auto* eltwise = dynamic_cast<EltwiseLayer*>(&l);
            flops = in_size * (2 * eltwise->insData.size() - 1);
        }},

        {"Power", std::bind(eltwise_complexity, _1, 3, 0)},
        {"Normalize", std::bind(eltwise_complexity, _1, 4, 0)},
        {"ReLU", std::bind(eltwise_complexity, _1, 1, 0)},
#ifdef AKS
        {"TanH", std::bind(eltwise_complexity, _1, 1, 0)},
        {"Sigmoid", std::bind(eltwise_complexity, _1, 1, 0)},
#endif
        {"Clamp", std::bind(eltwise_complexity, _1, 2, 0)},
        {"BatchNormalization", scale_complexity},
        {"ScaleShift", scale_complexity},

        // roughly count exp as 1 flop
        {"SoftMax", std::bind(eltwise_complexity, _1, 4, 0)},
    };

    if (layerComplexityLookup.count(type) > 0) {
        layerComplexityLookup.at(type)(*layer);
    }
    return {flops, params};
}

}  // namespace

namespace InferenceEngine {

std::unordered_map<std::string,
                   LayerComplexity> getNetworkComplexity(const InferenceEngine::ICNNNetwork &network) {
    std::unordered_map<std::string, LayerComplexity> networkComplexity;
    InferenceEngine::InputsDataMap networkInputs;
    network.getInputsInfo(networkInputs);
    if (networkInputs.empty()) {
        THROW_IE_EXCEPTION << "No inputs detected.";
    }

    // Get all network inputs
    CNNLayerSet inputs;
    for (auto input : networkInputs) {
        for (auto l : input.second->getInputData()->inputTo) {
            inputs.insert(l.second);
        }
    }

    CNNNetForestDFS(inputs, [&](InferenceEngine::CNNLayerPtr layer) {
        networkComplexity.emplace(layer->name, getComplexity(layer));
    }, false);
    return networkComplexity;
}

}   // namespace InferenceEngine
