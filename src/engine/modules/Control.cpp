// CONTROL family — placeholder until the DSP is implemented (constant 0).
#include "../Module.h"
#include "../ModuleSpec.h"

namespace weft
{
namespace
{
struct Constant final : ControlModule
{
    void prepare (double) override {}
    void reset() override {}
    float process (const ControlContext&, const ParamInputs&) override { return 0.0f; }
};
}

std::unique_ptr<ControlModule> createControlModuleImpl (ModuleType t)
{
    if (! isControlType (t)) return nullptr;
    return std::make_unique<Constant>();
}
} // namespace weft
