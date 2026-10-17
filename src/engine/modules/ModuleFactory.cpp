#include "../Module.h"
#include "../ModuleSpec.h"

namespace weft
{
std::unique_ptr<AudioModule> createAudioModule (ModuleType t)
{
    switch (getSpec (t).family)
    {
        case Family::Source:
        case Family::Utility: return createSourceModule (t);
        case Family::Shape:   return createShapeModule (t);
        case Family::Space:   return createSpaceModule (t);
        case Family::Control: return nullptr;
    }
    return nullptr;
}

std::unique_ptr<ControlModule> createControlModule (ModuleType t)
{
    return isControlType (t) ? createControlModuleImpl (t) : nullptr;
}
} // namespace weft
