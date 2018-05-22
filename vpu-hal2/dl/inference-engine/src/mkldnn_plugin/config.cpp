//
// INTEL CONFIDENTIAL
// Copyright 2016 Intel Corporation.
//
// The source code contained or described herein and all documents
// related to the source code ("Material") are owned by Intel Corporation
// or its suppliers or licensors. Title to the Material remains with
// Intel Corporation or its suppliers and licensors. The Material may
// contain trade secrets and proprietary and confidential information
// of Intel Corporation and its suppliers and licensors, and is protected
// by worldwide copyright and trade secret laws and treaty provisions.
// No part of the Material may be used, copied, reproduced, modified,
// published, uploaded, posted, transmitted, distributed, or disclosed
// in any way without Intel's prior express written permission.
//
// No license under any patent, copyright, trade secret or other
// intellectual property right is granted to or conferred upon you by
// disclosure or delivery of the Materials, either expressly, by implication,
// inducement, estoppel or otherwise. Any license under such intellectual
// property rights must be express and approved by Intel in writing.
//
// Include any supplier copyright notices as supplier requires Intel to use.
//
// Include supplier trademarks or logos as supplier requires Intel to use,
// preceded by an asterisk. An asterisked footnote can be added as follows:
// *Third Party trademarks are the property of their respective owners.
//
// Unless otherwise agreed by Intel in writing, you may not remove or alter
// this notice or any other notice embedded in Materials by Intel or Intel's
// suppliers or licensors in any way.
//

#include "config.h"
#include "ie_plugin_config.hpp"
#include "ie_common.h"

#include <string>
#include <map>
#include <algorithm>
#include <cpp_interfaces/exception2status.hpp>

namespace MKLDNNPlugin {

using namespace InferenceEngine;

void Config::readProperties(const std::map<std::string, std::string> &prop) {
    for (auto& kvp : prop) {
        std::string key = kvp.first;
        std::string val = kvp.second;

        if (key == PluginConfigParams::KEY_CPU_BIND_THREAD) {
            if (val == PluginConfigParams::YES) useThreadBinding = true;
            else if (val == PluginConfigParams::NO) useThreadBinding = false;
            else
                THROW_IE_EXCEPTION << "Wrong value for property key " << PluginConfigParams::KEY_CPU_BIND_THREAD
                                   << ". Expected only YES/NO";
        } else if (key == PluginConfigParams::KEY_DYN_BATCH_LIMIT) {
            int val_i = std::stoi(val);
            // zero and any negative value will be treated
            // as default batch size
            batchLimit = std::max(val_i, 0);
        } else if (key == PluginConfigParams::KEY_PERF_COUNT) {
            if (val == PluginConfigParams::YES) collectPerfCounters = true;
            else if (val == PluginConfigParams::NO) collectPerfCounters = false;
            else
                THROW_IE_EXCEPTION << "Wrong value for property key " << PluginConfigParams::KEY_PERF_COUNT
                                   << ". Expected only YES/NO";
        } else if (key == PluginConfigParams::KEY_EXCLUSIVE_ASYNC_REQUESTS) {
            if (val == PluginConfigParams::YES) exclusiveAsyncRequests = true;
            else if (val == PluginConfigParams::NO) exclusiveAsyncRequests = false;
            else
                THROW_IE_EXCEPTION << "Wrong value for property key " << PluginConfigParams::KEY_EXCLUSIVE_ASYNC_REQUESTS
                                   << ". Expected only YES/NO";
        } else {
            THROW_IE_EXCEPTION << NOT_FOUND_str << "Unsupported property key [" << key << "] by CPU plugin";
		
        }
    }
}

}  // namespace MKLDNNPlugin
