#ifndef EVO_I_STATEFUL_FEATURE_HPP
#define EVO_I_STATEFUL_FEATURE_HPP

namespace evo {

class IStateMachine;

/**
 * @brief Architectural contract enforcing that features (UI screens, background controllers,
 * media engines) must govern their lifecycle and operation via a formal State Machine.
 *
 * Any future feature added to EVO Player MUST implement this contract.
 */
class IStatefulFeature {
public:
    virtual ~IStatefulFeature() = default;

    /**
     * @brief Access the state machine governing this feature.
     * @return Pointer to the active state machine instance.
     */
    virtual IStateMachine* getStateMachine() = 0;
    virtual const IStateMachine* getStateMachine() const = 0;
};

} // namespace evo

#endif // EVO_I_STATEFUL_FEATURE_HPP
