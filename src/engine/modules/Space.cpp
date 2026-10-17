// Space family — placeholder until the DSP is implemented (passes audio through).
#include "../Module.h"
#include "../ModuleSpec.h"

namespace weft
{
namespace
{
struct Passthrough final : AudioModule
{
    void prepare (double) override {}
    void reset() override {}
    void process (const ProcessContext&, const ParamInputs&, float* const*, int) override {}
};
}

std::unique_ptr<AudioModule> createSpaceModule (ModuleType t)
{
    if (t == ModuleType::None) return nullptr;
    return std::make_unique<Passthrough>();
}
} // namespace weft
