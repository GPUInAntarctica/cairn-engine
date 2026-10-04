// CameraOrbit - slowly orbits the camera around the world origin.
#include <cairn/cairn.h>
using namespace Cairn;

class CameraOrbit : public Component {
public:
    void OnStart() override {
        m_radius = PropFloat("radius");
        m_height = PropFloat("height");
        m_angle = transform().GetEulerDegrees().y;
    }

    void OnUpdate(float deltaTime) override {
        m_angle += PropFloat("speed") * deltaTime;
        float rad = Cairn::Radians(m_angle);
        transform().position = Vec3(std::sin(rad) * m_radius, m_height, std::cos(rad) * m_radius);
        transform().SetEulerDegrees(Vec3(-15.0f, m_angle + 180.0f, 0.0f));
    }

    void DescribeProps(ScriptMeta& meta) override {
        meta.Add("speed", PropType::Float, 25.0f);
        meta.Add("radius", PropType::Float, 9.0f);
        meta.Add("height", PropType::Float, 4.0f);
    }

private:
    float m_radius = 9.0f;
    float m_height = 4.0f;
    float m_angle = 0.0f;
};

CAIRN_SCRIPT(CameraOrbit)
