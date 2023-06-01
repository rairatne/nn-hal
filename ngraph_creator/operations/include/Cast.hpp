#pragma once

#include <OperationsBase.hpp>

namespace android {
namespace hardware {
namespace neuralnetworks {
namespace nnhal {

class Cast : public OperationsBase {
public:
    Cast(int operationIndex, GraphMetadata graphMetadata);
    std::shared_ptr<ov::Node> createNode() override;
    void connectOperationToGraph() override;
};

}  // namespace nnhal
}  // namespace neuralnetworks
}  // namespace hardware
}  // namespace android
