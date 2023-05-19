#include <OperationsBase.hpp>
#undef LOG_TAG
#define LOG_TAG "OperationsBase"

namespace android {
namespace hardware {
namespace neuralnetworks {
namespace nnhal {

IntelDeviceType OperationsBase::sPluginType;
std::shared_ptr<NnapiModelInfo> OperationsBase::sModelInfo;

std::shared_ptr<ov::Node> OperationsBase::transpose(ConversionType type,
                                                    ov::Output<ov::Node> input) {
    ov::AxisVector order;
    switch (type) {
        case NHWC_NCHW:
            order = {0, 3, 1, 2};
            break;
        case NCHW_NHWC:
            order = {0, 2, 3, 1};
            break;
        case IHWO_OIHW:
            order = {3, 0, 1, 2};
            break;
        case OHWI_OIHW:
            order = {0, 3, 1, 2};
            break;
        case NHWC_CWHN:
            order = {3, 2, 1, 0};
            break;
        case CWHN_NHWC:
            order = {3, 2, 1, 0};
            break;
        case NHC_NCH:
            order = {0, 2, 1};
            break;
        case NCH_NHC:
            order = {0, 1, 2};
            break;
        case CNH_NHC:
            order = {1, 2, 0};
            break;
        case NHC_CNH:
            order = {2, 0, 1};
            break;
        case BTS_TBS:
            order = {1, 0, 2};
            break;
        case NHCW_NHWC:
            order = {0, 1, 3, 2};
            break;
        case NC_CN:
            order = {1, 0};
            break;
        default:
            ALOGE("Invalid transpose operation !!");
            break;
    }
    const auto order_node =
        ov::opset3::Constant::create(ov::element::i64, ov::Shape{order.size()}, order);
    return std::make_shared<ov::opset3::Transpose>(input, order_node);
}

// override createNodeForPlugin in case sPluginType specific implementation is required
std::shared_ptr<ov::Node> OperationsBase::createNodeForPlugin() { return createNode(); }

// override connectOperationToGraph in case Operation has multiple outputs
void OperationsBase::connectOperationToGraph() {
    auto outputNode = createNodeForPlugin();
    const auto op = sModelInfo->getOperand(mDefaultOutputIndex);
    if (op.type == OperandType::TENSOR_QUANT8_ASYMM) {
        outputNode = QuantizeNode(outputNode, mDefaultOutputIndex, ov::element::u8);
    }
    if (op.type == OperandType::TENSOR_QUANT8_ASYMM_SIGNED ||
        op.type == OperandType::TENSOR_QUANT8_SYMM) {
        outputNode = QuantizeNode(outputNode, mDefaultOutputIndex, ov::element::i8);
    }
    if (op.type == OperandType::TENSOR_QUANT16_ASYMM) {
        outputNode = QuantizeNode(outputNode, mDefaultOutputIndex, ov::element::u16);
    }
    if (op.type == OperandType::TENSOR_QUANT16_SYMM) {
        outputNode = QuantizeNode(outputNode, mDefaultOutputIndex, ov::element::i16);
    }
    if (op.lifetime == OperandLifeTime::SUBGRAPH_OUTPUT) {
        // addResultNode(mDefaultOutputIndex, outputNode);
    }
    if (outputNode != nullptr) {
        mNgraphNodes->setOutputAtOperandIndex(mDefaultOutputIndex, outputNode->get_default_output());
    } else {
        ALOGE("%s invalid nullptr output node encountered", __func__);
    }
}

void OperationsBase::addResultNode(size_t index, std::shared_ptr<ov::Node> resultNode) {
    mNgraphNodes->setResultNode(index, resultNode);
}

OperationsBase::OperationsBase(int operationIndex) : mNnapiOperationIndex(operationIndex) {
    mDefaultOutputIndex = 0;
}

void OperationsBase::setNgraphNodes(std::shared_ptr<NgraphNodes> nodes) { mNgraphNodes = nodes; }

bool OperationsBase::validate() { return true; }

bool OperationsBase::validateForPlugin() {
    // Add any plugin specific validations
    return validate();
}

bool OperationsBase::checkOperandType(uint32_t operandIndex, const int32_t expectedOperandType,
                                      const std::string& strLogInfo) {
    const auto operandType = (int32_t)sModelInfo->getOperandType(operandIndex);
    if (operandType != expectedOperandType) {
        ALOGV("OperationIndex %d %s Index %d type %d invalid", mNnapiOperationIndex,
              strLogInfo.c_str(), operandIndex, operandType);
        return false;
    }
    ALOGV("OperationIndex %d %s Index %d type %d PASSED", mNnapiOperationIndex, strLogInfo.c_str(),
          operandIndex, operandType);
    return true;
}
bool OperationsBase::checkOutputOperandType(uint32_t index, const int32_t expectedOperandType) {
    const auto& operandIndex = sModelInfo->getOperationOutput(mNnapiOperationIndex, index);
    return checkOperandType(operandIndex, expectedOperandType, "Output");
}
bool OperationsBase::checkInputOperandType(uint32_t index, const int32_t expectedOperandType) {
    const auto& operandIndex = sModelInfo->getOperationInput(mNnapiOperationIndex, index);
    return checkOperandType(operandIndex, expectedOperandType, "Input");
}
const vec<uint32_t> OperationsBase::getInputOperandDimensions(uint32_t inputIndex) {
    const auto& operandIndex = sModelInfo->getOperationInput(mNnapiOperationIndex, inputIndex);
    const auto& operand = sModelInfo->getOperand(operandIndex);
    return operand.dimensions;
}

bool OperationsBase::isValidInputTensor(uint32_t inputIndex) {
    size_t size = 1;
    const auto& dims = getInputOperandDimensions(inputIndex);
    ALOGV("%s dims.size(%lu)", __func__, dims.size());
    if (dims.empty()) return false;

    for (auto d : dims) size *= d;
    if (size == 0) return false;

    return true;
}

std::shared_ptr<ov::Node> OperationsBase::QuantizeNode(std::shared_ptr<ov::Node> input,
                                                       size_t index,
                                                       ov::element::Type quantizeType) {
    auto floatElementType = ov::element::f32;
    auto intElementType = ov::element::i32;

    float inputScale = sModelInfo->getOperandScale(index);
    int inputZeroPoint = sModelInfo->getOperandZeroPoint(index);

    auto scale = createConstNode(floatElementType, {}, convertToVector(inputScale));
    auto zeroPoint = createConstNode(intElementType, {}, convertToVector(inputZeroPoint));

    if (input->get_element_type() != ov::element::f32) {
        input = std::make_shared<ov::opset3::Convert>(input, floatElementType);
    }
    auto div = std::make_shared<ov::opset3::Divide>(input, scale);
    ov::op::v5::Round::RoundMode mode = ov::op::v5::Round::RoundMode::HALF_TO_EVEN;
    auto round = std::make_shared<ov::op::v5::Round>(div, mode);
    auto convertRound = std::make_shared<ov::opset3::Convert>(round, ov::element::i32);
    auto sum = std::make_shared<ov::opset3::Add>(convertRound, zeroPoint);
    std::shared_ptr<ov::Node> data;
    const auto operand = sModelInfo->getOperand(index);
    if (operand.type == OperandType::TENSOR_QUANT8_ASYMM)
        data = std::make_shared<ov::opset3::Clamp>(sum, 0, 255);
    else if (operand.type == OperandType::TENSOR_QUANT8_SYMM ||
             operand.type == OperandType::TENSOR_QUANT8_ASYMM_SIGNED)
        data = std::make_shared<ov::opset3::Clamp>(sum, -128, 127);
    else if (operand.type == OperandType::TENSOR_QUANT16_SYMM)
        data = std::make_shared<ov::opset3::Clamp>(sum, -32768, 32767);
    else if (operand.type == OperandType::TENSOR_QUANT16_ASYMM)
        data = std::make_shared<ov::opset3::Clamp>(sum, 0, 65535);

    std::shared_ptr<ov::Node> outputNode;
    if (data != nullptr && data->get_element_type() != quantizeType) {
        outputNode = std::make_shared<ov::opset3::Convert>(data, quantizeType);
    } else {
        outputNode = data;
    }
    return outputNode;
}

std::shared_ptr<ov::Node> OperationsBase::DequantizeNode(std::shared_ptr<ov::Node> input,
                                                         uint32_t index,
                                                         ov::element::Type dequantizeType) {
    const auto operand = sModelInfo->getOperand(index);
    std::shared_ptr<ov::Node> outputNode;

    if (input->get_element_type() != ov::element::f32)
        input = std::make_shared<ov::opset3::Convert>(input, ov::element::f32);

    if (operand.type == OperandType::TENSOR_QUANT8_SYMM_PER_CHANNEL) {
        vec<float> inputScales = operand.extraParams.channelQuant().scales;
        auto channelDim = operand.extraParams.channelQuant().channelDim;
        const auto inputRank = getInputOperandDimensions(0).size();

        std::vector<size_t> shape(inputRank - channelDim, 1);
        shape[0] = inputScales.size();

        auto scaleNode = createConstNode(ov::element::f32, ov::Shape{shape}, inputScales);
        outputNode = std::make_shared<ov::opset3::Multiply>(input, scaleNode);
    } else {
        auto scaleNode = createConstNode(ov::element::f32, {},
                                         convertToVector(sModelInfo->getOperandScale(index)));
        auto zeroPointNode = createConstNode(
            ov::element::f32, {}, convertToVector(sModelInfo->getOperandZeroPoint(index)));

        if (operand.type == OperandType::TENSOR_QUANT8_ASYMM ||
            operand.type == OperandType::TENSOR_QUANT16_ASYMM ||
            operand.type == OperandType::TENSOR_QUANT8_ASYMM_SIGNED)
            input = std::make_shared<ov::opset3::Subtract>(input, zeroPointNode);

        auto mul = std::make_shared<ov::opset3::Multiply>(input, scaleNode);
        outputNode = mul;
    }

    if (dequantizeType == ov::element::f16) {
        outputNode = std::make_shared<ov::opset3::Convert>(outputNode, dequantizeType);
    }

    return outputNode;
}

}  // namespace nnhal
}  // namespace neuralnetworks
}  // namespace hardware
}  // namespace android
