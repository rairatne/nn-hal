#ifndef __DEVICE_PLUGIN_H
#define __DEVICE_PLUGIN_H

#include <cpp/ie_cnn_network.h>
#include <cpp/ie_executable_network.hpp>
#include <cpp/ie_infer_request.hpp>
#include <ie_core.hpp>
#include <ie_input_info.hpp>
#include <openvino/openvino.hpp>
#include <openvino/runtime/infer_request.hpp>
#include <vector>

#include <android-base/logging.h>
#include <android/log.h>
#include <cutils/properties.h>
#include <log/log.h>
#include "utils.h"

namespace android::hardware::neuralnetworks::nnhal {

class IIENetwork {
public:
    virtual ~IIENetwork() = default;
    virtual void loadNetwork(const std::string& model_name) = 0;
    virtual bool createNetwork(std::shared_ptr<ov::Model> network, const std::string& ir_xml,
                               const std::string& ir_bin) = 0;
    virtual ov::InferRequest getInferRequest() = 0;
    virtual void infer() = 0;
    virtual bool queryState() = 0;
    virtual ov::Tensor getTensor(const std::string& outName) = 0;
    virtual ov::Tensor getInputTensor(const std::size_t index) = 0;
    virtual ov::Tensor getOutputTensor(const std::size_t index) = 0;
};

// Abstract this class for all accelerators
class IENetwork : public IIENetwork {
private:
    IntelDeviceType mTargetDevice;
    ov::InferRequest mInferRequest;
    bool isLoaded = false;

public:
    IENetwork(IntelDeviceType device) : mTargetDevice(device) {}

    virtual void loadNetwork(const std::string& model_name);
    virtual bool createNetwork(std::shared_ptr<ov::Model> network, const std::string& ir_xml,
                               const std::string& ir_bin);
    ov::Tensor getTensor(const std::string& outName);
    ov::Tensor getInputTensor(const std::size_t index);
    ov::Tensor getOutputTensor(const std::size_t index);
    ov::InferRequest getInferRequest() { return mInferRequest; }
    bool queryState() { return isLoaded; }
    void infer();
    bool getGrpcIpPort(char* ip_port);
};

}  // namespace android::hardware::neuralnetworks::nnhal

#endif
