// Player2D - side-scrolling controller: run, jump, and detect the goal.
// A/R/D or arrows to move, Space/W to jump. Demonstrates Input, physics and
// trigger callbacks together.
#include <cairn/cairn.h>
using namespace Cairn;

class Player2D : public Component {
public:
    void OnStart() override {
        Debug::Log("Player2D ready");
    }

    void FixedUpdate(float dt) override {
        RigidBody* rb = gameObject().GetComponent<RigidBody>();
        if (!rb) return;

        float axis = Input::GetAxis("Horizontal");
        rb->velocity.x = axis * PropFloat("moveSpeed");

        // Velocity-based ground check: near-zero vertical speed means we can jump.
        if (std::fabs(rb->velocity.y) < 0.15f) m_grounded = true;

        if (Input::GetKeyDown(KeyCode::Space) || Input::GetKeyDown(KeyCode::W) || Input::GetKeyDown(KeyCode::Up)) {
            if (m_grounded) {
                rb->velocity.y = PropFloat("jumpSpeed");
                m_grounded = false;
            }
        }
    }

    void OnTriggerEnter(Component* other) override {
        if (other && other->gameObject().name == "Goal") {
            Debug::Log("You reached the goal!");
        }
    }

    void DescribeProps(ScriptMeta& meta) override {
        meta.Add("moveSpeed", PropType::Float, 6.0f);
        meta.Add("jumpSpeed", PropType::Float, 11.0f);
    }

private:
    bool m_grounded = false;
};

CAIRN_SCRIPT(Player2D)
