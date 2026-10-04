// Rotator - spins the entity around its Y axis at a configurable speed.
#include <cairn/cairn.h>
using namespace Cairn;

class Rotator : public Component {
public:
    void OnUpdate(float deltaTime) override {
        Vec3 euler = transform().GetEulerDegrees();
        euler.y += PropFloat("speed") * deltaTime;
        transform().SetEulerDegrees(euler);
    }

    void DescribeProps(ScriptMeta& meta) override {
        meta.Add("speed", PropType::Float, 90.0f);
    }
};

CAIRN_SCRIPT(Rotator)
