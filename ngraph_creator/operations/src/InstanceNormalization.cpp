#include <InstanceNormalization.hpp>
#undef LOG_TAG
#define LOG_TAG "InstanceNormalization"

namespace android {
namespace hardware {
namespace neuralnetworks {
namespace nnhal {

InstanceNormalization::InstanceNormalization(int operationIndex, GraphMetadata graphMetadata ) : OperationsBase(operationIndex, graphMetadata ) {
    mDefaultOutputIndex = mOpModelInfo->getOperationOutput(mNnapiOperationIndex, 0);
}

bool InstanceNormalization::validate() {
    ALOGV("%s Entering", __func__);
    // check output type
    if (!checkOutputOperandType(0, (int32_t)OperandType::TENSOR_FLOAT32)) {
        ALOGE("%s Output operand 0 is not of type FP32. Unsupported operation", __func__);
        return false;
    }

    // Check Input Type
    if (!checkInputOperandType(0, (int32_t)OperandType::TENSOR_FLOAT32)) {
        ALOGE("%s Input operand 0 is not of type FP32. Unsupported operation", __func__);
        return false;
    }
    const auto inputRank = getInputOperandDimensions(0).size();
    if ((inputRank > 4) || (!isValidInputTensor(0))) {
        ALOGE("%s Invalid dimensions size for input(%lu)", __func__, inputRank);
        return false;
    }

    ALOGV("%s PASSED", __func__);
    return true;
}

std::shared_ptr<ov::Node> InstanceNormalization::createNode() {
    ALOGV("%s Entering", __func__);

    std::shared_ptr<ov::Node> inputNode;
    bool useNchw = false;
    const auto& inputsSize = mOpModelInfo->getOperationInputsSize(mNnapiOperationIndex);
    ALOGD("%s inputsSize %lu", __func__, inputsSize);

    // Read inputs
    inputNode = getInputNode(0);
    auto gamma = mOpModelInfo->ParseOperationInput<float>(mNnapiOperationIndex, 1);
    auto beta = mOpModelInfo->ParseOperationInput<float>(mNnapiOperationIndex, 2);
    auto epsilon = mOpModelInfo->ParseOperationInput<float>(mNnapiOperationIndex, 3);
    auto layout = mOpModelInfo->ParseOperationInput<uint8_t>(mNnapiOperationIndex, 4);
    if (layout) useNchw = true;

    if (!useNchw)  // No conversion needed if useNchw set
        inputNode = transpose(NHWC_NCHW, inputNode);

    // output[b, h, w, c] =   (input[b, h, w, c] - mean[b, c]) * gamma /
    //                                         sqrt(var[b, c] + epsilon) + beta
    // Instance Normalizatiom = MVN * gamma + beta
    bool normalize_variance = true;
    auto gammaNode = createConstNode(ov::element::f32, {1}, convertToVector(gamma));
    auto betaNode = createConstNode(ov::element::f32, {1}, convertToVector(beta));

    // Axis along which mean and variance is calculated
    std::vector<int32_t> axes{2, 3};
    std::shared_ptr<ov::Node> inputAxesNode = createConstNode(ov::element::i32, {2}, axes);
    std::shared_ptr<ov::Node> mvnNode = std::make_shared<ov::op::v6::MVN>(
        inputNode, inputAxesNode, normalize_variance, epsilon, ov::op::MVNEpsMode::INSIDE_SQRT);

    auto mulGamma = std::make_shared<ov::opset3::Multiply>(
        mvnNode, gammaNode, ov::op::AutoBroadcastType::NUMPY);
    std::shared_ptr<ov::Node> outputNode =
        std::make_shared<ov::opset3::Add>(mulGamma, betaNode);

    if (!useNchw) outputNode = transpose(NCHW_NHWC, outputNode);
    ALOGV("%s PASSED", __func__);

    return outputNode;
}

}  // namespace nnhal
}  // namespace neuralnetworks
}  // namespace hardware
}  // namespace android
