// Built-in resources — placeholder (implemented by the SOURCE family work).
#include "../Builtins.h"

namespace weft
{
const std::vector<std::string>& builtinSampleNames() { static const std::vector<std::string> n { "rain_tin.wav" }; return n; }
std::shared_ptr<const AudioResource> builtinSample (const std::string&) { return nullptr; }
std::shared_ptr<const AudioResource> builtinWavetable (int) { return nullptr; }
std::shared_ptr<const AudioResource> makeWavetableFromSample (const AudioResource&, const std::string&) { return nullptr; }
std::shared_ptr<const AudioResource> makeSampleResource (const std::string& name, float sr, int ch, std::vector<float> data)
{
    auto r = std::make_shared<AudioResource>();
    r->name = name; r->sampleRate = sr; r->channels = ch; r->data = std::move (data);
    return r;
}
} // namespace weft
