#include <Relu.hpp>
#undef LOG_TAG
#define LOG_TAG "Relu"

namespace android {
namespace hardware {
namespace neuralnetworks {
namespace nnhal {

Relu::Relu(int operationIndex) : OperationsBase(operationIndex) {
    mDefaultOutputIndex = sModelInfo->getOperationOutput(mNnapiOperationIndex, 0);
}

std::shared_ptr<ov::Node> Relu::createNode() {
    // Creating input nodes
    std::shared_ptr<ov::Node> input;

    input = getInputNode(0);

    std::shared_ptr<ov::Node> outputNode;

    outputNode = std::make_shared<ov::opset3::Relu>(input);

    return outputNode;
}

}  // namespace nnhal
}  // namespace neuralnetworks
}  // namespace hardware
}  // namespace android
